#ifndef __SCALABILITY_PROFILER_H__
#define __SCALABILITY_PROFILER_H__

#define DS_OP_INFO_CACHE_SIZE 4096
#define DS_OP_ARG_LENGTH 16
#define ABORT_INFO_DB_SIZE (1 << 20)
#define ABORT_INFO_SAMPLING_RATE (1ULL << 8)

#include <stdint.h>
#include <stdlib.h>

#include "rlu-wrapper.h"

typedef uint64_t __attribute__ ((aligned (64))) uint64_aligned_t;
extern uint64_aligned_t *pkt_cnts;

void profiler_init(int _num_cores);
int profiler_add_ds_op_info(void *ds_inst, uint16_t ds_id, uint16_t ds_op_id, void *args, size_t arg_length);
void profiler_show_profile();
void profiler_add_ds_inst(void *addr, const char *filename, int lineno);

static inline void profiler_inc_curr_op() {
    mvrlu_profiler_inc_curr_op(get_rlu_thread_data(), DS_OP_INFO_CACHE_SIZE);
}

// TODO: use NIC stats
static inline void profiler_pkt_cnt_init() {
    int num_workers = rte_lcore_count() - 1;
    pkt_cnts = calloc(num_workers, sizeof(uint64_aligned_t));
}

static inline void profiler_pkt_cnt_inc(int num_pkts) {
    pkt_cnts[get_rlu_thread_id()] += num_pkts;
}

static inline double profiler_get_load_imbalance() {
    double total_pkts_recved = 0;
    double max_pkts_recved = 0;
    int num_workers = rte_lcore_count() - 1;   
    for (int i = 0; i < num_workers; i++) {
        total_pkts_recved += pkt_cnts[i];
        max_pkts_recved = (max_pkts_recved < pkt_cnts[i]) ? pkt_cnts[i] : max_pkts_recved;
    }
    double avg_pkts_recved = total_pkts_recved / num_workers;
    return max_pkts_recved / avg_pkts_recved;
}

#endif
