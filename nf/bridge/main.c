#include <stdint.h>
#include <string.h>

#include "nf.h"
#include "nf-log.h"
#include "bridge_config.h"

#include "vigor/libvig/verified/ether.h"
#include "double-chain-exp.h"
#include "map.h"
#include "vector.h"

// temp hack for transaction chopping
#include "rlu-wrapper.h"
#include "scalability-profiler.h"

bool pkt_set_id_eq(void* a, void* b) {
  return true;
}

unsigned pkt_set_id_hash(void* obj) {
  return 0;
}

void pkt_set_state_allocate(void *obj) {
  return;
}

typedef struct nf_config {
  // Expiration time of macs in microseconds
  uint32_t expiration_time;

  // Capacity of the mac table
  uint32_t dyn_capacity;

  // TODO: Add static macs
} nf_config_t;

struct nf_state {
  // mac table entry allocator
  struct NfosDoubleChainExp *dyn_heap;

  // mac to map entry index mapping
  struct NfosMap *dyn_map;

  // mac table entry to device mapping
  struct NfosVector *dyn_vals;

  // mac table entry to macs mapping
  struct NfosVector *dyn_macs;

  // Configuration
  nf_config_t *cfg;
};


/* Data structure auxiliary functons */
struct mac_entry {
  uint16_t dev;
  vigor_time_t time;
};


void dev_allocate(void* obj) {
  struct mac_entry *entry  = (struct mac_entry *) obj;
  entry->dev = 0;
  entry->time = INT64_MAX;
}

int pkt_handler(nf_state_t *non_pkt_set_state, pkt_t *pkt,
                uint16_t incoming_dev, pkt_set_state_t *unused_1,
                pkt_set_id_t *unused_2);

void exp_mac(nf_state_t *non_pkt_set_state);


/* Init function */

nf_state_t *nf_init(vigor_time_t *validity_duration_out, char **lcores_out, bool *has_related_pkt_sets_out,
                    bool *do_expiration_out, bool *has_pkt_sets_out) {
  // Number of cores
  *lcores_out = LCORES;

  // Register packet handlers
  pkt_handler_t *handlers = malloc(sizeof(pkt_handler_t));
  handlers[0] = pkt_handler;
  register_pkt_handlers(handlers);

  // Other NFOS configs
  *has_related_pkt_sets_out = false;
  *do_expiration_out = false;
  *has_pkt_sets_out = false;

  // Non-pkt-set state
  nf_state_t *ret = malloc(sizeof(nf_state_t));

  ret->cfg = malloc(sizeof(nf_config_t));
  ret->cfg->expiration_time = EXPIRATION_TIME;
  ret->cfg->dyn_capacity = MAC_TABLE_SIZE;

  if (!nfos_map_allocate(ether_addr_eq, ether_addr_hash, sizeof(struct rte_ether_addr),
                         ret->cfg->dyn_capacity, &ret->dyn_map)) ret = NULL;
  if (!nfos_vector_allocate(sizeof(struct rte_ether_addr), ret->cfg->dyn_capacity,
                            ether_addr_allocate, &ret->dyn_macs)) ret = NULL;
  if (!nfos_vector_allocate(sizeof(struct mac_entry), ret->cfg->dyn_capacity,
                            dev_allocate, &ret->dyn_vals)) ret = NULL;
  if (!nfos_dchain_exp_allocate(ret->cfg->dyn_capacity,
    ret->cfg->expiration_time, &ret->dyn_heap)) ret = NULL;

  if (!register_periodic_handler(PERIODIC_HANDLER_PERIOD, exp_mac))
    ret = NULL;
 
  return ret;
}


/* Data plane handlers */

bool nf_pkt_parser(uint8_t *buffer, pkt_t *pkt) {
  pkt->ether_header = nf_then_get_ether_header(buffer);
  pkt->raw = buffer;
  return true;
}

int nf_pkt_dispatcher(const pkt_t *pkt, uint16_t incoming_dev, 
                   pkt_set_id_t *pkt_set_id, bool *has_pkt_set_state,
                   nf_state_t *non_pkt_set_state) {
  
  // This NF only has one packet class with no pkt sets
  *has_pkt_set_state = false;
  return 0;
}

int pkt_handler(nf_state_t *non_pkt_set_state, pkt_t *pkt,
                uint16_t incoming_dev, pkt_set_state_t *unused_1,
                pkt_set_id_t *unused_2) {
  struct rte_ether_hdr *ether_header = pkt->ether_header;
  vigor_time_t now = get_curr_time();
  NF_DEBUG("NOW: %ld", now);

  // MAC learning
  struct rte_ether_addr *src_mac = &ether_header->s_addr;
  NF_DEBUG("MAC learning: src_mac %2x:%2x:%2x:%2x:%2x:%2x",
          src_mac->addr_bytes[0],src_mac->addr_bytes[1],src_mac->addr_bytes[2],
          src_mac->addr_bytes[3],src_mac->addr_bytes[4],src_mac->addr_bytes[5]);

  int index;
  int present = nfos_map_get(non_pkt_set_state->dyn_map, (void *)src_mac, &index);
  if (present) {
    struct mac_entry *value;
    nfos_vector_borrow(non_pkt_set_state->dyn_vals, index, (void **)&value);
    // Optimization: Only refresh if last refreshed more than REFRESH_INTERVAL ago
    // Th bridge show work as if the expiration time is between (EXPIRATION_TIME
    // - REFRESH_INTERVAL) and EXPIRATION_TIME
    // TODO: Add a macro to turn off this opt.
    if (now > value->time + REFRESH_INTERVAL) {
      if (nfos_dchain_exp_rejuvenate_index(non_pkt_set_state->dyn_heap, index, now) == ABORT_HANDLER) {
        return ABORT_HANDLER;
      }
      if (nfos_vector_borrow_mut(non_pkt_set_state->dyn_vals, index, (void **)&value) == ABORT_HANDLER) {
        return ABORT_HANDLER;
      }
      value->time = now;

      NF_DEBUG("rejuvenate index %d", index);
    }

    // TODO: handle mac move

  } else {
    int allocated =
      nfos_dchain_exp_allocate_new_index(non_pkt_set_state->dyn_heap, &index, now);
    if (!allocated) {
      NF_DEBUG("No more space in the mac table");
      return 0;
    } else if (allocated == ABORT_HANDLER) {
      return ABORT_HANDLER;
    } else {
      NF_DEBUG("allocate index %d", index);
    }

    struct rte_ether_addr *key;
    struct mac_entry *value;
    if (nfos_vector_borrow_mut(non_pkt_set_state->dyn_macs, index, (void **)&key) == ABORT_HANDLER) {
      return ABORT_HANDLER;
    }
    if (nfos_vector_borrow_mut(non_pkt_set_state->dyn_vals, index, (void **)&value) == ABORT_HANDLER) {
      return ABORT_HANDLER;
    }
    memcpy(key, src_mac, sizeof(struct rte_ether_addr));
    value->dev = incoming_dev;
    value->time = now;

    if (nfos_map_put(non_pkt_set_state->dyn_map, src_mac, index) == ABORT_HANDLER) {
      return ABORT_HANDLER;
    }

    NF_DEBUG("MAC learned: [%2x:%2x:%2x:%2x:%2x:%2x, %d]",
          key->addr_bytes[0],key->addr_bytes[1],key->addr_bytes[2],
          key->addr_bytes[3],key->addr_bytes[4],key->addr_bytes[5],
          *value);
  }

  // Pkt forwarding
  struct rte_ether_addr *dst_mac = &ether_header->d_addr;
  NF_DEBUG("Forwarding: dst_mac: %2x:%2x:%2x:%2x:%2x:%2x",
          dst_mac->addr_bytes[0],dst_mac->addr_bytes[1],dst_mac->addr_bytes[2],
          dst_mac->addr_bytes[3],dst_mac->addr_bytes[4],dst_mac->addr_bytes[5]);

  int out_index;
  if (nfos_map_get(non_pkt_set_state->dyn_map, (void *)dst_mac, &out_index)) {
    // uint16_t *out_device;
    struct mac_entry *value;
    nfos_vector_borrow(non_pkt_set_state->dyn_vals, out_index, (void **)&value);
    send_pkt(pkt, value->dev);
    NF_DEBUG("pkt sent to port %d", *out_device);
  } else {
    flood_pkt(pkt);
    NF_DEBUG("pkt flooded");
  }

  return 0;
}


/* Periodic handlers */

void exp_mac(nf_state_t *non_pkt_set_state) {
  // temp hack for transaction chopping
  rlu_thread_data_t *rlu_data = get_rlu_thread_data();

  int index;
  vigor_time_t now = get_curr_time();
  int expiration_done = 0;

  NF_DEBUG("NOW: %ld", now);

retry_exp:
  RLU_READER_LOCK(rlu_data);
  // Temp hack to reset curr_free_list to 0 in case of aborts
  nfos_dchain_exp_reset_curr_free_list(non_pkt_set_state->dyn_heap);
  while (!expiration_done) {
    int ret = nfos_dchain_exp_expire_one_index(non_pkt_set_state->dyn_heap, &index, &expiration_done, now);
    if (ret == ABORT_HANDLER) {
      NF_DEBUG("ABORT: expire index");
      nfos_abort_txn(rlu_data);
      goto retry_exp;

    } else if (ret == 1) {
      struct rte_ether_addr *key;
      nfos_vector_borrow(non_pkt_set_state->dyn_macs, index, (void **)&key);   
      if (nfos_map_erase(non_pkt_set_state->dyn_map, (void *)key) == ABORT_HANDLER) {
        NF_DEBUG("ABORT: map erase");
        nfos_abort_txn(rlu_data);
        goto retry_exp;
      }

      NF_DEBUG("MAC deleted: mac %2x:%2x:%2x:%2x:%2x:%2x index %d",
          key->addr_bytes[0],key->addr_bytes[1],key->addr_bytes[2],
          key->addr_bytes[3],key->addr_bytes[4],key->addr_bytes[5],
          index);
    }
  }
  if (!RLU_READER_UNLOCK(rlu_data)) {
    nfos_abort_txn(rlu_data);
    NF_DEBUG("ABORT: exp read validation\n");
    goto retry_exp;
  }

  NF_DEBUG("EXP DONE");
}


/* Unspecified handlers */

int nf_unknown_pkt_set_handler(nf_state_t *non_pkt_set_state, pkt_t *pkt, uint16_t incoming_dev, pkt_set_id_t *pkt_set_id) {

}

int nf_expired_pkt_set_handler(nf_state_t *non_pkt_set_state, pkt_set_state_t *pkt_set_state) {

}

