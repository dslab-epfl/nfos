#ifndef LB_BST_H
#define LB_BST_H

#include <stdint.h>

struct partition {
    int offered_load;
    uint16_t core;
    int16_t left;
    int16_t right;
};

struct core {
    int offered_load;
    // uint16_t num_partitions;
    int16_t root_partition;
    // uint16_t partition[NUM_FLOW_PARTITIONS];
    uint16_t ind;
};

void insert(struct partition* partitions, struct core* core, int16_t ind);

// Return the index of the partition the offered load of which is just below
// offered_load. If such partition does not exist, return the index of the
// partition with the smallest offered load.
int16_t search(struct partition* partitions, struct core* core, int offered_load);

int delete(struct partition* partitions, struct core* core, int16_t ind);

#endif