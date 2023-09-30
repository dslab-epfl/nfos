#pragma once
#ifndef __LOAD_BALANCE_H__
#define __LOAD_BALANCE_H__
#include <inttypes.h>
#include "vigor/nf-util.h"
#include "vector.h"
// WARNING: using nfos_vector_borrow_unsafe instead of nfos_vector_borrow
// since the latter will cause SEG_FAULT if called in the nf_init when the threads
// are not initialized.
enum DPO_ACTION{
    DROP = 0, LB, FORWARD
};
typedef struct dpo_t_{
    uint16_t action;
    uint16_t send_device;
    uint32_t dst_ip_address;  //The ip address is in little endian
    struct rte_ether_addr dst_mac_address; // The ip address is in big endian
} dpo_t;
typedef struct load_balance_t_ {
    /*Numbers of buckets in the load-balance*/
    uint16_t n_buckets;
    uint8_t dscp;
    dpo_t *lb_buckets;
} load_balance_t;
extern struct NfosVector* load_balance_pool;
extern uint32_t load_balance_pool_index;

static inline const dpo_t*
load_balance_get_bucket_i(const load_balance_t *lb, uint16_t bucket){
    assert(bucket < lb->n_buckets);
    return (&lb->lb_buckets[bucket]);
}

static inline load_balance_t*
load_balance_get (uint32_t lb_index){
    load_balance_t* lb;
    nfos_vector_borrow(load_balance_pool, lb_index, (void **)(&lb));
    return lb;
}
static inline void
load_balance_set_dscp(uint32_t lb_index, uint8_t dscp){
    load_balance_t *lb;
    nfos_vector_borrow_unsafe(load_balance_pool, lb_index, (void**)(&lb));
    lb->dscp = dscp;
}
void
lb_vector_get(struct NfosVector* vector, load_balance_t **p, uint32_t *index);

// Attention here, ip address is in little endian while the dst_mac_address is in little endian.
uint32_t load_balance_create(uint32_t n_dpo, uint16_t *action, uint16_t *send_device, rte_be32_t *dst_ip_address, struct rte_ether_addr * dst_mac_address);
#endif