#pragma once

#include "scalability-profiler.h"
struct NfosDoubleChain;
#ifndef SCALABILITY_PROFILER
#define nfos_dchain_allocate(...) nfos_dchain_allocate_t(__VA_ARGS__)
#define nfos_dchain_allocate_new_index(...) nfos_dchain_allocate_new_index_t(__VA_ARGS__)
#define nfos_dchain_free_index(...) nfos_dchain_free_index_t(__VA_ARGS__)
#else
#define nfos_dchain_allocate(...) nfos_dchain_allocate_debug_t(__VA_ARGS__, __FILE__, __LINE__)
#define nfos_dchain_allocate_new_index(...) nfos_dchain_allocate_new_index_debug_t(__VA_ARGS__)
#define nfos_dchain_free_index(...) nfos_dchain_free_index_debug_t(__VA_ARGS__)
#endif

//   Allocate memory and initialize a new index allocator. The produced
//   allocator will operate on indexes [0-index).
//   @param index_range - the limit on the number of allocated indexes.
//   @param chain_out - an output pointer that will hold the pointer to the newly
//                      allocated allocator in the case of success.
//   @returns 0 if the allocation failed, and 1 if the allocation is successful.
int nfos_dchain_allocate_t(int index_range, struct NfosDoubleChain** chain_out);
int nfos_dchain_allocate_debug_t(int index_range, struct NfosDoubleChain** chain_out,
                                 const char *filename, int lineno);

//   Allocate a fresh index. If there is an unused index in the range,
//   allocate it.
//   @param chain - pointer to the allocator.
//   @param index_out - output pointer to the newly allocated index.
//   @returns 0 if there is no space, and 1 if the allocation is successful.
int nfos_dchain_allocate_new_index_t(struct NfosDoubleChain* chain, int* index_out);
int nfos_dchain_allocate_new_index_debug_t(struct NfosDoubleChain* chain, int* index_out);

//   Free an index.
//   @param chain - pointer to the allocator.
//   @param index - index to free.
//   @returns 0 if fails, and 1 if succeeds.
int nfos_dchain_free_index_t(struct NfosDoubleChain* chain, int index);
int nfos_dchain_free_index_debug_t(struct NfosDoubleChain* chain, int index);

// (Internal function for testing)
void _nfos_dchain_dump(struct NfosDoubleChain* chain, int index_range);
