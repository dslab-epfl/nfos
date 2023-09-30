#pragma once

#include "vigor/libvig/verified/vigor-time.h"
#include "scalability-profiler.h"

struct nfos_dchain_exp_cell;
struct NfosDoubleChainExp;


#ifndef SCALABILITY_PROFILER
#define nfos_dchain_exp_allocate(...) nfos_dchain_exp_allocate_t(__VA_ARGS__)
#define nfos_dchain_exp_allocate_new_index(...) nfos_dchain_exp_allocate_new_index_t(__VA_ARGS__)
#define nfos_dchain_exp_rejuvenate_index(...) nfos_dchain_exp_rejuvenate_index_t(__VA_ARGS__)
#define nfos_dchain_exp_expire_one_index(...) nfos_dchain_exp_expire_one_index_t(__VA_ARGS__)
#define nfos_dchain_exp_is_index_allocated(...) nfos_dchain_exp_is_index_allocated_t(__VA_ARGS__)
#else
#define nfos_dchain_exp_allocate(...) nfos_dchain_exp_allocate_debug_t(__VA_ARGS__, __FILE__, __LINE__)
#define nfos_dchain_exp_allocate_new_index(...) nfos_dchain_exp_allocate_new_index_debug_t(__VA_ARGS__)
#define nfos_dchain_exp_rejuvenate_index(...) nfos_dchain_exp_rejuvenate_index_debug_t(__VA_ARGS__)
#define nfos_dchain_exp_expire_one_index(...) nfos_dchain_exp_expire_one_index_debug_t(__VA_ARGS__)
#define nfos_dchain_exp_is_index_allocated(...) nfos_dchain_exp_is_index_allocated_debug_t(__VA_ARGS__)
#endif

//   Allocate memory and initialize a new index allocator with index expiration. The produced
//   allocator will operate on indexes [0-index).
//   @param validity_duration - index validity duration (in microseconds)
//   @param index_range - the limit on the number of allocated indexes.
//   @param chain_out - an output pointer that will hold the pointer to the newly
//                      allocated allocator in the case of success.
//   @returns 0 if the allocation failed, and 1 if the allocation is successful.
int nfos_dchain_exp_allocate_t(int index_range, vigor_time_t validity_duration, struct NfosDoubleChainExp **chain_out);
int nfos_dchain_exp_allocate_debug_t(int index_range, vigor_time_t validity_duration, struct NfosDoubleChainExp **chain_out,
                                     const char *filename, int lineno);

//   Allocate a fresh index. If there is an unused, or expired index in the range,
//   allocate it.
//   @param chain - pointer to the allocator.
//   @param index_out - output pointer to the newly allocated index.
//   @param time - current time returned from get_curr_time().
//   @returns 0 if there is no space, and 1 if the allocation is successful.
int nfos_dchain_exp_allocate_new_index_t(struct NfosDoubleChainExp *chain, int *index_out, vigor_time_t time);
int nfos_dchain_exp_allocate_new_index_debug_t(struct NfosDoubleChainExp *chain, int *index_out, vigor_time_t time);

//   Update the index timestamp. Needed to keep the index from expiration.
//   @param chain - pointer to the allocator.
//   @param index - the index to rejuvenate.
//   @param time - the current time, it will replace the old timestamp.
//   @returns 1 if the timestamp was updated, and 0 if the index is not tagged as
//            allocated.
int nfos_dchain_exp_rejuvenate_index_t(struct NfosDoubleChainExp *chain, int index, vigor_time_t time);
int nfos_dchain_exp_rejuvenate_index_debug_t(struct NfosDoubleChainExp *chain, int index, vigor_time_t time);

//   TODO: add doc
int nfos_dchain_exp_expire_one_index_t(struct NfosDoubleChainExp *chain, int *index_out,
                                     int *expiration_done_out, vigor_time_t time);
int nfos_dchain_exp_expire_one_index_debug_t(struct NfosDoubleChainExp *chain, int *index_out,
                                     int *expiration_done_out, vigor_time_t time);

int nfos_dchain_exp_is_index_allocated_t(struct NfosDoubleChainExp *chain, int index);
int nfos_dchain_exp_is_index_allocated_debug_t(struct NfosDoubleChainExp *chain, int index);

// Temp hack to reset curr_free_list to 0 in case of aborts
void nfos_dchain_exp_reset_curr_free_list(struct NfosDoubleChainExp *chain);
