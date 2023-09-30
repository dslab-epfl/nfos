#pragma once

#include "vigor/libvig/verified/vigor-time.h"

#include "nf.h"

#define FLOOD_PORT 65535

#ifdef PKT_PROCESS_BATCHING
#include <rte_mbuf.h>
#endif

bool _register_pkt_handlers(pkt_handler_t *handlers);

#ifdef PKT_PROCESS_BATCHING
uint16_t process_pkt(struct rte_mbuf **mbufs, uint16_t *dst_devices, uint16_t batch_size,
                  vigor_time_t now, uint16_t pkt_set_partition,
                  nf_state_t *non_pkt_set_state);
#else
uint16_t process_pkt(uint16_t device, uint8_t* buffer, uint16_t buffer_length,
                  vigor_time_t now, uint16_t pkt_set_partition,
                  nf_state_t *non_pkt_set_state);
#endif
