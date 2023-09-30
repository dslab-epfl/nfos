#pragma once

#include "vigor/libvig/verified/map-util.h"
#include "concurrent-double-chain.h"

struct ConcurrentMap;

int concurrent_map_allocate(map_keys_equality* keq, map_key_hash* khash,
                            int num_pkt_set_partitions,
                            int num_buckets_per_partition,
                            struct ConcurrentDoubleChain* dchain,
                            struct ConcurrentMap** map_out);

int concurrent_map_get(struct ConcurrentMap* map, void* key,
                       int pkt_set_partition, int* index_out);

void concurrent_map_put(struct ConcurrentMap* map, void* key,
                        int pkt_set_partition, int index);

void concurrent_map_erase(struct ConcurrentMap* map, void* key,
                          int pkt_set_partition, void** unused);
