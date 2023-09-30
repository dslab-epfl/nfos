#include <stdlib.h>

#include <rte_flow.h>

#include "nf-lb/util.h"
#include "nf-lb/lb.h"
#include "nf-lb/flow-rules.h"

struct partitions_info_per_core* partitions_info;

// Flow rules utils
// use flow group one + normal flow rules
// as LB engine

// Generate global rule that jumps from group 0 to 1
// This is taken from app/test-flow-perf
static struct rte_flow* nf_enable_flow_group_one() {
  struct rte_flow* flow = generate_jump_flow(0);
  return flow;
}

// Set initial flow rules that distributes flows based on dst. udp ports
// Partition_info is a prototype container to save the list of partitions
// of each core
void flow_rules_udp_init(unsigned num_queues)
{
  nf_enable_flow_group_one();

  partitions_info = calloc(num_queues, sizeof(struct partitions_info_per_core));
  for (int i = 0; i < num_queues; i++) {
    partitions_info[i].core_ind = i;
    partitions_info[i].num_partitions = 0;
    for (int k = 0; k < NUM_FLOW_PARTITIONS; k++)
      (partitions_info[i].partitions)[k] = 0;
  }

  /* create initial flow rules */
  uint64_t mask = NUM_FLOW_PARTITIONS - 1;
  for (int l = 0; l < NUM_FLOW_PARTITIONS; l++) {
    int queue_ind = l % num_queues;
    generate_dst_udp_flow(0, 1, l, mask, queue_ind);

    int num_partitions = partitions_info[queue_ind].num_partitions;
    (partitions_info[queue_ind].partitions)[num_partitions] = l;
    partitions_info[queue_ind].num_partitions++;
  }
}