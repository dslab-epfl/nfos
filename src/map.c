#include "map.h"

#include <stdlib.h>
#include <string.h>

#include <rte_malloc.h>

#include "rlu-wrapper.h"

struct nfos_map_entry {
  int busybits;
  unsigned khs;
  int chns;
  int vals;
  unsigned char key[0]; // address of key
} __attribute__((aligned(sizeof(void *))));

struct NfosMap {
  struct nfos_map_entry** entries;
  unsigned key_size;
  unsigned capacity;
  map_keys_equality* keys_eq;
  map_key_hash* khash;
};

// compute the next highest power of 2 of 32-bit v
// see https://graphics.stanford.edu/~seander/bithacks.html#RoundUpPowerOf2
static inline unsigned int next_pow2(unsigned int v) {
  v--;
  v |= v >> 1;
  v |= v >> 2;
  v |= v >> 4;
  v |= v >> 8;
  v |= v >> 16;
  v++;
  return v;
}

int nfos_map_allocate_t(map_keys_equality* keq, map_key_hash* khash, unsigned key_size, unsigned capacity, struct NfosMap** map_out) {
  // Increase capacity to next power of two for lookup performance
  capacity = next_pow2(capacity);

  struct NfosMap* map = (struct NfosMap*)malloc(sizeof(struct NfosMap));
  if (!map) return 0;
  map->entries = (struct nfos_map_entry**) rte_malloc(NULL, sizeof(struct nfos_map_entry*) * capacity, 0);
  if (!map->entries) {
    free((void*)map);
    return 0;
  }
  for (int i = 0; i < capacity; i++)
    map->entries[i] = (struct nfos_map_entry*) RLU_ALLOC(sizeof(struct nfos_map_entry) + key_size);

  map->key_size = key_size;
  map->capacity = capacity;
  map->keys_eq = keq;
  map->khash = khash;
  for (unsigned i = 0; i < capacity; i++) {
    struct nfos_map_entry* entry = map->entries[i];
    entry->busybits = 0;
    entry->chns = 0;
  }
  *map_out = map;
  return 1;
}

int nfos_map_get_t(struct NfosMap* map, void* key, int* value_out) {
  rlu_thread_data_t *rlu_data = get_rlu_thread_data();

  unsigned hash = map->khash(key);
  unsigned start = hash & (map->capacity - 1);
  for (unsigned i = 0; i < map->capacity; i++) {
    unsigned index = (start + i) & (map->capacity - 1);
    struct nfos_map_entry* entry = map->entries[index];
    entry = (struct nfos_map_entry*) RLU_DEREF(rlu_data, entry);

    if (entry->busybits != 0 && entry->khs == hash) {
      void* keyps = (void*) entry->key;
      if (map->keys_eq(keyps, key)) {
        *value_out = entry->vals;
        return 1;
      }
    } else if (entry->chns == 0) {
      return 0;
    }
  }
  return 0;
}

int nfos_map_put_t(struct NfosMap* map, void* key, int value) {
  rlu_thread_data_t *rlu_data = get_rlu_thread_data();
  size_t true_map_entry_size = sizeof(struct nfos_map_entry) + map->key_size;

  unsigned hash = map->khash(key);
  unsigned start = hash & (map->capacity - 1);
  for (unsigned i = 0; i < map->capacity; i++) {
    unsigned index = (start + i) & (map->capacity - 1);
    struct nfos_map_entry* entry = map->entries[index];
    entry = (struct nfos_map_entry*) RLU_DEREF(rlu_data, entry);

    if (entry->busybits == 0) {
      // Increment chain length metadata only if the key is successfully inserted
      // make sure the chain is updated in canonical order to prevent deadlocking
      if (start + i >= map->capacity) {
        // lock the entries wrapped around first
        for (unsigned k = 0; k < start + i - map->capacity; k++) {
          struct nfos_map_entry* affected_entry = map->entries[k];
          // Use raw try_lock to provide the correct object size
          if (!_mvrlu_try_lock(rlu_data, (void**)&affected_entry, true_map_entry_size)) {
            return ABORT_HANDLER;
          }
          affected_entry->chns += 1;
        }
        if (!_mvrlu_try_lock(rlu_data, (void**)&entry, true_map_entry_size)) {
          return ABORT_HANDLER;
        }
        entry->busybits = 1;
        memcpy((void*)entry->key, key, map->key_size);
        entry->khs = hash;
        entry->vals = value;

        // then lock the other entries
        for (unsigned k = start; k < map->capacity; k++) {
          struct nfos_map_entry* affected_entry = map->entries[k];
          if (!_mvrlu_try_lock(rlu_data, (void**)&affected_entry, true_map_entry_size)) {
            return ABORT_HANDLER;
          }
          affected_entry->chns += 1;
        }

      } else {
        // no entries wrapped around
        for (unsigned k = start; k < start + i; k++) {
          struct nfos_map_entry* affected_entry = map->entries[k];
          if (!_mvrlu_try_lock(rlu_data, (void**)&affected_entry, true_map_entry_size)) {
            return ABORT_HANDLER;
          }
          affected_entry->chns += 1;
        }
        if (!_mvrlu_try_lock(rlu_data, (void**)&entry, true_map_entry_size)) {
          return ABORT_HANDLER;
        }
        entry->busybits = 1;
        memcpy((void*)entry->key, key, map->key_size);
        entry->khs = hash;
        entry->vals = value;

      }

      return 1;
    }

  }
  return 0;
}

int nfos_map_erase_t(struct NfosMap* map, void* key) {
  rlu_thread_data_t *rlu_data = get_rlu_thread_data();
  size_t true_map_entry_size = sizeof(struct nfos_map_entry) + map->key_size;

  unsigned hash = map->khash(key);
  unsigned start = hash & (map->capacity - 1);

  for (unsigned i = 0; i < map->capacity; i++) {
    unsigned index = (start + i) & (map->capacity - 1);
    struct nfos_map_entry* entry = map->entries[index];
    entry = (struct nfos_map_entry*) RLU_DEREF(rlu_data, entry);

    if (entry->busybits != 0 && entry->khs == hash) {
      // found the key, needs to perform chain update
      void* keyps = (void*)entry->key;
      if (map->keys_eq(keyps, key)) {
        // decrement chain length metadata only if the key exists in the map
        // make sure the chain is updated in canonical order to prevent deadlocking
        if (start + i >= map->capacity) {
          // lock the entries wrapped around first
          for (unsigned k = 0; k < start + i - map->capacity; k++) {
            struct nfos_map_entry* affected_entry = map->entries[k];
            if (!_mvrlu_try_lock(rlu_data, (void**)&affected_entry, true_map_entry_size)) {
              return ABORT_HANDLER;
            }
            affected_entry->chns -= 1;
          }
          if (!_mvrlu_try_lock(rlu_data, (void**)&entry, true_map_entry_size)) {
            return ABORT_HANDLER;
          }
          entry->busybits = 0;
          // then lock the other entries
          for (unsigned k = start; k < map->capacity; k++) {
            struct nfos_map_entry* affected_entry = map->entries[k];
            if (!_mvrlu_try_lock(rlu_data, (void**)&affected_entry, true_map_entry_size)) {
              return ABORT_HANDLER;
            }
            affected_entry->chns -= 1;
          }

        } else {
          // no entries wrapped around
          for (unsigned k = start; k < start + i; k++) {
            struct nfos_map_entry* affected_entry = map->entries[k];
            if (!_mvrlu_try_lock(rlu_data, (void**)&affected_entry, true_map_entry_size)) {
              return ABORT_HANDLER;
            }
            affected_entry->chns -= 1;
          }
          if (!_mvrlu_try_lock(rlu_data, (void**)&entry, true_map_entry_size)) {
            return ABORT_HANDLER;
          }
          entry->busybits = 0;

        }

        return 1;
      }
    } else if (entry->chns == 0) {
      return 0;
    }

  }
  return 0;
}

int nfos_map_allocate_debug_t(map_keys_equality* keq, map_key_hash* khash,
                              unsigned key_size, unsigned capacity, struct NfosMap** map_out,
                              const char *filename, int lineno){
  int ret = nfos_map_allocate_t(keq, khash, key_size, capacity, map_out);
  profiler_add_ds_inst((void *)(*map_out), filename, lineno);
  return ret;
}

int nfos_map_put_debug_t(struct NfosMap* map, void* key, int value){
  profiler_add_ds_op_info(map, 2, 0, key, map->key_size);

  int ret = nfos_map_put_t(map, key, value);

  profiler_inc_curr_op();
  return ret;
}

int nfos_map_erase_debug_t(struct NfosMap* map, void* key){
  profiler_add_ds_op_info(map, 2, 1, key, map->key_size);

  int ret = nfos_map_erase_t(map,key);

  profiler_inc_curr_op();
  return ret;
}

int nfos_map_get_debug_t(struct NfosMap* map, void* key, int* value_out) {
  profiler_add_ds_op_info(map, 2, 2, key, map->key_size);

  int ret = nfos_map_get_t(map, key, value_out);

  profiler_inc_curr_op();
  return ret;
}