
#include <stdint.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>

#include <rte_ethdev.h>
#include <rte_cycles.h>
#include <rte_ring.h>
#include <rte_lcore.h>
#include <rte_malloc.h>

#include "nf-lb/lb.h"
#include "nf-lb/bst.h"
#include "nf-lb/rss.h"
#include "nf-lb/lb_algo.h"
#include "nf-lb/barrier.h"

#ifdef ENABLE_LOG
#include "nf-log.h"
#include "libvig/verified/vigor-time.h"
#endif

#define NUM_DESCS 128
#define Q_CNT_LB_THRESH 64
// assume the same epoch length for
// load and queue length monitoring
// in the first sketch implementation
#define MONITORING_EPOCH 100 // us
// TODO: separate epoch length for ...
// TODO: weighted sum of pkts cnt of multiple past epochs
// #define QLEN_MONITORING_EPOCH 50 // us
// #define LOAD_MONITORING_EPOCH 50 // us

#define WORK_RING_SIZE NUM_DESCS

static int num_queues;

// per-partition counters for pkts received by nf
struct nf_pkt_cnt* cores_nf_pkt_cnt;
int monitoring_epoch;

// For synchronizing workers upon new reta
uint16_t new_reta[NUM_FLOW_PARTITIONS];
volatile int migration_sync;

// Rings for dispatching pkts of migrated flows from src core to dst core
// One for each {core, port}
struct rte_ring** work_rings;
// Rings for returning pkts of migrated flows back from dst core to src core
// One for each {core, port}
struct rte_ring** work_done_rings;

static int lb_triggered(int* rxq_len, int* prev_rxq_len, int num_rxq) {
    int i;
    for (i = 0; i < num_rxq; i++) {
        if (rxq_len[i] > Q_CNT_LB_THRESH && rxq_len[i] > prev_rxq_len[i])
            break;
    }
    return (i < num_rxq);
}

int lb_init(int num_cores) {

    num_queues = num_cores;

    // TODO: Free
    cores_nf_pkt_cnt = rte_calloc(NULL, num_cores, sizeof(struct nf_pkt_cnt), 0);

    monitoring_epoch = 0;

    // For synchronizing workers upon new reta
    // assume port 0 is lan device
    get_rss_reta(0, new_reta);
    migration_sync = 0;
    // including the monitoring core
    barrier_init(num_cores + 1);

    // DEBUG
    // for (int i = 0; i < NUM_FLOW_PARTITIONS / 4; i++) {
    //     if (i % 2 == 0)
    //         new_reta[i] = 3 - new_reta[i];
    // }

    // Init work_ring/work_done_ring
    // TODO: free
    int num_ports = rte_eth_dev_count_avail();
    work_rings = calloc(num_cores * num_ports, sizeof(struct rte_ring*));
    work_done_rings = calloc(num_cores * num_ports, sizeof(struct rte_ring*));
    for (int port = 0; port < num_ports; port++) {
        for (int core = 0; core < num_cores; core++) {
            char work_ring_name[128];
            sprintf(work_ring_name, "work_ring_%d_%d", port, core);
            work_rings[port * num_cores + core] = rte_ring_create(work_ring_name,
                    WORK_RING_SIZE, rte_socket_id(), RING_F_SC_DEQ);

            char work_done_ring_name[128];
            sprintf(work_done_ring_name, "work_done_ring_%d_%d", port, core);
            work_done_rings[port * num_cores + core] = rte_ring_create(work_done_ring_name,
                    WORK_RING_SIZE, rte_socket_id(), RING_F_SC_DEQ);
        }
    }

    return 0;
}

#ifdef ENABLE_LOG
static void lb_log(int* curr_rxq_len, int* prev_rxq_len, int* rxq_len_diff,
                   int* core_counted_pkts,
                   uint16_t* old_reta, int* partition_counted_pkts,
                   struct core* cores, struct partition* flow_partitions,
                   int is_reta_updated, int num_partitions,
                   int num_queues, int num_ports) {
    
    // core stats
    for (int i = 0; i < num_queues; i++) {
        for (int port = 0; port < num_ports; port++) {
            NF_DEBUG("core %d port %d curr_rxq_len: %d prev_rxq_len: %d",
                           i, port, curr_rxq_len[i + port * num_queues],
                           prev_rxq_len[i + port * num_queues]);
        }                
        NF_DEBUG("core %d rxq_len_diff: %d", i, rxq_len_diff[i]);
        NF_DEBUG("core %d counted_pkts %d est. offered_load %d",
                  i, core_counted_pkts[i], cores[i].offered_load);
    }

    // partition stats
    // for (int i = 0; i < num_partitions; i++) {
    //     int core = old_reta[i];
    //     NF_DEBUG("P %d counted_pkts %d est. offered_load %d src_core %d dst_core %d",
    //               i, partition_counted_pkts[i], flow_partitions[i].offered_load,
    //               core, flow_partitions[i].core);
    // }

    NF_DEBUG("is_reta_updated: %d current time %ld", is_reta_updated, current_time());
}
#endif


int lb_main(void* unused) {

#ifdef ENABLE_LOG
    // get relative core id in lcore
    int lcore = rte_lcore_index(-1);
    // Init index of the log of current processing core
    RTE_PER_LCORE(_core_ind) = lcore;
#endif

    // Assume all ports are used
    // Assume reta index on all ports == partition index, so no translation is needed
    int num_ports = rte_eth_dev_count_avail();
    int prev_monitoring_epoch = 0;
    // TODO: free
    int* curr_rxq_len = calloc(num_queues * num_ports, sizeof(int));
    int* prev_rxq_len = calloc(num_queues * num_ports, sizeof(int));
    // TODO: calculate the rxq_len_diff for the ports differently
    // if we still use #pkts/sec as metric for load
    // the current way of integrating rxq_len_diff in load is inaccurate...
    int* rxq_len_diff = calloc(num_queues, sizeof(int));
    struct partition flow_partitions[NUM_FLOW_PARTITIONS];
    struct core* cores = calloc(num_queues, sizeof(struct core));

    int is_lb_triggered;
    int is_reta_update_needed;

    // For debugging
#ifdef ENABLE_LOG
    int* prev_rxq_len_debug = calloc(num_queues * num_ports, sizeof(int));
    int* core_counted_pkts_debug = calloc(num_queues, sizeof(int));
    uint16_t old_reta_debug[NUM_FLOW_PARTITIONS];
    int partition_counted_pkts_debug[NUM_FLOW_PARTITIONS];
    uint64_t tsc_mhz = rte_get_tsc_hz() / 1000 / 1000;
#endif

    // initialize flow partitions info from the reta of port 0
    uint16_t reta[NUM_FLOW_PARTITIONS];
    get_rss_reta(0, reta);
    for (int i = 0; i < NUM_FLOW_PARTITIONS; i++) {
        flow_partitions[i].core = reta[i];
    }

    // initialize core info
    for (int i = 0; i < num_queues; i++) {
        cores[i].ind = i;
        cores[i].root_partition = -1;
    }

    // temp code for the exp:
    // start load monitoring after 10 sec. 
    rte_delay_us_block(1000 * 1000 * 10);

    // LB monitoring & balancing loop
    while (1) {
        // Get number of pkts received on each partition
        // during last epoch
        prev_monitoring_epoch = monitoring_epoch;
        monitoring_epoch = 1 - monitoring_epoch;

        // Get current rxq length
        for (int port = 0; port < num_ports; port++) {
            for (int i = 0; i < num_queues; i++) {
                prev_rxq_len[i + port * num_queues] =
                    curr_rxq_len[i + port * num_queues];
                curr_rxq_len[i + port * num_queues] =
                    rte_eth_rx_queue_count(port, i);
            }
        }

        is_lb_triggered = lb_triggered(curr_rxq_len, prev_rxq_len, num_queues * num_ports);
        if (is_lb_triggered) {
            // Get goodput on partitions & cores
            for (int i = 0; i < num_queues; i++) {
                cores[i].offered_load = 0;
                rxq_len_diff[i] = 0;
                for (int port = 0; port < num_ports; port++) {
                    rxq_len_diff[i] += curr_rxq_len[i + port * num_queues] 
                                     - prev_rxq_len[i + port * num_queues];
                }                
            }
            for (int i = 0; i < NUM_FLOW_PARTITIONS; i++) {
                int core = flow_partitions[i].core;
                int nf_pkt_cnt =
                    cores_nf_pkt_cnt[core].pkt_cnt[prev_monitoring_epoch][i];
                flow_partitions[i].offered_load = nf_pkt_cnt;
                cores[core].offered_load += nf_pkt_cnt;
            }

#ifdef ENABLE_LOG
            for (int i = 0; i < num_queues; i++) {
                core_counted_pkts_debug[i] = cores[i].offered_load;
            }
            for (int i = 0; i < NUM_FLOW_PARTITIONS; i++) {
                partition_counted_pkts_debug[i] = flow_partitions[i].offered_load;
            }
#endif

            // Get offered load on partitions & cores
            for (int i = 0; i < NUM_FLOW_PARTITIONS; i++) {
                uint16_t core = flow_partitions[i].core;
                int goodput = flow_partitions[i].offered_load;
                if (cores[core].offered_load) {
                    flow_partitions[i].offered_load +=
                                                (goodput * rxq_len_diff[core]) /
                                                 cores[core].offered_load;
                }
                if (flow_partitions[i].offered_load < 0)
                    flow_partitions[i].offered_load = 0;
            }
            for (int i = 0; i < num_queues; i++) {
                cores[i].offered_load = 0;
            }
            for (int i = 0; i < NUM_FLOW_PARTITIONS; i++) {
                cores[flow_partitions[i].core].offered_load +=
                    flow_partitions[i].offered_load;
            }

            // rebalancing
            is_reta_update_needed = lb_rebalancing(flow_partitions, cores,
                                                       NUM_FLOW_PARTITIONS,
                                                       num_queues);
#ifdef ENABLE_LOG
            memcpy(old_reta_debug, new_reta, NUM_FLOW_PARTITIONS * sizeof(uint16_t));
#endif

            if (is_reta_update_needed) {
                // set migration flag
                migration_sync = 1;
                barrier_wait();

                // updating reta
                for (int i = 0; i < NUM_FLOW_PARTITIONS; i++)
                    new_reta[i] = flow_partitions[i].core;
                // reset migration flag
                migration_sync = 0;

                barrier_wait();
                // all worker cores will process flows
                // based on the new reta at this point
                
                // hard-coded reta size for now
                for (int port = 0; port < num_ports; port++) {
                    set_rss_reta(port, new_reta, NUM_FLOW_PARTITIONS);
                }
            }

#ifdef ENABLE_LOG
            memcpy(prev_rxq_len_debug, prev_rxq_len, num_ports * num_queues * sizeof(int));
#endif

            // reset queue length monitor and per-partition packet counter
            for (int core = 0; core < num_queues; core++) {
                for (int i = 0; i < NUM_FLOW_PARTITIONS; i++) {
                    cores_nf_pkt_cnt[core].pkt_cnt[prev_monitoring_epoch][i] = 0;
                }
            }

            prev_monitoring_epoch = monitoring_epoch;
            monitoring_epoch = 1 - monitoring_epoch;
            for (int port = 0; port < num_ports; port++) {
                for (int i = 0; i < num_queues; i++) {
                    prev_rxq_len[i + port * num_queues] =
                        curr_rxq_len[i + port * num_queues];
                    curr_rxq_len[i + port * num_queues] =
                        rte_eth_rx_queue_count(port, i);
                }
            }

        }

        // reset per-partition packet counter for next epoch
        for (int core = 0; core < num_queues; core++) {
            for (int i = 0; i < NUM_FLOW_PARTITIONS; i++) {
                cores_nf_pkt_cnt[core].pkt_cnt[prev_monitoring_epoch][i] = 0;
            }
        }

#ifdef ENABLE_LOG
        if (is_lb_triggered) {
            uint64_t timer_start = rte_get_tsc_cycles();
            lb_log(prev_rxq_len, prev_rxq_len_debug, rxq_len_diff,
                   core_counted_pkts_debug,
                   old_reta_debug, partition_counted_pkts_debug,
                   cores, flow_partitions,
                   is_reta_update_needed, NUM_FLOW_PARTITIONS,
                   num_queues, num_ports);
            uint64_t timer_end = rte_get_tsc_cycles();
            uint64_t time_diff_us = (timer_end - timer_start) / tsc_mhz;
            NF_DEBUG("time_logging: %ld us", time_diff_us);
            if (time_diff_us < MONITORING_EPOCH)
                rte_delay_us_block(MONITORING_EPOCH - time_diff_us);
        } else {
            rte_delay_us_block(MONITORING_EPOCH);
        }
#else
        // wait for next epoch
        rte_delay_us_block(MONITORING_EPOCH);
#endif
    }

    return 0;
}


// --- OLD ---
int lb_qlen_microbench(void* unused) {
    // Hard-coded port for now
    uint16_t port = 0;
    uint64_t timer_start;
    uint64_t timer_end;
    uint16_t rxq_cnts[1000];

    // ad-hoc code  
    rte_delay_us_block(1000 * 1000 * 10);

    while (rte_eth_rx_queue_count(port, 0) < 16)
        ;

    for (int i = 0; i < 1000; i++) {
        rxq_cnts[i] = rte_eth_rx_queue_count(port, 0);
        rte_delay_us_block(10);
    }
    
    for (int i = 0; i < 1000; i++)
        printf("Q0: %d\n", rxq_cnts[i]);

    fflush(stdout);
}

int lb_nic_stats_test(void* unused) {
    // Hard-coded port for now
    uint16_t port = 0;
    struct rte_eth_stats dev_stats;
    uint64_t tsc_mhz = rte_get_tsc_hz() / 1000 / 1000;
    uint64_t timer_start, timer_end;
    int epoch = 0;

    // ad-hoc code  
    rte_delay_us_block(1000 * 1000 * 10);
    while (rte_eth_rx_queue_count(port, 0) < 16)
        ;

    while (1) {
        rte_delay_us_block(1000 * 1000);
        printf("------%d------\n", epoch++);

        timer_start = rte_get_tsc_cycles();
        rte_eth_stats_get(port, &dev_stats);
        timer_end = rte_get_tsc_cycles();

        printf("time elapsed: %ld us\n", (timer_end - timer_start) / tsc_mhz);

        printf("ipackets: %ld\n", dev_stats.ipackets);
        printf("imissed: %ld\n", dev_stats.imissed);
        printf("ierrors: %ld\n", dev_stats.ierrors);
        printf("rx_nombuf: %ld\n", dev_stats.rx_nombuf);

        for (int i = 0; i < 16; i++) {
            printf("q_ipackets[%d]: %ld\n", i, dev_stats.q_ipackets[i]);
            printf("q_errors[%d]: %ld\n", i, dev_stats.q_errors[i]);
        }

        fflush(stdout);
    }
}