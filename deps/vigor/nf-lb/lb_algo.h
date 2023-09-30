#ifndef LB_ALGO_H
#define LB_ALGO_H

#include "nf-lb/bst.h"

int lb_rebalancing(struct partition* partitions, struct core* cores,
                   int num_partitions, int num_cores);

#endif