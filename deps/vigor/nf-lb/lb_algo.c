#include <stdint.h>
#include <string.h>
#include <stdlib.h>

#include "nf-lb/bst.h"

// maximum number of iterations in the LB algorithm
#define LB_MAX_NUM_ITER 10

static int cores_compar(const void* a, const void* b) {
    const struct core* core_a = a;
    const struct core* core_b = b;
    return core_a->offered_load - core_b->offered_load;
}

int lb_rebalancing(struct partition* partitions, struct core* cores,
                   int num_partitions, int num_cores) {
    // Check if skew degree of any core is above threshold
    int total_load = 0;
    for (int i = 0; i < num_cores; i++)
        total_load += cores[i].offered_load;
    int avg_load = total_load / num_cores;
    int allowed_extra_load = avg_load / (num_partitions / num_cores);

    int is_balanced = 1;
    for (int i = 0; i < num_cores; i++) {
        if (cores[i].offered_load > allowed_extra_load + avg_load) {
            is_balanced = 0;
            break;
        }
    }
    if (is_balanced)
        return 0;
    
    // maximum skew degree among cores is above threshold
    // Try to balance load

    // Populate the partition trees
    for (int i = 0; i < num_cores; i++)
        cores[i].root_partition = -1;
    for (int16_t i = 0; i < num_partitions; i++)
        insert(partitions, &(cores[partitions[i].core]), i);

    // Try to balance load
    int num_iter = 0;
    int epoch = 0;

    struct core cores_copy[num_cores];
    memcpy(cores_copy, cores, num_cores * sizeof(struct core));

    int max_offered_load = INT32_MAX;
    int prev_max_offered_load = INT32_MAX;
    int prev_prev_max_offered_load = INT32_MAX;

    uint16_t new_reta[num_partitions];

    while (1) {
        qsort(cores_copy, num_cores, sizeof(struct core), cores_compar);
        prev_prev_max_offered_load = prev_max_offered_load;
        prev_max_offered_load = max_offered_load;
        max_offered_load = cores_copy[num_cores-1].offered_load;

        // Stop if max skew degree among cores is not reduced after one iteration.
        // One iteration consists of at most two partition movement epochs.
        // An iteration finishes as soon as the max skew degree is reduced
        // after one or two epochs.
        if (max_offered_load >= prev_prev_max_offered_load) {
            if (epoch > 0)
                break;
            else
                epoch++;
        } else {
           if (max_offered_load < prev_max_offered_load) {
                num_iter++;
                epoch = 0;
                for (int i = 0; i < num_partitions; i++)
                    new_reta[i] = partitions[i].core;
            } else {
                epoch++;
            }
        }

        // Stop if the target skew degree is reached
        if (max_offered_load <= allowed_extra_load + avg_load)
            break;

        // Stop after enough number of iterations
        if (num_iter > LB_MAX_NUM_ITER)
            break;

        // Move "appropriate" partitions from overloaded cores to
        // under-loaded cores
        int first_ol_core = 0;
        while (cores_copy[first_ol_core].offered_load <= avg_load)
            first_ol_core++;
        
        int ol_core = num_cores - 1;
        int ul_core = 0;

        while ((ol_core >= first_ol_core) && (ul_core < first_ol_core)) {
            if (cores_copy[ol_core].offered_load <= avg_load)
                ol_core--;

            int load_missing = avg_load - cores_copy[ul_core].offered_load;

            if (load_missing > 0) {
                int16_t partition = search(partitions, &cores_copy[ol_core],
                                           load_missing);

                if (cores_copy[ol_core].offered_load == partitions[partition].offered_load) {
                    // Avoid moving all partitions of a core
                    ol_core--;
                } else {
                    delete(partitions, &cores_copy[ol_core], partition);
                    cores_copy[ol_core].offered_load -=
                        partitions[partition].offered_load;

                    partitions[partition].core = cores_copy[ul_core].ind;
                    insert(partitions, &cores_copy[ul_core], partition);
                    cores_copy[ul_core].offered_load +=
                        partitions[partition].offered_load;
                }

            } else {

                ul_core++;
            }
        }

    }

    for (int i = 0; i < num_partitions; i++)
        partitions[i].core = new_reta[i];

    return (num_iter > 1); 
}
