/*
 * Vignat impl where a packet set is a flow
 * 
 * This NF sits at the gateway of a private network. For each L4 connection
 * (flow) between a LAN host (host in private network) and a WAN host (host in
 * Internet), it maintains the mapping between the real IP/port of the LAN host
 * and its external IP/port visible to the WAN host. These mappings are stored
 * in a flow table. When the NF receives a packet from the private network, it
 * looks up in the flow table the external IP/port of its LAN host and updates
 * the packet with this IP/port. If it fails to do so, it will allocate an
 * external LAN host IP/port to the flow and add to the flow table the mapping
 * between real and external LAN host IP/port of the flow. When the NF receives
 * a packet from the internet, it looks up in the flow table the real IP/port
 * of its LAN host and updates the packet with this IP/port. It will drop the
 * packet if it fails to do so. Since there are limited number of entries
 * available in the flow table, the NF needs to remove mappings that are
 * expired. A mapping is expired if it is used after certain amount of time.
 * 
 * Note: In the case of vignat, all flows have the same external LAN host IP but
 * different external LAN host ports.
 * 
 * Note: This is a prototype impl of vignat. I wrote it to validate the draft NFOS
 *       interface. It omits some impl details not related to NFOS interface and it
 *       won't compile!
 */

#include <assert.h>

#include "nf.h"
#include "nat_config.h"
#include "static_mapping.h"

#include "load-balance.h"
#include "fib_table.h"
#include "mtrie.h"

#include "double-chain.h"
#include "map.h"

#include "scalability-profiler.h"

typedef struct nf_config {
  // WAN device, i.e. external
  uint16_t wan_device;
  // External IP address
  uint32_t external_addr;
  // Number of external IP address
  uint32_t num_external_addrs;
  // Expiration time of flows in microseconds
  uint32_t expiration_time;
  // MAC addresses of the NAT devices
  struct rte_ether_addr *device_macs;
  // MAC addresses of the endpoint devices
  struct rte_ether_addr *endpoint_macs;

  uint32_t lb_pool_capacity;
  uint32_t ip4_ply_pool_capacity;
  uint32_t n_fib_table;
  uint32_t n_static_mappings;

  uint16_t external_port_low;
  uint16_t external_port_high;

  int num_avail_ext_tuples;
} nf_config_t;

typedef struct session_t {
  uint32_t src_addr;
  uint16_t src_port;
  uint8_t proto;
}session;

typedef struct sess_list_node {
  int next;
  int prev;
} sess_list_node_t;

#define SESS_LIST_HEAD -1

typedef struct session_data {
  sess_list_node_t node;
  vigor_time_t last_used;
  uint32_t int_ip;
  uint16_t int_port;
  int user_index;
} session_data_t;

typedef struct user_data {
  sess_list_node_t sess_list_head;
  // number of sessions belonging to this user
  int num_sessions;
} user_data_t;

bool session_equal(void *a, void *b){
  session* s_a = (session*)a;
  session* s_b = (session*)b;
  return s_a->src_addr == s_b->src_addr &&
  s_a->src_port == s_b->src_port &&
  s_a->proto == s_b->proto;
}

unsigned int session_hash(void *a){
  session* s_a = (session*)a;
  unsigned hash = 0;
  hash = __builtin_ia32_crc32si(hash, s_a->src_addr);
  hash = __builtin_ia32_crc32si(hash, s_a->src_port);
  hash = __builtin_ia32_crc32si(hash, s_a->proto);
  return hash;
}

bool user_equal(void *a, void *b){
  int *u_a = (int *)a;
  int *u_b = (int *)b;
  return *u_a == *u_b;
}

unsigned int user_hash(void *a){
  int *u_a = (int *)a;
  unsigned hash = 0;
  hash = __builtin_ia32_crc32si(hash, *u_a);
  return hash;
}

struct nf_state {
  // Configuration
  nf_config_t *cfg;
  struct NfosMap *session_map;
  struct NfosVector *session_data;
  struct NfosDoubleChain *sess_indexes_tcp;
  struct NfosDoubleChain *sess_indexes_udp;
  struct NfosMap *user_map;
  struct NfosVector *user_data;
  struct NfosDoubleChain *user_indexes;
};

int alloc_user (uint32_t *user_key, int *user_index, nf_state_t *non_pkt_set_state) {
  int index;
  int ret = nfos_dchain_allocate_new_index(non_pkt_set_state->user_indexes, &index);
  if (ret == 1) {
    user_data_t *user_data;
    int ret2 = nfos_vector_borrow_mut(non_pkt_set_state->user_data, index, (void **)&user_data);
    if (ret2 == 1){
      int ret3 = nfos_map_put(non_pkt_set_state->user_map, (void*)user_key, index);
      if (ret3 == 1){
        user_data->num_sessions = 0;
        user_data->sess_list_head.next = SESS_LIST_HEAD;
        user_data->sess_list_head.prev = SESS_LIST_HEAD;
        *user_index = index;
        NF_DEBUG("alloc_user: succ user index %d", *user_index);
        return 1;
      } else if (ret3 == -1) {
        // no need to free index since the tx gets aborted here.
        return -1; 
      } else {
        NF_DEBUG("alloc_user: map put fail");
        // nfos_map_put shouldn't fail as long as dchain_allocate succeeds, but
        // still do the dchain_free here for complete implementation
        int ret4 = nfos_dchain_free_index(non_pkt_set_state->user_indexes, index);
        if (ret4 == -1) return -1; else return 0;
      }
    } else {
      // no need to free index since the tx gets aborted here.
      return ret2;
    }
  }
  else if (ret == 0)
    NF_DEBUG("alloc_user: max #user reached");
  return ret;
}

// For debugging purpose
#ifdef ENABLE_LOG  
void log_user_sess_chain (int user_index,  nf_state_t *non_pkt_set_state) {
  user_data_t *user_data;
  nfos_vector_borrow(non_pkt_set_state->user_data, user_index, (void **)&user_data);
  sess_list_node_t *head = &(user_data->sess_list_head);
  NF_DEBUG("User index: %d num_sessions: %d session chain head: [%d %d]",
           user_index, user_data->num_sessions, head->prev, head->next);
  session_data_t *curr_data;
  sess_list_node_t *curr = head;
  while (curr->next != SESS_LIST_HEAD) {
    nfos_vector_borrow(non_pkt_set_state->session_data, curr->next, (void **)&curr_data);
    curr = &(curr_data->node);
    NF_DEBUG("session last_used: %ld node [%d %d]",
             curr_data->last_used, curr->prev, curr->next);
  }
}
#endif

// Return value: -1 => abort handler, 1 => alloc_ip_port succ, 0 => alloc_ip_port fail
int alloc_session (int *sess_index, session *sess_key, nf_state_t *non_pkt_set_state, vigor_time_t now){
  int index;
  int ret;
  if (sess_key->proto == 6) {
    ret = nfos_dchain_allocate_new_index(non_pkt_set_state->sess_indexes_tcp, &index);
  } else {
    ret = nfos_dchain_allocate_new_index(non_pkt_set_state->sess_indexes_udp, &index);
    index += non_pkt_set_state->cfg->num_avail_ext_tuples;
  }

  // Free an expired index and retry index allocation
  if (ret == 0) {
    // WIP feature, not needed for our experiments
    assert(false);
  }

  if (ret == 1){
    session_data_t *sess_data;
    int ret2 = nfos_vector_borrow_mut(non_pkt_set_state->session_data, index, (void **)&sess_data);
    if (ret2 == 1){
      NF_DEBUG("alloc: session src ip %x src port %x proto %d", sess_key->src_addr, sess_key->src_port, sess_key->proto);
      int ret3 = nfos_map_put(non_pkt_set_state->session_map, (void*)sess_key, index);
      if (ret3 == 1){
        sess_data->last_used = now;
        sess_data->int_ip = sess_key->src_addr;
        sess_data->int_port = sess_key->src_port;
        // sess node will be initialized when inserting the session to the list
        *sess_index = index;
        NF_DEBUG("alloc: succ sess_index %d", *sess_index);
        return 1;
      } else if (ret3 == -1) {
        // no need to free index since the tx gets aborted here.
        return -1; 
      } else {
    	  NF_DEBUG("alloc: map put fail");
        // nfos_map_put shouldn't fail as long as dchain_allocate succeeds, but
        // still do the dchain_free here for complete implementation
        int ret4;
        if (sess_key->proto == 6) {
          ret4 = nfos_dchain_free_index(non_pkt_set_state->sess_indexes_tcp, index);
        } else {
          index -= non_pkt_set_state->cfg->num_avail_ext_tuples;
          ret4 = nfos_dchain_free_index(non_pkt_set_state->sess_indexes_udp, index);
        }
        if (ret4 == -1) return -1; else return 0;
      }
    } else {
      // no need to free index since the tx gets aborted here.
      return ret2;
    }
  } else if (ret == 0) {
    NF_DEBUG("alloc: No avail ip/port");
  }
  return ret;
}

static inline void sess_id_to_ext_tuple(int sess_id, uint8_t proto,
                                        nf_state_t *non_pkt_set_state,
                                        uint32_t *ext_ip, uint16_t *ext_port) {
  if (proto != 6)
    sess_id -= non_pkt_set_state->cfg->num_avail_ext_tuples;
  int port_space_size = non_pkt_set_state->cfg->external_port_high -
                        non_pkt_set_state->cfg->external_port_low + 1;
  *ext_port = (sess_id % port_space_size) + non_pkt_set_state->cfg->external_port_low;
  *ext_ip = (sess_id / port_space_size) + non_pkt_set_state->cfg->external_addr;
  // Assumption: little-endian machine
  *ext_port = RTE_STATIC_BSWAP16(*ext_port);
  *ext_ip = RTE_STATIC_BSWAP32(*ext_ip);
}

static inline int ext_tuple_to_sess_id(uint32_t ext_ip, uint16_t ext_port,
                                       uint8_t proto, nf_state_t *non_pkt_set_state) {
  int sess_id;
  int port_space_size = non_pkt_set_state->cfg->external_port_high -
                        non_pkt_set_state->cfg->external_port_low + 1;

  // Assumption: little-endian machine
  ext_port = RTE_STATIC_BSWAP16(ext_port);
  ext_ip = RTE_STATIC_BSWAP32(ext_ip);

  sess_id = (ext_port - non_pkt_set_state->cfg->external_port_low)
          + (port_space_size * (ext_ip - non_pkt_set_state->cfg->external_addr));
  if (proto != 6)
    sess_id += non_pkt_set_state->cfg->num_avail_ext_tuples;

  return sess_id;
}

// Return value: -1 => abort handler, 1 => dealloc_ip_port succ, 0 => dealloc_ip_port fail
int dealloc_session(session *sess_key, int sess_index, nf_state_t *non_pkt_set_state){
  session_data_t *sess_data;
  // use borrow first to prevent deadlock situation
  nfos_vector_borrow(non_pkt_set_state->session_data, sess_index, (void **)&sess_data);
	NF_DEBUG("dealloc: session src ip %x src port %x proto %d", sess_key->src_addr, sess_key->src_port, sess_key->proto);

  int ret3;
  if (sess_key->proto == 6) {
    ret3 = nfos_dchain_free_index(non_pkt_set_state->sess_indexes_tcp, sess_index);
  } else {
    sess_index -= non_pkt_set_state->cfg->num_avail_ext_tuples;
    ret3 = nfos_dchain_free_index(non_pkt_set_state->sess_indexes_udp, sess_index);
  }
  if (ret3 == 1){
    int ret2 = nfos_map_erase(non_pkt_set_state->session_map, (void *)sess_key);
    if (ret2 == -1){
      return -1;
    } else if (ret2 == 0) {
      NF_DEBUG("dealloc: map_erase error");
      return 0;
    } else {
      NF_DEBUG("dealloc: successful");
    }
  } else if (ret3 == 0) {
    NF_DEBUG("dealloc: dchain_free index error");
  }
  return ret3;
}

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

void sess_data_null_init(void *obj){
  memset(obj,0, sizeof(session_data_t));
}

void user_data_null_init(void *obj){
  memset(obj,0, sizeof(user_data_t));
}

int pkt_handler(nf_state_t *non_pkt_set_state, pkt_t *pkt,
                uint16_t incoming_dev, pkt_set_state_t *unused_1,
                pkt_set_id_t *unused_2);

/* Init function */

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
  *do_expiration_out = false;
  *has_pkt_sets_out = false;

  nf_state_t *ret = malloc(sizeof(nf_state_t));

  // Initialize NAT configuration
  ret->cfg = malloc(sizeof(nf_config_t));
  ret->cfg->wan_device = WAN_DEVICE;
  ret->cfg->external_addr = EXTERNAL_ADDR;
  ret->cfg->expiration_time = EXPIRATION_TIME;
  ret->cfg->num_external_addrs = NUM_EXTERNAL_ADDRS;
  ret->cfg->external_port_low = EXTERNAL_PORT_LOW;
  ret->cfg->external_port_high = EXTERNAL_PORT_HIGH;

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

  // Set up list of available external tuples
  int num_avail_ext_tuples = (EXTERNAL_PORT_HIGH - EXTERNAL_PORT_LOW + 1) * NUM_EXTERNAL_ADDRS;
  int max_num_sessions = MAX_NUM_SESSIONS;
  ret->cfg->num_avail_ext_tuples = num_avail_ext_tuples;
  // The majority of sessions will be TCP sessions, thus use max_num_sessions
  // here instead of (2 * max_avail_ext_tuples) to save map space. 
  // double the number of entries in map to avoid collision
  if (!nfos_map_allocate(session_equal, session_hash, sizeof(session), 2 * max_num_sessions, &(ret->session_map))) ret = NULL;
  // Session index space: 2 * #Public IPs * port range size
  if (!nfos_vector_allocate(sizeof(session_data_t), 2 * num_avail_ext_tuples,sess_data_null_init, &(ret->session_data))) ret = NULL;
  if (!nfos_dchain_allocate(num_avail_ext_tuples, &(ret->sess_indexes_tcp))) ret = NULL;
  if (!nfos_dchain_allocate(num_avail_ext_tuples, &(ret->sess_indexes_udp))) ret = NULL;

  // Set up user table
  int max_num_users = MAX_NUM_USERS;
  if (!nfos_map_allocate(user_equal, user_hash, sizeof(session), 2 * max_num_users, &(ret->user_map))) ret = NULL;
  if (!nfos_vector_allocate(sizeof(user_data_t), max_num_users, user_data_null_init, &(ret->user_data))) ret = NULL;
  // NOTE: try index alloactor first, if perf sucks, try to update map entries directly and template support in map.
  if (!nfos_dchain_allocate(max_num_users, &(ret->user_indexes))) ret = NULL;

  // FIB-related stuff
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

  // Initialize static_mappings
  ret->cfg->n_static_mappings = N_STATIC_MAPPINGS;
  if (!nat_static_mapping_init(ret->cfg->n_static_mappings)){
    ret = NULL;
  }
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

  pkt->raw = buffer;

  return true;
}

int nf_pkt_dispatcher(const pkt_t *pkt, uint16_t incoming_dev, 
                   pkt_set_id_t *pkt_set_id, bool *has_pkt_set_state,
                   nf_state_t *non_pkt_set_state) {
  // This NF only has one packet class with no pkt sets
  *has_pkt_set_state = false;
  return 0;
}

static inline void nat_pkt_action(nf_state_t *non_pkt_set_state, pkt_t *pkt,
                                      uint32_t ip, uint16_t port,
                                      uint16_t incoming_dev) {
  uint16_t dst_dev;

  // Update packet
  if (incoming_dev == non_pkt_set_state->cfg->wan_device) {
    // WIP feature
    // Shouldn't be called with our current experiment setup
    assert(false); 
  } else {
    pkt->tcpudp_header->src_port = port;
    pkt->ipv4_header->src_addr = ip;
  }
  nf_set_ipv4_udptcp_checksum(pkt->ipv4_header, pkt->tcpudp_header, pkt->raw);
  
  // Send packet
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
  if (dpo0->action == DROP) {
    drop_pkt(pkt);
  } else if (dpo0->action == FORWARD){
    dst_dev = dpo0->send_device;
    pkt->ether_header->s_addr = non_pkt_set_state->cfg->device_macs[dst_dev];
    pkt->ether_header->d_addr = dpo0->dst_mac_address;
    send_pkt(pkt, dst_dev);
    NF_DEBUG("Send pkt, port: [%x, %x], ip: [%x %x], proto: %x",
           pkt->tcpudp_header->src_port, pkt->tcpudp_header->dst_port,
           pkt->ipv4_header->src_addr, pkt->ipv4_header->dst_addr,
           pkt->ipv4_header->next_proto_id);
  }
}

int pkt_handler(nf_state_t *non_pkt_set_state, pkt_t *pkt,
                uint16_t incoming_dev, pkt_set_state_t *unused_1,
                pkt_set_id_t *unused_2) {

  if (pkt->ipv4_header->time_to_live <= 1) {
    drop_pkt(pkt);
    NF_DEBUG("TTL <= 1, drop packets");
  }

  int sess_index;
  int user_index;
  uint32_t ext_ip;
  uint16_t ext_port;
  vigor_time_t now = get_curr_time();

  // Get session data or create new session
  if (incoming_dev == non_pkt_set_state->cfg->wan_device) {
    // WIP feature
    // Shouldn't be called with our current experiment setup
    assert(false); 

  } else {
    session key = {
      .src_addr = pkt->ipv4_header->src_addr,
      .src_port = pkt->tcpudp_header->src_port,
      .proto = pkt->ipv4_header->next_proto_id
    };

    int index = 0;
    if (!nfos_map_get(non_pkt_set_state->session_map, (void *)&key, &sess_index)){

      // Check whether a static mapping exist, if not alloc new session
      uint32_t mapping_fib_entry;
      // TODO: check EI-NAT does static mapping affect user session quota???
      if (!nat_static_mapping_match(key.src_addr, key.src_port, LAN_DEVICE, key.proto,
                                 &ext_ip, &ext_port, &mapping_fib_entry)) {

        int alloc_session_succ = alloc_session(&sess_index, &key, non_pkt_set_state, now);
        // TODO: pass ABORT_HANDLER here
        if (alloc_session_succ == -1) {
          return -1;
        } else if (!alloc_session_succ) {
          drop_pkt(pkt);
          return 0;
        }

        // Allocate if new user
        uint32_t user_key = pkt->ipv4_header->src_addr;
        if (!nfos_map_get(non_pkt_set_state->user_map, (void *)&user_key, &user_index)) {
          int alloc_user_succ = alloc_user(&user_key, &user_index, non_pkt_set_state); 
          // TODO: pass ABORT_HANDLER here
          if (alloc_user_succ == -1) {
            return -1;
          } else if (!alloc_user_succ) {
            drop_pkt(pkt);
            return 0;
          }
        }

        // Insert it to the session chain of the user
        sess_list_node_t *head, *head_next, *sess;
        session_data_t *head_next_data, *sess_data;
        user_data_t *user_data;
        int head_read_ret, head_next_read_ret, sess_read_ret;

        head_read_ret = nfos_vector_borrow_mut(non_pkt_set_state->user_data, user_index, (void **)&user_data);
        if (head_read_ret == -1) {
          return -1;
        } else {
          head = &(user_data->sess_list_head);
        }

        if (head->next != SESS_LIST_HEAD) {
          head_next_read_ret = nfos_vector_borrow_mut(non_pkt_set_state->session_data, head->next, (void **)&head_next_data);
          if (head_next_read_ret != -1)
            head_next = &(head_next_data->node);
          else
            return -1;
        } else {
          head_next_read_ret = head_read_ret;
          head_next = head;
        }

        sess_read_ret = nfos_vector_borrow_mut(non_pkt_set_state->session_data, sess_index, (void **)&sess_data);
        if (sess_read_ret != -1) {
          sess = &(sess_data->node);
        } else {
          return -1;
        }

        sess->next = head->next;
        sess->prev = SESS_LIST_HEAD;
        head_next->prev = sess_index;
        head->next = sess_index;

        user_data->num_sessions++;
        // TODO: handle such case
        if (user_data->num_sessions >= MAX_NUM_SESSIONS_PER_USER)
          assert(false);

        // store user index in session data
        sess_data->user_index = user_index;

      } else {
        nat_pkt_action(non_pkt_set_state, pkt, ext_ip, ext_port, incoming_dev);
        return 0;
      }

    }
    sess_id_to_ext_tuple(sess_index, key.proto, non_pkt_set_state, &ext_ip, &ext_port);
  }


  // Refresh the session of this pkt set if last refreshed 1 sec ago
  session_data_t *sess_data;
  nfos_vector_borrow(non_pkt_set_state->session_data, sess_index, (void **)&sess_data);
  if (now > sess_data->last_used + EI_NAT_SECOND) {
    user_index = sess_data->user_index;
    // refresh timestamp
    if (nfos_vector_borrow_mut(non_pkt_set_state->session_data, sess_index, (void **)&sess_data) == -1)
      return -1;
    else
      sess_data->last_used = now;
    NF_DEBUG("Refresh sess: %d", sess_index);

    // bring the session to the head of the list
    sess_list_node_t *sess = &(sess_data->node);
    if (sess->prev != SESS_LIST_HEAD) {
      user_data_t *user_data;

      // extract the sess node from the list
      sess_list_node_t *prev, *next;
      session_data_t *sess_prev, *sess_next;
      int sess_prev_ret, sess_next_ret;

      sess_prev_ret = nfos_vector_borrow_mut(non_pkt_set_state->session_data,
                        sess->prev, (void **)&sess_prev);
      if (sess->next == SESS_LIST_HEAD) {
        sess_next_ret = nfos_vector_borrow_mut(non_pkt_set_state->user_data, user_index, (void **)&user_data);
      } else {
        sess_next_ret = nfos_vector_borrow_mut(non_pkt_set_state->session_data, sess->next, (void **)&sess_next);
      }
      if ( (sess_prev_ret == -1) || (sess_next_ret == -1) )
        return -1;

      prev = &(sess_prev->node);
      if (sess->next == SESS_LIST_HEAD) {
        next = &(user_data->sess_list_head);
      } else {
        next = &(sess_next->node);
      }
      prev->next = sess->next;
      next->prev = sess->prev;

      // insert the sess node after the list head
      sess_list_node_t *head, *head_next;
      session_data_t *head_next_data;
      int head_ret, head_next_ret;

      if (nfos_vector_borrow_mut(non_pkt_set_state->user_data, user_index, (void **)&user_data) != -1)
        head = &(user_data->sess_list_head);
      else
        return -1;
      if (nfos_vector_borrow_mut(non_pkt_set_state->session_data, head->next, (void **)&head_next_data) != -1)
        head_next = &(head_next_data->node);
      else
        return -1;

      sess->next = head->next;
      sess->prev = SESS_LIST_HEAD;
      head->next = sess_index;
      head_next->prev = sess_index;
    }
  }

#ifdef ENABLE_LOG
  log_user_sess_chain(user_index, non_pkt_set_state);
#endif

  nat_pkt_action(non_pkt_set_state, pkt, ext_ip, ext_port, incoming_dev);
  return 0;
}

/* Unspecified handlers */

int nf_unknown_pkt_set_handler(nf_state_t *non_pkt_set_state, pkt_t *pkt, uint16_t incoming_dev, pkt_set_id_t *pkt_set_id) {

}

int nf_expired_pkt_set_handler(nf_state_t *non_pkt_set_state, pkt_set_state_t *pkt_set_state) {

}
