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

#include "double-chain.h"
#include "map.h"
#include "vector.h"

/* pkt set auxiliary func */

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

void pkt_set_state_allocate(void *obj) {
  pkt_set_state_t *state = (pkt_set_state_t *) obj;
  state->flow_size = 0;
}

typedef struct nf_config {
  // WAN device, i.e. external
  uint16_t wan_device;
  // External IP address
  uint32_t external_addr;
  // Number of external IP address
  uint32_t num_external_addrs;
  // Expiration time of flows in microseconds
  uint32_t flow_expiration_time;
  // MAC addresses of the NAT devices
  struct rte_ether_addr *device_macs;
  // MAC addresses of the endpoint devices
  struct rte_ether_addr *endpoint_macs;

  uint16_t external_port_low;
  uint16_t external_port_high;

  int num_avail_ext_tuples;
} nf_config_t;

typedef struct session_t {
  uint32_t src_addr;
  uint16_t src_port;
  uint8_t proto;
}session;

typedef struct session_data {
  vigor_time_t last_used;
  uint32_t int_ip;
  uint16_t int_port;
} session_data_t;

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

struct nf_state {
  // Configuration
  nf_config_t *cfg;
  struct NfosMap *session_map;
  struct NfosVector *session_data;
  struct NfosDoubleChain *sess_indexes_tcp;
  struct NfosDoubleChain *sess_indexes_udp;
};

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

void sess_data_null_init(void *obj){
  memset(obj,0, sizeof(session_data_t));
}

int pkt_handler(nf_state_t *non_pkt_set_state, pkt_t *pkt,
                uint16_t incoming_dev, pkt_set_state_t *unused_1,
                pkt_set_id_t *unused_2);

int wan_handler(nf_state_t *non_pkt_set_state, pkt_t *pkt,
                uint16_t incoming_dev, pkt_set_state_t *unused_1,
                pkt_set_id_t *unused_2);

/* Init function */

nf_state_t *nf_init(vigor_time_t *validity_duration_out, char **lcores_out, bool *has_related_pkt_sets_out,
                    bool *do_expiration_out, bool *has_pkt_sets_out) {
  // Number of cores
  *lcores_out = LCORES;

  // register packet handlers
  pkt_handler_t *handlers = malloc(sizeof(pkt_handler_t) * 2);
  handlers[0] = pkt_handler;
  handlers[1] = wan_handler;
  register_pkt_handlers(handlers);

  // Other NFOS configs
  *has_related_pkt_sets_out = false;
  *do_expiration_out = true;
  *has_pkt_sets_out = true;
  *validity_duration_out = EXPIRATION_TIME;

  nf_state_t *ret = malloc(sizeof(nf_state_t));

  // Initialize NAT configuration
  ret->cfg = malloc(sizeof(nf_config_t));
  ret->cfg->wan_device = WAN_DEVICE;
  ret->cfg->external_addr = EXTERNAL_ADDR;
  ret->cfg->flow_expiration_time = EXPIRATION_TIME;
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

  // LAN
  if (incoming_dev != non_pkt_set_state->cfg->wan_device) {
    *has_pkt_set_state = true;
    pkt_set_id->src_port = pkt->tcpudp_header->src_port;
    pkt_set_id->dst_port = pkt->tcpudp_header->dst_port;
    pkt_set_id->src_ip = pkt->ipv4_header->src_addr;
    pkt_set_id->dst_ip = pkt->ipv4_header->dst_addr;
    pkt_set_id->protocol = pkt->ipv4_header->next_proto_id;

    pkt_class = 0;

    NF_DEBUG("pkt set id: port: [%x, %x], ip: [%x %x], proto: %x",
             pkt_set_id->src_port, pkt_set_id->dst_port,
             pkt_set_id->src_ip, pkt_set_id->dst_ip,
             pkt_set_id->protocol);
  // WAN
  } else {
    *has_pkt_set_state = false;
    pkt_set_id = NULL;
    pkt_class = 1;
  }

  return pkt_class;
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

  dst_dev = 1 - incoming_dev;
  send_pkt(pkt, dst_dev);
}

int pkt_handler(nf_state_t *non_pkt_set_state, pkt_t *pkt,
                uint16_t incoming_dev, pkt_set_state_t *local_state,
                pkt_set_id_t *unused_2) {

  if (pkt->ipv4_header->time_to_live <= 1) {
    drop_pkt(pkt);
    NF_DEBUG("TTL <= 1, drop packets");
  }

  // Check if above flow size
  if (local_state->flow_size > FLOW_SIZE_THRESHOLD) {
    drop_pkt(pkt);
    NF_DEBUG("Exceed flow size threshold, drop flow");
  } else {
    local_state->flow_size++;
  }

  // Get session data or create new session
  int sess_index;
  uint32_t ext_ip;
  uint16_t ext_port;
  vigor_time_t now = get_curr_time();

  session key = {
    .src_addr = pkt->ipv4_header->src_addr,
    .src_port = pkt->tcpudp_header->src_port,
    .proto = pkt->ipv4_header->next_proto_id
  };

  if (!nfos_map_get(non_pkt_set_state->session_map, (void *)&key, &sess_index)){
      int alloc_session_succ = alloc_session(&sess_index, &key, non_pkt_set_state, now);
      // TODO: pass ABORT_HANDLER here
      if (alloc_session_succ == -1) {
        return -1;
      } else if (!alloc_session_succ) {
        drop_pkt(pkt);
        return 0;
      }
  }
  sess_id_to_ext_tuple(sess_index, key.proto, non_pkt_set_state, &ext_ip, &ext_port);


  // Refresh the session of this pkt set if last refreshed 1 sec ago
  session_data_t *sess_data;
  nfos_vector_borrow(non_pkt_set_state->session_data, sess_index, (void **)&sess_data);
  if (now > sess_data->last_used + EI_NAT_SECOND) {
    // refresh timestamp
    if (nfos_vector_borrow_mut(non_pkt_set_state->session_data, sess_index, (void **)&sess_data) == -1)
      return -1;
    else
      sess_data->last_used = now;
    NF_DEBUG("Refresh sess: %d", sess_index);
  }

  nat_pkt_action(non_pkt_set_state, pkt, ext_ip, ext_port, incoming_dev);
  return 0;
}

int wan_handler(nf_state_t *non_pkt_set_state, pkt_t *pkt,
                uint16_t incoming_dev, pkt_set_state_t *unused_1,
                pkt_set_id_t *unused_2) {
  // WIP feature
  // Shouldn't be called with our current experiment setup
  assert(false);
  return 0;
}

int nf_unknown_pkt_set_handler(nf_state_t *non_pkt_set_state, pkt_t *pkt, uint16_t incoming_dev, pkt_set_id_t *pkt_set_id) {
  pkt_set_state_t local_state = {.flow_size = 0};
  // Try to register the packet set
  bool succ = add_pkt_set(pkt_set_id, &local_state, NULL);
  NF_DEBUG("New pkt set added %s", succ ? "successfully": "unsuccessfully");

  return pkt_handler(non_pkt_set_state, pkt, incoming_dev, &local_state, NULL);
}

int nf_expired_pkt_set_handler(nf_state_t *non_pkt_set_state, pkt_set_state_t *pkt_set_state) {return 0;}
