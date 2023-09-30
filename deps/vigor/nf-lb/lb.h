#ifndef LB_H
#define LB_H

#include <rte_ring.h>

#include <stdint.h>

#define NUM_FLOW_PARTITIONS 128

struct nf_pkt_cnt {
    uint16_t pkt_cnt[2][NUM_FLOW_PARTITIONS];
} __attribute__ ((aligned(64)));

struct partitions {
    int num_partitions;
    uint16_t inds[NUM_FLOW_PARTITIONS];
} __attribute__ ((aligned(64)));

extern struct nf_pkt_cnt* cores_nf_pkt_cnt;
extern int monitoring_epoch;

extern uint16_t new_reta[NUM_FLOW_PARTITIONS];
extern volatile int migration_sync;

extern struct rte_ring** work_rings;
extern struct rte_ring** work_done_rings;

int lb_init(int num_cores);
int lb_main(void* unused);
int lb_qlen_microbench(void* unused);
int lb_nic_stats_test(void* unused);

#endif