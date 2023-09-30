#include "concurrent-map.h"

#include <stdio.h>
#include <stdlib.h>
#include <stddef.h>

#include <rte_malloc.h>

struct ConcurrentMap {
  int* buckets;
  int num_pkt_set_partitions;
  int num_buckets_per_partition_log_two;
  int num_buckets_per_partition;
  map_keys_equality* keys_eq;
  map_key_hash* khash;
  struct ConcurrentDoubleChain* dchain;
};

static inline unsigned rehash(unsigned int x) {
  // x = ((x >> 16) ^ x) * 0x45d9f3b;
  // x = ((x >> 16) ^ x) * 0x45d9f3b;
  // x = (x >> 16) ^ x;
  return x;
}

int concurrent_map_allocate(map_keys_equality* keq, map_key_hash* khash,
                            int num_pkt_set_partitions, int num_buckets_per_partition,
                            struct ConcurrentDoubleChain* dchain, struct ConcurrentMap** _map) {
  struct ConcurrentMap* map = (struct ConcurrentMap*)malloc(sizeof(struct ConcurrentMap));
  if (map == NULL) {
    return 0;
  }

  int num_buckets = num_buckets_per_partition * num_pkt_set_partitions;
  // TODO: this translates to some specific x86 inst, cause problem to verification?
  map->num_buckets_per_partition_log_two = __builtin_ctz(num_buckets_per_partition);
  map->num_buckets_per_partition = num_buckets_per_partition;
  map->buckets = (int*)rte_malloc(NULL, sizeof(int) * num_buckets, 0);
  if (!map->buckets) {
    free((void*)map);
    return 0;
  }

  map->keys_eq = keq;
  map->khash = khash;
  map->dchain = dchain;
  // -1 means the bucket is empty
  for (unsigned i = 0; i < num_buckets; i++) {
    map->buckets[i] = -1;
  }
  *_map = map;
  return 1;
}

int concurrent_map_get(struct ConcurrentMap* map, void* key, int pkt_set_partition, int* index_out) {
  unsigned hash = rehash(map->khash(key));
  unsigned bucket_id = (hash & (map->num_buckets_per_partition - 1)) +
                       (pkt_set_partition << map->num_buckets_per_partition_log_two);

  struct concurrent_dchain_cell *map_entry;
  pkt_set_id_t *id;
  int index;

  index = map->buckets[bucket_id];
  while (index != -1) {
    map_entry = concurrent_dchain_cell_out(map->dchain, index);
    id = &(map_entry->id);
    if (map->keys_eq(key, (void *)id)) {
      *index_out = index;
      return 1;
    }
    index = map_entry->map_chain_next;
  }

  return 0;
}

void concurrent_map_put(struct ConcurrentMap* map, void* key, int pkt_set_partition, int index) {
  unsigned hash = rehash(map->khash(key));
  unsigned bucket_id = (hash & (map->num_buckets_per_partition - 1)) +
                       (pkt_set_partition << map->num_buckets_per_partition_log_two);

  struct concurrent_dchain_cell *map_entry;
  struct concurrent_dchain_cell *new_map_entry;

  // The bucket is empty
  if (map->buckets[bucket_id] == -1) {
    map->buckets[bucket_id] = index;
  } else {
    map_entry = concurrent_dchain_cell_out(map->dchain, map->buckets[bucket_id]);
    while (map_entry->map_chain_next != -1)
      map_entry = concurrent_dchain_cell_out(map->dchain, map_entry->map_chain_next);
    map_entry->map_chain_next = index;
  }

  new_map_entry = concurrent_dchain_cell_out(map->dchain, index);
  new_map_entry->id = *((pkt_set_id_t*) key);

  return;
}

void concurrent_map_erase(struct ConcurrentMap* map, void* key, int pkt_set_partition, void** unused) {
  unsigned hash = rehash(map->khash(key));
  unsigned bucket_id = (hash & (map->num_buckets_per_partition - 1)) +
                       (pkt_set_partition << map->num_buckets_per_partition_log_two);

  struct concurrent_dchain_cell *map_entry;
  struct concurrent_dchain_cell *prev_map_entry;
  map_entry = concurrent_dchain_cell_out(map->dchain, map->buckets[bucket_id]);
  pkt_set_id_t *id = &(map_entry->id);
  if (map->keys_eq(key, (void *)id)) {
    map->buckets[bucket_id] = map_entry->map_chain_next;
    map_entry->map_chain_next = -1;
  } else {
    do {
      prev_map_entry = map_entry;
      map_entry = concurrent_dchain_cell_out(map->dchain, map_entry->map_chain_next);
      id = &(map_entry->id);
    } while (!map->keys_eq(key, (void *)id));
    prev_map_entry->map_chain_next = map_entry->map_chain_next;
    map_entry->map_chain_next = -1;
  }

  return;
}
