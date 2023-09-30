#include <stdlib.h>
#include <stdint.h>
#include <stdio.h>
#include <assert.h>

#include "nf-lb/bst.h"


static int get_tree_size(struct partition* partitions, int16_t root) {
    if (root >= 0)
        return 1 + get_tree_size(partitions, partitions[root].left)
                 + get_tree_size(partitions, partitions[root].right);
    else
        return 0; 
}

static int* get_tree_data(struct partition* partitions, int16_t root,
                          int* data) {
    if (root >= 0) {
        data = get_tree_data(partitions, partitions[root].left, data);
        *(data++) = partitions[root].offered_load;
        return get_tree_data(partitions, partitions[root].right, data);
    } else {
        return data;
    }
}

static void test_tree_sorted(struct partition* partitions, int16_t root) {
    int tree_size = get_tree_size(partitions, root);
    int tree_data[tree_size];
    get_tree_data(partitions, root, tree_data);
    for (int i = 0; i < tree_size - 1; i++)
        assert(tree_data[i] <= tree_data[i+1]);
}

static int is_ind_exist(struct partition* partitions, struct core* core,
                        int16_t ind) {
    int16_t root = core->root_partition;
    int offered_load = partitions[ind].offered_load;

    while (root >= 0) {
        if (ind == root) {
            return 1;
        } else if (partitions[root].offered_load > offered_load) {
            root = partitions[root].left;
        } else {
            root = partitions[root].right;
        }
    }

    return 0;
}

int main() {

    // Input
    struct partition partitions[32];
    struct core cores[4];
    for (int i = 0; i < 32; i++) {
        partitions[i].core = i & 3;
        partitions[i].offered_load = (int16_t) (((uint32_t)rand() >> 18) * 2 + 1);
    }

    for (int i = 4; i < 32; i += 8) {
        partitions[i].offered_load = partitions[i-4].offered_load;
    }

    for (int i = 0; i < 32; i++)
        printf("node %d offered_load %d\n", i, partitions[i].offered_load);

    for (int i = 0; i < 4; i++)
        cores[i].root_partition = -1;

    // Insertion
    for (int16_t i = 0; i < 32; i++)
        insert(partitions, &(cores[partitions[i].core]), i);

    for (int i = 0; i < 4; i++) {
        assert(get_tree_size(partitions, cores[i].root_partition) == 8);
        test_tree_sorted(partitions, cores[i].root_partition);
    }
    printf("[PASSED] insertion test\n");

    // Search
    for (int16_t i = 0; i < 32; i++) {
        int16_t ind = search(partitions, &(cores[partitions[i].core]),
                             partitions[i].offered_load + 1);
        assert(partitions[ind].offered_load == partitions[i].offered_load);
    }

    // Search for -1, should return the partition with the smallest offered load
    for (int i = 0; i < 4; i++) {
        int16_t search_res = search(partitions, &(cores[i]), -1);
        int16_t ind = cores[i].root_partition;
        while (partitions[ind].left >= 0)
            ind = partitions[ind].left;
        assert(ind == search_res); 
    }

    printf("[PASSED] search test\n");

    // Deletion
    int partitions_num[4] = {8, 8, 8, 8};
    for (int16_t i = 0; i < 32; i++) {
        assert(is_ind_exist(partitions, &(cores[partitions[i].core]), i));
        delete(partitions, &(cores[partitions[i].core]), i);
        assert(!is_ind_exist(partitions, &(cores[partitions[i].core]), i));

        int core = partitions[i].core;
        partitions_num[core]--;
        assert(get_tree_size(partitions, cores[core].root_partition) == 
               partitions_num[core]);

        test_tree_sorted(partitions, cores[core].root_partition);
    }

    printf("[PASSED] deletion test\n");

    return 0;
}