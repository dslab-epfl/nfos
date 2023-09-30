#include "static_mapping.h"
struct NfosVector *static_mappings;
struct Map *mapping_hash;
int nat_static_mapping_match(uint32_t match_addr, uint16_t match_port, uint32_t match_fib_index, uint8_t match_protocol,
uint32_t *mapping_addr, uint16_t *mapping_port, uint32_t *mapping_fib_index){
    uint64_t key = init_nat_k(match_addr, match_port, match_fib_index,match_protocol);
    /*Search in the hash table for index*/
    int index;
    if (!map_get(mapping_hash,(void*)(&key), &index)){
      return 0;
    }
    nat_static_mapping_t *one_mapping;
    nfos_vector_borrow(static_mappings, index, (void**)(&one_mapping));
    (*mapping_addr) = one_mapping->local_address;
    (*mapping_port) = (one_mapping->is_addr_only_static_mapping)? match_port : one_mapping->local_port;
    (*mapping_fib_index) = one_mapping->fib_index;
    /*Get the mapping from the pool*/
    /*Return 0 if not matched 1 if matched*/
    return 1;
}

void static_mapping_null_init(void *obj){
  nat_static_mapping_t *m = (nat_static_mapping_t*)obj;
  memset(m, 0, sizeof(nat_static_mapping_t));
}
bool key_equal(void *k1, void *k2){
  uint64_t *a = (uint64_t *)k1;
  uint64_t *b = (uint64_t *)k2;
  return (*a) == (*b); 
}
unsigned key_hash(void *k1){
  uint64_t a = *((uint64_t *)k1);
  return (unsigned)(((a>>32) + a) & 0xFFFFFFFF);
}
int nat_static_mapping_init(uint32_t num){
  if(!nfos_vector_allocate(sizeof(nat_static_mapping_t),num,static_mapping_null_init, &static_mappings )){
    return 0;
  }
  if(!map_allocate(key_equal,key_hash,num, &mapping_hash)){
    return 0;
  }

  return 1;
}