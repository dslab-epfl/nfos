#pragma once
#ifndef __MTRIE_H__
#define __MTRIE_H__
#include <unistd.h>
#include <stdlib.h>
#include <inttypes.h>
#include "vector.h"
#include "vigor/nf-util.h"
#define PLY_16_SIZE (1 << 16)
#define PLY_8_SIZE (1 << 8)
// WARNING: using nfos_vector_borrow_unsafe instead of nfos_vector_borrow
// since the latter will cause SEG_FAULT if called in the nf_init when the threads
// are not initialized.

/*a.b.c.d -> 0xabcd -> d c b a*/
typedef uint32_t ip4_fib_mtrie_leaf_t;
/*This is the 1st layer of the 16-8-8 mtrie*/
typedef struct ip4_fib_mtrie_16_ply_t_
{
    /*To be filled with leaves*/
    ip4_fib_mtrie_leaf_t leaves[PLY_16_SIZE];
    /*Prefix length (only for terminal leaves)*/
    uint8_t dst_address_bits_of_leaves[PLY_16_SIZE];
} ip4_fib_mtrie_16_ply_t;

/*This is the 2nd and 3rd layer of the 16-8-8 mtrie*/
typedef struct ip4_fib_mtrie_8_ply_t_
{
    ip4_fib_mtrie_leaf_t leaves[PLY_8_SIZE];
    uint8_t dst_address_bits_of_leaves[PLY_8_SIZE];
    /*Number of non-empty leafs (Terminal and Non-terminal)*/
    int32_t n_non_empty_leafs;
    /**
    * The length of the ply's covering prefix. Also a measure of its depth
    * If a leaf in a slot has a mask length longer than this then it is
    * 'non-empty'. Otherwise it is the value of the cover.
    */
    int32_t dst_address_bits_base;

} ip4_fib_mtrie_8_ply_t;
extern struct NfosVector* ip4_ply_pool;
extern uint32_t ip4_ply_pool_index;

typedef struct{
    ip4_fib_mtrie_16_ply_t root_ply;
} ip4_fib_mtrie_t;

typedef struct{
  uint32_t dst_address;
  uint32_t dst_address_length;
  uint32_t adj_index;
} ip4_fib_mtrie_set_unset_leaf_args_t;

 /**
  * @brief Lookup step number 1.  Processes 2 bytes of 4 byte ip4 address.
  */
 static inline ip4_fib_mtrie_leaf_t
 ip4_fib_mtrie_lookup_step_one (const ip4_fib_mtrie_t * m,
                                const rte_be32_t dst_address)
 {
   ip4_fib_mtrie_leaf_t next_leaf;

   next_leaf = m->root_ply.leaves[(dst_address >> 16) & 0xFFFF];
   
   return next_leaf;
 }
 
 /*
 * Determine whether the leaf is terminal (deciding by the last bit)
 */
static inline uint8_t
ip4_fib_mtrie_leaf_is_terminal(ip4_fib_mtrie_leaf_t n) { return n&1;}

static inline uint32_t
 ip4_fib_mtrie_leaf_get_next_ply_index (ip4_fib_mtrie_leaf_t n)
 {
   assert ((n & 1) == 0);
   return n >> 1;
 }

static inline ip4_fib_mtrie_8_ply_t *
 get_next_ply_for_leaf (ip4_fib_mtrie_t * m, ip4_fib_mtrie_leaf_t l, uint32_t *index)
 {
   uint32_t n = ip4_fib_mtrie_leaf_get_next_ply_index (l);
   ip4_fib_mtrie_8_ply_t *leaf;
   (*index) = n;
   nfos_vector_borrow_unsafe(ip4_ply_pool, n, (void**)(&leaf));
   return leaf;
 }

 
static inline ip4_fib_mtrie_leaf_t
 ip4_fib_mtrie_leaf_set_next_ply_index (uint32_t i)
 {
   ip4_fib_mtrie_leaf_t l;
   l = 0 + 2 * i;
   assert (ip4_fib_mtrie_leaf_get_next_ply_index (l) == i);
   return l;
 }

  /**
  * @brief Lookup step.  Processes 1 byte of 4 byte ip4 address.
  */
static inline ip4_fib_mtrie_leaf_t
 ip4_fib_mtrie_lookup_step (const ip4_fib_mtrie_t * m,
                            ip4_fib_mtrie_leaf_t current_leaf,
                            const rte_be32_t dst_address,
                            uint32_t dst_address_byte_index)
 {
   ip4_fib_mtrie_8_ply_t *ply;
 
   uint8_t current_is_terminal = ip4_fib_mtrie_leaf_is_terminal (current_leaf);
 
   if (!current_is_terminal)
     {
       nfos_vector_borrow(ip4_ply_pool, (current_leaf >> 1), (void**)(&ply));
       return (ply->leaves[(dst_address >> (24 - 8 * dst_address_byte_index)) & 0xFF]);
     }
 
   return current_leaf;
 }


/*
Init a mtrie;
*/
void
ip4_mtrie_init(ip4_fib_mtrie_t *m);

 #define PLY_INIT_LEAVES(p)                                              \
 {                                                                       \
   uint32_t *l;                                                          \
                                                                         \
   for (l = p->leaves; l < p->leaves + PLY_8_SIZE; l += 4)               \
     {                                                                   \
       l[0] = init;                                                      \
       l[1] = init;                                                      \
       l[2] = init;                                                      \
       l[3] = init;                                                      \
       }                                                                 \
 }
 
 #define PLY_INIT(p, init, prefix_len, ply_base_len)                     \
 {                                                                       \
   /*                                                                    \
    * A leaf is 'empty' if it represents a leaf from the covering PLY    \
    * i.e. if the prefix length of the leaf is less than or equal to     \
    * the prefix length of the PLY                                       \
    */                                                                   \
   p->n_non_empty_leafs = (prefix_len > ply_base_len ?                   \
                           PLY_8_SIZE : 0);                              \
   memset (p->dst_address_bits_of_leaves, prefix_len,                    \
           sizeof (p->dst_address_bits_of_leaves));                      \
   p->dst_address_bits_base = ply_base_len;                              \
                                                                         \
   /* Initialize leaves. */                                              \
   PLY_INIT_LEAVES(p);                                                   \
 }
 
 void
 ply_8_init (ip4_fib_mtrie_8_ply_t * p,
             ip4_fib_mtrie_leaf_t init, uint32_t prefix_len, uint32_t ply_base_len);

void
mtrie_vector_get(struct NfosVector* vector, ip4_fib_mtrie_8_ply_t **p, uint32_t *index);

void
mtrie_vector_get(struct NfosVector* vector, ip4_fib_mtrie_8_ply_t **p, uint32_t *index);

ip4_fib_mtrie_leaf_t
 ply_create (ip4_fib_mtrie_t * m,
             ip4_fib_mtrie_leaf_t init_leaf,
             uint32_t leaf_prefix_len, uint32_t ply_base_len);
 
static inline ip4_fib_mtrie_leaf_t
 ip4_fib_mtrie_leaf_set_adj_index (uint32_t adj_index)
 {
   ip4_fib_mtrie_leaf_t l;
   l = 1 + 2 * adj_index;
   return l;
 }
static inline uint32_t
 ip4_fib_mtrie_leaf_is_non_empty (ip4_fib_mtrie_8_ply_t * p, uint8_t dst_byte)
 {
   /*
    * It's 'non-empty' if the length of the leaf stored is greater than the
    * length of a leaf in the covering ply. i.e. the leaf is more specific
    * than it's would be cover in the covering ply
    */
   if (p->dst_address_bits_of_leaves[dst_byte] > p->dst_address_bits_base)
     return (1);
   return (0);
 }
void
 set_ply_with_more_specific_leaf (ip4_fib_mtrie_t * m,
                                  ip4_fib_mtrie_8_ply_t * ply,
                                  ip4_fib_mtrie_leaf_t new_leaf,
                                  uint32_t new_leaf_dst_address_bits);
void
 set_leaf (ip4_fib_mtrie_t * m,
           const ip4_fib_mtrie_set_unset_leaf_args_t * a,
           uint32_t old_ply_index, uint32_t dst_address_byte_index);
 
void
 set_root_leaf (ip4_fib_mtrie_t * m,
                const ip4_fib_mtrie_set_unset_leaf_args_t * a);

/*
Add a route to mtrie; Here adj_index is the index in the load-balance pool.
*/
void ip4_fib_mtrie_route_add (ip4_fib_mtrie_t *m, uint32_t dst_address, uint32_t dst_address_length, uint32_t adj_index);
#endif
 
