#pragma once

#include "timer.h"

#include "scalability-profiler.h"

struct NfosMeObj;

//   Custom function initializing an obj
typedef void (*nfos_me_obj_init_t)(void *obj);

//   Custom function updating an obj replica.
//   For example, if obj is an integer counter with initial value of 0,
//   this function is simply one line `*((int *)replica) += 1;`
typedef void (*nfos_me_obj_update_handler_t)(void *replica);

//   Custom function merging replica to obj.
//   For example, if obj is an integer counter with initial value of 0,
//   this function is simply one line `*((int *)obj) += *((int *)replica);`
//   @param obj - pointer to main object.
//   @param replica - pointer to a replica.
typedef void (*nfos_me_obj_merge_t)(void *obj, void *replica);

//   Allocate memory and initialize a new mergeable object(me_obj).
//   @param obj_size - the size of me_obj.
//   @param staleness_usec - the maximum staleness of the object returned by nfos_me_obj_read().
//   @param init_obj - custom initialization function of a me_obj
//   @param me_obj_out - an output pointer that will hold the pointer to the newly
//                      allocated me_obj in the case of success.
//   @returns 0 if the allocation failed, and 1 if the allocation is successful.
int nfos_me_obj_allocate_t(int obj_size, int64_t staleness_usec, nfos_me_obj_init_t init_obj,
                           struct NfosMeObj **me_obj_out);
int nfos_me_obj_allocate_debug_t(int obj_size, int64_t staleness_usec, nfos_me_obj_init_t init_obj,
                                 struct NfosMeObj **me_obj_out, const char *filename, int lineno);

//   Update a me_obj
//   @param me_obj - pointer to the me_obj
//   @param update_obj - custom me_obj update function.
int nfos_me_obj_update_t(struct NfosMeObj *me_obj, nfos_me_obj_update_handler_t update_obj);
int nfos_me_obj_update_debug_t(struct NfosMeObj *me_obj, nfos_me_obj_update_handler_t update_obj);

//   Read the value of a me_obj object after merging all replicas. The value read has bounded staleness.
//   @param me_obj - pointer to the me_obj
//   @param curr_ts - current time (returned by get_curr_time())
//   @param init_obj - custom initialization function of a me_obj
//   @param merge_obj - custom me_obj merge function.
//   @param obj_out - an output pointer that will hold the reference to the merged object.
int nfos_me_obj_read_t(struct NfosMeObj *me_obj, vigor_time_t curr_ts, nfos_me_obj_init_t init_obj, nfos_me_obj_merge_t merge_obj, void **obj_out);
int nfos_me_obj_read_debug_t(struct NfosMeObj *me_obj, vigor_time_t curr_ts, nfos_me_obj_init_t init_obj, nfos_me_obj_merge_t merge_obj, void **obj_out);

#ifndef SCALABILITY_PROFILER
#define nfos_me_obj_allocate(...) nfos_me_obj_allocate_t(__VA_ARGS__)
#define nfos_me_obj_read(...) nfos_me_obj_read_t(__VA_ARGS__)
#define nfos_me_obj_update(...) nfos_me_obj_update_t(__VA_ARGS__)
#else
#define nfos_me_obj_allocate(...) nfos_me_obj_allocate_debug_t(__VA_ARGS__, __FILE__, __LINE__)
#define nfos_me_obj_read(...) nfos_me_obj_read_debug_t(__VA_ARGS__)
#define nfos_me_obj_update(...) nfos_me_obj_update_debug_t(__VA_ARGS__)
#endif
