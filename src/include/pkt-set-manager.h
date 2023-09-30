#pragma once

#include <stdbool.h>

#include "vigor/libvig/verified/vigor-time.h"
#include "vigor/libvig/verified/vector.h"

#include "nf.h"
#include "concurrent-map.h"

typedef struct add_pkt_set_log_entry {
    // True -> NFOS should add the pkt set to the map
    bool add_pkt_set;
    pkt_set_id_t related_pkt_set_id;
    pkt_set_state_t pkt_set_state;
} add_pkt_set_log_entry_t;

bool init_pkt_set_manager(map_keys_equality *pkt_set_id_eq,
                          map_key_hash *pkt_set_id_hash,
                          vigor_time_t _pkt_set_validity_duration,
                          bool _has_related_pkt_sets);

/* Utils for logging/committing add_pkt_set op */
void add_pkt_set_log_clear();

bool add_pkt_set_log(pkt_set_state_t *_state, int pkt_set_partition,
                     pkt_set_id_t *related_pkt_set_id);

void add_pkt_set_commit(pkt_set_id_t *pkt_set_id,
                        int pkt_set_partition, vigor_time_t time);

bool get_pkt_set_state(pkt_set_id_t *pkt_set_id, pkt_set_state_t **_state,
                       int pkt_set_partition, vigor_time_t time);

int delete_expired_pkt_sets(vigor_time_t time, int pkt_set_partition,
                            nf_state_t *non_pkt_set_state);
