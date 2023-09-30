#include "nf.h"
#include "fw_config.h"
#include "tcp-state.h"
#include "load-balance.h"
#include "fib_table.h"
#include "mtrie.h"

/* pkt set auxiliary func */

bool pkt_set_id_eq(void* a, void* b) {
  pkt_set_id_t *id1 = (pkt_set_id_t *) a;
  pkt_set_id_t *id2 = (pkt_set_id_t *) b;
  return (id1->internal_ip == id2->internal_ip)
     && (id1->external_ip == id2->external_ip)
     && (id1->internal_port == id2->internal_port)
     && (id1->external_port == id2->external_port)
     && (id1->protocol == id2->protocol);
}

unsigned pkt_set_id_hash(void* obj) {
  pkt_set_id_t *id = (pkt_set_id_t *) obj;

  unsigned hash = 0;
  hash = __builtin_ia32_crc32si(hash, id->internal_ip);
  hash = __builtin_ia32_crc32si(hash, id->external_ip);
  hash = __builtin_ia32_crc32si(hash, id->internal_port);
  hash = __builtin_ia32_crc32si(hash, id->external_port);
  hash = __builtin_ia32_crc32si(hash, id->protocol);
  return hash;
}

void pkt_set_state_allocate(void *obj) {
  pkt_set_state_t *state = (pkt_set_state_t *) obj;
  // random value to make sure the compiler prefaults the state
  state->internal_device = 0;
  state->tcp_flag.as_u16 = 0;
}


typedef struct nf_config {
  // WAN device, i.e. external
  uint16_t wan_device;
  // Flow table size
  uint32_t max_num_flows;
  // Expiration time of flows in microseconds
  uint32_t expiration_time;
  // MAC addresses of the FW devices
  struct rte_ether_addr *device_macs;
  // MAC addresses of the endpoint devices
  struct rte_ether_addr *endpoint_macs;
  
  uint32_t lb_pool_capacity;
  uint32_t ip4_ply_pool_capacity;
  uint32_t n_fib_table;
} nf_config_t;

struct nf_state {
  // Configuration
  nf_config_t *cfg;
};

/* Init function */
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

int pkt_handler(nf_state_t *non_pkt_set_state, pkt_t *pkt,
                uint16_t incoming_dev, pkt_set_state_t *local_state,
                pkt_set_id_t *pkt_set_id);

nf_state_t *nf_init(vigor_time_t *validity_duration_out, char **lcores_out, bool *has_related_pkt_sets_out,
                    bool *do_expiration_out, bool *has_pkt_sets_out) {
  // Number of cores
  *lcores_out = LCORES;

  // register packet handlers
  pkt_handler_t *handlers = malloc(sizeof(pkt_handler_t));
  handlers[0] = pkt_handler;
  register_pkt_handlers(handlers);

  // Other NFOS configs
  *has_related_pkt_sets_out = false;
  *do_expiration_out = true;
  *has_pkt_sets_out = true;
  *validity_duration_out = EXPIRATION_TIME;

  nf_state_t *ret = malloc(sizeof(nf_state_t));

  // Initialize configuration
  ret->cfg = malloc(sizeof(nf_config_t));
  ret->cfg->wan_device = WAN_DEVICE;
  ret->cfg->max_num_flows = MAX_NUM_FLOWS;
  ret->cfg->expiration_time = EXPIRATION_TIME;

  // Fill in the device mac addresses
  int num_devs = rte_eth_dev_count_avail();
  ret->cfg->device_macs = malloc(num_devs * sizeof(struct rte_ether_addr));
  if (!ret->cfg->device_macs) ret = NULL;
  ret->cfg->endpoint_macs = malloc(num_devs * sizeof(struct rte_ether_addr));
  if (!ret->cfg->endpoint_macs) ret = NULL;
  uint8_t mac_bytes[6] = ENDPOINT_MAC;
  for (int i = 0; i < num_devs; ++i) {
    rte_eth_macaddr_get(i, &(ret->cfg->device_macs[i]));
    memcpy((ret->cfg->endpoint_macs[i]).addr_bytes, mac_bytes, 6);
    (ret->cfg->endpoint_macs[i]).addr_bytes[0] += i;
  }

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
  uint32_t index0 = load_balance_create(1,&action, &send_device, &dst_ip_address, &dst_mac_address);
  action = FORWARD;
  send_device = WAN_DEVICE;
  dst_ip_address = 0xAE000302;
  dst_mac_address.addr_bytes[0] = 0x12;
  dst_mac_address.addr_bytes[1] = 0x34;
  dst_mac_address.addr_bytes[2] = 0x56;
  dst_mac_address.addr_bytes[3] = 0x78;
  dst_mac_address.addr_bytes[4] = 0x90;
  dst_mac_address.addr_bytes[5] = 0xab;
  uint32_t index1 = load_balance_create(1,&action, &send_device, &dst_ip_address, &dst_mac_address);
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

  ip4_fib_t *fib = ip4_fib_get(LAN_DEVICE);
  ip4_fib_mtrie_route_add(&(fib->mtrie), 0, 0, index1);
  ip4_fib_mtrie_route_add(&(fib->mtrie), 0, 32,index0);

  fib = ip4_fib_get(WAN_DEVICE);
  ip4_fib_mtrie_route_add(&(fib->mtrie), 0, 0, index1);
  ip4_fib_mtrie_route_add(&(fib->mtrie), 0, 32,index0);

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
  pkt->payload = (uint8_t *)pkt->tcpudp_header;

  pkt->raw = buffer;

  return true;
}

/*
 * Note: The non-pkt-set state is only used to determine if the packet is received 
 *       on WAN device or not. We can still offload the distribution of packet sets
 *       among cores to the NIC. For instance, we can configure RSS to
 *       distribute packets using the src IP/port and dst IP/port of the packet,
 *       on the WAN and LAN device, respectively.
 */
int nf_pkt_dispatcher(const pkt_t *pkt, uint16_t incoming_dev, 
                   pkt_set_id_t *pkt_set_id, bool *has_pkt_set_state,
                   nf_state_t *non_pkt_set_state) {
  
  *has_pkt_set_state = true;

  if (incoming_dev != non_pkt_set_state->cfg->wan_device) {
    pkt_set_id->internal_port = pkt->tcpudp_header->src_port;
    pkt_set_id->external_port = pkt->tcpudp_header->dst_port;
    pkt_set_id->internal_ip = pkt->ipv4_header->src_addr;
    pkt_set_id->external_ip = pkt->ipv4_header->dst_addr;
  } else {
    pkt_set_id->internal_port = pkt->tcpudp_header->dst_port;
    pkt_set_id->external_port = pkt->tcpudp_header->src_port;
    pkt_set_id->internal_ip = pkt->ipv4_header->dst_addr;
    pkt_set_id->external_ip = pkt->ipv4_header->src_addr;
  }
  pkt_set_id->protocol = pkt->ipv4_header->next_proto_id;

  NF_DEBUG("Pkt set id: port: [%x, %x], ip: [%x %x], proto: %x",
           pkt_set_id->internal_port, pkt_set_id->external_port,
           pkt_set_id->internal_ip, pkt_set_id->external_ip,
           pkt_set_id->protocol);

  // This NF only has one packet class
  return 0;
}

int pkt_handler(nf_state_t *non_pkt_set_state, pkt_t *pkt,
                uint16_t incoming_dev, pkt_set_state_t *local_state,
                pkt_set_id_t *pkt_set_id) {

  /*
   * The Packet set is allocated, meaning the flow is valid in this case.
   * Thus send the packet.
   */
  uint16_t dst_dev;

  if (pkt->ipv4_header->time_to_live <= 1){
    NF_DEBUG("ttl <= 1, dropping");
    drop_pkt(pkt);
  } 
  if (pkt->ipv4_header->next_proto_id == PROTOCOL_TCP){
    // TODO: remove this temporary hack to get FLAGS, SEQ and ACK
    uint8_t tcp_flags = pkt->payload[13];
    track_session((incoming_dev!=non_pkt_set_state->cfg->wan_device),&(local_state->tcp_flag),tcp_flags);
  }
  if (incoming_dev == non_pkt_set_state->cfg->wan_device) {
    dst_dev = local_state->internal_device;
    pkt->ether_header->s_addr = non_pkt_set_state->cfg->device_macs[dst_dev];
    pkt->ether_header->d_addr = non_pkt_set_state->cfg->endpoint_macs[dst_dev];
    send_pkt(pkt, dst_dev);
  } else {
    uint32_t le_dst_addr = RTE_STATIC_BSWAP32(pkt->ipv4_header->dst_addr);
    NF_DEBUG("0x%08x\n", le_dst_addr);
    ip4_fib_mtrie_t *mtrie = &(ip4_fib_get(incoming_dev)->mtrie);
    ip4_fib_mtrie_leaf_t leaf0;
    leaf0 = ip4_fib_mtrie_lookup_step_one(mtrie, le_dst_addr);
    leaf0 = ip4_fib_mtrie_lookup_step(mtrie, leaf0, le_dst_addr,2);
    leaf0 = ip4_fib_mtrie_lookup_step(mtrie, leaf0, le_dst_addr,3);
    const load_balance_t *lb0;
    lb0 = load_balance_get(leaf0 >> 1);
    const dpo_t *dpo0;
    dpo0 = load_balance_get_bucket_i(lb0, 0);
    NF_DEBUG("Action: %u\n",dpo0->action);
    if (dpo0->action == DROP) drop_pkt(pkt);
    if (dpo0->action == FORWARD){
      dst_dev = dpo0->send_device;
      pkt->ether_header->s_addr = non_pkt_set_state->cfg->device_macs[dst_dev];
      pkt->ether_header->d_addr = dpo0->dst_mac_address;
      send_pkt(pkt, dst_dev);
    }
  }


  NF_DEBUG("Send pkt, port: [%x, %x], ip: [%x %x], proto: %x",
           pkt->tcpudp_header->src_port, pkt->tcpudp_header->dst_port,
           pkt->ipv4_header->src_addr, pkt->ipv4_header->dst_addr,
           pkt->ipv4_header->next_proto_id);

  return 0;
}

int nf_unknown_pkt_set_handler(nf_state_t *non_pkt_set_state, pkt_t *pkt,
                               uint16_t incoming_dev, pkt_set_id_t *pkt_set_id) {
  // WAN -> LAN packet
  if (incoming_dev == non_pkt_set_state->cfg->wan_device) {
      drop_pkt(pkt);
      NF_DEBUG("Unknown external flow, dropping");

  // LAN -> WAN packet
  } else {
    // Allocate and initialize the local state of the packet set
    pkt_set_state_t local_state = {
      .internal_device = incoming_dev,
      .tcp_flag = 0
    };
    // Try to register the packet set
    if (add_pkt_set(pkt_set_id, &local_state, NULL)) {
      NF_DEBUG("New flow inserted");
    } else {
      NF_DEBUG("Flow insertion fails");
    }
    // Send the packet
    pkt_handler(non_pkt_set_state, pkt, incoming_dev, &local_state, pkt_set_id);

  }

  return 0;
}

int nf_expired_pkt_set_handler(nf_state_t *non_pkt_set_state, pkt_set_state_t *local_state) {
  NF_DEBUG("Flow expired & removed");
  return 0;
}
