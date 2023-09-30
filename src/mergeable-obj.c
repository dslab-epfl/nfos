#include "mergeable-obj.h"

#include <stdlib.h>
#include <stdint.h>

#include <rte_lcore.h>
#include <rte_malloc.h>

#include "timer.h"
#include "rlu-wrapper.h"

typedef struct {
  vigor_time_t ts;
  unsigned char val[0];
} obj_merged_val_t;

struct NfosMeObj {
  obj_merged_val_t **merged_val_caches;
  void **replicas;
  int obj_size;
  vigor_time_t staleness;
};

int nfos_me_obj_allocate_t(int obj_size, int64_t staleness_usec, nfos_me_obj_init_t init_obj,
                           struct NfosMeObj **me_obj_out) {
  struct NfosMeObj *me_obj = (struct NfosMeObj *) rte_malloc(NULL, sizeof(struct NfosMeObj), 0);
  if (!me_obj) return 0;

  me_obj->obj_size = obj_size;

  me_obj->staleness = nfos_usec_to_tsc_cycles(staleness_usec);
  
  // one replica and merged val cache per worker core
  int num_replicas = rte_lcore_count() - 1;
  me_obj->replicas = (void **) rte_malloc(NULL, sizeof(void *) * num_replicas, 0);
  if (!me_obj->replicas) {
    free(me_obj);
    return 0;
  }
  me_obj->merged_val_caches = (obj_merged_val_t **) rte_malloc(NULL, sizeof(obj_merged_val_t *) * num_replicas, 0);
  if (!me_obj->merged_val_caches) {
    free(me_obj);
    return 0;
  }
  for (int i = 0; i < num_replicas; i++) {
    me_obj->replicas[i] = RLU_ALLOC(obj_size);
    init_obj(me_obj->replicas[i]);

    posix_memalign((void **)(me_obj->merged_val_caches + i), 64, sizeof(obj_merged_val_t) + obj_size);
    me_obj->merged_val_caches[i]->ts = 0;
    init_obj((void *)(me_obj->merged_val_caches[i]->val));
  }

  *me_obj_out = me_obj;
  return 1;
}

int nfos_me_obj_update_t(struct NfosMeObj *me_obj, nfos_me_obj_update_handler_t update_obj) {
  rlu_thread_data_t *rlu_data = get_rlu_thread_data();
  int replica_id = get_rlu_thread_id();
  void *replica = me_obj->replicas[replica_id];

  // TODO: can we avoid try_lock here?
  if (!_mvrlu_try_lock(rlu_data, &replica, me_obj->obj_size)) {
    return ABORT_HANDLER;
  } else {
    update_obj(replica);
    return 1;
  }
}

// TODO: embed ts in obj to reduce number of try_locks
int nfos_me_obj_read_t(struct NfosMeObj *me_obj, vigor_time_t curr_ts, nfos_me_obj_init_t init_obj,
                       nfos_me_obj_merge_t merge_obj, void **obj_out) {
  rlu_thread_data_t *rlu_data = get_rlu_thread_data();
  int merged_val_cache_id = get_rlu_thread_id();

  void *obj = me_obj->merged_val_caches[merged_val_cache_id]->val;

  vigor_time_t *obj_ts = &(me_obj->merged_val_caches[merged_val_cache_id]->ts);
  if (curr_ts - *obj_ts >= me_obj->staleness) {
    // reset main object
    init_obj(obj);
    // merge replicas
    int num_replicas = rte_lcore_count() - 1;
    for (int i = 0; i < num_replicas; i++) {
      void *replica = RLU_DEREF(rlu_data, me_obj->replicas[i]);
      merge_obj(obj, replica);
    }

    // Update ts
    *obj_ts = curr_ts;
  }

  *obj_out = obj;
  return 1;
}

int nfos_me_obj_allocate_debug_t(int obj_size, int64_t staleness_usec, nfos_me_obj_init_t init_obj,
                                 struct NfosMeObj **me_obj_out, const char *filename, int lineno) {
  int ret = nfos_me_obj_allocate_t(obj_size, staleness_usec, init_obj, me_obj_out);
  profiler_add_ds_inst((void *)(*me_obj_out), filename, lineno);
  return ret;
}

int nfos_me_obj_read_debug_t(struct NfosMeObj *me_obj, vigor_time_t curr_ts, nfos_me_obj_init_t init_obj,
                             nfos_me_obj_merge_t merge_obj, void **obj_out) {
  profiler_add_ds_op_info(me_obj, 1, 0, &curr_ts, sizeof(vigor_time_t));

  int ret = nfos_me_obj_read_t(me_obj, curr_ts, init_obj, merge_obj, obj_out);

  profiler_inc_curr_op();
  return ret;
}

int nfos_me_obj_update_debug_t(struct NfosMeObj *me_obj, nfos_me_obj_update_handler_t update_obj) {
  profiler_add_ds_op_info(me_obj, 1, 1, NULL, 0);

  int ret = nfos_me_obj_update_t(me_obj, update_obj);

  profiler_inc_curr_op();
  return ret;
}
