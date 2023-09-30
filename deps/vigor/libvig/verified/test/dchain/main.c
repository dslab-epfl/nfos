#include <inttypes.h>
// DPDK uses these but doesn't include them. :|
#include <linux/limits.h>
#include <sys/types.h>

#include <stdlib.h>
#include <stdio.h>
#include <assert.h>

#include <rte_common.h>
#include <rte_eal.h>
#include <rte_lcore.h>
#include <rte_cycles.h>

#include "libvig/verified/double-chain.h"
#define DCHAIN_SIZE 65536
// must >= 4
#define NUM_PARTITIONS 4

struct lcore_func {
  int (* func)(void*);
  void* arg;
};

// entry function to various NF threads, a bit ugly...
static int lcore_entry(void* arg) {
  struct lcore_func* lcore_func = (struct lcore_func*)arg;
  int (* func)(void*) = lcore_func->func;
  func(lcore_func->arg);
}

static struct DoubleChain* dchain;
static int alloc_inds[NUM_PARTITIONS][DCHAIN_SIZE];
// number of allocated index retrieved from dchain_test_allocation()
static int alloc_inds_size[NUM_PARTITIONS];
// number of allocated index retrieved from dchain_is_index_allocated()...
static int is_allocated_inds_size[NUM_PARTITIONS];

static int dchain_test_init() {
  return dchain_allocate(DCHAIN_SIZE, &dchain);
}

/* TODO: Improve this
 * Note: We assume all the tests that frees indexes of a partition
 * will free either none or all of its indexes, otherwise all the tests
 * will fail in weird ways...
 * This is hacky but I didn't get time to write a better one which
 * uses linked list to hold the allocated indexes...
 */

static int dchain_test_alloc_ind(void* arg) {
  // hack: wait for 10us to make sure other threads start which frees the inds
  // in case when all inds are allocated
  // otherwise the func returns immediately
  rte_delay_us_block(50);

  int partition = (int)arg;
  int i = alloc_inds_size[partition];
  while (dchain_allocate_new_index(dchain, &alloc_inds[partition][i], 0, partition))
    i++;
  alloc_inds_size[partition] = i;
  return 0;
}

static int dchain_test_free_ind(void* arg) {
  int partition = (int)arg;
  int cnt_freed = 0;
  for (int i = 0; i < alloc_inds_size[partition]; i++) {
    if (dchain_free_index(dchain, alloc_inds[partition][i], partition))
      cnt_freed++;
  }
  alloc_inds_size[partition] -= cnt_freed;
  return 0;
}

static int dchain_test_exp_ind(void* arg) {
  int partition = (int)arg;
  int index_out;
  while (dchain_expire_one_index(dchain, &index_out, 1, partition))
    alloc_inds_size[partition]--;
  return 0;
}

static int dchain_test_rejuvenate(void* arg) {
  int partition = (int)arg;
  for (int i = 0; i < alloc_inds_size[partition]; i++)
    dchain_rejuvenate_index(dchain, alloc_inds[partition][i], 2,
                            partition);
}

static int dchain_test_is_allocated(void* arg) {
  int partition = (int)arg;
  int alloc_ind_cnt = 0;
  for (int i = 0; i < alloc_inds_size[partition]; i++)
    alloc_ind_cnt += dchain_is_index_allocated(dchain, alloc_inds[partition][i],
                                               partition);
  is_allocated_inds_size[partition] = alloc_ind_cnt;
  return 0;
}

static int dchain_test_is_allocated_foreign_partition(void* arg) {
  rte_delay_us_block(50);

  int partition = (int)arg;
  int alloc_ind_cnt = 0;
  for (int i = 0; i < alloc_inds_size[NUM_PARTITIONS-1]; i++)
    alloc_ind_cnt += dchain_is_index_allocated(dchain, alloc_inds[NUM_PARTITIONS-1][i],
                                               partition);
  is_allocated_inds_size[partition] = alloc_ind_cnt;
  return 0;
}

// auxiliary func
static int dchain_num_allocated(int partition) {
  int alloc_ind_cnt = 0;
  for (int i = 0; i < alloc_inds_size[partition]; i++)
    alloc_ind_cnt += dchain_is_index_allocated(dchain, alloc_inds[partition][i],
                                               partition);
  return alloc_ind_cnt;
}

// --- Main ---

int main(int argc, char *argv[]) {
  // Initialize the Environment Abstraction Layer (EAL)
  int ret = rte_eal_init(argc, argv);
  if (ret < 0) {
    rte_exit(EXIT_FAILURE, "Error with EAL initialization, ret=%d\n", ret);
  }

  unsigned num_lcores = rte_lcore_count() - 1;
  struct lcore_func* lcore_funcs = calloc(num_lcores, sizeof(struct lcore_func) );

  int total_cnt_ind_alloc;
  int lcore;
  int partition;


  assert(num_lcores == NUM_PARTITIONS);
  // test dchain init
  assert(dchain_test_init());

  // test concurrent index alloc
  // all threads trying to allocate indexes
  // assumes correctness of dchain_num_allocated()
  for (lcore = 0; lcore < num_lcores; lcore++) {
    lcore_funcs[lcore].func = dchain_test_alloc_ind;
    lcore_funcs[lcore].arg = (void*)lcore;
  }
  
  partition = 0;
  RTE_LCORE_FOREACH_SLAVE(lcore) {
    rte_eal_remote_launch(lcore_entry, (void*)(&lcore_funcs[partition]), lcore);
    partition++;
  }
  rte_eal_mp_wait_lcore();

  total_cnt_ind_alloc = 0;
  for (int partition = 0; partition < NUM_PARTITIONS; partition++) {
    assert(dchain_num_allocated(partition) == alloc_inds_size[partition]);
    total_cnt_ind_alloc += alloc_inds_size[partition];
  }
  assert(total_cnt_ind_alloc == DCHAIN_SIZE);

  // test concurrent index free & alloc
  // first two threads free indexes
  // other threads allocate indexes 
  // assumes correctness of dchain_num_allocated()
  for (lcore = 0; lcore < num_lcores / 2; lcore++) {
    lcore_funcs[lcore].func = dchain_test_free_ind;
    lcore_funcs[lcore].arg = (void*)lcore;
  }
  for (; lcore < num_lcores; lcore++) {
    lcore_funcs[lcore].func = dchain_test_alloc_ind;
    lcore_funcs[lcore].arg = (void*)lcore;
  }

  partition = 0;
  RTE_LCORE_FOREACH_SLAVE(lcore) {
    rte_eal_remote_launch(lcore_entry, (void*)(&lcore_funcs[partition]), lcore);
    partition++;
  }
  rte_eal_mp_wait_lcore();

  for (int partition = 0; partition < NUM_PARTITIONS; partition++) {
    assert(dchain_num_allocated(partition) == alloc_inds_size[partition]);
  }

  total_cnt_ind_alloc = 0;
  for (int partition = 0; partition < NUM_PARTITIONS / 2; partition++)
    total_cnt_ind_alloc += alloc_inds_size[partition];
  assert(total_cnt_ind_alloc == 0);

  total_cnt_ind_alloc = 0;
  for (int partition = NUM_PARTITIONS / 2; partition < NUM_PARTITIONS; partition++)
    total_cnt_ind_alloc += alloc_inds_size[partition];
  assert(total_cnt_ind_alloc == DCHAIN_SIZE);

  // test concurrent index exp & alloc
  // first two threads allocate indexes
  // other threads exp indexes 
  // assumes correctness of dchain_num_allocated()
  for (lcore = 0; lcore < num_lcores / 2; lcore++) {
    lcore_funcs[lcore].func = dchain_test_alloc_ind;
    lcore_funcs[lcore].arg = (void*)lcore;
  }
  for (; lcore < num_lcores; lcore++) {
    lcore_funcs[lcore].func = dchain_test_exp_ind;
    lcore_funcs[lcore].arg = (void*)lcore;
  }

  partition = 0;
  RTE_LCORE_FOREACH_SLAVE(lcore) {
    rte_eal_remote_launch(lcore_entry, (void*)(&lcore_funcs[partition]), lcore);
    partition++;
  }
  rte_eal_mp_wait_lcore();

  // Rerun the allocation threads of current test in case allocation threads
  // finishes before expiration threads
  partition = 0;
  RTE_LCORE_FOREACH_SLAVE(lcore) {
    if (partition >= num_lcores / 2)
      rte_eal_remote_launch(lcore_entry, (void*)(&lcore_funcs[partition]), lcore);
    partition++;
  }
  rte_eal_mp_wait_lcore();

  for (int partition = 0; partition < NUM_PARTITIONS; partition++)
    assert(dchain_num_allocated(partition) == alloc_inds_size[partition]);

  total_cnt_ind_alloc = 0;
  for (int partition = 0; partition < NUM_PARTITIONS / 2; partition++)
    total_cnt_ind_alloc += alloc_inds_size[partition];
  assert(total_cnt_ind_alloc == DCHAIN_SIZE);

  total_cnt_ind_alloc = 0;
  for (int partition = NUM_PARTITIONS / 2; partition < NUM_PARTITIONS; partition++)
    total_cnt_ind_alloc += alloc_inds_size[partition];
  assert(total_cnt_ind_alloc == 0);

  // test concurrent index rejuvenate & free & alloc
  // first two threads allocate indexes
  // other threads exp indexes 
  // assumes correctness of dchain_num_allocated()
  for (lcore = 0; lcore < num_lcores / 2; lcore++) {
    lcore_funcs[lcore].func = dchain_test_free_ind;
    lcore_funcs[lcore].arg = (void*)lcore;
  }
  for (; lcore < num_lcores; lcore++) {
    lcore_funcs[lcore].func = dchain_test_alloc_ind;
    lcore_funcs[lcore].arg = (void*)lcore;
  }
  lcore_funcs[0].func = dchain_test_rejuvenate;

  partition = 0;
  RTE_LCORE_FOREACH_SLAVE(lcore) {
    rte_eal_remote_launch(lcore_entry, (void*)(&lcore_funcs[partition]), lcore);
    partition++;
  }
  rte_eal_mp_wait_lcore();

  // Rerun the allocation threads of current test in case allocation threads
  // finishes before expiration threads
  partition = 0;
  RTE_LCORE_FOREACH_SLAVE(lcore) {
    if (partition >= num_lcores / 2)
      rte_eal_remote_launch(lcore_entry, (void*)(&lcore_funcs[partition]), lcore);
    partition++;
  }
  rte_eal_mp_wait_lcore();

  int temp = alloc_inds_size[0];
  for (lcore = 0; lcore < num_lcores; lcore++) {
    lcore_funcs[lcore].func = dchain_test_exp_ind;
    lcore_funcs[lcore].arg = (void*)lcore;
  }

  partition = 0;
  RTE_LCORE_FOREACH_SLAVE(lcore) {
    rte_eal_remote_launch(lcore_entry, (void*)(&lcore_funcs[partition]), lcore);
    partition++;
  }
  rte_eal_mp_wait_lcore();

  assert(alloc_inds_size[0] == temp);

  // test concurrent index_is_allocated & exp & alloc
  // first two threads allocate indexes
  // other threads exp indexes 
  // assumes correctness of dchain_num_allocated()
  temp = alloc_inds_size[0];
  for (lcore = 0; lcore < num_lcores / 2; lcore++) {
    lcore_funcs[lcore].func = dchain_test_exp_ind;
    lcore_funcs[lcore].arg = (void*)lcore;
  }
  for (; lcore < num_lcores; lcore++) {
    lcore_funcs[lcore].func = dchain_test_alloc_ind;
    lcore_funcs[lcore].arg = (void*)lcore;
  }
  lcore_funcs[0].func = dchain_test_is_allocated;

  partition = 0;
  RTE_LCORE_FOREACH_SLAVE(lcore) {
    rte_eal_remote_launch(lcore_entry, (void*)(&lcore_funcs[partition]), lcore);
    partition++;
  }
  rte_eal_mp_wait_lcore();

  // Rerun the allocation threads of current test in case allocation threads
  // finishes before expiration threads
  partition = 0;
  RTE_LCORE_FOREACH_SLAVE(lcore) {
    if (partition >= num_lcores / 2)
      rte_eal_remote_launch(lcore_entry, (void*)(&lcore_funcs[partition]), lcore);
    partition++;
  }
  rte_eal_mp_wait_lcore();

  assert(is_allocated_inds_size[0] == temp);


  // test concurrent index_is_allocated_foriegn_partition & free & alloc
  // first thread try to check if index belonging to last partition is allocated
  // to the first partition
  // second thread allocates allocate indexes
  // other threads free indexes 
  // assumes correctness of dchain_num_allocated()
  for (lcore = 0; lcore < num_lcores / 2; lcore++) {
    lcore_funcs[lcore].func = dchain_test_alloc_ind;
    lcore_funcs[lcore].arg = (void*)lcore;
  }
  for (; lcore < num_lcores; lcore++) {
    lcore_funcs[lcore].func = dchain_test_free_ind;
    lcore_funcs[lcore].arg = (void*)lcore;
  }
  lcore_funcs[0].func = dchain_test_is_allocated_foreign_partition;

  partition = 0;
  RTE_LCORE_FOREACH_SLAVE(lcore) {
    rte_eal_remote_launch(lcore_entry, (void*)(&lcore_funcs[partition]), lcore);
    partition++;
  }
  rte_eal_mp_wait_lcore();

  // Rerun the allocation threads of current test in case allocation threads
  // finishes before expiration threads
  partition = 0;
  RTE_LCORE_FOREACH_SLAVE(lcore) {
    if ((partition < num_lcores / 2) && (partition != 0))
      rte_eal_remote_launch(lcore_entry, (void*)(&lcore_funcs[partition]), lcore);
    partition++;
  }
  rte_eal_mp_wait_lcore();

  assert(is_allocated_inds_size[0] == 0);


  printf("\nALL TESTS PASSED!\n");

  return 0;
}
