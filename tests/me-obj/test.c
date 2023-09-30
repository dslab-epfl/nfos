#include <inttypes.h>
// DPDK uses these but doesn't include them. :|
#include <linux/limits.h>
#include <sys/types.h>
#include <unistd.h>

#include <stdlib.h>
#include <signal.h>

#include <rte_common.h>
#include <rte_eal.h>
#include <rte_lcore.h>
#include <rte_cycles.h>

#include "mergeable-obj.h"
#include "rlu-wrapper.h"

// #define ENABLE_LOG
// #define SIGTERM_HANDLING
#include "vigor/nf-log.h"

#define NUM_ITERS 10000

/* Mergeable object: A pair of counters */

#define INIT_A 0
#define INIT_B (-2)
#define INCREMENT_A 1
#define INCREMENT_B 2

struct counter {
  int a;
  int b;
};

static void obj_init(void *obj) {
  struct counter *cnt = (struct counter *)obj;
  cnt->a = INIT_A;
  cnt->b = INIT_B;
}

static void obj_update(void *replica) {
  struct counter *cnt = (struct counter *)replica;
  cnt->a += INCREMENT_A;
  cnt->b += INCREMENT_B;
}

static void obj_merge(void *obj, void *replica) {
  struct counter *cnt = (struct counter *)obj;
  struct counter *cnt_rep = (struct counter *)replica;
  cnt->a += cnt_rep->a - INIT_A;
  cnt->b += cnt_rep->b - INIT_B;
}

static struct NfosMeObj *me_obj;


/* test function */

static int nfos_me_obj_test(void* unused) {
  rlu_thread_data_t *rlu_data = get_rlu_thread_data();

  for (int i = 0; i < NUM_ITERS; i++) {
retry:
    RLU_READER_LOCK(rlu_data);

    if (nfos_me_obj_update(me_obj, obj_update) == ABORT_HANDLER) {
      RLU_ABORT(rlu_data);
      // TODO (bug): The test will fail if I change the next line to use printf...
      NF_DEBUG("Abort update [%d] %d", get_rlu_thread_id(), i);
      goto retry;
    }

    struct counter *cnt;
    if (nfos_me_obj_read(me_obj, rte_get_tsc_cycles(), obj_init, obj_merge, (void **)&cnt) == ABORT_HANDLER) {
      RLU_ABORT(rlu_data);
      NF_DEBUG("Abort read [%d] %d", get_rlu_thread_id(), i);
      goto retry;
    }
    NF_DEBUG("%d %d\n", cnt->a, cnt->b);

    if (!RLU_READER_UNLOCK(rlu_data)) {
      RLU_ABORT(rlu_data);
      NF_DEBUG("ABORT: read validation");
      goto retry;
    }
  }

  return 0;
}

// Default placeholder idle task
static int idle_main(void* unused) {
  return 0;
}


/* Ugly thread stuff */

// TODO: Ugly to define those here, should have a rlu_wrapper.c
// RLU per thread data
rlu_thread_data_t **rlu_threads_data;
RTE_DEFINE_PER_LCORE(int, rlu_thread_id);

// entry function to various NF threads, a bit ugly...
static int lcore_entry(void* arg) {
  int lcore_ind = rte_lcore_index(-1);

  // Used for threads to locate the local rlu data
  RTE_PER_LCORE(rlu_thread_id) = lcore_ind;

#ifdef ENABLE_LOG
  // Init index of the log of current processing core
  RTE_PER_LCORE(_core_ind) = lcore_ind;
#endif

  int (** lcore_funcs)(void*) = (int (**)(void*))arg;
  int (* lcore_func)(void*) = lcore_funcs[lcore_ind];
  lcore_func(NULL);
}


/* Debugging */
#ifdef SIGTERM_HANDLING
static void signal_handler(int signum) {
#ifdef SCALABILITY_PROFILER
  // Print mvrlu stats
  for (int i = 0; i < rte_lcore_count(); i++)
    mvrlu_merge_stats(rlu_threads_data[i]);
  RLU_PRINT_STATS();

  profiler_show_profile();
#endif

#ifdef ENABLE_LOG
  // flush and clean up logs
  logging_fini(rte_lcore_count());
#endif
  /* exit with the expected status */
  signal(signum, SIG_DFL);
  kill(getpid(), signum);
}
#endif


int main(int argc, char *argv[]) {

#ifdef SIGTERM_HANDLING
  // For debugging
  signal(SIGINT, signal_handler);
  signal(SIGTERM, signal_handler);
#endif

  // Initialize the Environment Abstraction Layer (EAL)
  int ret = rte_eal_init(argc, argv);
  if (ret < 0) {
    rte_exit(EXIT_FAILURE, "Error with EAL initialization, ret=%d\n", ret);
  }

#ifdef ENABLE_LOG
  // init logging
  logging_init(rte_lcore_count());
  // init log index of the master core
  RTE_PER_LCORE(_core_ind) = rte_lcore_index(-1);
#endif

  unsigned num_lcores = rte_lcore_count();
  int (** lcore_funcs)(void*) = calloc(num_lcores, sizeof(int (*)(void*)) );

  // Init DS
  nfos_me_obj_allocate(sizeof(struct counter), 5, obj_init, &me_obj);

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
    lcore_funcs[lcore] = nfos_me_obj_test;
  }
  lcore_funcs[lcore] = idle_main;
  
  NF_DEBUG("Threads started");
  rte_eal_mp_remote_launch(lcore_entry, (void*)lcore_funcs, CALL_MASTER);
  rte_eal_mp_wait_lcore();

  // Check results
  NF_DEBUG("Threads finished");
  rlu_thread_data_t *rlu_data = get_rlu_thread_data();
  RLU_READER_LOCK(rlu_data);
  struct counter *cnt;
  nfos_me_obj_read(me_obj, rte_get_tsc_cycles(), obj_init, obj_merge, (void **)&cnt);
  printf("Counter: %d %d\n", cnt->a, cnt->b);
  RLU_READER_UNLOCK(rlu_data);

  // FINI RLU
  for (int i = 0; i < num_lcores; i++) {
    RLU_THREAD_FINISH(rlu_threads_data[i]);
  }
  RLU_FINISH();

  return 0;
}
