#pragma once
#include "tcp-state.h"
/*
 * Here a packet set is a flow. A flow is the set of packets belonging to the
 * same L4 connection between a LAN and a WAN host (Here a connection is
 * defined to include packets in both directions).
 * 
 */
struct pkt_set_id {
  // L4 port of the internal host
  uint16_t internal_port;
  // L4 port of the external host
  uint16_t external_port;
  // IP of the internal host
  uint32_t internal_ip;
  // IP of the external host
  uint32_t external_ip;
  // L4 protocol
  uint8_t protocol;
};

bool pkt_set_id_eq(void* a, void* b);

unsigned pkt_set_id_hash(void* obj);

void pkt_set_state_allocate(void *obj);

struct pkt_set_state {
  /*
   * The LAN side device through which the FW receives/transmits packets to the
   * LAN host. 
   */
  uint16_t internal_device;
  union tcp_flag_t tcp_flag;
};
