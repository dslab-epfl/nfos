#include <inttypes.h>
// DPDK uses these but doesn't include them. :|
#include <linux/limits.h>
#include <sys/types.h>
#include <unistd.h>

#include <stdlib.h>
#include <stdio.h>

#include <rte_common.h>
#include <rte_eal.h>
#include <rte_lcore.h>

#include "vector.h"
#include "rlu-wrapper.h"

#define VECTOR_SIZE 3
#define NUM_ITERS 10000000

struct element {
  int a;
  int b;
};

static struct NfosVector *vector;

// TODO: Ugly to define those here, should have a rlu_wrapper.c
// RLU per thread data
rlu_thread_data_t **rlu_threads_data;
RTE_DEFINE_PER_LCORE(int, rlu_thread_id);

// entry function to various NF threads, a bit ugly...
static int lcore_entry(void* arg) {
  int lcore_ind = rte_lcore_index(-1);

  // Used for threads to locate the local rlu data
  RTE_PER_LCORE(rlu_thread_id) = lcore_ind;

  int (** lcore_funcs)(void*) = (int (**)(void*))arg;
  int (* lcore_func)(void*) = lcore_funcs[lcore_ind];
  lcore_func(NULL);
}

static void elem_init(void *obj) {
  struct element *elem = (struct element *)obj;
  elem->a = 0;
  elem->b = 0;
}

static int nfos_vector_test(void* unused) {
  rlu_thread_data_t *rlu_data = get_rlu_thread_data();

  for (int i = 0; i < NUM_ITERS; i++) {
retry:
    RLU_READER_LOCK(rlu_data);
    int index = 1;
    struct element *elem;
    // nfos_vector_borrow(vector, index, (void **)&elem);
    // int new_a = elem->a + 1;
    // int new_b = elem->b + 2;
    if (nfos_vector_borrow_mut(vector, index, (void **)&elem) == ABORT_HANDLER) {
      RLU_ABORT(rlu_data);
      goto retry;
    }
    elem->a++;
    elem->b++;
    // printf("%d %d\n", elem->a, elem->b);
    RLU_READER_UNLOCK(rlu_data);
  }

  return 0;
}

// Default placeholder idle task
static int idle_main(void* unused) {
  return 0;
}

int main(int argc, char *argv[]) {
  // Initialize the Environment Abstraction Layer (EAL)
  int ret = rte_eal_init(argc, argv);
  if (ret < 0) {
    rte_exit(EXIT_FAILURE, "Error with EAL initialization, ret=%d\n", ret);
  }

  unsigned num_lcores = rte_lcore_count();
  int (** lcore_funcs)(void*) = calloc(num_lcores, sizeof(int (*)(void*)) );

  // Init DS
  nfos_vector_allocate(sizeof(struct element), VECTOR_SIZE, elem_init, &vector);
  // Check results
  for (int i = 0; i < VECTOR_SIZE; i++) {
    struct element *elem;
    nfos_vector_borrow_unsafe(vector, i, (void **)&elem);
    printf("vector[%d]: %d %d\n", i, elem->a, elem->b);
  }

  // Init RLU
  // Hacked the mv-rlu lib to put the gp_thread to the last isolated core: 46 on icdslab[5-8].epfl.ch
  RLU_INIT(46);
  // Init (mv-)RLU per-thread data
  rlu_threads_data = malloc(num_lcores * sizeof(rlu_thread_data_t *));
  for (int i = 0; i < num_lcores; i++) {
    rlu_threads_data[i] = RLU_THREAD_ALLOC();
	  RLU_THREAD_INIT(rlu_threads_data[i]);
  }

  // Test
  // This is ugly, some data structures assume the last core is not used
  int lcore;
  for (lcore = 0; lcore < num_lcores - 1; lcore++) {
    lcore_funcs[lcore] = nfos_vector_test;
  }
  lcore_funcs[lcore] = idle_main;
  
  printf("Threads started\n");
  rte_eal_mp_remote_launch(lcore_entry, (void*)lcore_funcs, CALL_MASTER);
  rte_eal_mp_wait_lcore();

  // Check results
  printf("Threads finished\n");
  rlu_thread_data_t *rlu_data = get_rlu_thread_data();
  RLU_READER_LOCK(rlu_data);
  for (int i = 0; i < VECTOR_SIZE; i++) {
    struct element *elem;
    nfos_vector_borrow(vector, i, (void **)&elem);
    printf("vector[%d]: %d %d\n", i, elem->a, elem->b);
  }
  RLU_READER_UNLOCK(rlu_data);

  // FINI RLU
  for (int i = 0; i < num_lcores; i++) {
    RLU_THREAD_FINISH(rlu_threads_data[i]);
  }
  RLU_FINISH();

  return 0;
}
