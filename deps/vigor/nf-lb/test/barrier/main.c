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

#include "nf-lb/barrier.h"

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

static int cnt = 0;

int waiter_thread(void* unused) {
  uint64_t timer_start = rte_get_tsc_cycles(); 
  for (int i = 0; i < 1000000; i++) {
    barrier_wait();
    barrier_wait();
    assert(cnt == i+1);
  }
  printf("%ld\n", rte_get_tsc_cycles() - timer_start);
  return 0;
}

int master_thread(void* unused) {
  uint64_t timer_start = rte_get_tsc_cycles(); 
  for (int i = 0; i < 1000000; i++) {
    barrier_wait();
    cnt++;
    barrier_wait();
  }
  printf("%ld\n", rte_get_tsc_cycles() - timer_start); 
  return 0;
}

static volatile int flag = 0;

int waiter_thread_new(void* unused) {
  barrier_wait();

  while (1) {
    if (flag) {
      barrier_wait();
      barrier_wait();
      assert(cnt == 1);
      printf("%d\n", cnt);
      break;
    }
  }
}

int master_thread_new(void* unused) {
  barrier_wait();

  uint64_t timer_start = rte_get_tsc_cycles();
  flag = 1;
  barrier_wait();
  cnt++;
  flag = 0;
  barrier_wait();
  printf("%ld\n", rte_get_tsc_cycles() - timer_start);
  return 0;
}

// --- Main ---

int main(int argc, char *argv[]) {
  // Initialize the Environment Abstraction Layer (EAL)
  int ret = rte_eal_init(argc, argv);
  if (ret < 0) {
    rte_exit(EXIT_FAILURE, "Error with EAL initialization, ret=%d\n", ret);
  }

  unsigned num_lcores = rte_lcore_count() - 1;
  barrier_init(num_lcores);
  struct lcore_func* lcore_funcs = calloc(num_lcores, sizeof(struct lcore_func) );

  int lcore, partition;

  // test 1
  for (lcore = 0; lcore < num_lcores - 1; lcore++) {
    lcore_funcs[lcore].func = waiter_thread; 
    lcore_funcs[lcore].arg = NULL;
  }
  lcore_funcs[lcore].func = master_thread;
  lcore_funcs[lcore].arg = NULL;
  
  partition = 0;
  RTE_LCORE_FOREACH_SLAVE(lcore) {
    rte_eal_remote_launch(lcore_entry, (void*)(&lcore_funcs[partition]), lcore);
    partition++;
  }
  rte_eal_mp_wait_lcore();

  // test 2
  cnt = 0;
  for (lcore = 0; lcore < num_lcores - 1; lcore++) {
    lcore_funcs[lcore].func = waiter_thread_new; 
    lcore_funcs[lcore].arg = NULL;
  }
  lcore_funcs[lcore].func = master_thread_new;
  lcore_funcs[lcore].arg = NULL;
  
  partition = 0;
  RTE_LCORE_FOREACH_SLAVE(lcore) {
    rte_eal_remote_launch(lcore_entry, (void*)(&lcore_funcs[partition]), lcore);
    partition++;
  }
  rte_eal_mp_wait_lcore();

  printf("\nALL TESTS PASSED!\n");

  return 0;
}
