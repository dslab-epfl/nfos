#pragma once

struct pkt_set_id {
  uint8_t place_holder;
};

bool pkt_set_id_eq(void* a, void* b);

unsigned pkt_set_id_hash(void* obj);

struct pkt_set_state {
  uint8_t place_holder;
};

void pkt_set_state_allocate(void *obj);
