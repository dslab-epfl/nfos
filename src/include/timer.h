#pragma once

#include <stdint.h>

#include <rte_lcore.h>
#include <rte_cycles.h>

#include "vigor/libvig/verified/vigor-time.h"

RTE_DECLARE_PER_LCORE(vigor_time_t, curr_time);

// Timer utils
// TODO: put these in a separate module
static inline void nfos_timer_tick() {
  RTE_PER_LCORE(curr_time) = rte_get_tsc_cycles();
}

static inline vigor_time_t nfos_get_time() {
  return RTE_PER_LCORE(curr_time);
}

static inline vigor_time_t nfos_usec_to_tsc_cycles(int64_t usecs) {
  return (rte_get_tsc_hz() / 1000000) * usecs;
}
