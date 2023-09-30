#pragma once

struct pkt_set_id {
  // Src port of the packet
  uint16_t src_port;
  // Dst port of the packet
  uint16_t dst_port;
  // Src IP of the packet
  uint32_t src_ip;
  // Dst IP of the packet
  uint32_t dst_ip;
  // L4 port of the packet
  uint8_t protocol;
};

bool pkt_set_id_eq(void* a, void* b);

unsigned pkt_set_id_hash(void* obj);

struct pkt_set_state {
  uint32_t flow_size;
};

void pkt_set_state_allocate(void *obj);
