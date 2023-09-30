#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>

#include <rte_lcore.h>

#include "vigor/libvig/verified/vigor-time.h"
#include "vigor/libvig/verified/vector.h"

#include "nf.h"
#include "concurrent-map.h"
#include "concurrent-double-chain.h"
#include "pkt-set-manager.h"
#include "rlu-wrapper.h"

// TODO: expose these params to NF dev
#ifndef MAX_NUM_PKT_SETS
#define MAX_NUM_PKT_SETS 1369000
#endif
// #define MAX_NUM_PKT_SETS 155000U // vigpol
// #define MAX_NUM_PKT_SETS 1376256U // 21 * 65536
#define NUM_PKT_SET_PARTITIONS 128

static struct ConcurrentMap *pkt_set_id_to_state;
static struct ConcurrentDoubleChain *pkt_set_chain;
static struct Vector *pkt_set_state;
static vigor_time_t pkt_set_validity_duration;
static bool has_related_pkt_sets;

// Per-core logs of add_pkt_set() operation. Needed to avoid modification on
// the packet set map before the corresponding NF handler commits.
static RTE_DEFINE_PER_LCORE(add_pkt_set_log_entry_t, add_pkt_set_log_entry);

// TODO: maybe these two callbacks need to be specified by NF dev for verification?
static void pkt_set_id_allocate(void *obj) {}
//static void pkt_set_state_allocate(void *obj) {}

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

bool init_pkt_set_manager(map_keys_equality *pkt_set_id_eq,
                          map_key_hash *pkt_set_id_hash,
                          vigor_time_t _pkt_set_validity_duration,
                          bool _has_related_pkt_sets) {
  // For now set this to be equal to the number of data plane cores
  int num_pkt_set_partitions = rte_lcore_count() - 1;

  has_related_pkt_sets = _has_related_pkt_sets;

  // TODO: Ensure map_size do not overflow...
  unsigned int map_size = MAX_NUM_PKT_SETS;
  // Double the map size if the NF allows two packet sets to share packet set state.
  if (has_related_pkt_sets)
    map_size *= 2;
  // Round to next power of 2 for better map perf...
  map_size = next_pow2(map_size);

  int map_size_per_partition = next_pow2(map_size / num_pkt_set_partitions);

  if (!concurrent_dchain_allocate(MAX_NUM_PKT_SETS, &(pkt_set_chain))) return false;
  if (!concurrent_map_allocate(pkt_set_id_eq, pkt_set_id_hash, num_pkt_set_partitions,
                               map_size_per_partition,
                               pkt_set_chain, &(pkt_set_id_to_state))) return false;
  if (!vector_allocate(sizeof(pkt_set_state_t), MAX_NUM_PKT_SETS,
                       pkt_set_state_allocate, &(pkt_set_state))) return false;
  pkt_set_validity_duration = _pkt_set_validity_duration;

  return true;
}

/* Utils for logging/committing add_pkt_set op */
// ASSUMPTION: only one call to add_pkt_set_log per NF tx
void add_pkt_set_log_clear() {
  RTE_PER_LCORE(add_pkt_set_log_entry).add_pkt_set = false;
}

bool add_pkt_set_log(pkt_set_state_t *_state, int pkt_set_partition,
                     pkt_set_id_t *related_pkt_set_id) {
  if (!concurrent_dchain_has_free_indexes(pkt_set_chain, pkt_set_partition))
    return false;

  RTE_PER_LCORE(add_pkt_set_log_entry).add_pkt_set = true;
  if (related_pkt_set_id)
    RTE_PER_LCORE(add_pkt_set_log_entry).related_pkt_set_id = *related_pkt_set_id;
  RTE_PER_LCORE(add_pkt_set_log_entry).pkt_set_state = *_state; 
  return true;
}

void add_pkt_set_commit(pkt_set_id_t *pkt_set_id,
                        int pkt_set_partition, vigor_time_t time) {
  if (RTE_PER_LCORE(add_pkt_set_log_entry).add_pkt_set) {

    pkt_set_id_t *related_pkt_set_id = 
      &(RTE_PER_LCORE(add_pkt_set_log_entry).related_pkt_set_id);
    pkt_set_state_t *_state =
      &(RTE_PER_LCORE(add_pkt_set_log_entry).pkt_set_state);

    int index;

    concurrent_dchain_allocate_new_index(pkt_set_chain, &index, time,
                                         pkt_set_partition);

    concurrent_map_put(pkt_set_id_to_state, (void *)pkt_set_id, pkt_set_partition, index);

    // Temp hack to support related pkt sets
    if (has_related_pkt_sets) {
      concurrent_map_put(pkt_set_id_to_state, (void *)related_pkt_set_id,
                         pkt_set_partition, index + MAX_NUM_PKT_SETS);
    }

    pkt_set_state_t *state_ptr;
    vector_borrow(pkt_set_state, index, (void **)&state_ptr);
    memcpy((void *)state_ptr, (void *)_state, sizeof(pkt_set_state_t));
    vector_return(pkt_set_state, index, state_ptr);
  }
}

bool get_pkt_set_state(pkt_set_id_t *pkt_set_id, pkt_set_state_t **_state,
                       int pkt_set_partition, vigor_time_t time) {
  int index;
  if (!concurrent_map_get(pkt_set_id_to_state, pkt_set_id, pkt_set_partition, &index))
    return false;

  // Temp hack to support related pkt sets
  if (index >= MAX_NUM_PKT_SETS)
    index -= MAX_NUM_PKT_SETS;

  concurrent_dchain_rejuvenate_index(pkt_set_chain, index, time, pkt_set_partition);

  pkt_set_state_t *state_ptr;
  vector_borrow(pkt_set_state, index, (void **)&state_ptr);
  // TEMP HACK to return a mutable reference to the pkt set state
  *_state = state_ptr;
  // memcpy((void *)_state, (void *)state_ptr, sizeof(pkt_set_state_t));
  // vector_return(pkt_set_state, index, state_ptr);

  return true;
}

int delete_expired_pkt_sets(vigor_time_t time, int pkt_set_partition,
                            nf_state_t *non_pkt_set_state) {
  assert(time >= 0); // we don't support the past
  assert(sizeof(vigor_time_t) <= sizeof(uint64_t));
  uint64_t time_u = (uint64_t)time; // OK because of the two asserts
  vigor_time_t last_time = time_u - pkt_set_validity_duration;

  int index, count = 0;
  rlu_thread_data_t *rlu_data = get_rlu_thread_data();

  while (concurrent_dchain_has_expired_index(pkt_set_chain, &index, last_time,
                                            pkt_set_partition)) {

    pkt_set_state_t *state_ptr;
    vector_borrow(pkt_set_state, index, (void **)&state_ptr);

restart:
	  RLU_READER_LOCK(rlu_data);
    if (nf_expired_pkt_set_handler(non_pkt_set_state, state_ptr) == ABORT_HANDLER) {
      NF_DEBUG("ABORT: nf_expired_pkt_set_handler\n");
      nfos_abort_txn(rlu_data);
      goto restart;
    }
    if (!RLU_READER_UNLOCK(rlu_data)) {
      nfos_abort_txn(rlu_data);
      NF_DEBUG("ABORT: read validation\n");
      goto restart;
    }

    vector_return(pkt_set_state, index, state_ptr);

    struct concurrent_dchain_cell *cell = 
           concurrent_dchain_cell_out(pkt_set_chain, index);

    pkt_set_id_t *key;
    key = &(cell->id);
    concurrent_map_erase(pkt_set_id_to_state, key, pkt_set_partition, (void **)&key);

    // FIXME: this does not work if packet set state can be shared by either one or two packet sets.
    // Temp hack to support related pkt sets
    if (has_related_pkt_sets) {
      cell = concurrent_dchain_cell_out(pkt_set_chain, index + MAX_NUM_PKT_SETS);
      key = &(cell->id);
      concurrent_map_erase(pkt_set_id_to_state, key, pkt_set_partition, (void **)&key);
    }

    // Make sure map/vector entry of the packet set map is invalidated
    // before returning the index to the free list
    __asm__ __volatile__ ("" : : : "memory");

    concurrent_dchain_free_index(pkt_set_chain, index, pkt_set_partition);

    ++count;
  }
  return count;
}
