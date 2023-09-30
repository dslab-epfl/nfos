#pragma once

#include <rte_lcore.h>

#ifdef SCALABILITY_PROFILER
// temp hack to pass the flag to mvrlu.h. mvrlu.h is the public interface of mv-rlu
#define MVRLU_PROFILER
#endif
#include "mv-rlu/include/mvrlu.h"

#define ABORT_HANDLER (-1)

RTE_DECLARE_PER_LCORE(int, rlu_thread_id);
extern rlu_thread_data_t **rlu_threads_data;

static inline rlu_thread_data_t *get_rlu_thread_data() {
    return rlu_threads_data[RTE_PER_LCORE(rlu_thread_id)];
}

static inline int get_rlu_thread_id() {
    return RTE_PER_LCORE(rlu_thread_id);
}

// Temp hack: this should be put in scalability-profiler.h
void profiler_add_abort_info();

static inline void nfos_abort_txn(rlu_thread_data_t *self) {
#ifdef SCALABILITY_PROFILER
    profiler_add_abort_info();
#endif
    RLU_ABORT(self);
}
