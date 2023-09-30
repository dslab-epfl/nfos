#include <inttypes.h>
// DPDK uses these but doesn't include them. :|
#include <linux/limits.h>
#include <sys/types.h>

#include <string.h>
#include <stdlib.h>

#ifndef FLOW_PERF_BENCH
#define LOAD_BALANCING
#endif
#define SIGTERM_HANDLING
#ifdef SIGTERM_HANDLING
#include <signal.h>
#include <unistd.h>
#endif

#include <rte_common.h>
#include <rte_eal.h>
#include <rte_errno.h>
#include <rte_ethdev.h>
#include <rte_lcore.h>
#include <rte_mbuf.h>
#include <rte_flow.h>
#include <rte_ring.h>

#include "libvig/verified/boilerplate-util.h"
#include "libvig/verified/packet-io.h"
#include "nf-log.h"
#include "nf-util.h"
#include "nf.h"

#ifdef FLOW_PERF_BENCH
#include "nf-lb/exp-utils/flow-perf-benchmarks.h"
#endif

#include "nf-lb/util.h"
#include "nf-lb/lb.h"
#include "nf-lb/barrier.h"

#include "nf-lb/rss.h"

#ifdef KLEE_VERIFICATION
#  include "libvig/models/hardware.h"
#  include "libvig/models/verified/vigor-time-control.h"
#  include <klee/klee.h>
#endif // KLEE_VERIFICATION

#define VIGOR_ALLOW_DROPS

#ifndef VIGOR_BATCH_SIZE
// A batch size of 4 does not work on E810... Don't know why
#  define VIGOR_BATCH_SIZE 32
#endif

#ifdef VIGOR_DEBUG_PERF
#  include <stdio.h>
#  include "papi.h"
#  if VIGOR_BATCH_SIZE != 1
#    error Batch and perf debugging are not supported together
#  endif
#endif

#ifdef NFOS
#  define MAIN nf_main
#else // NFOS
#  define MAIN main
#endif // NFOS

#ifdef KLEE_VERIFICATION
#  define VIGOR_LOOP_BEGIN                                                     \
    unsigned _vigor_lcore_id = rte_lcore_id();                                 \
    vigor_time_t _vigor_start_time = start_time();                             \
    int _vigor_loop_termination = klee_int("loop_termination");                \
    unsigned VIGOR_DEVICES_COUNT;                                              \
    klee_possibly_havoc(&VIGOR_DEVICES_COUNT, sizeof(VIGOR_DEVICES_COUNT),     \
                        "VIGOR_DEVICES_COUNT");                                \
    vigor_time_t VIGOR_NOW;                                                    \
    klee_possibly_havoc(&VIGOR_NOW, sizeof(VIGOR_NOW), "VIGOR_NOW");           \
    unsigned VIGOR_DEVICE;                                                     \
    klee_possibly_havoc(&VIGOR_DEVICE, sizeof(VIGOR_DEVICE), "VIGOR_DEVICE");  \
    unsigned _d;                                                               \
    klee_possibly_havoc(&_d, sizeof(_d), "_d");                                \
    while (klee_induce_invariants() & _vigor_loop_termination) {               \
      nf_loop_iteration_border(_vigor_lcore_id, _vigor_start_time);            \
      VIGOR_NOW = current_time();                                              \
      /* concretize the device to avoid leaking symbols into DPDK */           \
      VIGOR_DEVICES_COUNT = rte_eth_dev_count_avail();                               \
      VIGOR_DEVICE = klee_range(0, VIGOR_DEVICES_COUNT, "VIGOR_DEVICE");       \
      for (_d = 0; _d < VIGOR_DEVICES_COUNT; _d++)                             \
        if (VIGOR_DEVICE == _d) {                                              \
          VIGOR_DEVICE = _d;                                                   \
          break;                                                               \
        }                                                                      \
      stub_hardware_receive_packet(VIGOR_DEVICE);
#  define VIGOR_LOOP_END                                                       \
    stub_hardware_reset_receive(VIGOR_DEVICE);                                 \
    nf_loop_iteration_border(_vigor_lcore_id, VIGOR_NOW);                      \
    }
#else // KLEE_VERIFICATION
#  define VIGOR_LOOP_BEGIN                                                     \
    while (1) {                                                                \
      vigor_time_t VIGOR_NOW = current_time();                                 \
      unsigned VIGOR_DEVICES_COUNT = rte_eth_dev_count_avail();                      \
      for (uint16_t VIGOR_DEVICE = 0; VIGOR_DEVICE < VIGOR_DEVICES_COUNT;      \
           VIGOR_DEVICE++) {
#  define VIGOR_LOOP_END                                                       \
    }                                                                          \
    }
#endif // KLEE_VERIFICATION

// Queue sizes for receiving/transmitting packets
// NOT powers of 2 so that ixgbe doesn't use vector stuff
// but they have to be multiples of 8, and at least 32, otherwise the driver
// refuses
static const uint16_t RX_QUEUE_SIZE = 256;
static const uint16_t TX_QUEUE_SIZE = 256;

void flood(struct rte_mbuf *frame, uint16_t skip_device, uint16_t nb_devices, uint16_t txq) {
  rte_mbuf_refcnt_set(frame, nb_devices - 1);
  int total_sent = 0;
  for (uint16_t device = 0; device < nb_devices; device++) {
    if (device == skip_device)
      continue;
    total_sent += rte_eth_tx_burst(device, txq, &frame, 1);
  }
  if (total_sent != nb_devices - 1) {
    rte_pktmbuf_free(frame);
  }
}

// Buffer count for mempools
#ifdef LOAD_BALANCING
// TODO: optimize (reduce) the mempool size if needed,
// makes sure the pools fits in LLC
// or use shared mempool
static const unsigned MEMPOOL_BUFFER_COUNT = 1024;
#else
static const unsigned MEMPOOL_BUFFER_COUNT = 512;
#endif

// --- Initialization ---
static int nf_init_device(uint16_t device, struct rte_mempool **mbuf_pools, uint16_t num_queues) {
  // Number of RX/TX queues
  uint16_t RX_QUEUES_COUNT = num_queues;
  uint16_t TX_QUEUES_COUNT = num_queues;

  int retval;

  // device_conf passed to rte_eth_dev_configure cannot be NULL
  // rx crc strip is enabled by default in DPDK 19.05
  // see https://mails.dpdk.org/archives/dev/2018-September/110744.html
  struct rte_eth_conf device_conf = {
    .rxmode = { .max_rx_pkt_len = RTE_ETHER_MAX_LEN }
  };

  // Enable RSS to distribute pkts based on udp ports
  rss_init(&device_conf, device);

  // Configure the device
  // ******DPDK changes the reta size to RX_QUEUES_COUNT after this step*****
  retval = rte_eth_dev_configure(device, RX_QUEUES_COUNT, TX_QUEUES_COUNT,
                                 &device_conf);
  if (retval != 0) {
    return retval;
  }

  // Allocate and set up TX queues
  for (int txq = 0; txq < TX_QUEUES_COUNT; txq++) {
    retval = rte_eth_tx_queue_setup(device, txq, TX_QUEUE_SIZE,
                                    rte_eth_dev_socket_id(device), NULL);
    if (retval != 0) {
      return retval;
    }
  }

  // Allocate and set up RX queues
  for (int rxq = 0; rxq < RX_QUEUES_COUNT; rxq++) {
    retval = rte_eth_rx_queue_setup(device, rxq, RX_QUEUE_SIZE,
                                    rte_eth_dev_socket_id(device),
                                    NULL, // default config
                                    mbuf_pools[rxq]);
    if (retval != 0) {
      return retval;
    }
  }

  // Enable RX in promiscuous mode, just in case
  rte_eth_promiscuous_enable(device);
  if (rte_eth_promiscuous_get(device) != 1) {
    return retval;
  }

  // Start the device
  retval = rte_eth_dev_start(device);
  if (retval != 0) {
    return retval;
  }

  // reset rss reta to default
  set_default_rss_reta(device, num_queues);

  return 0;
}

// --- Per-core work ---

// entry function to various NF threads, a bit ugly...
static int lcore_entry(void* arg) {
  int lcore_ind = rte_lcore_index(-1);
  int (** lcore_funcs)(void*) = (int (**)(void*))arg;
  int (* lcore_func)(void*) = lcore_funcs[lcore_ind];
  lcore_func(NULL);
}

static int process_main(void* unused) {
  //
  int num_queues = rte_lcore_count() - 1;

  // get relative core id in lcore
  int lcore = rte_lcore_index(-1);
  if (lcore == -1) {
    NF_INFO("Can't find lcore!");
    exit(-1);
  }  

#ifdef ENABLE_LOG
  // Init index of the log of current processing core
  RTE_PER_LCORE(_core_ind) = lcore;
#endif

  // get absolute core id in true_lcore
  unsigned true_lcore = rte_lcore_id();

  for (uint16_t device = 0; device < 1; device++) {
    if (rte_eth_dev_socket_id(device) > 0 &&
        rte_eth_dev_socket_id(device) != (int)rte_socket_id()) {
      NF_INFO("Device %" PRIu8 " is on remote NUMA node to lcore %u.",
              device, true_lcore);
    }
  }

  if (!nf_init()) {
    rte_exit(EXIT_FAILURE, "Error initializing NF");
  }

  NF_INFO("Core %u forwarding packets on NUMA socket %u",
          true_lcore, rte_socket_id());

#ifdef VIGOR_DEBUG_PERF
  NF_INFO("Counters: cycles, instructions, L1d, L1i, L2, L3");
  int papi_events[] = {PAPI_TOT_CYC, PAPI_TOT_INS, PAPI_L1_DCM, PAPI_L1_ICM, PAPI_L2_TCM, PAPI_L3_TCM};
  #define papi_events_count sizeof(papi_events)/sizeof(papi_events[0])
  #define papi_batch_size 10000
  long long papi_values[papi_batch_size][papi_events_count];
  if (PAPI_start_counters(papi_events, papi_events_count) != PAPI_OK) {
    rte_exit(EXIT_FAILURE, "Couldn't start PAPI counters.");
  }
  uint64_t papi_counter = 0;
  uint64_t papi_batch_counter = 0;
#endif

#if VIGOR_BATCH_SIZE == 1
  VIGOR_LOOP_BEGIN

#ifdef VIGOR_DEBUG_PERF
    PAPI_read_counters(papi_values[papi_counter], papi_events_count);
#endif

    struct rte_mbuf *mbuf;
    if (rte_eth_rx_burst(VIGOR_DEVICE, lcore, &mbuf, 1) != 0) {

      // TODO: extend this to support arbitrary num_queues
      // Get partition index from rss hash
      // Assume num_partitions is power of two
      uint16_t partition = mbuf->hash.rss & 127;
      partitions_nf_pkt_cnt[partition].pkt_cnt[monitoring_epoch]++;

      uint8_t* packet = rte_pktmbuf_mtod(mbuf, uint8_t*);
      packet_state_total_length(packet, &(mbuf->pkt_len));
      uint16_t dst_device = nf_process(mbuf->port, packet, mbuf->data_len, VIGOR_NOW, partition);
      nf_return_all_chunks(packet);

      if (dst_device == VIGOR_DEVICE) {
        rte_pktmbuf_free(mbuf);
      } else if (dst_device == FLOOD_FRAME) {
        flood(mbuf, VIGOR_DEVICE, VIGOR_DEVICES_COUNT, lcore);
      } else {
        concretize_devices(&dst_device, rte_eth_dev_count_avail());
        if (rte_eth_tx_burst(dst_device, lcore, &mbuf, 1) != 1) {
#if defined(VIGOR_ALLOW_DROPS) || defined(VIGOR_DEBUG_PERF)
          rte_pktmbuf_free(mbuf);
#else
          printf("We assume the hardware will allways accept a packet for send.\n");
          exit(1);
#endif
        }
      }

#ifdef VIGOR_DEBUG_PERF
      PAPI_read_counters(papi_values[papi_counter], papi_events_count);
      papi_counter++;
      if (papi_counter == papi_batch_size) {
        for (uint64_t n = 0; n < papi_batch_size; n++) {
          for (uint64_t e = 0; e < papi_events_count; e++) {
            printf("%lld ", papi_values[n][e]);
          }
          printf("\n");
        }
        papi_counter = 0;
        papi_batch_counter++;
        if (papi_batch_counter == VIGOR_DEBUG_PERF)
        {
          exit(0);
        }
      }
#endif

    }
  VIGOR_LOOP_END
#else
  if (rte_eth_dev_count_avail() != 2) {
    printf("We assume there will be exactly 2 devices for our simple batching implementation.");
    exit(1);
  }
  NF_INFO("Running with batches, this code is unverified!");

#ifdef LOAD_BALANCING
  int partition_to_expire = 0;
#endif

  VIGOR_LOOP_BEGIN
    // Try to expire one partition per vigor loop iteration
#ifdef LOAD_BALANCING
    // sync all workers to make sure they see the new_reta
    // after this point
    if (migration_sync) {
      barrier_wait();
      barrier_wait();
    }

    uint16_t partition_owner = new_reta[partition_to_expire];
    if (partition_owner == lcore)
      nf_expire_allocated_ind(VIGOR_NOW, partition_to_expire);
    partition_to_expire++;
    if (partition_to_expire > NUM_FLOW_PARTITIONS)
      partition_to_expire = 0;
#else
    nf_expire_allocated_ind(VIGOR_NOW, lcore);
#endif

    struct rte_mbuf *mbufs[VIGOR_BATCH_SIZE];
    struct rte_mbuf *mbufs_to_send[VIGOR_BATCH_SIZE];
    int mbuf_send_index = 0;
    uint16_t received_count = rte_eth_rx_burst(VIGOR_DEVICE, lcore, mbufs, VIGOR_BATCH_SIZE);

#ifdef FLOW_PERF_BENCH
    // Intel's software counter seems broken... Do it manually here
    // Used for NIC flow engine (flow rules, RSS, ...) benchmarking
    pkts_cnt[lcore] += received_count;
#endif

    for (uint16_t n = 0; n < received_count; n++) {

      // TODO: extend this to support arbitrary num_queues
      // Get partition index from rss hash
      // Assume num_partitions is power of two
#ifdef LOAD_BALANCING

      // sync all workers to make sure they see the new_reta
      // after this point
      // do here also to minimize waiting time of lcores
      if (migration_sync) {
        barrier_wait();
        barrier_wait();
      }

      uint16_t partition = mbufs[n]->hash.rss & 127;

      // pkt belongs to a migrated flow
      // forward it to the right core
      if (new_reta[partition] != lcore) {
        struct rte_ring* dst_work_ring = work_rings[VIGOR_DEVICE * num_queues + new_reta[partition]];

        // drop pkt if no space in the queue
        // should not happen since the hardware reta will
        // be updated soon and no more pkts will be queued here afterwards...
        if (rte_ring_mp_enqueue(dst_work_ring, mbufs[n])) {
          rte_pktmbuf_free(mbufs[n]);
        }

        continue;
      }

      cores_nf_pkt_cnt[lcore].pkt_cnt[monitoring_epoch][partition]++;
#else
      uint16_t partition = lcore;
#endif

      uint8_t* packet = rte_pktmbuf_mtod(mbufs[n], uint8_t*);
      packet_state_total_length(packet, &(mbufs[n]->pkt_len));
      uint16_t dst_device = nf_process(mbufs[n]->port, packet, mbufs[n]->data_len, VIGOR_NOW, partition);
      nf_return_all_chunks(packet);

      if (dst_device == VIGOR_DEVICE) {
        rte_pktmbuf_free(mbufs[n]);
      } else { // includes flood when 2 devices, which is equivalent to just a send
        mbufs_to_send[mbuf_send_index] = mbufs[n];
        mbuf_send_index++;
      }
    }

    uint16_t sent_count = rte_eth_tx_burst(1 - VIGOR_DEVICE, lcore, mbufs_to_send, mbuf_send_index);
    for (uint16_t n = sent_count; n < mbuf_send_index; n++) {
      rte_pktmbuf_free(mbufs[n]); // should not happen, but we're in the unverified case anyway
    }

    // End of vigor loop iter in common case

#ifdef LOAD_BALANCING
    // poll packets from the work_ring
    // do the processing and return them to the src core
    struct rte_ring* work_ring = work_rings[VIGOR_DEVICE * num_queues + lcore];

    if (!rte_ring_empty(work_ring)) {
      // poll for packets
      struct rte_mbuf* mbufs[VIGOR_BATCH_SIZE];
      int work_count = rte_ring_sc_dequeue_burst(work_ring, (void**)mbufs,
                                                VIGOR_BATCH_SIZE, NULL);
      for (int i = 0; i < work_count; i++) {
        struct rte_mbuf* mbuf = mbufs[i];
        uint16_t partition = mbuf->hash.rss & 127;
        uint16_t src_device = mbuf->port;
        uint16_t dst_device;
        // See mempool creation in main for a description of this hack
        int src_core = mbuf->pool->name[0];

        cores_nf_pkt_cnt[lcore].pkt_cnt[monitoring_epoch][partition]++;

        // processing the packet
        uint8_t* packet = rte_pktmbuf_mtod(mbuf, uint8_t*);
        packet_state_total_length(packet, &(mbuf->pkt_len));
        dst_device = nf_process(src_device, packet,
                                mbuf->data_len, VIGOR_NOW, partition);
        nf_return_all_chunks(packet);

        // Drop pkt if src_dev == dst_dev
        if (dst_device == src_device) {
          rte_pktmbuf_free(mbuf);
        } else {
          // return the processed packet to the src core
          struct rte_ring* dst_work_done_ring = work_done_rings[VIGOR_DEVICE * num_queues + src_core];
          // drop pkt if no space in the queue
          // should not happen since the hardware reta will
          // be updated soon and no more pkts will be queued here afterwards...
          if (rte_ring_mp_enqueue(dst_work_done_ring, mbuf)) {
            rte_pktmbuf_free(mbuf);
          }
        }
      }
    }

    // Poll packets from the work_done_ring and tx it to the NIC
    struct rte_ring* work_done_ring = work_done_rings[VIGOR_DEVICE * num_queues + lcore];

    if (!rte_ring_empty(work_done_ring)) {
      // poll for packets
      struct rte_mbuf* mbufs[VIGOR_BATCH_SIZE];
      int work_count = rte_ring_sc_dequeue_burst(work_done_ring, (void**)mbufs,
                                                VIGOR_BATCH_SIZE, NULL);

      // tx to NIC
      uint16_t sent_count = rte_eth_tx_burst(1 - VIGOR_DEVICE, lcore, mbufs, work_count);
      for (uint16_t n = sent_count; n < work_count; n++) {
        rte_pktmbuf_free(mbufs[n]); // should not happen, but we're in the unverified case anyway
      }
    }

#endif

  VIGOR_LOOP_END
#endif

  return 0;
}

// Default placeholder idle task
static int idle_main(void* unused) {
  return 0;
}

#ifdef SIGTERM_HANDLING
// For debugging
static void signal_handler(int signum) {
  /* Print stats from the devices */
  int nb_devices = rte_eth_dev_count_avail();
  struct rte_eth_stats dev_stats[nb_devices];
  for (int port = 0; port < nb_devices; port++) {
    rte_eth_stats_get(port, &dev_stats[port]);
    printf("port%d:\n", port);
    printf("ipackets: %ld\n", dev_stats[port].ipackets);
    printf("imissed: %ld\n", dev_stats[port].imissed);
    printf("ierrors: %ld\n", dev_stats[port].ierrors);
    printf("rx_nombuf: %ld\n", dev_stats[port].rx_nombuf);
    for (int i = 0; i < 16; i++) {
        printf("q_ipackets[%d]: %ld\n", i, dev_stats[port].q_ipackets[i]);
        printf("q_errors[%d]: %ld\n", i, dev_stats[port].q_errors[i]);
    }
    fflush(stdout);
  }

#ifdef ENABLE_LOG
  // flush and clean up logs
  logging_fini(rte_lcore_count());
#endif

  // TEMP DEBUG
  // for (int i = 0; i < 128; i++)
    // NF_INFO("partition %d cnt %d\n", i, partitions_nf_pkt_cnt[i].pkt_cnt[monitoring_epoch]);
  /* exit with the expected status */
  signal(signum, SIG_DFL);
  kill(getpid(), signum);
}
#endif

// --- Main ---

int MAIN(int argc, char *argv[]) {

#ifdef SIGTERM_HANDLING
  // For debugging
  signal(SIGINT, signal_handler);
  signal(SIGTERM, signal_handler);
#endif

  // Initialize the Environment Abstraction Layer (EAL)
  int ret = rte_eal_init(argc, argv);
  if (ret < 0) {
    rte_exit(EXIT_FAILURE, "Error with EAL initialization, ret=%d\n", ret);
  }
  argc -= ret;
  argv += ret;

#ifdef ENABLE_LOG
  // init logging
  logging_init(rte_lcore_count());
  // init log index of the master core
  RTE_PER_LCORE(_core_ind) = rte_lcore_index(-1);
#endif

  // NF-specific config
  nf_config_init(argc, argv);
  nf_config_print();

  // Create a memory pool on all lcores
  unsigned nb_devices = rte_eth_dev_count_avail();
  struct rte_mempool** mbuf_pools = calloc(rte_lcore_count(), sizeof(struct rte_mempool*));
  unsigned lcore = 0;
  unsigned true_lcore;
  RTE_LCORE_FOREACH(true_lcore) {
    char pool_name[128];
    // dirty hack to get mapping from mempool to lcore
    // useful for LB
    sprintf(pool_name, "%c", lcore);
    NF_DEBUG("pool_name: %d", pool_name[0]);
    mbuf_pools[lcore] = rte_pktmbuf_pool_create(
      pool_name, 
      MEMPOOL_BUFFER_COUNT * nb_devices, 
      0, 
      0, 
      RTE_MBUF_DEFAULT_BUF_SIZE, 
      rte_lcore_to_socket_id(true_lcore) 
    );
    if (mbuf_pools[lcore] == NULL) {
      rte_exit(EXIT_FAILURE, "Cannot create mbuf pool: %s\n", rte_strerror(rte_errno));
    }
    lcore++;
  }

  // Initialize all devices
  for (uint16_t device = 0; device < nb_devices; device++) {
    // reserve one core for the stats loop
    ret = nf_init_device(device, mbuf_pools, rte_lcore_count() - 1);
    if (ret == 0) {
      NF_INFO("Initialized device %" PRIu16 ".", device);
    } else {
      rte_exit(EXIT_FAILURE, "Cannot init device %" PRIu16 ", ret=%d", device,
               ret);
    }
  }

  unsigned num_lcores = rte_lcore_count();

  // Run!
  // In multi-threaded mode, yeah!
  int (** lcore_funcs)(void*) = calloc(num_lcores, sizeof(int (*)(void*)) );
  for (lcore = 0; lcore < num_lcores - 1; lcore++)
    lcore_funcs[lcore] = process_main;

  // Use the last core to run flow rule performance benchmarks
#ifdef FLOW_PERF_BENCH
  lcore_funcs[lcore] = flow_bench_main;
#else
#ifndef LOAD_BALANCING
  lcore_funcs[lcore] = idle_main;
#else
  lcore_funcs[lcore] = lb_main;
  lb_init(num_lcores - 1);
#endif
#endif


  rte_eal_mp_remote_launch(lcore_entry, (void*)lcore_funcs, CALL_MASTER);
  rte_eal_mp_wait_lcore();

  return 0;
}
