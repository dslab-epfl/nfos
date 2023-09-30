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

#include "map.h"
#include "rlu-wrapper.h"

#define MAP_SIZE 128
#define NUM_ITERS 10000

struct element {
  int a;
  int b;
};

static struct NfosMap *map;

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

static bool element_eq(void *obj1, void *obj2) {
  struct element *elem1 = (struct element *)obj1;
  struct element *elem2 = (struct element *)obj2;
  return (elem1->a == elem2->a) && (elem1->b == elem2->b);
}

static unsigned int element_hash(void *obj) {
  struct element *elem = (struct element *)obj;
  unsigned hash = 0;
  hash = __builtin_ia32_crc32si(hash, elem->a);
  hash = __builtin_ia32_crc32si(hash, elem->b);
  return hash;
}

static int nfos_map_test(void* unused) {
  rlu_thread_data_t *rlu_data = get_rlu_thread_data();
  int thread_id = get_rlu_thread_id();
  int num_cores = rte_lcore_count() - 1;

  int offset = thread_id * (MAP_SIZE / num_cores);
  int stride = MAP_SIZE / num_cores;

  for (int i = 0; i < NUM_ITERS; i++) {
    for (int k = offset; k < offset + stride; k++) {
retry1:
      RLU_READER_LOCK(rlu_data);
      struct element key = {.a = k, .b = k};
      if (nfos_map_put(map, &key, k) == ABORT_HANDLER) {
        RLU_ABORT(rlu_data);
        goto retry1;
      }
      RLU_READER_UNLOCK(rlu_data);
    }

    for (int k = offset; k < offset + stride; k++) {
retry2:
      RLU_READER_LOCK(rlu_data);
      struct element key = {.a = k, .b = k};
      if (nfos_map_erase(map, &key) == ABORT_HANDLER) {
        RLU_ABORT(rlu_data);
        goto retry2;
      }
      RLU_READER_UNLOCK(rlu_data);
    }
  }

  for (int k = offset; k < offset + stride; k++) {
retry3:
    RLU_READER_LOCK(rlu_data);
    struct element key = {.a = k, .b = k};
    if (nfos_map_put(map, &key, k) == ABORT_HANDLER) {
      RLU_ABORT(rlu_data);
      goto retry3;
    }
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
  nfos_map_allocate(element_eq, element_hash, sizeof(struct element), MAP_SIZE, &map);

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
    lcore_funcs[lcore] = nfos_map_test;
  }
  lcore_funcs[lcore] = idle_main;
  
  printf("Threads started\n");
  rte_eal_mp_remote_launch(lcore_entry, (void*)lcore_funcs, CALL_MASTER);
  rte_eal_mp_wait_lcore();

  // Check results
  printf("Threads finished\n");
  rlu_thread_data_t *rlu_data = get_rlu_thread_data();
  for (int i = 0; i < MAP_SIZE; i++) {
    RLU_READER_LOCK(rlu_data);
    struct element elem = {.a = i, .b = i};
    int value;
    nfos_map_get(map, &elem, &value);
    printf("key: [%d %d] val: %d\n", elem.a, elem.b, value);
    RLU_READER_UNLOCK(rlu_data);
  }

  // FINI RLU
  for (int i = 0; i < num_lcores; i++) {
    RLU_THREAD_FINISH(rlu_threads_data[i]);
  }
  RLU_FINISH();

  return 0;
}
