#include <stdint.h>
#include <stdlib.h>
#include <stdio.h>

#include <rte_flow.h>
#include <rte_cycles.h>
#include <rte_lcore.h>

#include "nf-lb/util.h"
#include "nf-lb/rss.h"
#include "nf-log.h"

#ifndef FLOW_RULE_LEVELS
#define FLOW_RULE_LEVELS 9
#endif

// received pkt counter
// assume 4 queues
uint64_t pkts_cnt[4] = {0};

// Initial RSS flow rules
static struct rte_flow* nf_init_rss_flow(int num_queues) {
  // set indirection table size to the maximum
  // maximum should be retrieved using dev_info before dev_configure()
	int reta_size = 512;
  // printf("use reta size %d\n", reta_size);

  uint16_t reta[reta_size];
  for (int i = 0; i < reta_size; i++)
    reta[i] = i % num_queues;

  struct rte_flow* flow = generate_rss_flow(0, 1, reta, reta_size);
  return flow;
}

// Generate global rule that jumps from group 0 to 1
// This is taken from app/test-flow-perf
static struct rte_flow* nf_enable_flow_group_one() {
  struct rte_flow* flow = generate_jump_flow(0);
  return flow;
}

static void flow_rules_test_init(unsigned num_queues)
{
  /* create initial flow rules */
  uint64_t flow_rule_lvls = FLOW_RULE_LEVELS;

  uint64_t mask = (1 << flow_rule_lvls) - 1;
  for (int l = 0; l < (1 << flow_rule_lvls); l++)
    if ((l % (num_queues - 1)) != num_queues - 2)
      generate_dst_udp_flow(0, 1, l, mask, l % (num_queues - 1));
  return;
}

static struct rte_flow** flow_rules_test(unsigned num_queues, uint8_t priority)
{
  /* mv flow rules from second last queue to last queue */
  uint64_t flow_rule_lvls = FLOW_RULE_LEVELS;
  int num_flows_total = 1 << flow_rule_lvls;
  int num_queues_used = num_queues - 1;
  int num_new_flows = num_flows_total / num_queues_used;
  struct rte_flow** flow_list = calloc(num_new_flows, sizeof(struct rte_flow*));

  uint64_t mask = num_flows_total - 1;
  for (int i = 0; i < num_new_flows; i++) {
    int spec = (num_queues_used - 1) + (num_queues_used * i);
    flow_list[i] = generate_dst_udp_flow(0, priority, spec, mask, num_queues - 1);
  }
  return flow_list;
}

static struct rte_flow** flow_rules_test_reverse(unsigned num_queues, uint8_t priority)
{
  /* mv flow rules from last queue to second last queue */
  uint64_t flow_rule_lvls = FLOW_RULE_LEVELS;
  int num_flows_total = 1 << flow_rule_lvls;
  int num_queues_used = num_queues - 1;
  int num_new_flows = num_flows_total / num_queues_used;
  struct rte_flow** flow_list = calloc(num_new_flows, sizeof(struct rte_flow*));

  uint64_t mask = num_flows_total - 1;
  for (int i = 0; i < num_new_flows; i++) {
    int spec = (num_queues_used - 1) + (num_queues_used * i);
    flow_list[i] = generate_dst_udp_flow(0, priority, spec, mask, num_queues_used - 1);
  }
  return flow_list;
}

static int flow_rules_deletion_test(struct rte_flow** flows, int num_flows)
{
  for (int i = 0; i < num_flows; i++)
    delete_flow(0, flows[i]);
  return 0;
}

static struct rte_flow* rss_flow_creation_test(int num_queues, uint8_t priority)
{
  // set indirection table size to the maximum
  // maximum should be retrieved using dev_info before dev_configure()
	int reta_size = 512;
  // printf("use reta size %d\n", reta_size);

  uint16_t reta[reta_size];
  for (int i = 0; i < reta_size; i++)
    reta[i] = num_queues - 1 - (i % num_queues);

  struct rte_flow* flow = generate_rss_flow(0, priority, reta, reta_size);
  return flow;
}

static struct rte_flow* rss_flow_creation_test_reverse(int num_queues, uint8_t priority)
{
  // set indirection table size to the maximum
  // maximum should be retrieved using dev_info before dev_configure()
	int reta_size = 512;
  // printf("use reta size %d\n", reta_size);

  uint16_t reta[reta_size];
  for (int i = 0; i < reta_size; i++)
    reta[i] = i % num_queues;

  struct rte_flow* flow = generate_rss_flow(0, priority, reta, reta_size);
  return flow;
}

static int rss_flow_deletion_test(struct rte_flow* flow)
{
  delete_flow(0, flow);
  return 0;
}

// wait until first packet arrives
static void waiting_for_packets()
{
    struct nf_xstats xstats_trigger;
    char* xstats_trigger_names[1] = {"rx_packets_phy"};
    nf_init_xstats(&xstats_trigger, 0, xstats_trigger_names, 1);   
    while ((xstats_trigger.stats)[0] == 0)
        nf_update_xstats(&xstats_trigger);
}

// 1st step in load balancing
// Test packet drops while creating
// a new higher priority rule
static int bench0()
{
    // setup stats counter
    unsigned nb_devices = 1;
    struct nf_xstats* xstats = calloc(nb_devices, sizeof(struct nf_xstats));
    char* xstats_names[2] = {"rx_discards_phy", "rx_out_of_buffer"};
    for (uint16_t device = 0; device < nb_devices; device++)   
        nf_init_xstats(&(xstats[device]), device, xstats_names, 2);

#ifndef RSS
    flow_rules_test_init(rte_lcore_count() - 1);
#endif

    // setup initial RSS flow rule
#ifdef RSS
    rss_flow_creation_test_reverse(rte_lcore_count() - 1, 1);   
#else
    flow_rules_test_reverse(rte_lcore_count() - 1, 1);
#endif

    // wait for first packets
    waiting_for_packets();

    // wait another 12 seconds
    rte_delay_us_block(1000 * 1000 * 12);

    // perform flow rule ops and measure stats
    nf_update_xstats(&(xstats[0]));
#ifdef RSS
    rss_flow_creation_test(rte_lcore_count() - 1, 0);
#else
    flow_rules_test(rte_lcore_count() - 1, 0);
#endif
    nf_update_xstats(&(xstats[0]));
    nf_show_xstats(&(xstats[0]), 1);

    // no clean up of xstats
    return 0;
}

// 2nd step in load balancing
// Test packet drops while deleting
// the initial lower priority rule
static int bench1()
{
    // setup stats counter
    unsigned nb_devices = 1;
    struct nf_xstats* xstats = calloc(nb_devices, sizeof(struct nf_xstats));
    char* xstats_names[2] = {"rx_discards_phy", "rx_out_of_buffer"};
    for (uint16_t device = 0; device < nb_devices; device++)   
        nf_init_xstats(&(xstats[device]), device, xstats_names, 2);

#ifndef RSS
    flow_rules_test_init(rte_lcore_count() - 1);
    // hardcorded hack
    uint64_t flow_rule_lvls = FLOW_RULE_LEVELS;
    int num_flows_total = 1 << flow_rule_lvls;
    int num_queues_used = rte_lcore_count() - 2;
    int num_flows = num_flows_total / num_queues_used;
#endif

#ifdef RSS
    // setup initial RSS flow rules
    struct rte_flow* flow;
    flow = rss_flow_creation_test_reverse(rte_lcore_count() - 1, 1);   
    rss_flow_creation_test(rte_lcore_count() - 1, 0);
#else
    struct rte_flow** flows;
    flows = flow_rules_test_reverse(rte_lcore_count() - 1, 1);
    flow_rules_test(rte_lcore_count() - 1, 0);
#endif

    // wait for first packets
    waiting_for_packets();

    // wait another 12 seconds
    rte_delay_us_block(1000 * 1000 * 12);

    // perform flow rule ops and measure stats
    nf_update_xstats(&(xstats[0]));
#ifdef RSS
    delete_flow(0, flow);
#else
    flow_rules_deletion_test(flows, num_flows);
#endif
    nf_update_xstats(&(xstats[0]));
    nf_show_xstats(&(xstats[0]), 1);

    // no clean up of xstats
    return 0;
}

// 3rd step in load balancing
// Test packet drops while duplicate the
// new rule with lower priority 
static int bench2()
{
    // setup stats counter
    unsigned nb_devices = 1;
    struct nf_xstats* xstats = calloc(nb_devices, sizeof(struct nf_xstats));
    char* xstats_names[2] = {"rx_discards_phy", "rx_out_of_buffer"};
    for (uint16_t device = 0; device < nb_devices; device++)   
        nf_init_xstats(&(xstats[device]), device, xstats_names, 2);

#ifndef RSS
    flow_rules_test_init(rte_lcore_count() - 1);
    // hardcorded hack
    uint64_t flow_rule_lvls = FLOW_RULE_LEVELS;
    int num_flows_total = 1 << flow_rule_lvls;
    int num_queues_used = rte_lcore_count() - 2;
    int num_flows = num_flows_total / num_queues_used;
#endif

#ifdef RSS
    // setup initial RSS flow rules
    struct rte_flow* flow;
    flow = rss_flow_creation_test_reverse(rte_lcore_count() - 1, 1);   
    rss_flow_creation_test(rte_lcore_count() - 1, 0);
    delete_flow(0, flow);
#else
    struct rte_flow** flows;
    flows = flow_rules_test_reverse(rte_lcore_count() - 1, 1);
    flow_rules_test(rte_lcore_count() - 1, 0);
    flow_rules_deletion_test(flows, num_flows);
#endif

    // wait for first packets
    waiting_for_packets();

    // wait another 12 seconds
    rte_delay_us_block(1000 * 1000 * 12);

    // perform flow rule ops and measure stats
    nf_update_xstats(&(xstats[0]));
#ifdef RSS
    rss_flow_creation_test(rte_lcore_count() - 1, 1);
#else
    flow_rules_test(rte_lcore_count() - 1, 1);
#endif
    nf_update_xstats(&(xstats[0]));
    nf_show_xstats(&(xstats[0]), 1);

    // no clean up of xstats
    return 0;
}

// 4th step in load balancing
// Test packet drops while 
// deleting the new rule with higher priority 
static int bench3()
{
    // setup stats counter
    unsigned nb_devices = 1;
    struct nf_xstats* xstats = calloc(nb_devices, sizeof(struct nf_xstats));
    char* xstats_names[2] = {"rx_discards_phy", "rx_out_of_buffer"};
    for (uint16_t device = 0; device < nb_devices; device++)   
        nf_init_xstats(&(xstats[device]), device, xstats_names, 2);

#ifndef RSS
    flow_rules_test_init(rte_lcore_count() - 1);
    // hardcorded hack
    uint64_t flow_rule_lvls = FLOW_RULE_LEVELS;
    int num_flows_total = 1 << flow_rule_lvls;
    int num_queues_used = rte_lcore_count() - 2;
    int num_flows = num_flows_total / num_queues_used;
#endif

#ifdef RSS
    // setup initial RSS flow rules
    struct rte_flow *flow, *new_flow;
    flow = rss_flow_creation_test_reverse(rte_lcore_count() - 1, 1);   
    new_flow = rss_flow_creation_test(rte_lcore_count() - 1, 0);
    delete_flow(0, flow);
    rss_flow_creation_test(rte_lcore_count() - 1, 1);
#else
    struct rte_flow **flows, **new_flows;
    flows = flow_rules_test_reverse(rte_lcore_count() - 1, 1);
    new_flows = flow_rules_test(rte_lcore_count() - 1, 0);
    flow_rules_deletion_test(flows, num_flows);
    flow_rules_test(rte_lcore_count() - 1, 1);
#endif

    // wait for first packets
    waiting_for_packets();

    // wait another 12 seconds
    rte_delay_us_block(1000 * 1000 * 12);

    // perform flow rule ops and measure stats
    nf_update_xstats(&(xstats[0]));
#ifdef RSS
    delete_flow(0, new_flow);
#else
    flow_rules_deletion_test(new_flows, num_flows);
#endif
    nf_update_xstats(&(xstats[0]));
    nf_show_xstats(&(xstats[0]), 1);
   // no clean up of xstats
    return 0;
}

// Test throughput when using RSS flow rules
static int bench4()
{
    // setup stats counter
    unsigned nb_devices = 1;
    struct nf_xstats* xstats = calloc(nb_devices, sizeof(struct nf_xstats));
    for (uint16_t device = 0; device < nb_devices; device++)   
        nf_init_xstats(&(xstats[device]), device, NULL, 0);

#ifndef RSS
    flow_rules_test_init(rte_lcore_count() - 1);
#endif

#ifdef RSS
    // setup initial RSS flow rule
    rss_flow_creation_test_reverse(rte_lcore_count() - 1, 1);   
#else
    flow_rules_test_reverse(rte_lcore_count() - 1, 1);
#endif

    // wait for first packets
    waiting_for_packets();

    // print stats
    while (1) {
      rte_delay_us_block(1000 * 1000 * 1);
      nf_update_xstats(&(xstats[0]));
      nf_show_xstats_abs(&(xstats[0]), 1);
    }
    // no clean up of xstats
    return 0;
}

// Measure latency of each steps of load balancing
// Assume each flow API call is blocking
static int bench5()
{
    // setup initial RSS flow rule
    uint64_t latency_step0, latency_step1, latency_step2, latency_step3;
    uint64_t timer_start;
    uint64_t tsc_mhz = rte_get_tsc_hz() / 1000000;
    int num_iter = 20;
    int num_queues = rte_lcore_count() - 1;

#ifndef RSS
    flow_rules_test_init(num_queues);
    // hardcorded hack
    uint64_t flow_rule_lvls = FLOW_RULE_LEVELS;
    int num_flows_total = 1 << flow_rule_lvls;
    int num_queues_used = num_queues - 1;
    int num_flows = num_flows_total / num_queues_used;

#endif

#ifdef RSS
    struct rte_flow *flow, *new_flow;
    flow = rss_flow_creation_test_reverse(num_queues, 1);
#else
    struct rte_flow **flows, **new_flows;
    flows = flow_rules_test_reverse(num_queues, 1);

    // HACK, assume port 65535/4 is not used in the RX packets
    // Workaround against PMD's weirdness
    // generate a hack flow to reduce latency of subsequent flow gen
    generate_dst_udp_flow(0, 0, 0xffff, 0xffff, num_queues - 1);
    generate_dst_udp_flow(0, 0, 0xfffe, 0xffff, num_queues_used - 1);
#endif

    // wait for first packets
    waiting_for_packets();

    for (int i = 0; i < num_iter; i++) {

      timer_start = rte_get_tsc_cycles();
#ifdef RSS
      if (!(i % 2)) 
        new_flow = rss_flow_creation_test(num_queues, 0);
      else
        new_flow = rss_flow_creation_test_reverse(num_queues, 0);
#else
      if (!(i % 2)) 
        new_flows = flow_rules_test(num_queues, 0);
      else
        new_flows = flow_rules_test_reverse(num_queues, 0);
#endif
      latency_step0 = (rte_get_tsc_cycles() - timer_start) / tsc_mhz;


      timer_start = rte_get_tsc_cycles();
#ifdef RSS
      delete_flow(0, flow);
#else
      flow_rules_deletion_test(flows, num_flows);
#endif
      latency_step1 = (rte_get_tsc_cycles() - timer_start) / tsc_mhz;


      timer_start = rte_get_tsc_cycles();
#ifdef RSS
      if (!(i % 2)) 
        flow = rss_flow_creation_test(num_queues, 1);
      else
        flow = rss_flow_creation_test_reverse(num_queues, 1);
#else
      if (!(i % 2)) 
        flows = flow_rules_test(num_queues, 1);
      else
        flows = flow_rules_test_reverse(num_queues, 1);
#endif
      latency_step2 = (rte_get_tsc_cycles() - timer_start) / tsc_mhz;


      timer_start = rte_get_tsc_cycles();
#ifdef RSS
      delete_flow(0, new_flow);
#else
      flow_rules_deletion_test(new_flows, num_flows);
#endif
      latency_step3 = (rte_get_tsc_cycles() - timer_start) / tsc_mhz;


      printf("[BENCH] RSS flow ops step 0: %ld us"
             " step 1: %ld us"
             " step 2: %ld us"
             " step 3: %ld us\n",
             latency_step0, latency_step1, latency_step2, latency_step3);
      fflush(stdout);
    }

    return 0;
}

// ------ Benchs for raw RSS ------

// check packet loss
// packet loss will be checked by the traffic generator
// assume 4 queues
static int bench6() {
  // waiting for packets
  while (pkts_cnt[0] == 0) {
    rte_delay_us_block(1000 * 1000);
  } 

  // update RSS reta on port 0 after 5 sec.
  rte_delay_us_block(15 * 1000 * 1000);

  int reta_sz = 128;
  uint16_t reta[reta_sz];
  for (int i = 0; i < reta_sz; i++)
    reta[i] = i & 1;

  set_rss_reta(0, reta, reta_sz);

  // print pkts distribution after reta update
  uint64_t prev_pkts_cnt[4];
  for (int i = 0; i < 4; i++)
    prev_pkts_cnt[i] = pkts_cnt[i];

  while (1) {
    rte_delay_us_block(1000 * 1000);
    for (int i = 0; i < 4; i++) {
      printf("[BENCH] Q%d: %ld\n", i, pkts_cnt[i] - prev_pkts_cnt[i]);
      prev_pkts_cnt[i] = pkts_cnt[i];
    }
    fflush(stdout);
  } 

  return 0;
}

// check delay of updating reta
// assume 4 queues
static int bench7() {
  // Check if there are only 4 queues configured
  if (rte_lcore_count() != 5) {
    NF_INFO("We expect 4 cores processing pkts and one core running this bench!");
    return 1;
  }

  double tsc_ghz = ((double)rte_get_tsc_hz()) / 1000000000;
  // waiting for packets
  while (pkts_cnt[0] == 0) {
    rte_delay_us_block(1000 * 1000);
  } 

  // update RSS reta on port 0 after 10 sec.
  rte_delay_us_block(10 * 1000 * 1000);

  int reta_sz = get_rss_reta_size(0);
  int epoch = 0;
  while (epoch < 5) {
    rte_delay_us_block(1000 * 1000);
    uint16_t reta[reta_sz];
    for (int i = 0; i < reta_sz; i++)
      if ((epoch & 1) == 0)
        reta[i] = i & 1;
      else
        reta[i] = (i & 1) + 2;
      
    uint64_t timer_start = rte_get_tsc_cycles();
    set_rss_reta(0, reta, reta_sz);
    double latency = (rte_get_tsc_cycles() - timer_start) / tsc_ghz;

#ifdef ENABLE_LOG
    get_rss_reta(0, reta);
    for (int i = 0; i < reta_sz; i++)
      NF_DEBUG("reta[%d]: %d", i, reta[i]);
#endif

    NF_INFO("Epoch: %d; Latency: %lfns", epoch, latency);
    for (int i = 0; i < 4; i++)
      NF_INFO("q %d cnt %ld", i, pkts_cnt[i]);

    epoch++;
  } 

  return 0;
}

#ifndef BENCH_FLOW
#define BENCH_FLOW bench0
#endif

/* function of benchmark thread */
int flow_bench_main(void* unused)
{
    // Enable flow group 1
#ifdef GROUP_ONE
    nf_enable_flow_group_one();
#endif

    // get absolute core id in true_lcore
    unsigned true_lcore = rte_lcore_id();
    printf("Core %u collecting stats on NUMA socket %u",
           true_lcore, rte_socket_id());

    BENCH_FLOW();

    return 0;
}