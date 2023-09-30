#include "concurrent-double-chain.h"

#include <stdlib.h>
#include <stddef.h>

#include <rte_lcore.h>
#include <rte_malloc.h>

#include "concurrent-double-chain-impl.h"

#ifndef NULL
#define NULL 0
#endif//NULL

struct ConcurrentDoubleChain {
  struct concurrent_dchain_cell* cells;
};

int concurrent_dchain_allocate(int index_range, struct ConcurrentDoubleChain** chain_out)
{
  // the last core is not used for data plane
  int num_cores = rte_lcore_count() - 1;

  struct ConcurrentDoubleChain* old_chain_out = *chain_out;
  struct ConcurrentDoubleChain* chain_alloc = (struct ConcurrentDoubleChain*) malloc(sizeof(struct ConcurrentDoubleChain));
  if (chain_alloc == NULL) return 0;
  *chain_out = (struct ConcurrentDoubleChain*) chain_alloc;

  // temp hack to support related packet sets
  struct concurrent_dchain_cell* cells_alloc =
    (struct concurrent_dchain_cell*) rte_malloc(NULL, sizeof (struct concurrent_dchain_cell)*(index_range * 2 + (num_cores << LIST_HEAD_PADDING) * 2 + 1), 0);
  if (cells_alloc == NULL) {
    free(chain_alloc);
    *chain_out = old_chain_out;
    return 0;
  }
  (*chain_out)->cells = cells_alloc;

  concurrent_dchain_impl_init((*chain_out)->cells, index_range, num_cores);
  return 1;
}

int concurrent_dchain_has_free_indexes(struct ConcurrentDoubleChain* chain,
                                       int partition)
{
  return concurrent_dchain_impl_has_free_indexes(chain->cells, partition);
}

int concurrent_dchain_allocate_new_index(struct ConcurrentDoubleChain* chain,
                                         int *index_out, vigor_time_t time,
                                         int partition)
{
  int ret = concurrent_dchain_impl_allocate_new_index(chain->cells, index_out, partition, time);
  // try to get index from the global pool if local pool is empty
  if (!ret) {
    ret = concurrent_dchain_impl_allocate_new_index_global(chain->cells, index_out, partition, time);
  }

  return ret;
}

int concurrent_dchain_rejuvenate_index(struct ConcurrentDoubleChain* chain,
                                       int index, vigor_time_t time,
                                       int partition)
{
  int ret = concurrent_dchain_impl_rejuvenate_index(chain->cells, index, partition, time);
  return ret;
}

int concurrent_dchain_expire_one_index(struct ConcurrentDoubleChain* chain,
                                       int* index_out, vigor_time_t time,
                                       int partition)
{
  int has_ind = concurrent_dchain_impl_get_oldest_index(chain->cells, index_out, partition);
  if (has_ind) {
    struct concurrent_dchain_cell *cell = concurrent_dchain_impl_cell_out(chain->cells, *index_out);
    if (cell->time < time) {
      int rez = concurrent_dchain_impl_free_index(chain->cells, *index_out, partition);
      return rez;
    }

  }
  return 0;
}

int concurrent_dchain_has_expired_index(struct ConcurrentDoubleChain* chain,
                                       int* index_out, vigor_time_t time,
                                       int partition)
{
  int has_ind = concurrent_dchain_impl_get_oldest_index(chain->cells, index_out, partition);
  if (has_ind) {
    struct concurrent_dchain_cell *cell = concurrent_dchain_impl_cell_out(chain->cells, *index_out);
    if (cell->time < time) {
      return 1;
    }
  }
  return 0;
}

int concurrent_dchain_is_index_allocated(struct ConcurrentDoubleChain* chain, int index, int partition)
{
  return concurrent_dchain_impl_is_index_allocated(chain->cells, index, partition);
}

int concurrent_dchain_free_index(struct ConcurrentDoubleChain* chain, int index, int partition)
{
  return concurrent_dchain_impl_free_index(chain->cells, index, partition);
}

struct concurrent_dchain_cell *concurrent_dchain_cell_out(struct ConcurrentDoubleChain* chain, int index)
{
  return concurrent_dchain_impl_cell_out(chain->cells, index);
}
