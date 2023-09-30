#include <rte_lcore.h>
#include <rte_malloc.h>

#include "double-chain-exp.h"
#include "rlu-wrapper.h"

#include "timer.h"

// Debugging
// #include "vigor/nf-log.h"
#define NF_DEBUG // Redefine the macro to disable it
// Separate free/alloc list head cells with 7 (2^3 - 1) padding cells
#define LIST_HEAD_PADDING 3

struct nfos_dchain_exp_cell {
  int prev;
  int next;
  int list_ind; // ind of corresponding al_list, -1 means the cell is free
  vigor_time_t time; // time to free the index
};

struct NfosDoubleChainExp {
  struct nfos_dchain_exp_cell **cells;
  int curr_free_list; // current per-core free list to check for expired index
  int num_free_lists;
  vigor_time_t validity_duration;
};

static int ALLOC_LIST_HEAD, FREE_LIST_HEAD, GLOBAL_FREE_LIST_HEAD, INDEX_SHIFT, NUM_FREE_LISTS;

int nfos_dchain_exp_allocate_t(int index_range, vigor_time_t validity_duration, struct NfosDoubleChainExp **chain_out)
{
  /* Allocate memory for the chain */

  // the last core is not used for data plane
  int num_cores = rte_lcore_count() - 1;

  struct NfosDoubleChainExp* old_chain_out = *chain_out;
  struct NfosDoubleChainExp* chain_alloc = (struct NfosDoubleChainExp*) malloc(sizeof(struct NfosDoubleChainExp));
  if (chain_alloc == NULL) return 0;
  *chain_out = (struct NfosDoubleChainExp*) chain_alloc;

  int num_cells = index_range + (num_cores << LIST_HEAD_PADDING) * 2 + 1;
  struct nfos_dchain_exp_cell **cells_alloc =
    (struct nfos_dchain_exp_cell **) rte_malloc(NULL, sizeof(struct nfos_dchain_exp_cell *) * num_cells, 0);
  if (cells_alloc == NULL) {
    free(chain_alloc);
    *chain_out = old_chain_out;
    return 0;
  }
  (*chain_out)->cells = cells_alloc;
  (*chain_out)->curr_free_list = 0;
  (*chain_out)->num_free_lists = num_cores;
  // Convert validity duration to tsc cycles
  (*chain_out)->validity_duration = nfos_usec_to_tsc_cycles(validity_duration);

  for (int i = 0; i < num_cells; i++)
    cells_alloc[i] = (struct nfos_dchain_exp_cell *) RLU_ALLOC(sizeof(struct nfos_dchain_exp_cell));


  /* Init the chain */

  struct nfos_dchain_exp_cell **cells = (*chain_out)->cells;
  int size = index_range;

  ALLOC_LIST_HEAD = 0;
  FREE_LIST_HEAD = num_cores << LIST_HEAD_PADDING;
  GLOBAL_FREE_LIST_HEAD = (num_cores * 2) << LIST_HEAD_PADDING;
  INDEX_SHIFT = GLOBAL_FREE_LIST_HEAD + 1;
  NUM_FREE_LISTS = num_cores;

  // Init per-core lists of allocated index
  struct nfos_dchain_exp_cell* al_head;
  int i = ALLOC_LIST_HEAD;
  for (; i < FREE_LIST_HEAD; i += (1 << LIST_HEAD_PADDING))
  {
    al_head = cells[i];
    al_head->prev = i;
    al_head->next = i;
    al_head->list_ind = i - ALLOC_LIST_HEAD;
    al_head->time = INT64_MAX;
  }

  // Init per-core lists of free index
  struct nfos_dchain_exp_cell* fl_head;
  for (i = FREE_LIST_HEAD; i < GLOBAL_FREE_LIST_HEAD; i += (1 << LIST_HEAD_PADDING))
  {
    fl_head = cells[i];
    fl_head->next = INDEX_SHIFT + ((size / num_cores) * ((i - FREE_LIST_HEAD) >> LIST_HEAD_PADDING));
    fl_head->prev = fl_head->next;
    fl_head->list_ind = i - FREE_LIST_HEAD;
    fl_head->time = INT64_MAX;
  }

  // Init global list of free index
  struct nfos_dchain_exp_cell* glb_fl_head = cells[GLOBAL_FREE_LIST_HEAD];
  glb_fl_head->next = GLOBAL_FREE_LIST_HEAD;
  glb_fl_head->prev = glb_fl_head->next;
  glb_fl_head->list_ind = -1;
  glb_fl_head->time = INT64_MAX;

  // Partition indexes among per-core lists of free index
  int core_id;
  for (i = INDEX_SHIFT, core_id = 0; i < size + INDEX_SHIFT; i += size / num_cores, core_id++)
  {
    int j;
    for (j = 0; (j < size / num_cores - 1) && (i + j < size + INDEX_SHIFT - 1); j++)
    {
        struct nfos_dchain_exp_cell* current = cells[i + j];
        current->next = i + j + 1;
        current->prev = current->next;
        current->list_ind = -1;
        current->time = INT64_MAX;
    }
    struct nfos_dchain_exp_cell* last = cells[i + j];
    last->next = FREE_LIST_HEAD + (core_id << LIST_HEAD_PADDING);
    last->prev = last->next;
    last->list_ind = -1;
    last->time = INT64_MAX;
  }

  return 1;
}

int nfos_dchain_exp_allocate_new_index_t(struct NfosDoubleChainExp *chain, int *index_out, vigor_time_t time)
{
  struct nfos_dchain_exp_cell **cells = chain->cells;
  int al_head_ind = get_rlu_thread_id() << LIST_HEAD_PADDING;

  rlu_thread_data_t *rlu_data = get_rlu_thread_data();

  struct nfos_dchain_exp_cell* glb_fl_head = cells[GLOBAL_FREE_LIST_HEAD];
  struct nfos_dchain_exp_cell* fl_head = cells[FREE_LIST_HEAD + al_head_ind];
  struct nfos_dchain_exp_cell* al_head = cells[ALLOC_LIST_HEAD + al_head_ind];

  struct nfos_dchain_exp_cell* allocp;

  /* Get a free index */

  fl_head = (struct nfos_dchain_exp_cell *) RLU_DEREF(rlu_data, fl_head);
  int allocated = fl_head->next;
  NF_DEBUG("fl_head->next: %d", allocated);

  int alloc_from_local = 1;
  int alloc_from_glb = 0;
  // if the global free list is re-filled during the operation
  int glb_fl_refilled = 0;

  // Allocation from local pool failed
  if (allocated == FREE_LIST_HEAD + al_head_ind) {
    alloc_from_local = 0;

    if (!RLU_TRY_LOCK(rlu_data, &glb_fl_head)) {
      NF_DEBUG("abort at 1");
      return ABORT_HANDLER;
    }

    allocated = glb_fl_head->next;
    NF_DEBUG("glb_fl_head->next: %d", allocated);
    // the global free list is empty
    if (allocated == GLOBAL_FREE_LIST_HEAD) {

      // Try to migrate one index from each of the per-core free lists to the global list
      for (int i = 0; i < NUM_FREE_LISTS; i++) {
        int list_head_ind = i << LIST_HEAD_PADDING;

        struct nfos_dchain_exp_cell* list_head = cells[FREE_LIST_HEAD + list_head_ind];
        list_head = (struct nfos_dchain_exp_cell *) RLU_DEREF(rlu_data, list_head);

        int migrated = list_head->next;
        if (migrated == (FREE_LIST_HEAD + list_head_ind)) {
          continue;
        }
        struct nfos_dchain_exp_cell* migratedp = cells[migrated];
        // Extract the link from the local free list.
        if (!RLU_TRY_LOCK(rlu_data, &list_head)) {
          NF_DEBUG("abort at 2");
          return ABORT_HANDLER;
        }
        migratedp = (struct nfos_dchain_exp_cell *) RLU_DEREF(rlu_data, migratedp);
        list_head->next = migratedp->next;
        list_head->prev = list_head->next;

        // Add the link to the global free list.
        if (!RLU_TRY_LOCK(rlu_data, &migratedp)) {
          NF_DEBUG("abort at 3");
          return ABORT_HANDLER;
        }
        migratedp->next = glb_fl_head->next;
        migratedp->prev = migratedp->next;
        migratedp->list_ind = -1;

        // make sure allocp points to the pending version of the last migratedp
        // otherwise we are using staled version
        allocp = migratedp;

        NF_DEBUG("migrated: %d migratedp: (%d %d)", migrated, migratedp->prev, migratedp->next);
        NF_DEBUG("migrated from %d list_head: (%d %d)", list_head_ind, list_head->prev, list_head->next);

        glb_fl_head->next = migrated;
        glb_fl_head->prev = glb_fl_head->next;

      }

      // retry
      allocated = glb_fl_head->next;
      NF_DEBUG("retry: glb_fl_head->next: %d", allocated);
      if (allocated != GLOBAL_FREE_LIST_HEAD) {
        alloc_from_glb = 1;
        glb_fl_refilled = 1;
      }

    } else {
      alloc_from_glb = 1;
    }
  }

  /* Install the free index to the local list of allocated indexes */

  if (alloc_from_local == 0 && alloc_from_glb == 0) {
    return 0;
  } else {
    // The allocp is already locked if glb_fl was refilled
    if (glb_fl_refilled == 0) {
      allocp = cells[allocated];
      if (!RLU_TRY_LOCK(rlu_data, &allocp)) {
        NF_DEBUG("abort at 4");
        return ABORT_HANDLER;
      }
    }

    // Extract the link from the "empty" chain.
    if (alloc_from_glb == 1) {
      glb_fl_head->next = allocp->next;
      glb_fl_head->prev = glb_fl_head->next;
      NF_DEBUG("glb_fl_head (%d %d)", glb_fl_head->prev, glb_fl_head->next);
    } else {
      if (!RLU_TRY_LOCK(rlu_data, &fl_head)) {
        NF_DEBUG("abort at 5");
        return ABORT_HANDLER;
      }
      fl_head->next = allocp->next;
      fl_head->prev = fl_head->next;
      NF_DEBUG("fl_head (%d %d)", fl_head->prev, fl_head->next);
    }

    // Add the link to the "new"-end "alloc" chain.

    allocp->next = ALLOC_LIST_HEAD + al_head_ind;
    al_head = (struct nfos_dchain_exp_cell *) RLU_DEREF(rlu_data, al_head);
    allocp->prev = al_head->prev;
    allocp->list_ind = al_head_ind;
    allocp->time = time + chain->validity_duration;
    NF_DEBUG("allocated: %d allocp: (%d %d)", allocated, allocp->prev, allocp->next);

    struct nfos_dchain_exp_cell* alloc_head_prevp = cells[al_head->prev];

    // Avoid locking the al_head twice if alloc_head_prevp == al_head
    if (al_head->prev != (ALLOC_LIST_HEAD + al_head_ind)) {
      if (!RLU_TRY_LOCK(rlu_data, &alloc_head_prevp)) {
        NF_DEBUG("abort at 6 %d %d", al_head->prev, allocated);
        return ABORT_HANDLER;
      }
      alloc_head_prevp->next = allocated;

      if (!RLU_TRY_LOCK(rlu_data, &al_head)) {
        NF_DEBUG("abort at 7");
        return ABORT_HANDLER;
      }
      al_head->prev = allocated;

      NF_DEBUG("alloc_head_prevp->next: %d al_head->prev: %d", alloc_head_prevp->next, al_head->prev);
    } else {
      if (!RLU_TRY_LOCK(rlu_data, &al_head)) {
        NF_DEBUG("abort at 8");
        return ABORT_HANDLER;
      }
      al_head->next = allocated;
      al_head->prev = allocated;
      NF_DEBUG("al_head: (%d %d)", al_head->prev, al_head->next);
    }

    *index_out = allocated - INDEX_SHIFT;
    return 1;
  }

}

int nfos_dchain_exp_rejuvenate_index_t(struct NfosDoubleChainExp *chain, int index, vigor_time_t time)
{
  struct nfos_dchain_exp_cell **cells = chain->cells;
  rlu_thread_data_t *rlu_data = get_rlu_thread_data();

  int lifted = index + INDEX_SHIFT;
  struct nfos_dchain_exp_cell *liftedp = cells[lifted];
  liftedp = (struct nfos_dchain_exp_cell *) RLU_DEREF(rlu_data, liftedp);

  // The index is not allocated
  if (liftedp->list_ind == -1) {
    return 0;
  }
  int al_head_ind = liftedp->list_ind;
  struct nfos_dchain_exp_cell *al_head = cells[ALLOC_LIST_HEAD + al_head_ind];

  int lifted_next = liftedp->next;
  int lifted_prev = liftedp->prev;

  // The index is already at the end of the list, nothing to do other than
  // refreshing the timestamp
  if (lifted_next == ALLOC_LIST_HEAD + al_head_ind) {
    if (!RLU_TRY_LOCK(rlu_data, &liftedp)) {
      NF_DEBUG("const abort at 3");
      return ABORT_HANDLER;
    }
    liftedp->time = time + chain->validity_duration;
    return 1;
  }

  struct nfos_dchain_exp_cell *lifted_prevp = cells[lifted_prev];
  if (!RLU_TRY_LOCK(rlu_data, &lifted_prevp)) {
    NF_DEBUG("abort at 9");
    return ABORT_HANDLER;
  }
  lifted_prevp->next = lifted_next;

  struct nfos_dchain_exp_cell *lifted_nextp = cells[lifted_next];
  if (!RLU_TRY_LOCK(rlu_data, &lifted_nextp)) {
    NF_DEBUG("abort at 10");
    return ABORT_HANDLER;
  }
  lifted_nextp->prev = lifted_prev;

  // even if lifted_prevp == al_head, al_head->prev is not modified yet...
  // so safe to return from the latest copy
  al_head = (struct nfos_dchain_exp_cell *) RLU_DEREF(rlu_data, al_head);
  int al_head_prev = al_head->prev;

  // Link it at the very end - right before the special link.
  if (!RLU_TRY_LOCK(rlu_data, &liftedp)) {
    NF_DEBUG("abort at 11");
    return ABORT_HANDLER;
  }
  liftedp->next = ALLOC_LIST_HEAD + al_head_ind;
  liftedp->prev = al_head_prev;
  liftedp->time = time + chain->validity_duration;

  // Make sure the al_head_prev cell is not locked twice
  if (al_head_prev != lifted_next) {
    struct nfos_dchain_exp_cell *al_head_prevp = cells[al_head_prev];
    if (!RLU_TRY_LOCK(rlu_data, &al_head_prevp)) {
      NF_DEBUG("abort at 12");
      return ABORT_HANDLER;
    }
    al_head_prevp->next = lifted;
  } else {
    lifted_nextp->next = lifted;
  }

  // Make sure the al_head cell is not locked twice
  if (al_head_ind + ALLOC_LIST_HEAD != lifted_prev) {
    if (!RLU_TRY_LOCK(rlu_data, &al_head)) {
      NF_DEBUG("abort at 13");
      return ABORT_HANDLER;
    }
    al_head->prev = lifted;
  } else {
    lifted_prevp->prev = lifted;
  }

  return 1;
}

void nfos_dchain_exp_reset_curr_free_list(struct NfosDoubleChainExp *chain)
{
  chain->curr_free_list = 0;
  return;
}

// TODO: merge expiration into the index allocation process
int nfos_dchain_exp_expire_one_index_t(struct NfosDoubleChainExp *chain, int *index_out,
                                     int *expiration_done_out, vigor_time_t time)
{
  struct nfos_dchain_exp_cell **cells = chain->cells;
  rlu_thread_data_t *rlu_data = get_rlu_thread_data();

  int al_head_ind = chain->curr_free_list << LIST_HEAD_PADDING;
  struct nfos_dchain_exp_cell *al_head = cells[ALLOC_LIST_HEAD + al_head_ind];

  al_head = (struct nfos_dchain_exp_cell *) RLU_DEREF(rlu_data, al_head);
  // No allocated indexes
  if (al_head->next == al_head->prev) {
    if (al_head->next == ALLOC_LIST_HEAD + al_head_ind) {
      // Check for another free list next time
      // Assuming expiration is only done by one core...
      chain->curr_free_list++;
      if (chain->curr_free_list >= chain->num_free_lists) {
        chain->curr_free_list = 0;
        // No expired index left...
        *expiration_done_out = 1;
      }
      return 0;
    }
  }

  // Get oldest index
  int freed = al_head->next;
  struct nfos_dchain_exp_cell *freedp = cells[freed];
  freedp = (struct nfos_dchain_exp_cell *) RLU_DEREF(rlu_data, freedp);
  // Oldest index still valid
  if (freedp->time >= time) {
    // potential perf bottleneck here
    // Check for another free list next time
    // Assuming expiration is only done by one core...
    chain->curr_free_list++;
    if (chain->curr_free_list >= chain->num_free_lists) {
      chain->curr_free_list = 0;
      // No expired index left...
      *expiration_done_out = 1;
    }
    return 0;
  }

  // Remove oldest index
  int freed_prev = freedp->prev;
  int freed_next = freedp->next;
  struct nfos_dchain_exp_cell* freed_prevp = cells[freed_prev];
  struct nfos_dchain_exp_cell* freed_nextp = cells[freed_next];

  // Add the link to the "free" chain.
  struct nfos_dchain_exp_cell* fr_head = cells[FREE_LIST_HEAD + al_head_ind];
  fr_head = (struct nfos_dchain_exp_cell *) RLU_DEREF(rlu_data, fr_head);

  if (!RLU_TRY_LOCK(rlu_data, &freedp)) {
    NF_DEBUG("abort at 16");
    return ABORT_HANDLER;
  }
  freedp->next = fr_head->next;
  freedp->prev = freedp->next;
  freedp->list_ind = -1;
  freedp->time = INT64_MAX;
  NF_DEBUG("freed: %d freedp: (%d %d)", freed, freedp->prev, freedp->next);

  if (!RLU_TRY_LOCK(rlu_data, &fr_head)) {
    NF_DEBUG("abort at 17");
    return ABORT_HANDLER;
  }
  fr_head->next = freed;
  fr_head->prev = fr_head->next;
  NF_DEBUG("fr_head: (%d %d)", fr_head->prev, fr_head->next);

  // Extract the link from the "alloc" chain.
  // Avoid locking the same object twice
  if (freed_prev != freed_next) {
    if ( (!RLU_TRY_LOCK(rlu_data, &freed_nextp)) || (!RLU_TRY_LOCK(rlu_data, &freed_prevp)) ) {
      NF_DEBUG("abort at 14");
      return ABORT_HANDLER;
    }
    freed_prevp->next = freed_next;
    freed_nextp->prev = freed_prev;

    NF_DEBUG("freed_prev: %d freed_prevp: (%d %d)", freed_prev, freed_prevp->prev, freed_prevp->next);
    NF_DEBUG("freed_next: %d freed_nextp: (%d %d)", freed_next, freed_nextp->prev, freed_nextp->next);

  } else {
    if ( !RLU_TRY_LOCK(rlu_data, &freed_prevp) ) {
      NF_DEBUG("abort at 15");
      return ABORT_HANDLER;
    }
    freed_prevp->next = freed_next;
    freed_prevp->prev = freed_prev;

    NF_DEBUG("freed_prev: %d freed_prevp: (%d %d)", freed_prev, freed_prevp->prev, freed_prevp->next);
  }

  *index_out = freed - INDEX_SHIFT;
  return 1;
}

int nfos_dchain_exp_is_index_allocated_t(struct NfosDoubleChainExp *chain, int index)
{
  struct nfos_dchain_exp_cell **cells = chain->cells;
  rlu_thread_data_t *rlu_data = get_rlu_thread_data();

  int elem = index + INDEX_SHIFT;
  struct nfos_dchain_exp_cell *elemp = cells[elem];
  elemp = (struct nfos_dchain_exp_cell *) RLU_DEREF(rlu_data, elemp);

  return !(elemp->list_ind == -1);
}

int nfos_dchain_exp_allocate_debug_t(int index_range, vigor_time_t validity_duration, struct NfosDoubleChainExp **chain_out,
                                     const char *filename, int lineno){
  int ret = nfos_dchain_exp_allocate_t(index_range, validity_duration, chain_out);
  profiler_add_ds_inst((void *)(*chain_out), filename, lineno);
  return ret;
}

int nfos_dchain_exp_allocate_new_index_debug_t(struct NfosDoubleChainExp *chain, int *index_out, vigor_time_t time){
  profiler_add_ds_op_info(chain, 0, 0, &time, sizeof(vigor_time_t));

  int ret = nfos_dchain_exp_allocate_new_index_t(chain, index_out, time);

  profiler_inc_curr_op();
  return ret;
}

typedef struct nfos_dchain_exp_rejuvenate_index_arg {
  int index;
  vigor_time_t time;
} nfos_dchain_exp_rejuvenate_index_arg_t;

int nfos_dchain_exp_rejuvenate_index_debug_t(struct NfosDoubleChainExp *chain, int index, vigor_time_t time){
  nfos_dchain_exp_rejuvenate_index_arg_t args = {
    .index = index,
    .time = time
  };

  profiler_add_ds_op_info(chain, 0, 1, &args, sizeof(nfos_dchain_exp_rejuvenate_index_arg_t));

  int ret = nfos_dchain_exp_rejuvenate_index_t(chain, index, time);

  profiler_inc_curr_op();
  return ret;
}

int nfos_dchain_exp_expire_one_index_debug_t(struct NfosDoubleChainExp *chain, int *index_out,
                                     int *expiration_done_out, vigor_time_t time){
  profiler_add_ds_op_info(chain, 0, 2, &time, sizeof(vigor_time_t));

  int ret = nfos_dchain_exp_expire_one_index_t(chain, index_out, expiration_done_out, time);

  profiler_inc_curr_op();
  return ret;
}

int nfos_dchain_exp_is_index_allocated_debug_t(struct NfosDoubleChainExp *chain, int index) {
  profiler_add_ds_op_info(chain, 0, 3, &index, sizeof(index));

  int ret = nfos_dchain_exp_is_index_allocated_t(chain, index);

  profiler_inc_curr_op();
  return ret;
}
