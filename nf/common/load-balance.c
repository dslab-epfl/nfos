#include "load-balance.h"
struct NfosVector* load_balance_pool;
uint32_t load_balance_pool_index = 0;
void
lb_vector_get(struct NfosVector* vector, load_balance_t **p, uint32_t *index){
  assert(load_balance_pool_index < nfos_vector_get_capacity(vector));
  nfos_vector_borrow_unsafe(vector, load_balance_pool_index, (void**)p);
  (*index) = load_balance_pool_index;
  load_balance_pool_index ++;
}
uint32_t load_balance_create(uint32_t n_dpo, uint16_t *action, uint16_t *send_device, rte_be32_t *dst_ip_address, struct rte_ether_addr * dst_mac_address){
    load_balance_t *lb;
    uint32_t index;
    lb_vector_get(load_balance_pool, &lb, &index);
    lb ->n_buckets = n_dpo;
    lb ->lb_buckets = (dpo_t *)malloc(n_dpo * sizeof(dpo_t));
    for(uint32_t i =0; i < n_dpo; i++){
        lb->lb_buckets[i].action = action[i];
    }
    for(uint32_t i =0; i < n_dpo; i++){
        lb->lb_buckets[i].send_device = send_device[i];
    }
    for(uint32_t i =0; i < n_dpo; i++)
        lb->lb_buckets[i].dst_ip_address = dst_ip_address[i];
    for(uint32_t i =0; i < n_dpo; i++)
        lb->lb_buckets[i].dst_mac_address = dst_mac_address[i];
    return index;
}