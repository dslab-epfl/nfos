#pragma once

#include <stdint.h>

#include "vigor/libvig/verified/vigor-time.h"

#include "concurrent-double-chain-impl.h"

struct ConcurrentDoubleChain;

//   Allocate memory and initialize a new double chain allocator. The produced
//   allocator will operate on indexes [0-index).
//   @param index_range - the limit on the number of allocated indexes.
//   @param chain_out - an output pointer that will hold the pointer to the newly
//                      allocated allocator in the case of success.
//   @returns 0 if the allocation failed, and 1 if the allocation is successful.
int concurrent_dchain_allocate(int index_range, struct ConcurrentDoubleChain** chain_out);

int concurrent_dchain_has_free_indexes(struct ConcurrentDoubleChain* chain, int partition);

//   Allocate a fresh index. If there is an unused, or expired index in the range,
//   allocate it.
//   @param chain - pointer to the allocator.
//   @param index_out - output pointer to the newly allocated index.
//   @param time - current time. Allocator will note this for the new index.
//   @returns 0 if there is no space, and 1 if the allocation is successful.
int concurrent_dchain_allocate_new_index(struct ConcurrentDoubleChain* chain,
                                         int* index_out, vigor_time_t time,
                                         int partition);


//   Update the index timestamp. Needed to keep the index from expiration.
//   @param chain - pointer to the allocator.
//   @param index - the index to rejuvenate.
//   @param time - the current time, it will replace the old timestamp.
//   @returns 1 if the timestamp was updated, and 0 if the index is not tagged as
//            allocated.
int concurrent_dchain_rejuvenate_index(struct ConcurrentDoubleChain* chain,
                                       int index, vigor_time_t time,
                                       int partition);


//   Make space in the allocator by expiring the least recently used index.
//   @param chain - pointer to the allocator.
//   @param index_out - output pointer to the expired index.
//   @param time - the time border, separating expired indexes from non-expired
//                ones.
//   @returns 1 if the oldest index is older then current time and is expired,
//   0 otherwise.
int concurrent_dchain_expire_one_index(struct ConcurrentDoubleChain* chain,
                                       int* index_out, vigor_time_t time,
                                       int partition);

int concurrent_dchain_has_expired_index(struct ConcurrentDoubleChain* chain,
                                       int* index_out, vigor_time_t time,
                                       int partition);

int concurrent_dchain_is_index_allocated(struct ConcurrentDoubleChain* chain, int index,
                                         int partition);

int concurrent_dchain_free_index(struct ConcurrentDoubleChain* chain, int index,
                                 int partition);

struct concurrent_dchain_cell *concurrent_dchain_cell_out(struct ConcurrentDoubleChain* chain, int index);
