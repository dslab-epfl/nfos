#include "mtrie.h"
struct NfosVector* ip4_ply_pool;
uint32_t ip4_ply_pool_index = 0;
void
ip4_mtrie_init(ip4_fib_mtrie_t *m)
{
  memset(m->root_ply.dst_address_bits_of_leaves, 0, sizeof(m->root_ply.dst_address_bits_of_leaves));
  for (int i =0; i < PLY_16_SIZE; i++) {m->root_ply.leaves[i] = 1;}
}
void
ply_8_init (ip4_fib_mtrie_8_ply_t * p,
            ip4_fib_mtrie_leaf_t init, uint32_t prefix_len, uint32_t ply_base_len)
{
PLY_INIT (p, init, prefix_len, ply_base_len);
}
void
mtrie_vector_get(struct NfosVector* vector, ip4_fib_mtrie_8_ply_t **p, uint32_t *index){
  assert(ip4_ply_pool_index < nfos_vector_get_capacity(vector));
  nfos_vector_borrow_unsafe(vector, ip4_ply_pool_index, (void**)p);
  (*index) = ip4_ply_pool_index;
  ip4_ply_pool_index ++;
}
ip4_fib_mtrie_leaf_t
 ply_create (ip4_fib_mtrie_t * m,
             ip4_fib_mtrie_leaf_t init_leaf,
             uint32_t leaf_prefix_len, uint32_t ply_base_len)
 {
   ip4_fib_mtrie_8_ply_t *p;
   /* Get cache aligned ply. */
   uint32_t index;

   mtrie_vector_get(ip4_ply_pool, &p, &index);
   ply_8_init (p, init_leaf, leaf_prefix_len, ply_base_len);
   return ip4_fib_mtrie_leaf_set_next_ply_index (index);
 }
void
 set_ply_with_more_specific_leaf (ip4_fib_mtrie_t * m,
                                  ip4_fib_mtrie_8_ply_t * ply,
                                  ip4_fib_mtrie_leaf_t new_leaf,
                                  uint32_t new_leaf_dst_address_bits)
 {
   ip4_fib_mtrie_leaf_t old_leaf;
   uint32_t i;
 
   assert (ip4_fib_mtrie_leaf_is_terminal (new_leaf));
 
   for (i = 0; i < PLY_8_SIZE; i++)
     {
       old_leaf = ply->leaves[i];
 
       /* Recurse into sub plies. */
       if (!ip4_fib_mtrie_leaf_is_terminal (old_leaf))
         {
           uint32_t index;
           ip4_fib_mtrie_8_ply_t *sub_ply =
             get_next_ply_for_leaf (m, old_leaf, &index);
           set_ply_with_more_specific_leaf (m, sub_ply, new_leaf,
                                            new_leaf_dst_address_bits);
         }
 
       /* Replace less specific terminal leaves with new leaf. */
       else if (new_leaf_dst_address_bits >=
                ply->dst_address_bits_of_leaves[i])
         {
           ply->leaves[i] = new_leaf;
           ply->dst_address_bits_of_leaves[i] = new_leaf_dst_address_bits;
           ply->n_non_empty_leafs += ip4_fib_mtrie_leaf_is_non_empty (ply, i);
         }
     }
 }
void
 set_leaf (ip4_fib_mtrie_t * m,
           const ip4_fib_mtrie_set_unset_leaf_args_t * a,
           uint32_t old_ply_index, uint32_t dst_address_byte_index)
 {
   ip4_fib_mtrie_leaf_t old_leaf, new_leaf;
   int32_t n_dst_bits_next_plies;
   uint8_t dst_byte;
   ip4_fib_mtrie_8_ply_t *old_ply;
 
   nfos_vector_borrow_unsafe(ip4_ply_pool, old_ply_index, (void**)(&old_ply));
 
   assert (a->dst_address_length <= 32);
   assert (dst_address_byte_index < 32 / 8);
 
   /* how many bits of the destination address are in the next PLY */
   n_dst_bits_next_plies =
     a->dst_address_length - 8 * (dst_address_byte_index + 1);
 
   dst_byte = (a->dst_address >> (24 - 8 * dst_address_byte_index)) & (0xFF);
 
   /* Number of bits next plies <= 0 => insert leaves this ply. */
   if (n_dst_bits_next_plies <= 0)
     {
       /* The mask length of the address to insert maps to this ply */
       uint32_t old_leaf_is_terminal;
       uint32_t i, n_dst_bits_this_ply;
 
       /* The number of bits, and hence slots/buckets, we will fill */
       n_dst_bits_this_ply = (8 < -n_dst_bits_next_plies) ? 8: (-n_dst_bits_next_plies);
 
       /* Starting at the value of the byte at this section of the v4 address
        * fill the buckets/slots of the ply */
       for (i = dst_byte; i < dst_byte + (1 << n_dst_bits_this_ply); i++)
         {
           ip4_fib_mtrie_8_ply_t *new_ply;
 
           old_leaf = old_ply->leaves[i];
           old_leaf_is_terminal = ip4_fib_mtrie_leaf_is_terminal (old_leaf);
 
           if (a->dst_address_length >= old_ply->dst_address_bits_of_leaves[i])
             {
               /* The new leaf is more or equally specific than the one currently
                * occupying the slot */
               new_leaf = ip4_fib_mtrie_leaf_set_adj_index (a->adj_index);
 
               if (old_leaf_is_terminal)
                 {
                   /* The current leaf is terminal, we can replace it with
                    * the new one */
                   old_ply->n_non_empty_leafs -=
                     ip4_fib_mtrie_leaf_is_non_empty (old_ply, i);
 
                   old_ply->dst_address_bits_of_leaves[i] =
                     a->dst_address_length;
                   old_ply->leaves[i] = new_leaf;
 
                   old_ply->n_non_empty_leafs +=
                     ip4_fib_mtrie_leaf_is_non_empty (old_ply, i);
                 }
               else
                 {
                   /* Existing leaf points to another ply.  We need to place
                    * new_leaf into all more specific slots. */
                   uint32_t index;
                   new_ply = get_next_ply_for_leaf (m, old_leaf, &index);
                   set_ply_with_more_specific_leaf (m, new_ply, new_leaf,
                                                    a->dst_address_length);
                 }
             }
           else if (!old_leaf_is_terminal)
             {
               /* The current leaf is less specific and not termial (i.e. a ply),
                * recurse on down the trie */
               uint32_t index;
               new_ply = get_next_ply_for_leaf (m, old_leaf, &index);
               set_leaf (m, a, index,
                         dst_address_byte_index + 1);
             }
           /*
            * else
            *  the route we are adding is less specific than the leaf currently
            *  occupying this slot. leave it there
            */
         }
     }
   else
     {
       /* The address to insert requires us to move down at a lower level of
        * the trie - recurse on down */
       ip4_fib_mtrie_8_ply_t *new_ply;
       uint8_t ply_base_len;
 
       ply_base_len = 8 * (dst_address_byte_index + 1);
 
       old_leaf = old_ply->leaves[dst_byte];
       uint32_t index;
 
       if (ip4_fib_mtrie_leaf_is_terminal (old_leaf))
         {
           /* There is a leaf occupying the slot. Replace it with a new ply */
           old_ply->n_non_empty_leafs -=
             ip4_fib_mtrie_leaf_is_non_empty (old_ply, dst_byte);
 
           new_leaf =
             ply_create (m, old_leaf,
                         old_ply->dst_address_bits_of_leaves[dst_byte],
                         ply_base_len);
           new_ply = get_next_ply_for_leaf (m, new_leaf, &index);
 
           /* Refetch since ply_create may move pool. */
           nfos_vector_borrow_unsafe(ip4_ply_pool,old_ply_index, (void**)(&old_ply));
 
           old_ply->leaves[dst_byte] = new_leaf;
           old_ply->dst_address_bits_of_leaves[dst_byte] = ply_base_len;
 
           old_ply->n_non_empty_leafs +=
             ip4_fib_mtrie_leaf_is_non_empty (old_ply, dst_byte);
           assert (old_ply->n_non_empty_leafs >= 0);
         }
       else
         new_ply = get_next_ply_for_leaf (m, old_leaf, &index);
 
       set_leaf (m, a, index, dst_address_byte_index + 1);
     }
 }
void
 set_root_leaf (ip4_fib_mtrie_t * m,
                const ip4_fib_mtrie_set_unset_leaf_args_t * a)
 {
   ip4_fib_mtrie_leaf_t old_leaf, new_leaf;
   ip4_fib_mtrie_16_ply_t *old_ply;
   int32_t n_dst_bits_next_plies;
   uint16_t dst_byte;
 
   old_ply = &m->root_ply;
 
   assert (a->dst_address_length <= 32);
 
   /* how many bits of the destination address are in the next PLY */
   n_dst_bits_next_plies = a->dst_address_length - 16;
 
   dst_byte = (a->dst_address >> 16) & (0xFFFF);
 
   /* Number of bits next plies <= 0 => insert leaves this ply. */
   if (n_dst_bits_next_plies <= 0)
     {
       /* The mask length of the address to insert maps to this ply */
       uint32_t old_leaf_is_terminal;
       uint32_t i, n_dst_bits_this_ply;
 
       /* The number of bits, and hence slots/buckets, we will fill */
       n_dst_bits_this_ply = 16 - a->dst_address_length;
 
       /* Starting at the value of the byte at this section of the v4 address
        * fill the buckets/slots of the ply */
       for (i = 0; i < (1 << n_dst_bits_this_ply); i++)
         {
           ip4_fib_mtrie_8_ply_t *new_ply;
           uint16_t slot;
 
           slot = dst_byte + i;
 
           old_leaf = old_ply->leaves[slot];
           old_leaf_is_terminal = ip4_fib_mtrie_leaf_is_terminal (old_leaf);
 
           if (a->dst_address_length >=
               old_ply->dst_address_bits_of_leaves[slot])
             {
               /* The new leaf is more or equally specific than the one currently
                * occupying the slot */
               new_leaf = ip4_fib_mtrie_leaf_set_adj_index (a->adj_index);
 
               if (old_leaf_is_terminal)
                 {
                   /* The current leaf is terminal, we can replace it with
                    * the new one */
                   old_ply->dst_address_bits_of_leaves[slot] =
                     a->dst_address_length;
                   old_ply->leaves[slot] = new_leaf;
                 }
               else
                 {
                   /* Existing leaf points to another ply.  We need to place
                    * new_leaf into all more specific slots. */
                   uint32_t index;
                   new_ply = get_next_ply_for_leaf (m, old_leaf, &index);
                   set_ply_with_more_specific_leaf (m, new_ply, new_leaf,
                                                    a->dst_address_length);
                 }
             }
           else if (!old_leaf_is_terminal)
             {
               /* The current leaf is less specific and not termial (i.e. a ply),
                * recurse on down the trie */
               uint32_t index;
               new_ply = get_next_ply_for_leaf (m, old_leaf, &index);
               set_leaf (m, a, index, 2);
             }
           /*
            * else
            *  the route we are adding is less specific than the leaf currently
            *  occupying this slot. leave it there
            */
         }
     }
   else
     {
       /* The address to insert requires us to move down at a lower level of
        * the trie - recurse on down */
       ip4_fib_mtrie_8_ply_t *new_ply;
       uint8_t ply_base_len;
 
       ply_base_len = 16;
 
       old_leaf = old_ply->leaves[dst_byte];
       uint32_t index;
 
       if (ip4_fib_mtrie_leaf_is_terminal (old_leaf))
         {
           /* There is a leaf occupying the slot. Replace it with a new ply */
           new_leaf =
             ply_create (m, old_leaf,
                         old_ply->dst_address_bits_of_leaves[dst_byte],
                         ply_base_len);
           new_ply = get_next_ply_for_leaf (m, new_leaf, &index);
           old_ply->leaves[dst_byte] = new_leaf;
           old_ply->dst_address_bits_of_leaves[dst_byte] = ply_base_len;
         }
       else{
         new_ply = get_next_ply_for_leaf (m, old_leaf, &index);
       }
       set_leaf (m, a, index, 2);
     }
 }
void ip4_fib_mtrie_route_add (ip4_fib_mtrie_t *m, uint32_t dst_address, uint32_t dst_address_length, uint32_t adj_index)
{
  ip4_fib_mtrie_set_unset_leaf_args_t a;
  a.dst_address = dst_address & ((0xFFFFFFFF) << (32 - dst_address_length));
  a.dst_address_length = dst_address_length;
  a.adj_index = adj_index;
  set_root_leaf(m, &a);
}