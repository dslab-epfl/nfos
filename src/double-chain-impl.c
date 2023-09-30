#include "double-chain-impl.h"
#include "rlu-wrapper.h"

// Debugging
// #include "vigor/nf-log.h"
#define NF_DEBUG // Redefine the macro to disable it

static int ALLOC_LIST_HEAD, FREE_LIST_HEAD, GLOBAL_FREE_LIST_HEAD, INDEX_SHIFT, NUM_FREE_LISTS;

void nfos_dchain_impl_init(struct nfos_dchain_cell **cells, int size, int num_cores)
{
  ALLOC_LIST_HEAD = 0;
  FREE_LIST_HEAD = num_cores << LIST_HEAD_PADDING;
  GLOBAL_FREE_LIST_HEAD = (num_cores * 2) << LIST_HEAD_PADDING;
  INDEX_SHIFT = GLOBAL_FREE_LIST_HEAD + 1;
  NUM_FREE_LISTS = num_cores;

  // Init per-core lists of allocated index
  struct nfos_dchain_cell* al_head;
  int i = ALLOC_LIST_HEAD;
  for (; i < FREE_LIST_HEAD; i += (1 << LIST_HEAD_PADDING))
  {
    al_head = cells[i];
    al_head->prev = i;
    al_head->next = i;
    al_head->list_ind = i - ALLOC_LIST_HEAD;
  }

  // Init per-core lists of free index
  struct nfos_dchain_cell* fl_head;
  for (i = FREE_LIST_HEAD; i < GLOBAL_FREE_LIST_HEAD; i += (1 << LIST_HEAD_PADDING))
  {
    fl_head = cells[i];
    fl_head->next = INDEX_SHIFT + ((size / num_cores) * ((i - FREE_LIST_HEAD) >> LIST_HEAD_PADDING));
    fl_head->prev = fl_head->next;
    fl_head->list_ind = i - FREE_LIST_HEAD;
  }

  // Init global list of free index
  struct nfos_dchain_cell* glb_fl_head = cells[GLOBAL_FREE_LIST_HEAD];
  glb_fl_head->next = GLOBAL_FREE_LIST_HEAD;
  glb_fl_head->prev = glb_fl_head->next;
  glb_fl_head->list_ind = -1;

  // Partition indexes among per-core lists of free index
  int core_id;
  for (i = INDEX_SHIFT, core_id = 0; i < size + INDEX_SHIFT; i += size / num_cores, core_id++)
  {
    int j;
    for (j = 0; (j < size / num_cores - 1) && (i + j < size + INDEX_SHIFT - 1); j++)
    {
        struct nfos_dchain_cell* current = cells[i + j];
        current->next = i + j + 1;
        current->prev = current->next;
        current->list_ind = -1;
    }
    struct nfos_dchain_cell* last = cells[i + j];
    last->next = FREE_LIST_HEAD + (core_id << LIST_HEAD_PADDING);
    last->prev = last->next;
    last->list_ind = -1;
  }

}

int nfos_dchain_impl_allocate_new_index(struct nfos_dchain_cell **cells, int *index, int core_id) 
{
  int al_head_ind = core_id << LIST_HEAD_PADDING;

  rlu_thread_data_t *rlu_data = get_rlu_thread_data();

  struct nfos_dchain_cell* glb_fl_head = cells[GLOBAL_FREE_LIST_HEAD];
  struct nfos_dchain_cell* fl_head = cells[FREE_LIST_HEAD + al_head_ind];
  struct nfos_dchain_cell* al_head = cells[ALLOC_LIST_HEAD + al_head_ind];

  struct nfos_dchain_cell* allocp;

  /* Get a free index */

  /* TODO: Guard fl_head with try_lock_const */
  fl_head = (struct nfos_dchain_cell *) RLU_DEREF(rlu_data, fl_head);
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

        struct nfos_dchain_cell* list_head = cells[FREE_LIST_HEAD + list_head_ind];
        list_head = (struct nfos_dchain_cell *) RLU_DEREF(rlu_data, list_head);

        int migrated = list_head->next;
        if (migrated == (FREE_LIST_HEAD + list_head_ind)) {
          continue;
        }
        struct nfos_dchain_cell* migratedp = cells[migrated];
        // Extract the link from the local free list.
        if (!RLU_TRY_LOCK(rlu_data, &list_head)) {
          NF_DEBUG("abort at 2");
          return ABORT_HANDLER;
        }
        migratedp = (struct nfos_dchain_cell *) RLU_DEREF(rlu_data, migratedp);
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
    al_head = (struct nfos_dchain_cell *) RLU_DEREF(rlu_data, al_head);
    allocp->prev = al_head->prev;
    allocp->list_ind = al_head_ind;
    NF_DEBUG("allocated: %d allocp: (%d %d)", allocated, allocp->prev, allocp->next);

    struct nfos_dchain_cell* alloc_head_prevp = cells[al_head->prev];

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

    *index = allocated - INDEX_SHIFT;
    return 1;
  }

}

int nfos_dchain_impl_free_index(struct nfos_dchain_cell **cells, int index, int core_id)
{
  int al_head_ind = core_id << LIST_HEAD_PADDING;
  rlu_thread_data_t *rlu_data = get_rlu_thread_data();

  int freed = index + INDEX_SHIFT;
  struct nfos_dchain_cell* freedp = cells[freed];
  freedp = (struct nfos_dchain_cell *) RLU_DEREF(rlu_data, freedp);
  // The index is already free.
  if (freedp->list_ind == -1) {
    return 0;
  }

  // Extract the link from the "alloc" chain.
  int freed_prev = freedp->prev;
  int freed_next = freedp->next;
  struct nfos_dchain_cell* freed_prevp = cells[freed_prev];
  struct nfos_dchain_cell* freed_nextp = cells[freed_next];

  // Avoid locking the same object twice
  if (freed_prev != freed_next) {
    if ( (!RLU_TRY_LOCK(rlu_data, &freed_prevp)) || (!RLU_TRY_LOCK(rlu_data, &freed_nextp)) ) {
      NF_DEBUG("abort at 9");
      return ABORT_HANDLER;
    }
    freed_prevp->next = freed_next;
    freed_nextp->prev = freed_prev;

    NF_DEBUG("freed_prev: %d freed_prevp: (%d %d)", freed_prev, freed_prevp->prev, freed_prevp->next);
    NF_DEBUG("freed_next: %d freed_nextp: (%d %d)", freed_next, freed_nextp->prev, freed_nextp->next);

  } else {
    if ( !RLU_TRY_LOCK(rlu_data, &freed_prevp) ) {
      NF_DEBUG("abort at 10");
      return ABORT_HANDLER;
    }
    freed_prevp->next = freed_next;
    freed_prevp->prev = freed_prev;

    NF_DEBUG("freed_prev: %d freed_prevp: (%d %d)", freed_prev, freed_prevp->prev, freed_prevp->next);
  }

  // Add the link to the "free" chain.
  struct nfos_dchain_cell* fr_head = cells[FREE_LIST_HEAD + al_head_ind];
  fr_head = (struct nfos_dchain_cell *) RLU_DEREF(rlu_data, fr_head);

  if (!RLU_TRY_LOCK(rlu_data, &freedp)) {
    NF_DEBUG("abort at 11");
    return ABORT_HANDLER;
  }
  freedp->next = fr_head->next;
  freedp->prev = freedp->next;
  freedp->list_ind = -1;
  NF_DEBUG("freed: %d freedp: (%d %d)", freed, freedp->prev, freedp->next);

  if (!RLU_TRY_LOCK(rlu_data, &fr_head)) {
    NF_DEBUG("abort at 12");
    return ABORT_HANDLER;
  }
  fr_head->next = freed;
  fr_head->prev = fr_head->next;
  NF_DEBUG("fr_head: (%d %d)", fr_head->prev, fr_head->next);

  return 1;
}
