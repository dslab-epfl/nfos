#include "nf-lb/bst.h"

void insert(struct partition* partitions, struct core* core, int16_t ind) {
    int16_t next; 
    int16_t root = core->root_partition;

    // partitions with 0 offered load will never be migrated...
    // so do not insert them in the partition tree that is solely for the
    // rebalancing algorithm
    if (partitions[ind].offered_load <= 0)
        return;

    partitions[ind].left = -1;
    partitions[ind].right = -1;

    // set the first partition with non-zero offered load as tree root
    if (root < 0) {
        core->root_partition = ind;
        return;
    }

    while (1) {
        if (partitions[ind].offered_load >= partitions[root].offered_load) {
            next = partitions[root].right;
            if (next >= 0) {
                root = next;
            } else {
                partitions[root].right = ind;
                break;
            }
        } else {
            next = partitions[root].left;
            if (next >= 0) {
                root = next;
            } else {
                partitions[root].left = ind;
                break;
            }
        }
    }
}

// Return the index of the partition the offered load of which is just below
// offered_load. If such partition does not exist, return the index of the
// partition with the smallest offered load.
int16_t search(struct partition* partitions, struct core* core, int offered_load) {
    int16_t ret = -1;
    int16_t smallest = -1;
    int16_t root = core->root_partition;

    while (root >= 0) {
        if (partitions[root].offered_load == offered_load) {
            ret = root;
            break;
        } else if (partitions[root].offered_load < offered_load) {
            ret = root;
            root = partitions[root].right;
        } else {
            smallest = root;
            root = partitions[root].left;
        }
    }

    if (ret >= 0)
        return ret;
    else
        return smallest;
}

int delete(struct partition* partitions, struct core* core, int16_t ind) {
    int16_t root = core->root_partition;

    // empty tree
    if (root < 0)
        return 1;

    // search the parent of ind
    int16_t parent = -1;
    int is_left_of_parent = 0;
    int offered_load = partitions[ind].offered_load;

    while (root >= 0) {
        if (ind == root) {
            break;
        } else if (partitions[root].offered_load > offered_load) {
            parent = root;
            is_left_of_parent = 1;
            root = partitions[root].left;
        } else {
            parent = root;
            is_left_of_parent = 0;
            root = partitions[root].right;
        }
    }

    // The node has only right child or no child
    // Replace the node with its right child
    if (partitions[ind].left < 0) {
        if (parent < 0)
            core->root_partition = partitions[ind].right;
        else
            if (is_left_of_parent)
                partitions[parent].left = partitions[ind].right;
            else
                partitions[parent].right = partitions[ind].right;           

    // The node has only left child or no child
    // Replace the node with its left child
    } else if (partitions[ind].right < 0) {
        if (parent < 0)
            core->root_partition = partitions[ind].left;
        else
            if (is_left_of_parent)
                partitions[parent].left = partitions[ind].left;
            else
                partitions[parent].right = partitions[ind].left;           

    // The node has both children
    // Replace it with its in-order successor
    } else {
        // Search for in-order successor and its parent
        int16_t succ_parent = ind; 
        int16_t successor = partitions[ind].right;
        while (partitions[successor].left >= 0) {
            succ_parent = successor;
            successor = partitions[successor].left;
        }

        // Replace node with the in-order successor: update successor and
        // succ_parent
        if (succ_parent == ind) {
            partitions[successor].left = partitions[ind].left;
        } else {
            partitions[succ_parent].left = partitions[successor].right;
            partitions[successor].left = partitions[ind].left;
            partitions[successor].right = partitions[ind].right;
        }

        // Replace node with the in-order successor: update parent/root
        if (parent < 0)
            core->root_partition = successor;
        else
            if (is_left_of_parent)
                partitions[parent].left = successor;
            else
                partitions[parent].right = successor;           
    }

    return 0;
}

