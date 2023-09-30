#pragma once

#include "scalability-profiler.h"
#include "vigor/libvig/verified/map-util.h"

struct NfosMap;

/*
 * NOTE: This map only supports values of int type. After inserting a key-value
 * pair, the value cannot be updated.
 * 
 * To implement a map with values of other types, use this map in combination
 * with a vector and an index allocator. The index allocator allocates indexes to the vector
 * in the range [0, map capacity), the map stores mappings from keys to these indexes,
 * and the vector stores the actual values.
 *
 * To implement a map with expirable mappings, use index allocator variant that supports
 * index expiration (double-chain-exp.h). In this case, you will need an extra vector to
 * store keys inserted in the map. When a map entry is expired, one gets its index first
 * and then look up the key of the index from this key vector, only then can one erase the key-index
 * mapping from the map.
 */

//   Allocate memory and initialize a new map.
//   @param keq - function to compare equality of two keys, returns true if equal
//   @param khash - function to compute and return a 32-bit hash of the key
//   @param key_size - the size of the key in bytes.
//   @param capacity - maximum number of keys. Whenever possible, double the capacity to avoid map collisions for best performance.
//   @param map_out - an output pointer that will hold the pointer to the newly
//                      allocated map in the case of success.
//   @returns 0 if the allocation failed, and 1 if the allocation is successful.
int nfos_map_allocate_t(map_keys_equality* keq, map_key_hash* khash,
                        unsigned key_size, unsigned capacity, struct NfosMap** map_out);
int nfos_map_allocate_debug_t(map_keys_equality* keq, map_key_hash* khash,
                              unsigned key_size, unsigned capacity, struct NfosMap** map_out,
                              const char *filename, int lineno);

//   Look up the value of a key.
//   @param map - pointer to the map
//   @param key - key
//   @param value_out - pointer to the value.
//   @returns 0 if the key does not exist, and 1 if lookup succeeds.
int nfos_map_get_t(struct NfosMap* map, void* key, int* value_out);
int nfos_map_get_debug_t(struct NfosMap* map, void* key, int* value_out);

#ifndef SCALABILITY_PROFILER
#define nfos_map_allocate(...) nfos_map_allocate_t(__VA_ARGS__)
#define nfos_map_put(...) nfos_map_put_t(__VA_ARGS__)
#define nfos_map_erase(...) nfos_map_erase_t(__VA_ARGS__)
#define nfos_map_get(...) nfos_map_get_t(__VA_ARGS__)
#else
#define nfos_map_allocate(...) nfos_map_allocate_debug_t(__VA_ARGS__, __FILE__, __LINE__)
#define nfos_map_put(...) nfos_map_put_debug_t(__VA_ARGS__)
#define nfos_map_erase(...) nfos_map_erase_debug_t(__VA_ARGS__)
#define nfos_map_get(...) nfos_map_get_debug_t(__VA_ARGS__)
#endif

//   Insert a key-value pair.
//   @param map - pointer to the map
//   @param key - key
//   @param value - value.
//   @returns 0 if insertion fails, and 1 if it succeeds.
//   NOTE: Do not insert a key again if it already exists in map.
//   TODO: Check this automatically in the put function
int nfos_map_put_t(struct NfosMap* map, void* key, int value);
int nfos_map_put_debug_t(struct NfosMap* map, void* key, int value);

//   Remove a key-value pair.
//   @param map - pointer to the map
//   @param key - key
//   @returns 0 if it fails, and 1 if succeeds.
int nfos_map_erase_t(struct NfosMap* map, void* key);
int nfos_map_erase_debug_t(struct NfosMap* map, void* key);
