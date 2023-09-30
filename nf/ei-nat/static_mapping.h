#pragma once
#ifndef __STATIC_MAPPING_H__
#define __STATIC_MAPPING_H__
#include "nf.h"
#include "vector.h"
#include "deps/vigor/libvig/verified/map.h"
#define STATIC_MAPPINGS_SIZE 1
typedef struct nat_static_mapping_t_{
  uint32_t fib_index;
  uint32_t local_address;
  bool is_addr_only_static_mapping;
  uint16_t local_port;
}nat_static_mapping_t;
static inline uint64_t init_nat_k(uint32_t ip4_addr, uint16_t port, uint32_t fib_index, uint8_t proto){
    return (uint64_t) ip4_addr << 32 | (uint64_t) port << 16 | fib_index << 3 |
     (proto & 0x7);
}
extern struct NfosVector *static_mappings;
extern struct Map *mapping_hash;
/*For now only search is supported, we don't support read yet since it is not used in our trace now
* 1: matched; 0: not matched.
*/
int nat_static_mapping_match(uint32_t match_addr, uint16_t match_port, uint32_t match_fib_index, uint8_t match_protocol,
uint32_t *mapping_addr, uint16_t *mapping_port, uint32_t *mapping_fib_index);

void static_mapping_null_init(void *obj);
bool key_equal(void *k1, void *k2);
unsigned key_hash(void *k1);
int nat_static_mapping_init(uint32_t num);
#endif