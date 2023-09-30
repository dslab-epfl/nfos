/*
 * Maglev impl.
 */

#include "nf.h"
#include "nf-log.h"
#include "maglev_config.h"

#include "vigor/libvig/verified/ether.h"
#include "double-chain-exp.h"
#include "map.h"
#include "vector.h"
#include "cht.h"

// temp hack for transaction chopping
#include "rlu-wrapper.h"

#include "load-balance.h"
#include "mtrie.h"
#include "fib_table.h"
#include "scalability-profiler.h"

/* pkt set auxiliart func */

bool pkt_set_id_eq(void* a, void* b) {
  pkt_set_id_t *id1 = (pkt_set_id_t *) a;
  pkt_set_id_t *id2 = (pkt_set_id_t *) b;
  return (id1->src_ip == id2->src_ip)
    && (id1->dst_ip == id2->dst_ip)
    && (id1->src_port == id2->src_port)
    && (id1->dst_port == id2->dst_port)
    && (id1->protocol == id2->protocol);
}

unsigned pkt_set_id_hash(void* obj) {
  pkt_set_id_t *id = (pkt_set_id_t *) obj;

  unsigned hash = 0;
  hash = __builtin_ia32_crc32si(hash, id->src_ip);
  hash = __builtin_ia32_crc32si(hash, id->dst_ip);
  hash = __builtin_ia32_crc32si(hash, id->src_port);
  hash = __builtin_ia32_crc32si(hash, id->dst_port);
  hash = __builtin_ia32_crc32si(hash, id->protocol);
  return hash;
}

static inline unsigned get_hash_from_pkt(const pkt_t *pkt) {
  unsigned hash = 0;
  hash = __builtin_ia32_crc32si(hash, pkt->ipv4_header->src_addr);
  hash = __builtin_ia32_crc32si(hash, pkt->ipv4_header->dst_addr);
  hash = __builtin_ia32_crc32si(hash, pkt->tcpudp_header->src_port);
  hash = __builtin_ia32_crc32si(hash, pkt->tcpudp_header->dst_port);
  hash = __builtin_ia32_crc32si(hash, pkt->ipv4_header->next_proto_id);
  return hash;
}

void pkt_set_state_allocate(void *obj) {
  return;
}

typedef struct nf_config {
  // WAN device, i.e. the device that receives the client packets
  uint16_t wan_device[2];

  // MAC addresses of the devices the backends are connected to
  struct rte_ether_addr *device_macs;

  // Size of the flow table
  uint32_t flow_capacity;

  // Expiration time of flows in microseconds
  vigor_time_t flow_expiration_time;

  // The maximum number of backends we can balance at the same time
  uint32_t backend_capacity;

  /* 
   * The height of the consistent hashing table.
   * Bigger value induces more memory usage, but can achieve finer
   * granularity.
   */
  uint32_t cht_height;

  /* 
   * The time in microseconds for which the load balancer is willing to wait
   * hoping to get another heartbeat. If no heartbeat comes for a host for this
   * time, it is considered down and removed from the pool of backends.
   */
  vigor_time_t backend_expiration_time;

  // fib-related stuff 
  uint32_t lb_pool_capacity;
  uint32_t ip4_ply_pool_capacity;
  uint32_t n_fib_table;
} nf_config_t;


/* Backend info and auxiliary callbacks */
typedef struct backend_info {
  // The ethernet device on the load balancer that connects to the backend
  uint16_t dev;

  // The dst MAC of packets which the load balancer sends to the backend 
  struct rte_ether_addr mac;

  // The IP of the backend
  uint32_t ip;
} backend_info_t;

// Todo: Auto-gen these callbacks as in vigor
void backend_info_allocate(void* obj) {
  backend_info_t *id = (backend_info_t *) obj;
  id->dev = 0;
  id->mac.addr_bytes[0] = 0;
  id->mac.addr_bytes[1] = 0;
  id->mac.addr_bytes[2] = 0;
  id->mac.addr_bytes[3] = 0;
  id->mac.addr_bytes[4] = 0;
  id->mac.addr_bytes[5] = 0;

  id->ip = 0;
}


/* ip_addr and auxiliary callbacks */
struct ip_addr {
  uint32_t addr;
};

bool ip_addr_eq(void* a, void* b) {
  struct ip_addr* id1 = (struct ip_addr*) a;
  struct ip_addr* id2 = (struct ip_addr*) b;
  return (id1->addr == id2->addr);
}

unsigned ip_addr_hash(void* obj) {
  struct ip_addr* id = (struct ip_addr*) obj;
  unsigned hash = 0;
  hash = __builtin_ia32_crc32si(hash, id->addr);
  return hash;
}

void ip_addr_allocate(void* obj) {
  struct ip_addr* id = (struct ip_addr*) obj;
  id->addr = 0;
}


/* cht auxiliary functions */
static void cht_null_init(void *obj)
{
  *(uint32_t *)obj = 0;
}

/* fib-related stuff auxiliary functions */
void lb_null_init(void *obj){
  load_balance_t *lb = (load_balance_t *) obj;
  lb->n_buckets = 0;
  lb->lb_buckets = NULL;
}
void ip4_null_init(void *obj){
  ip4_fib_mtrie_8_ply_t *leaf = (ip4_fib_mtrie_8_ply_t*) obj;
  memset(leaf->leaves, 0, PLY_8_SIZE * sizeof(ip4_fib_mtrie_leaf_t));
  memset(leaf->leaves, 0, PLY_8_SIZE * sizeof(uint8_t));
  leaf->n_non_empty_leafs = 0;
  leaf->dst_address_bits_base =0;
}
void fib_table_null_init(void *obj){
  ip4_fib_t *fib = (ip4_fib_t *)obj;
  fib->index = 0;
  memset(fib->mtrie.root_ply.leaves, 0, PLY_16_SIZE * sizeof(ip4_fib_mtrie_leaf_t));
  memset(fib->mtrie.root_ply.dst_address_bits_of_leaves, 0, PLY_16_SIZE *sizeof(uint8_t));
}


struct nf_state {
  /* The first three data structures here form the table of active backends */
  // Mapping from backend ip to backend id
  struct NfosMap *backend_ip_to_backend_id;

  // To use vigor's map, one must maintain a vector of the keys of map entries...
  struct NfosVector *backend_ips;

  // Info of backends. Todo: use vigor's vector to make it symbexable.
  struct NfosVector *backend_info;

  // Id of backends
  struct NfosDoubleChainExp *backends;

  // Consistent hash table mapping flows to backends
  struct NfosVector *cht;

  // Configuration
  nf_config_t *cfg;
};


/* Auxiliary functions */
static inline void send_pkt_to_backend(pkt_t *pkt, backend_info_t *backend,
                                       nf_state_t *non_pkt_set_state) {
  if (backend->dev != non_pkt_set_state->cfg->wan_device[0] &&
      backend->dev != non_pkt_set_state->cfg->wan_device[1]) {
    pkt->ipv4_header->dst_addr = backend->ip;
    pkt->ether_header->s_addr = non_pkt_set_state->cfg->device_macs[backend->dev];
    pkt->ether_header->d_addr = backend->mac;
    // Update checksum
    nf_set_ipv4_udptcp_checksum(pkt->ipv4_header, pkt->tcpudp_header, pkt->raw);
    send_pkt(pkt, backend->dev);
  } else {
    drop_pkt(pkt);
  }
}


/* Init function */

void expire_backend(nf_state_t *non_pkt_set_state);

int client_pkt_handler(nf_state_t *non_pkt_set_state, pkt_t *pkt,
                        uint16_t incoming_dev, pkt_set_state_t *local_state,
                        pkt_set_id_t *pkt_set_id);

int heartbeat_handler(nf_state_t *non_pkt_set_state, pkt_t *pkt,
                       uint16_t incoming_dev, pkt_set_state_t *unused1,
                       pkt_set_id_t *unused2);

nf_state_t *nf_init(vigor_time_t *validity_duration_out, char **lcores_out, bool *has_related_pkt_sets_out,
                    bool *do_expiration_out, bool *has_pkt_sets_out) {
   // List of cores to use
  *lcores_out = LCORES;
  // Register packet handlers
  pkt_handler_t *handlers = malloc(sizeof(pkt_handler_t) * 2);
  handlers[0] = client_pkt_handler;
  handlers[1] = heartbeat_handler;
  register_pkt_handlers(handlers);

  // Other NFOS configs
  *validity_duration_out = EXPIRATION_TIME;
  *has_related_pkt_sets_out = false;
  *do_expiration_out = true;
  *has_pkt_sets_out = true;
                   
  nf_state_t *ret = malloc(sizeof(nf_state_t));

  /* Initialize configuration */
  ret->cfg = malloc(sizeof(nf_config_t));
  ret->cfg->wan_device[0] = WAN_DEVICE_ONE;
  ret->cfg->wan_device[1] = WAN_DEVICE_TWO;
  // For now we assume NFOS support a fixed maximum number of packet sets
  // ret->cfg->flow_capacity = FLOW_CAPACITY;
  ret->cfg->flow_expiration_time = EXPIRATION_TIME;

  // Fill in the mac addresses
  int num_devs = rte_eth_dev_count_avail();
  ret->cfg->device_macs = malloc(num_devs * sizeof(struct rte_ether_addr));
  if (!ret->cfg->device_macs) ret = NULL;
  for (int i = 0; i < num_devs; ++i)
    rte_eth_macaddr_get(i, &(ret->cfg->device_macs[i]));

  ret->cfg->backend_capacity = BACKEND_CAPACITY;
  ret->cfg->backend_expiration_time = BACKEND_EXPIRATION_TIME;

  /* Set up backend table */
  if (!nfos_map_allocate(ip_addr_eq, ip_addr_hash, sizeof(struct ip_addr),
       ret->cfg->backend_capacity, &ret->backend_ip_to_backend_id)) ret = NULL;
  if (!nfos_vector_allocate(sizeof(struct ip_addr), ret->cfg->backend_capacity,
                       ip_addr_allocate, &ret->backend_ips)) ret = NULL;
  if (!nfos_vector_allocate(sizeof(backend_info_t), ret->cfg->backend_capacity,
                       backend_info_allocate, &ret->backend_info)) ret = NULL;
  if (!nfos_dchain_exp_allocate(ret->cfg->backend_capacity,
               ret->cfg->backend_expiration_time, &ret->backends)) ret = NULL;

  // Set up cht
  ret->cfg->cht_height = CHT_HEIGHT;
  if (!nfos_vector_allocate(sizeof(uint32_t), ret->cfg->backend_capacity * ret->cfg->cht_height,
                       cht_null_init, &ret->cht)) ret = NULL;
  if (!nfos_cht_fill_cht(ret->cht, ret->cfg->cht_height, ret->cfg->backend_capacity))
    ret = NULL;

  // Register backend expirator
  if (!register_periodic_handler(PERIODIC_HANDLER_PERIOD, expire_backend))
    ret = NULL;



  /* Initialize fib-related stuff, not core functionality of the NF
   * This is for feature pairing with VPP.
   * Issue: The IP address and mac address are hard coded.
   */

  // Initialize load-balance pool
  ret->cfg->lb_pool_capacity = LB_CAPACITY;
  if (!nfos_vector_allocate(sizeof(load_balance_t), ret->cfg->lb_pool_capacity, lb_null_init, &load_balance_pool)) {
    ret = NULL;
  }
  uint16_t action, send_device;
  rte_be32_t dst_ip_address;
  struct rte_ether_addr dst_mac_address;
  action = DROP;
  send_device = 0xFFFF;
  dst_ip_address = 0xFFFFFFFF;
  for(int i = 0; i < 6; i++) {dst_mac_address.addr_bytes[i] = 0xFF;}
  // TODO: rename the load_balance APIs to avoid confusion with the Maglev Load Balancing.
  uint32_t index0 = load_balance_create(1,&action, &send_device, &dst_ip_address, &dst_mac_address);
  action = LB;
  uint32_t index1 = load_balance_create(1,&action, &send_device, &dst_ip_address, &dst_mac_address);
  load_balance_set_dscp(index1,1);
  uint32_t index2 = load_balance_create(1,&action, &send_device, &dst_ip_address, &dst_mac_address);
  load_balance_set_dscp(index2,2);
  action = FORWARD;
  send_device = WAN_DEVICE_ONE;
  dst_ip_address = 0xAE000302;
  dst_mac_address.addr_bytes[0] = 0x12;
  dst_mac_address.addr_bytes[1] = 0x34;
  dst_mac_address.addr_bytes[2] = 0x56;
  dst_mac_address.addr_bytes[3] = 0x78;
  dst_mac_address.addr_bytes[4] = 0x90;
  dst_mac_address.addr_bytes[5] = 0xab;
  uint32_t index3 = load_balance_create(1,&action, &send_device, &dst_ip_address, &dst_mac_address);
  dst_ip_address = 0xAE000202;
  dst_mac_address.addr_bytes[0] = 0x11;
  uint32_t index4 = load_balance_create(1,&action, &send_device, &dst_ip_address, &dst_mac_address);
  send_device = LAN_DEVICE_ONE;
  dst_ip_address = 0x0A000300;
  dst_mac_address.addr_bytes[0] = 0x13;
  uint32_t index5 = load_balance_create(1,&action, &send_device, &dst_ip_address, &dst_mac_address);
  send_device = LAN_DEVICE_TWO;
  dst_ip_address = 0x0A000400;
  dst_mac_address.addr_bytes[0] = 0x14;
  uint32_t index6 = load_balance_create(1,&action, &send_device, &dst_ip_address, &dst_mac_address);
  
  // Initialize fib_table;
  ret->cfg->n_fib_table = N_FIB_TABLE;
  if (!nfos_vector_allocate(sizeof(ip4_fib_t), ret->cfg->n_fib_table, fib_table_null_init, &ip4_fibs)) {
    ret = NULL;
  }
  for (uint32_t i = 0; i< N_FIB_TABLE; i++){
    ip4_fib_t *fib = ip4_fib_get(i);
    fib->index = i;
    ip4_mtrie_init(&(fib->mtrie));
  }

  // Initialize ip4_ply_pool
  ret->cfg->ip4_ply_pool_capacity = IP4_PLY_POOL_CAPACITY;
  if (!nfos_vector_allocate(sizeof(ip4_fib_mtrie_8_ply_t), ret->cfg->ip4_ply_pool_capacity,ip4_null_init, &ip4_ply_pool)) {
    ret = NULL;
  }

  ip4_fib_t *fib = ip4_fib_get(WAN_DEVICE_ONE);
  ip4_fib_mtrie_route_add(&(fib->mtrie), 0, 0, index0);
  ip4_fib_mtrie_route_add(&(fib->mtrie), 0, 32,index0);
  ip4_fib_mtrie_route_add(&(fib->mtrie), 0x76000001, 32, index1);
  ip4_fib_mtrie_route_add(&(fib->mtrie), 0x76000002, 32, index2);
  ip4_fib_mtrie_route_add(&(fib->mtrie), 0x0A000301, 32, index5);
  ip4_fib_mtrie_route_add(&(fib->mtrie), 0x0A000401, 32, index6);

  fib = ip4_fib_get(WAN_DEVICE_TWO);
  ip4_fib_mtrie_route_add(&(fib->mtrie), 0, 0, index0);
  ip4_fib_mtrie_route_add(&(fib->mtrie), 0, 32,index0);
  ip4_fib_mtrie_route_add(&(fib->mtrie), 0x76000001, 32, index1);
  ip4_fib_mtrie_route_add(&(fib->mtrie), 0x76000002, 32, index2);
  ip4_fib_mtrie_route_add(&(fib->mtrie), 0x0A000301, 32, index5);
  ip4_fib_mtrie_route_add(&(fib->mtrie), 0x0A000401, 32, index6);
  
  fib = ip4_fib_get(LAN_DEVICE_ONE);
  ip4_fib_mtrie_route_add(&(fib->mtrie), 0, 0, index0);
  ip4_fib_mtrie_route_add(&(fib->mtrie), 0, 32,index0);
  ip4_fib_mtrie_route_add(&(fib->mtrie), 0x76000001, 32, index3);
  ip4_fib_mtrie_route_add(&(fib->mtrie), 0x76000002, 32, index4);
  ip4_fib_mtrie_route_add(&(fib->mtrie), 0x0A000301, 32, index5);
  ip4_fib_mtrie_route_add(&(fib->mtrie), 0x0A000401, 32, index6);
  
  fib = ip4_fib_get(LAN_DEVICE_TWO);
  ip4_fib_mtrie_route_add(&(fib->mtrie), 0, 0, index0);
  ip4_fib_mtrie_route_add(&(fib->mtrie), 0, 32,index0);
  ip4_fib_mtrie_route_add(&(fib->mtrie), 0x76000001, 32, index3);
  ip4_fib_mtrie_route_add(&(fib->mtrie), 0x76000002, 32, index4);
  ip4_fib_mtrie_route_add(&(fib->mtrie), 0x0A000301, 32, index5);
  ip4_fib_mtrie_route_add(&(fib->mtrie), 0x0A000401, 32, index6);

  /* End of initializing fib-related stuff, not core functionality of the NF */

  return ret;
}


/* Data plane handlers */

bool nf_pkt_parser(uint8_t *buffer, pkt_t *pkt) {
  pkt->ether_header = nf_then_get_ether_header(buffer);

  uint8_t *ip_options;
  pkt->ipv4_header =
      nf_then_get_ipv4_header(pkt->ether_header, buffer, &ip_options);
  if (pkt->ipv4_header == NULL) {
    NF_DEBUG("Malformed IPv4, dropping");
    return false;
  }

  pkt->tcpudp_header =
      nf_then_get_tcpudp_header(pkt->ipv4_header, buffer);
  if (pkt->tcpudp_header == NULL) {
    NF_DEBUG("Not TCP/UDP, dropping");
    return false;
  }

  // payload is not used here
  pkt->payload = NULL;

  pkt->raw = buffer;

  return true;
}

int nf_pkt_dispatcher(const pkt_t *pkt, uint16_t incoming_dev, 
                   pkt_set_id_t *pkt_set_id, bool *has_pkt_set_state,
                   nf_state_t *non_pkt_set_state) {
  int pkt_class;

  // Client packet
  if (incoming_dev == non_pkt_set_state->cfg->wan_device[0] ||
      incoming_dev == non_pkt_set_state->cfg->wan_device[1]) {
    *has_pkt_set_state = true;
    pkt_set_id->src_port = pkt->tcpudp_header->src_port;
    pkt_set_id->dst_port = pkt->tcpudp_header->dst_port;
    pkt_set_id->src_ip = pkt->ipv4_header->src_addr;
    pkt_set_id->dst_ip = pkt->ipv4_header->dst_addr;
    pkt_set_id->protocol = pkt->ipv4_header->next_proto_id;

    pkt_class = 0;

    NF_DEBUG("Client pkt set id: port: [%x, %x], ip: [%x %x], proto: %x",
             pkt_set_id->src_port, pkt_set_id->dst_port,
             pkt_set_id->src_ip, pkt_set_id->dst_ip,
             pkt_set_id->protocol);
  // Heartbeat
  } else {
    *has_pkt_set_state = false;
    pkt_set_id = NULL;
    pkt_class = 1;

    NF_DEBUG("Heartbeat: ip: [%x %x], eth: [%s, %s]",
             pkt->ipv4_header->src_addr, pkt->ipv4_header->dst_addr,
             nf_mac_to_str(&(pkt->ether_header->s_addr)), 
             nf_mac_to_str(&(pkt->ether_header->d_addr)) 
             );
  }

  return pkt_class;
}

int client_pkt_handler(nf_state_t *non_pkt_set_state, pkt_t *pkt,
                        uint16_t incoming_dev, pkt_set_state_t *local_state,
                        pkt_set_id_t *pkt_set_id) {
                      
  /* Fib-related stuff, not core functionality of the NF */

  if (pkt->ipv4_header->time_to_live == 1) drop_pkt(pkt);
  NF_DEBUG("Existing pkt set, backend id: %d", local_state->backend_id);
  ip4_fib_mtrie_t *mtrie = &(ip4_fib_get(incoming_dev)->mtrie);
  ip4_fib_mtrie_leaf_t leaf0;
  // Remember here the dst_addr is in big endian
  uint32_t le_dst_addr = RTE_STATIC_BSWAP32(pkt->ipv4_header->dst_addr);
  NF_DEBUG("0x%08x\n", le_dst_addr);
  leaf0 = ip4_fib_mtrie_lookup_step_one(mtrie, le_dst_addr);
  leaf0 = ip4_fib_mtrie_lookup_step(mtrie, leaf0, le_dst_addr,2);
  leaf0 = ip4_fib_mtrie_lookup_step(mtrie, leaf0, le_dst_addr,3);
  const load_balance_t *lb0;
  lb0 = load_balance_get(leaf0 >> 1);
  const dpo_t *dpo0;
  dpo0 = load_balance_get_bucket_i(lb0, 0);
  NF_DEBUG("Action: %u\n",dpo0->action);
  if (dpo0->action == DROP) {drop_pkt(pkt); return 0;}
  // Allocate another backend if assigned backend is expired

  /* End of fib-related stuff, not core functionality of the NF */

  if (dpo0->action == LB){
    NF_DEBUG("DSCP Bit: %u\n", lb0->dscp);
    if (0 == nfos_dchain_exp_is_index_allocated(non_pkt_set_state->backends, local_state->backend_id)) {

      NF_DEBUG("Reallocate backend, old backend: %d", local_state->backend_id);

      int found = nfos_cht_find_preferred_available_backend(
          (uint64_t)get_hash_from_pkt(pkt), non_pkt_set_state->cht,
          non_pkt_set_state->backends, non_pkt_set_state->cfg->cht_height,
          non_pkt_set_state->cfg->backend_capacity, &local_state->backend_id);

      if (!found) {
        drop_pkt(pkt);
        return 0;
      }

      NF_DEBUG("Reallocate backend, new backend: %d", local_state->backend_id);

    }

    backend_info_t *backend;
    // TODO: There should be 1 visit to the load_balance_pool to get the DPO
    // which is skipped here.

    nfos_vector_borrow(non_pkt_set_state->backend_info, local_state->backend_id, (void **)&backend);
    pkt->ipv4_header->type_of_service = (lb0->dscp & 0x3F) << 2;
    send_pkt_to_backend(pkt, backend, non_pkt_set_state);
  }
  return 0;
}

int heartbeat_handler(nf_state_t *non_pkt_set_state, pkt_t *pkt,
                       uint16_t incoming_dev, pkt_set_state_t *unused1,
                       pkt_set_id_t *unused2) {
  int backend_id;
  vigor_time_t now = get_curr_time();

  // Backend not in the backend table, try to insert it
  struct ip_addr src_ip = {.addr = pkt->ipv4_header->src_addr};
  if (!nfos_map_get(non_pkt_set_state->backend_ip_to_backend_id, &src_ip,
                    &backend_id)) {
    int allocated = nfos_dchain_exp_allocate_new_index(non_pkt_set_state->backends, &backend_id, now);
    if (allocated == ABORT_HANDLER) {
      return ABORT_HANDLER;
    } else if (allocated == 1) {
      backend_info_t *new_backend;

      if(nfos_vector_borrow_mut(non_pkt_set_state->backend_info, backend_id, (void **)&new_backend)
         == ABORT_HANDLER) {
        return ABORT_HANDLER;
      }
      new_backend->ip = pkt->ipv4_header->src_addr;
      new_backend->mac = pkt->ether_header->s_addr;
      new_backend->dev = incoming_dev;

      struct ip_addr *key;
      if (nfos_vector_borrow_mut(non_pkt_set_state->backend_ips, backend_id, (void **)&key)
          == ABORT_HANDLER) {
        return ABORT_HANDLER;
      }
      memcpy((void *)key, (void *)&src_ip, sizeof(struct ip_addr));

      if (nfos_map_put(non_pkt_set_state->backend_ip_to_backend_id, key, backend_id)
          == ABORT_HANDLER) {
        return ABORT_HANDLER;
      }

      NF_DEBUG("New backend index: %d at time %ld", backend_id, now);
    }
    // Otherwise ignore this backend, we are full.

  // Backend in the backend table, refresh its timestamp
  } else {
    if (nfos_dchain_exp_rejuvenate_index(non_pkt_set_state->backends, backend_id, now)
        == ABORT_HANDLER) {
      return ABORT_HANDLER;
    }
    NF_DEBUG("Refresh backend index: %d at time %ld", backend_id, now);

  }

  return 0;
}

int nf_unknown_pkt_set_handler(nf_state_t *non_pkt_set_state, pkt_t *pkt,
                               uint16_t incoming_dev, pkt_set_id_t *pkt_set_id) {
  // Allocate backend
  int backend_id = 0;
  if (pkt->ipv4_header->time_to_live == 1) drop_pkt(pkt);
  int found = nfos_cht_find_preferred_available_backend(
      (uint64_t)get_hash_from_pkt(pkt), non_pkt_set_state->cht,
      non_pkt_set_state->backends, non_pkt_set_state->cfg->cht_height,
      non_pkt_set_state->cfg->backend_capacity, &backend_id);

  // Backend allocation succeeds
  if (found) {
    // The packet set state of a client packet set is just its backend id
    pkt_set_state_t local_state = {.backend_id = backend_id};
    // Try to register the client packet set
    bool succ = add_pkt_set(pkt_set_id, &local_state, NULL);
    NF_DEBUG("New pkt set added %s, backend id: %d",
             succ ? "successfully": "unsuccessfully",
             local_state.backend_id);

    // Send the packet to the assigned backend
    backend_info_t *backend;
    nfos_vector_borrow(non_pkt_set_state->backend_info, backend_id, (void **)&backend);
    send_pkt_to_backend(pkt, backend, non_pkt_set_state);
 
  // Backend allocation fails, drop the packet
  } else {
    drop_pkt(pkt);

  }

  return 0;
}

int nf_expired_pkt_set_handler(nf_state_t *non_pkt_set_state,
                               pkt_set_state_t *pkt_set_state) {return 0;}


/* Periodic handlers */

// Check for expired backends and remove them if any
void expire_backend(nf_state_t *non_pkt_set_state) {
  // temp hack for transaction chopping
  rlu_thread_data_t *rlu_data = get_rlu_thread_data();

  int index;
  vigor_time_t now = get_curr_time();
  int expiration_done = 0;

  NF_DEBUG("NOW: %ld", now);

// TODO: remove RLU stuff from NF code...

retry_exp:
  RLU_READER_LOCK(rlu_data);
  // Temp hack to reset curr_free_list to 0 in case of aborts
  nfos_dchain_exp_reset_curr_free_list(non_pkt_set_state->backends);
  while (!expiration_done) {
    int ret = nfos_dchain_exp_expire_one_index(non_pkt_set_state->backends, &index, &expiration_done, now);
    if (ret == ABORT_HANDLER) {
      NF_DEBUG("ABORT: expire index");
      nfos_abort_txn(rlu_data);
      goto retry_exp;

    } else if (ret == 1) {
      struct rte_ether_addr *key;
      nfos_vector_borrow(non_pkt_set_state->backend_ips, index, (void **)&key);   
      if (nfos_map_erase(non_pkt_set_state->backend_ip_to_backend_id, (void *)key) == ABORT_HANDLER) {
        NF_DEBUG("ABORT: map erase");
        nfos_abort_txn(rlu_data);
        goto retry_exp;
      }

      NF_DEBUG("backend deleted: index %d", index);
    }
  }
  if (!RLU_READER_UNLOCK(rlu_data)) {
    nfos_abort_txn(rlu_data);
    NF_DEBUG("ABORT: exp read validation\n");
    goto retry_exp;
  }

  NF_DEBUG("EXP DONE");
}
