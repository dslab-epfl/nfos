#include <stdlib.h>

#include "concurrent-double-chain-impl.h"
#include "lock.h"

// enum DCHAIN_ENUM {
//     ALLOC_LIST_HEAD = 0,
//     FREE_LIST_HEAD = NUM_AL_LISTS,
//     INDEX_SHIFT = DCHAIN_RESERVED
// };

static struct mcslock_t *dchain_locks;
static struct mcslock_t dchain_global_lock;
static int ALLOC_LIST_HEAD, FREE_LIST_HEAD, GLOBAL_FREE_LIST_HEAD, INDEX_SHIFT, NUM_FREE_LISTS;

void concurrent_dchain_impl_init(struct concurrent_dchain_cell *cells, int size, int num_cores)
{
  ALLOC_LIST_HEAD = 0;
  FREE_LIST_HEAD = num_cores << LIST_HEAD_PADDING;
  GLOBAL_FREE_LIST_HEAD = (num_cores * 2) << LIST_HEAD_PADDING;
  INDEX_SHIFT = GLOBAL_FREE_LIST_HEAD + 1;
  NUM_FREE_LISTS = num_cores;

  mcslock_init(&dchain_global_lock);
  dchain_locks = calloc(num_cores, sizeof(struct mcslock_t));
  for (int i = 0; i < num_cores; i++)
    mcslock_init(&(dchain_locks[i]));

  // Init per-core lists of allocated index
  struct concurrent_dchain_cell* al_head;
  int i = ALLOC_LIST_HEAD;
  for (; i < FREE_LIST_HEAD; i += (1 << LIST_HEAD_PADDING))
  {
    al_head = cells + i;
    al_head->prev = i;
    al_head->next = i;
    al_head->list_ind = i - ALLOC_LIST_HEAD;
  }

  // Init per-core lists of free index
  struct concurrent_dchain_cell* fl_head;
  for (i = FREE_LIST_HEAD; i < GLOBAL_FREE_LIST_HEAD; i += (1 << LIST_HEAD_PADDING))
  {
    fl_head = cells + i;
    fl_head->next = INDEX_SHIFT + ((size / num_cores) * ((i - FREE_LIST_HEAD) >> LIST_HEAD_PADDING));
    fl_head->prev = fl_head->next;
    fl_head->list_ind = i - FREE_LIST_HEAD;
  }

  // Init global list of free index
  struct concurrent_dchain_cell* glb_fl_head = cells + GLOBAL_FREE_LIST_HEAD;
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
        struct concurrent_dchain_cell* current = cells + i + j;
        current->next = i + j + 1;
        current->prev = current->next;
        current->list_ind = -1;
        current->time = INT64_MAX;
        current->map_chain_next = -1;

        // temp hack to support related packet sets
        struct concurrent_dchain_cell* current_related = current + size;
        current_related->map_chain_next = -1;
    }
    struct concurrent_dchain_cell* last = cells + i + j;
    last->next = FREE_LIST_HEAD + (core_id << LIST_HEAD_PADDING);
    last->prev = last->next;
    last->time = INT64_MAX;
    last->list_ind = -1;
    last->map_chain_next = -1;

    // temp hack to support related packet sets
    struct concurrent_dchain_cell* last_related = last + size;
    last_related->map_chain_next = -1;
  }

}

int concurrent_dchain_impl_has_free_indexes(struct concurrent_dchain_cell *cells, int core_id) 
{
  int al_head_ind = core_id << LIST_HEAD_PADDING;

  struct concurrent_dchain_cell* fl_head = cells + FREE_LIST_HEAD + al_head_ind;
  int allocated = fl_head->next;
  return (allocated != FREE_LIST_HEAD + al_head_ind);
}

int concurrent_dchain_impl_allocate_new_index(struct concurrent_dchain_cell *cells, int *index, int core_id,
                                              vigor_time_t time) 
{
  int al_head_ind = core_id << LIST_HEAD_PADDING;

  struct concurrent_dchain_cell* fl_head = cells + FREE_LIST_HEAD + al_head_ind;
  struct concurrent_dchain_cell* al_head = cells + ALLOC_LIST_HEAD + al_head_ind;
  struct mcsqnode_t qnode;
  mcslock_lock(&(dchain_locks[core_id]), &qnode);
  int allocated = fl_head->next;
  if (allocated == FREE_LIST_HEAD + al_head_ind)
  {
    mcslock_unlock(&(dchain_locks[core_id]), &qnode);
    return 0;
  }
  struct concurrent_dchain_cell* allocp = cells + allocated;
  // Extract the link from the "empty" chain.
  fl_head->next = allocp->next;
  fl_head->prev = fl_head->next;
  mcslock_unlock(&(dchain_locks[core_id]), &qnode);

  // Add the link to the "new"-end "alloc" chain.
  allocp->next = ALLOC_LIST_HEAD + al_head_ind;
  allocp->prev = al_head->prev;
  allocp->list_ind = al_head_ind;
  allocp->time = time;
  struct concurrent_dchain_cell* alloc_head_prevp = cells + al_head->prev;
  alloc_head_prevp->next = allocated;
  al_head->prev = allocated;

  *index = allocated - INDEX_SHIFT;
  return 1;
}

// Try to move a free index from a local list to the global list.
// Returns 1 if succeeds
static inline int concurrent_dchain_impl_fill_global_free_list(struct concurrent_dchain_cell *cells, int core_id) 
{
  int fl_head_ind = core_id << LIST_HEAD_PADDING;

  struct concurrent_dchain_cell* fl_head = cells + FREE_LIST_HEAD + fl_head_ind;
  struct concurrent_dchain_cell* glb_fl_head = cells + GLOBAL_FREE_LIST_HEAD;
  int allocated = fl_head->next;
  if (allocated == FREE_LIST_HEAD + fl_head_ind)
  {
    return 0;
  }
  struct concurrent_dchain_cell* allocp = cells + allocated;
  // Extract the link from the local free list.
  fl_head->next = allocp->next;
  fl_head->prev = fl_head->next;

  // Add the link to the global free list.
  allocp->next = glb_fl_head->next;
  allocp->prev = allocp->next;
  allocp->list_ind = -1;
  allocp->time = INT64_MAX;

  glb_fl_head->next = allocated;
  glb_fl_head->prev = glb_fl_head->next;

  return 1;
}

int concurrent_dchain_impl_allocate_new_index_global(struct concurrent_dchain_cell *cells, int *index, int core_id,
                                                     vigor_time_t time) 
{
  int al_head_ind = core_id << LIST_HEAD_PADDING;

  struct concurrent_dchain_cell* glb_fl_head = cells + GLOBAL_FREE_LIST_HEAD;
  struct concurrent_dchain_cell* al_head = cells + ALLOC_LIST_HEAD + al_head_ind;
  int allocated;
  int ret = 0;

  struct mcsqnode_t glb_qnode;
  mcslock_lock(&dchain_global_lock, &glb_qnode);

  allocated = glb_fl_head->next;
  // the global free list is empty
  if (allocated == GLOBAL_FREE_LIST_HEAD)
  {
    // Grab all the per-core locks...
    struct mcsqnode_t qnodes[NUM_FREE_LISTS];
    for (int i = 0; i < NUM_FREE_LISTS; i++)
      mcslock_lock(&(dchain_locks[i]), &(qnodes[i]));

    // Try to take one index from each of the per-core free lists
    for (int i = 0; i < NUM_FREE_LISTS; i++)
      concurrent_dchain_impl_fill_global_free_list(cells, i);

    // retry
    allocated = glb_fl_head->next;
    if (allocated != GLOBAL_FREE_LIST_HEAD)
      ret = 1;

    for (int i = 0; i < NUM_FREE_LISTS; i++)
      mcslock_unlock(&(dchain_locks[i]), &(qnodes[i]));
  } else {
    ret = 1;
  }
 
  if (ret == 0) {
    mcslock_unlock(&dchain_global_lock, &glb_qnode);
    return 0;
  }

  struct concurrent_dchain_cell* allocp = cells + allocated;
  // Extract the link from the "empty" chain.
  glb_fl_head->next = allocp->next;
  glb_fl_head->prev = glb_fl_head->next;
  mcslock_unlock(&dchain_global_lock, &glb_qnode);

  // Add the link to the "new"-end "alloc" chain.
  allocp->next = ALLOC_LIST_HEAD + al_head_ind;
  allocp->prev = al_head->prev;
  allocp->list_ind = al_head_ind;
  allocp->time = time;
  struct concurrent_dchain_cell* alloc_head_prevp = cells + al_head->prev;
  alloc_head_prevp->next = allocated;
  al_head->prev = allocated;

  *index = allocated - INDEX_SHIFT;
  return 1;
}

int concurrent_dchain_impl_free_index(struct concurrent_dchain_cell *cells, int index, int core_id)
{
  int al_head_ind = core_id << LIST_HEAD_PADDING;

  int freed = index + INDEX_SHIFT;
  struct concurrent_dchain_cell* freedp = cells + freed;
  // make sure the index belongs to the partition
  if (freedp->list_ind != al_head_ind)
    return 0;
  int freed_prev = freedp->prev;
  int freed_next = freedp->next;
  // The index is already free.
  if (freed_next == freed_prev) {
    if (freed_prev != ALLOC_LIST_HEAD + al_head_ind) {
      return 0;
    } else {
    }
  } else {
  }

  struct concurrent_dchain_cell* fr_head = cells + FREE_LIST_HEAD + al_head_ind;


  // Extract the link from the "alloc" chain.
  struct concurrent_dchain_cell* freed_prevp = cells + freed_prev;
  freed_prevp->next = freed_next;

  struct concurrent_dchain_cell* freed_nextp = cells + freed_next;
  freed_nextp->prev = freed_prev;

  struct mcsqnode_t qnode;
  mcslock_lock(&(dchain_locks[core_id]), &qnode);
  // Add the link to the "free" chain.
  freedp->next = fr_head->next;
  freedp->prev = freedp->next;
  freedp->list_ind = -1;

  fr_head->next = freed;
  fr_head->prev = fr_head->next;
  mcslock_unlock(&(dchain_locks[core_id]), &qnode);
  return 1;
}

int concurrent_dchain_impl_get_oldest_index(struct concurrent_dchain_cell *cells, int *index, int core_id)
{
  int al_head_ind = core_id << LIST_HEAD_PADDING;

  struct concurrent_dchain_cell *al_head = cells + ALLOC_LIST_HEAD + al_head_ind;
  // No allocated indexes.
  if (al_head->next == al_head->prev) {
    if (al_head->next == ALLOC_LIST_HEAD + al_head_ind) {
      return 0;
    }
  }
  *index = al_head->next - INDEX_SHIFT;
  return 1;
}

int concurrent_dchain_impl_rejuvenate_index(struct concurrent_dchain_cell *cells, int index, int core_id,
                                            vigor_time_t time)
{
  int al_head_ind = core_id << LIST_HEAD_PADDING;

  struct concurrent_dchain_cell *al_head = cells + ALLOC_LIST_HEAD + al_head_ind;
  int lifted = index + INDEX_SHIFT;
  struct concurrent_dchain_cell *liftedp = cells + lifted;
  int lifted_next = liftedp->next;
  int lifted_prev = liftedp->prev;
  // make sure the index is allocated on al_head_ind
  if (liftedp->list_ind != al_head_ind)
    return 0;
  // The index is not allocated.
  if (lifted_next == lifted_prev) {
    if (lifted_next != ALLOC_LIST_HEAD + al_head_ind) {
      return 0;
    } else {
      return 1;
    }
  } else {
  }

  struct concurrent_dchain_cell *lifted_prevp = cells + lifted_prev;
  lifted_prevp->next = lifted_next;

  struct concurrent_dchain_cell *lifted_nextp = cells + lifted_next;
  lifted_nextp->prev = lifted_prev;

  int al_head_prev = al_head->prev;

  // Link it at the very end - right before the special link.
  liftedp->next = ALLOC_LIST_HEAD + al_head_ind;
  liftedp->prev = al_head_prev;
  liftedp->time = time;

  struct concurrent_dchain_cell *al_head_prevp = cells + al_head_prev;
  al_head_prevp->next = lifted;
  al_head->prev = lifted;
  return 1;
}

int concurrent_dchain_impl_is_index_allocated(struct concurrent_dchain_cell *cells, int index, int core_id)
{
  int al_head_ind = core_id << LIST_HEAD_PADDING;

  int lifted = index + INDEX_SHIFT;
  struct concurrent_dchain_cell *liftedp = cells + lifted;
  int lifted_next = liftedp->next;
  int lifted_prev = liftedp->prev;
  // make sure the index is allocated on al_head_ind
  if (liftedp->list_ind != al_head_ind) {
    return 0;
  }
  int result;
  if (lifted_next == lifted_prev) {
    if (lifted_next != ALLOC_LIST_HEAD + al_head_ind) {
      return 0;
    } else {
      return 1;
    }
  } else {
    return 1;
  }
}

struct concurrent_dchain_cell *concurrent_dchain_impl_cell_out(struct concurrent_dchain_cell *cells, int index)
{
  return cells + index + INDEX_SHIFT;
}
