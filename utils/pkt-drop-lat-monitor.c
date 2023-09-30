// temp hack
#ifdef ENABLE_STAT

#include <stdint.h>
#include <time.h>

#include <rte_common.h>
#include <rte_ethdev.h>
#include <rte_lcore.h>
#include <rte_cycles.h>

#include "pkt-drop-lat-monitor.h"

#define PKT_STATS_INTERVAL 10000 // usecs
#define PKT_STATS_FINE_GRAINED_INTERVAL 10 // usecs
#define MAX_MONITOR_DURATION (100 * 1000 * 1000) // usecs
#define MAX_NUM_PKT_STATS (MAX_MONITOR_DURATION / PKT_STATS_INTERVAL) 
#define MAX_NUM_PKT_STATS_FINE_GRAINED (MAX_MONITOR_DURATION / PKT_STATS_FINE_GRAINED_INTERVAL) 

// TODO: make sure to check once in a while that these are stable... Especially
// the offset
#define SYS_CLOCK_OFFSET -44593.780300810235 
#define TSC_HZ 2992968001.9323783

// PKT STAT DEBUG
typedef struct debug_pkt_stat {
  int num_drop;
  int num_dequeued;
  int fine_grained_stat_start_id;
  int fine_grained_stat_end_id;
} debug_pkt_stat_t;

static debug_pkt_stat_t *debug_pkt_stats;
static int debug_pkt_stats_ind;

static int *pkt_drop_data_fine_grained;
static int *pkt_dequeued_data_fine_grained;
static int64_t *ts_fine_grained;

static inline void update_dev_stats(struct rte_eth_stats *dev_stats, int nb_devices) {
  for (int port = 0; port < nb_devices; port++)
    rte_eth_stats_get(port, &dev_stats[port]);
}

int pkt_stats_monitor_thread(void* unused) {

  int64_t prev_drop_cnt = 0;
  int64_t prev_dequeued_cnt = 0;
  debug_pkt_stats = calloc(MAX_NUM_PKT_STATS, sizeof(debug_pkt_stat_t));
  debug_pkt_stats_ind = 0;

  int64_t prev_drop_cnt_fine_grained = 0;
  int64_t prev_dequeued_cnt_fine_grained = 0;
  pkt_drop_data_fine_grained = calloc(MAX_NUM_PKT_STATS_FINE_GRAINED, sizeof(int));
  pkt_dequeued_data_fine_grained = calloc(MAX_NUM_PKT_STATS_FINE_GRAINED, sizeof(int));
  ts_fine_grained = calloc(MAX_NUM_PKT_STATS_FINE_GRAINED, sizeof(uint64_t));
  int debug_pkt_stats_fine_grained_ind = 0;

  int nb_devices = rte_eth_dev_count_avail();
  struct rte_eth_stats *dev_stats = calloc(nb_devices, sizeof(struct rte_eth_stats));

  uint64_t period_start, period_end;
  uint64_t duration = rte_get_tsc_hz() * ((double) PKT_STATS_INTERVAL / 1000 / 1000);

  while (1) {
    period_start = rte_get_tsc_cycles();

    update_dev_stats(dev_stats, nb_devices);
    int64_t drop_cnt = dev_stats[0].imissed;
    // assume 0->rx_dev, 1->tx_dev
    // NOTE:
    // Use opackets here since there is some bug with ipackets (sometimes reporting negative difference)
    // This is ok as long as we confirm total #ipackets = #opackets
    int64_t dequeued_cnt = dev_stats[1].opackets;
    debug_pkt_stats[debug_pkt_stats_ind].num_drop = drop_cnt - prev_drop_cnt;
    debug_pkt_stats[debug_pkt_stats_ind].num_dequeued = dequeued_cnt - prev_dequeued_cnt;
    debug_pkt_stats_ind++;
    prev_drop_cnt = drop_cnt;
    prev_dequeued_cnt = dequeued_cnt;

    // Get fine grained stat inds for next monitoring period
    debug_pkt_stats[debug_pkt_stats_ind].fine_grained_stat_start_id = debug_pkt_stats_fine_grained_ind;
    prev_drop_cnt_fine_grained = drop_cnt;
    prev_dequeued_cnt_fine_grained = dequeued_cnt;

    // Get fine grained stats for next monitoring period if we see any drops
    period_end = rte_get_tsc_cycles();
    while (period_end < period_start + duration) {
      if (debug_pkt_stats[debug_pkt_stats_ind - 1].num_drop > 0) {
        // rte_delay_us_block(200);

        ts_fine_grained[debug_pkt_stats_fine_grained_ind] = rte_get_tsc_cycles();

        update_dev_stats(dev_stats, rte_eth_dev_count_avail());
        int64_t drop_cnt_fine_grained = dev_stats[0].imissed;
        int64_t dequeued_cnt_fine_grained = dev_stats[1].opackets;
        pkt_drop_data_fine_grained[debug_pkt_stats_fine_grained_ind] = drop_cnt_fine_grained - prev_drop_cnt_fine_grained;
        pkt_dequeued_data_fine_grained[debug_pkt_stats_fine_grained_ind++] = dequeued_cnt_fine_grained - prev_dequeued_cnt_fine_grained;
        prev_drop_cnt_fine_grained = drop_cnt_fine_grained;
        prev_dequeued_cnt_fine_grained = dequeued_cnt_fine_grained;
      }
      period_end = rte_get_tsc_cycles();
    }

    // Get fine grained stat inds for next monitoring period
    debug_pkt_stats[debug_pkt_stats_ind].fine_grained_stat_end_id = debug_pkt_stats_fine_grained_ind;
  }

  return 0;
}
 
// DEBUG Pkt drop
void pkt_stats_log() {
  printf("\n#pkts offered/drop per 10msec:\n");

  double tsc_hz = TSC_HZ;

  for (int i = 0; i < debug_pkt_stats_ind; i++) {
    debug_pkt_stat_t *stat = debug_pkt_stats + i;
    printf("%d %d %d\n", i, stat->num_drop + stat->num_dequeued, stat->num_drop);
    for (int j = stat->fine_grained_stat_start_id; j < stat->fine_grained_stat_end_id; j++) {
      double ts = SYS_CLOCK_OFFSET + (ts_fine_grained[j] / tsc_hz);
      printf("[%d %d %.9f] %d %d\n", i, j, ts, pkt_dequeued_data_fine_grained[j], pkt_drop_data_fine_grained[j]);
    }
  }

  fflush(stdout);
}

#endif
