#include <rte_cycles.h>

#include "nf.h"
#include "dummy_config.h"
#include "tcp-state.h"
#include "rand-gen.h"
#include "rlu-wrapper.h"

#include "vector.h"

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
  
} nf_config_t;

struct nf_state {
  // Configuration
  nf_config_t *cfg;
  struct NfosVector *data;
};

void data_null_init(void *obj){
  int *elem = (int *)obj;
  *elem = 0;
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

  if (!nfos_vector_allocate(sizeof(int), DATA_SIZE, data_null_init, &(ret->data))) ret = NULL;

  init_rand_seqs(DATA_ZIPF_FACTOR, DATA_SIZE);

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
  uint16_t dst_dev = incoming_dev - 1;

  // Artificially increase the load via busy loop
  volatile uint64_t timer_end = 0;
  volatile uint64_t timer_start = rte_get_tsc_cycles();
  while (timer_end < timer_start + 100)
    timer_end = rte_get_tsc_cycles();

  int id = get_rand_num();
  int *data_elem;

#ifdef WRITE_PER_PACKET
  if (nfos_vector_borrow_mut_t(non_pkt_set_state->data, id, (void **)&data_elem) == ABORT_HANDLER) {
    return ABORT_HANDLER;
  }
  *data_elem = 0;
#endif

  nfos_vector_borrow(non_pkt_set_state->data, id, (void **)&data_elem);
  dst_dev += *data_elem;

  send_pkt(pkt, dst_dev);

  NF_DEBUG("Send pkt, port: [%x, %x], ip: [%x %x], proto: %x",
           pkt->tcpudp_header->src_port, pkt->tcpudp_header->dst_port,
           pkt->ipv4_header->src_addr, pkt->ipv4_header->dst_addr,
           pkt->ipv4_header->next_proto_id);
  NF_DEBUG("rand: %d", id);

  // temp hack to avoid updating seq id if abort
  update_rand_num_id();
  return 0;
}

int nf_unknown_pkt_set_handler(nf_state_t *non_pkt_set_state, pkt_t *pkt,
                               uint16_t incoming_dev, pkt_set_id_t *pkt_set_id) {
#ifdef WRITE_PER_FLOW
  int id = get_rand_num();
  int *data_elem;

  if (nfos_vector_borrow_mut_t(non_pkt_set_state->data, id, (void **)&data_elem) == ABORT_HANDLER) {
    return ABORT_HANDLER;
  }
  *data_elem = 0;
#endif

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

  return 0;
}

int nf_expired_pkt_set_handler(nf_state_t *non_pkt_set_state, pkt_set_state_t *local_state) {
  NF_DEBUG("Flow expired & removed");
  return 0;
}
