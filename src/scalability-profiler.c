#include "scalability-profiler.h"

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <stdbool.h>
#include <assert.h>

#include <rte_malloc.h>

#include "rlu-wrapper.h"
#include "vigor/nf-log.h"
#include "nf.h"

// temp hack map
#define CCM_SIZE 10
#define DSM_SIZE 10


struct ds_op_info {
    uint64_t txn_ts;
    void *ds_inst;
    uint16_t ds_id;
    uint16_t ds_op_id;
    uint16_t thr_id;
    uint8_t args[DS_OP_ARG_LENGTH];
    uint8_t args_len;
};

typedef struct ds_op_cache {
    int thr_id;
    struct ds_op_info op_infos[DS_OP_INFO_CACHE_SIZE];
} __attribute__ ((aligned (64))) ds_op_cache_t;

struct abort_info {
    void *ds_inst;
    uint16_t ds_id;
    uint16_t ds_op_id;
    uint16_t thr_id;
    uint16_t conflict_ds_op_id;
    uint16_t conflict_thr_id;
    uint8_t args[DS_OP_ARG_LENGTH];
    uint8_t conflict_args[DS_OP_ARG_LENGTH];
    uint64_t txn_ts;
    uint64_t conflict_txn_ts;
};

typedef struct abort_info_db {
    uint64_t num_aborts;
    int num_samples;
    int num_false_abort_samples;
    int num_missing_samples;
    struct abort_info *abort_infos;
} __attribute__ ((aligned (64))) abort_info_db_t;

struct conflict_cause {
    void *ds_inst;
    uint16_t ds_id;
    uint16_t ds_op_id;
    uint16_t conflict_ds_op_id;
    int16_t id;
};

// TODO: use a real map, not array
struct conflict_cause_entry {
    struct conflict_cause key;
    uint64_t num_samples;
};

struct conflict_cause_map {
    int num_conflict_causes;
    struct conflict_cause_entry conflict_causes[CCM_SIZE];
};

struct ds_inst_entry {
    void *addr;
    const char *filename;
    int lineno;
};

struct ds_inst_map {
    int num_ds_insts;
    struct ds_inst_entry ds_insts[DSM_SIZE];
};

uint64_aligned_t *pkt_cnts;
static int num_cores;
static ds_op_cache_t *ds_op_caches;
static abort_info_db_t *abort_info_dbs;
// map from data structure instance to where it is initialized
static struct ds_inst_map *di_map;
// TODO: let each data structure register the names.
static char *ds_names[5] = {"resource allocator", "mergeable object", "map", "vector", "resource allocator"};
static char *op_names[5][4] = {{"alloc", "refresh", "undefined", "undefined"},
                               {"read", "update", "undefined", "undefined"},
                               {"put", "erase", "get", "undefined"},
                               {"write", "read", "undefined", "undefined"},
                               {"alloc", "free", "undefined", "undefined"}};


static bool conflict_cause_equal(struct conflict_cause *a, struct conflict_cause *b){
    return a->ds_inst == b->ds_inst &&
    a->ds_id == b->ds_id &&
    a->ds_op_id == b->ds_op_id &&
    a->conflict_ds_op_id == b->conflict_ds_op_id &&
    a->id == b->id;
}

static void conflict_cause_map_add_sample(struct conflict_cause_map *ccm, struct conflict_cause *key) {
    int i = 0;
    while (i < ccm->num_conflict_causes) {
        struct conflict_cause_entry *cc_entry = &(ccm->conflict_causes[i]);
        if (conflict_cause_equal(key, &(cc_entry->key))) {
            cc_entry->num_samples++;
            return;
        }
        i++; 
    }

    ccm->num_conflict_causes++;
    assert(ccm->num_conflict_causes <= CCM_SIZE);

    struct conflict_cause_entry *cc_entry = &(ccm->conflict_causes[i]);
    cc_entry->key = *key;
    cc_entry->num_samples = 1;
    return;
}

static int cc_compar(const void *a, const void *b) {
    struct conflict_cause_entry *cc_a = (struct conflict_cause_entry *)a;
    struct conflict_cause_entry *cc_b = (struct conflict_cause_entry *)b;
    // sort in descending order
    if (cc_a->num_samples > cc_b->num_samples) {
        return -1; 
    } else if (cc_a->num_samples == cc_b->num_samples) {
        return 0;
    } else {
        return 1;
    }
}

static void conflict_cause_map_sort(struct conflict_cause_map *ccm) {
    qsort(ccm->conflict_causes, ccm->num_conflict_causes, sizeof(struct conflict_cause_entry), cc_compar);
}

static void ds_inst_map_put(struct ds_inst_map *dim, struct ds_inst_entry *kvp) {
    int i = 0;
    while (i < dim->num_ds_insts) {
        struct ds_inst_entry *di_entry = &(dim->ds_insts[i]);
        if (kvp->addr == di_entry->addr) {
            return;
        }
        i++;
    }

    dim->num_ds_insts++;
    assert(dim->num_ds_insts <= DSM_SIZE);

    struct ds_inst_entry *di_entry = &(dim->ds_insts[i]);
    *di_entry = *kvp;
    return;
}

static struct ds_inst_entry *ds_inst_map_get(struct ds_inst_map *dim, void *key) {
    int i = 0;
    while (i < dim->num_ds_insts) {
        struct ds_inst_entry *di_entry = &(dim->ds_insts[i]);
        if (key == di_entry->addr) {
            return di_entry;
        }
        i++;
    }
    return NULL;
}

static int16_t get_conflict_cause_id_fast_path(uint16_t ds_id, uint16_t op, uint16_t conflict_op) {
    // By default only one type of conflict cause for an op pair
    // cc_id = -1 indicates that slow path is needed.
    int16_t cc_id = 0;
    switch (ds_id) {
        case 0:
            if (op == 0 && conflict_op == 1)
                cc_id = -1;
            if (op == 1 && conflict_op == 0)
                cc_id = -1;
            break;
        case 2:
            if (op == 2 && conflict_op == 2)
                cc_id = 0;
            else
                cc_id = -1;
            break;
        case 1:
        case 3:
        case 4:
            break;
    }

    return cc_id;
}
// static int get_conflict_cause_id_slow_path(struct abort_info *abort) {}

// TODO: use a map to store conflict cause to recipe mappings
static char *get_recipe(struct conflict_cause *cc) {
    uint16_t ds_id = cc->ds_id;
    uint16_t op = cc->ds_op_id; 
    uint16_t conflict_op = cc->conflict_ds_op_id;
    int16_t cc_id = cc->id;

    char *recipe = "placeholder";
    if (cc_id == -1) return recipe;

    switch (ds_id) {
        case 0:
            if (op == 0 && conflict_op == 0) {
                recipe = "Overprovision resource";
            } else if (op == 1 && conflict_op == 1) {
                recipe = "Increase resource refreshing interval";
            } else if ((op == 0 && conflict_op == 1) || (op == 1 && conflict_op == 0)) {
                if (cc_id == 0) {
                    recipe = "Increase resource refreshing interval";
                } else {
                    recipe = "Overprovision resource";
                }
            }
            break;
        case 1:
            recipe = "Increase maximum allowed staleness";
            break;
        case 2:
            if (cc_id = 0)
                recipe = "Use mergeable object and increase maximum allowed staleness";
            else
                recipe = "Increase map size or change hash function that reduces collision";
            break;
        case 3:
            recipe = "Use mergeable object and increase maximum allowed staleness";
            break;
        case 4:
            if (op == 0 && conflict_op == 0) {
                recipe = "Overprovision resource";
            }
            break;
    }

    return recipe;
}

#ifdef DCHAIN_EXP_LEGACY
static void conflict_cause_map_merge_dchain_exp_legacy(struct conflict_cause_map *ccm) {
    int cc_merge_base;
    for (int i = 0; i < ccm->num_conflict_causes; i++) {
        struct conflict_cause_entry *cc_entry = &(ccm->conflict_causes[i]);
        uint16_t ds_id = cc_entry->key.ds_id;
        uint16_t op_id = cc_entry->key.ds_op_id;
        uint16_t conflict_op_id = cc_entry->key.conflict_ds_op_id;
        if (ds_id == 0 && op_id == 1 && conflict_op_id == 1) {
            cc_merge_base = i;
            break;
        }
    }
    for (int i = 0; i < ccm->num_conflict_causes; i++) {
        struct conflict_cause_entry *cc_entry = &(ccm->conflict_causes[i]);
        uint16_t ds_id = cc_entry->key.ds_id;
        if (ds_id == 3)
            ccm->conflict_causes[cc_merge_base].num_samples += cc_entry->num_samples;
    }

    struct conflict_cause_entry conflict_causes_copy[CCM_SIZE];
    int size_conflict_causes_copy = 0;
    for (int i = 0; i < ccm->num_conflict_causes; i++) {
        struct conflict_cause_entry *cc_entry = &(ccm->conflict_causes[i]);
        uint16_t ds_id = cc_entry->key.ds_id;
        if (ds_id != 3) {
            conflict_causes_copy[size_conflict_causes_copy] = *cc_entry;
            size_conflict_causes_copy++;
        }
    }
    ccm->num_conflict_causes = size_conflict_causes_copy;
    memcpy(ccm->conflict_causes, conflict_causes_copy,
        size_conflict_causes_copy * sizeof(struct conflict_cause_entry));
}
#endif

static void conflict_cause_map_show(struct conflict_cause_map *ccm) {
    unsigned long total_txns = mvrlu_profiler_get_total_txns();
    NF_PROFILE("Total txs: %ld", total_txns);
    uint64_t total_num_samples = 0;
    for (int i = 0; i < ccm->num_conflict_causes; i++) {
        struct conflict_cause_entry *cc_entry = &(ccm->conflict_causes[i]);
        total_num_samples += cc_entry->num_samples;
    }
    uint64_t total_aborts = total_num_samples * ABORT_INFO_SAMPLING_RATE;
    NF_PROFILE("Total tx aborts: %ld", total_aborts);
    float abort_ratio = (100.0 * (float) total_aborts) / (float) total_txns;
    NF_PROFILE("Total tx abort ratio: %.1f%%", abort_ratio);

    NF_PROFILE("===");

    NF_PROFILE("Bottlenecks:");
    for (int i = 0; i < ccm->num_conflict_causes; i++) {
        struct conflict_cause_entry *cc_entry = &(ccm->conflict_causes[i]);
        void *ds_inst = cc_entry->key.ds_inst;
        uint16_t ds_id = cc_entry->key.ds_id;
        uint16_t op_id = cc_entry->key.ds_op_id;
        uint16_t conflict_op_id = cc_entry->key.conflict_ds_op_id;
        int16_t cc_id = cc_entry->key.id;
        uint64_t num_aborts = cc_entry->num_samples * ABORT_INFO_SAMPLING_RATE;
        float ratio = (100.0 * (float) num_aborts) / (float) total_aborts;
        if (ratio < 1.0) {
            NF_PROFILE("");
            NF_PROFILE("Not showing the rest of bottlenecks which cause less than 1%% of the aborts");
            return;
        }
        struct ds_inst_entry *di_entry = ds_inst_map_get(di_map, ds_inst);

        char *ds_name = ds_names[ds_id];
        const char *ds_inst_filename = di_entry->filename;
        int ds_inst_lineno = di_entry->lineno;
        char *op = op_names[ds_id][op_id];
        char *conflict_op = op_names[ds_id][conflict_op_id];
        char *recipe = get_recipe(&(cc_entry->key));

        NF_PROFILE("");
        NF_PROFILE("[%d] %s initialized at %s:L%d, addr:%p", i+1, ds_name,
                    ds_inst_filename, ds_inst_lineno, ds_inst);
        NF_PROFILE("conflict cause <%s, %s>#%d", op, conflict_op, cc_id+1);
        NF_PROFILE("tx aborts: %ld %.2f%%", num_aborts, ratio);
        NF_PROFILE("Recipe: %s", recipe);
    }
}

void profiler_init(int _num_cores) {
    num_cores = _num_cores;
    ds_op_caches = calloc(num_cores, sizeof(ds_op_cache_t));
    for (int i = 0; i < num_cores; i++)
        ds_op_caches[i].thr_id = i;
    
    abort_info_dbs = calloc(num_cores, sizeof(abort_info_db_t));
    for (int i = 0; i < num_cores; i++)
        abort_info_dbs[i].abort_infos = rte_calloc(NULL, ABORT_INFO_DB_SIZE, sizeof(struct abort_info), 0);

    di_map = calloc(1, sizeof(struct ds_inst_map));
}

int profiler_add_ds_op_info(void *ds_inst, uint16_t ds_id, uint16_t ds_op_id, void *args, size_t arg_length) {
    int thr_id = get_rlu_thread_id();
    ds_op_cache_t *ds_op_cache = ds_op_caches + thr_id;

    uint64_t txn_ts;
    uint16_t curr_op = mvrlu_profiler_get_curr_op_and_txn_ts(get_rlu_thread_data(), &txn_ts);

    struct ds_op_info *op_infos = ds_op_cache->op_infos;
    op_infos[curr_op].txn_ts = txn_ts;
    op_infos[curr_op].ds_inst = ds_inst;
    op_infos[curr_op].ds_id = ds_id;
    op_infos[curr_op].ds_op_id = ds_op_id;
    op_infos[curr_op].thr_id = thr_id;

    if (arg_length) {
        memcpy(op_infos[curr_op].args, args, arg_length);
        op_infos[curr_op].args_len = arg_length;
    }
}

void profiler_show_profile() {
    struct conflict_cause_map *ccm = calloc(1, sizeof(struct conflict_cause_map));
    for (int i = 0; i < num_cores; i++) {
        abort_info_db_t *abort_info_db = abort_info_dbs + i;
        struct abort_info *abort_infos = abort_info_db->abort_infos;
        for (int j = 0; j < abort_info_db->num_samples; j++) {
            struct conflict_cause cc = {
                .ds_inst = abort_infos[j].ds_inst,
                .ds_id = abort_infos[j].ds_id,
                .ds_op_id = abort_infos[j].ds_op_id,
                .conflict_ds_op_id = abort_infos[j].conflict_ds_op_id
            };
            cc.id = get_conflict_cause_id_fast_path(cc.ds_id, cc.ds_op_id, cc.conflict_ds_op_id); 
            conflict_cause_map_add_sample(ccm, &cc);
        }
    }
#ifdef DCHAIN_EXP_LEGACY
    conflict_cause_map_merge_dchain_exp_legacy(ccm);
#endif
    conflict_cause_map_sort(ccm);
    conflict_cause_map_show(ccm);

    NF_PROFILE("===");
    NF_PROFILE("Load imbalance factor: %f (Define finer-grained packet sets if above 1.5)", profiler_get_load_imbalance());
}

static inline void add_abort_info_sample(abort_info_db_t *abort_info_db) {
    uint16_t thr_id, op, conflict_thr_id, conflict_op;

    if (mvrlu_profiler_get_confict_ops(get_rlu_thread_data(), &thr_id, &op,
                                &conflict_thr_id, &conflict_op)) {
        if (thr_id != conflict_thr_id) {
            struct ds_op_info *op_info = ((ds_op_caches + thr_id)->op_infos + op);
            struct ds_op_info *conflict_op_info = ((ds_op_caches + conflict_thr_id)->op_infos + conflict_op);
            struct abort_info *curr_abort_info = abort_info_db->abort_infos + abort_info_db->num_samples;

            curr_abort_info->ds_inst = op_info->ds_inst;
            curr_abort_info->ds_id = op_info->ds_id;
            curr_abort_info->thr_id = op_info->thr_id;
            curr_abort_info->ds_op_id = op_info->ds_op_id;
            curr_abort_info->conflict_thr_id = conflict_op_info->thr_id;
            curr_abort_info->conflict_ds_op_id = conflict_op_info->ds_op_id;
            curr_abort_info->txn_ts = op_info->txn_ts;
            curr_abort_info->conflict_txn_ts = conflict_op_info->txn_ts;

            if (op_info->args_len)
                memcpy(curr_abort_info->args, op_info->args, op_info->args_len);
            if (conflict_op_info->args_len)
                memcpy(curr_abort_info->conflict_args, conflict_op_info->args, conflict_op_info->args_len);

            abort_info_db->num_samples++;
        } else {
            abort_info_db->num_false_abort_samples++;
        }
    } else {
        abort_info_db->num_missing_samples++;
    }
}

void profiler_add_abort_info() {
    int rlu_thr_id = get_rlu_thread_id();
    abort_info_db_t *abort_info_db = abort_info_dbs + rlu_thr_id;
    if (!(abort_info_db->num_aborts & (ABORT_INFO_SAMPLING_RATE - 1)))
        add_abort_info_sample(abort_info_db);
    abort_info_db->num_aborts++;
}

void profiler_add_ds_inst(void *addr, const char *filename, int lineno) {
    struct ds_inst_entry kvp = {
        .addr = addr,
        .filename =filename,
        .lineno = lineno
    };
    ds_inst_map_put(di_map, &kvp);
}
