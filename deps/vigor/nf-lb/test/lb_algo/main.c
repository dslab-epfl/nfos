#include <stdio.h>
#include <stdlib.h>
#include <assert.h>

#include "nf-lb/lb_algo.h"
#include "nf-lb/bst.h"

static void test(struct partition* partitions, struct core* cores) {
    // Before balancing
    printf("Before load balancing\n");
    for (int i = 0; i < 4; i++) {
        printf("core %d offered_load %d\n", i, cores[i].offered_load);
    }

    int is_new_partition_assignment = lb_rebalancing(partitions, cores, 128, 4);
    // assert(is_new_partition_assignment == 1);

    for (int i = 0; i < 4; i++)
        cores[i].offered_load = 0;
    for (int i = 0; i < 128; i++)
        cores[partitions[i].core].offered_load += partitions[i].offered_load;

    printf("After load balancing\n");
    for (int i = 0; i < 4; i++) {
        printf("core %d offered_load %d\n", i, cores[i].offered_load);
    }
}

int main() {
    // Input
    struct partition partitions[128];
    struct core cores[4];

    // test case one
    printf("test case one\n");
    for (int i = 0; i < 4; i++) {
        cores[i].ind = i;
        cores[i].offered_load = 0;
    }
    for (int i = 0; i < 128; i++) {
        partitions[i].core = i & 3;
        if (partitions[i].core == 0)
            partitions[i].offered_load = 10;
        else
            partitions[i].offered_load = 2;       
        cores[partitions[i].core].offered_load += partitions[i].offered_load;
    }
    test(partitions, cores);

    // test case two
    printf("test case two\n");
    for (int i = 0; i < 4; i++) {
        cores[i].ind = i;
        cores[i].offered_load = 0;
    }
    for (int i = 0; i < 128; i++) {
        partitions[i].core = i & 3;
        partitions[i].offered_load = 0;
    }
    int data[10] = {90, 90, 80, 80, 90, 90, 150, 150, 90, 90};
    for (int i = 0; i < 10; i++) {
        partitions[i].offered_load = data[i];
        cores[partitions[i].core].offered_load += partitions[i].offered_load;
    }
    test(partitions, cores);

    // test case three
    printf("test case three\n");
    for (int i = 0; i < 4; i++) {
        cores[i].ind = i;
        cores[i].offered_load = 0;
    }
    for (int i = 0; i < 128; i++) {
        partitions[i].core = i & 3;
        if (i & 3) {
            partitions[i].offered_load = rand() % 10;
            cores[partitions[i].core].offered_load += partitions[i].offered_load;
        } else {
            partitions[i].offered_load = 0;
        }
    }
    partitions[0].offered_load = 50;
    partitions[4].offered_load = 100;
    partitions[8].offered_load = 100;
    cores[0].offered_load = 250;
    test(partitions, cores);

    return 0;
}