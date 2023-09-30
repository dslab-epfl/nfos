#include "double-chain.h"

#include "double-chain-impl.h"

#include "rlu-wrapper.h"

#include <stdlib.h>
#include <stddef.h>
#include <stdio.h>

#include <rte_lcore.h>
#include <rte_malloc.h>

#ifndef NULL
#define NULL 0
#endif//NULL

struct NfosDoubleChain {
  struct nfos_dchain_cell **cells;
};

int nfos_dchain_allocate_t(int index_range, struct NfosDoubleChain** chain_out)
{
  // the last core is not used for data plane
  int num_cores = rte_lcore_count() - 1;

  struct NfosDoubleChain* old_chain_out = *chain_out;
  struct NfosDoubleChain* chain_alloc = (struct NfosDoubleChain*) malloc(sizeof(struct NfosDoubleChain));
  if (chain_alloc == NULL) return 0;
  *chain_out = (struct NfosDoubleChain*) chain_alloc;

  int num_cells = index_range + (num_cores << LIST_HEAD_PADDING) * 2 + 1;
  struct nfos_dchain_cell **cells_alloc =
    (struct nfos_dchain_cell **) rte_malloc(NULL, sizeof(struct nfos_dchain_cell *) * num_cells, 0);
  if (cells_alloc == NULL) {
    free(chain_alloc);
    *chain_out = old_chain_out;
    return 0;
  }
  (*chain_out)->cells = cells_alloc;

  for (int i = 0; i < num_cells; i++)
    cells_alloc[i] = (struct nfos_dchain_cell *) RLU_ALLOC(sizeof(struct nfos_dchain_cell));

  nfos_dchain_impl_init((*chain_out)->cells, index_range, num_cores);
  return 1;
}

int nfos_dchain_allocate_new_index_t(struct NfosDoubleChain* chain, int *index_out)
{
  int core_id = get_rlu_thread_id();

  int ret = nfos_dchain_impl_allocate_new_index(chain->cells, index_out, core_id);

  return ret;
}

int nfos_dchain_free_index_t(struct NfosDoubleChain* chain, int index)
{
  int core_id = get_rlu_thread_id();

  return nfos_dchain_impl_free_index(chain->cells, index, core_id);
}

void _nfos_dchain_dump(struct NfosDoubleChain* chain, int index_range)
{
  // the last core is not used for data plane
  int num_cores = rte_lcore_count() - 1;
  int num_cells = index_range + (num_cores << LIST_HEAD_PADDING) * 2 + 1;
  struct nfos_dchain_cell **cells = chain->cells;

  printf("cell ind: list_ind next prev\n");
  for (int i = 0; i < num_cells; i++) {
    struct nfos_dchain_cell *cell = cells[i];
    printf("cell %d: %8d %8d %8d\n", i, cell->list_ind, cell->next, cell->prev);
  }
}

int nfos_dchain_allocate_debug_t(int index_range, struct NfosDoubleChain** chain_out,
                                 const char *filename, int lineno) {
  int ret = nfos_dchain_allocate_t(index_range, chain_out);
  profiler_add_ds_inst((void *)(*chain_out), filename, lineno);
  return ret;
}

int nfos_dchain_allocate_new_index_debug_t(struct NfosDoubleChain* chain, int* index_out){
  profiler_add_ds_op_info(chain, 4, 0, NULL, 0);

  int ret = nfos_dchain_allocate_new_index_t(chain, index_out);

  profiler_inc_curr_op();
  return ret;
}

int nfos_dchain_free_index_debug_t(struct NfosDoubleChain* chain, int index){
  profiler_add_ds_op_info(chain, 4, 1, &index, sizeof(index));

  int ret = nfos_dchain_free_index_t(chain, index);

  profiler_inc_curr_op();
  return ret;
}