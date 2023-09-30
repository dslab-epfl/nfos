/* Flow-rules-based dynamic flow partitioning engine */
/* PROTOTYPE DIRTY CODE */
#ifndef FLOW_RULES_H
#define FLOW_RULES_H

#include "nf-lb/lb.h"

struct partitions_info_per_core
{
  int core_ind;
  int partitions[NUM_FLOW_PARTITIONS];
  int num_partitions;
};

extern struct partitions_info_per_core* partitions_info;

void flow_rules_udp_init(unsigned num_queues);

#endif

