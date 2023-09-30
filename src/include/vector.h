#pragma once
#include "scalability-profiler.h"

struct NfosVector;

typedef void (*nfos_vector_init_elem_t)(void *elem);
#ifndef SCALABILITY_PROFILER
#define nfos_vector_allocate(...) nfos_vector_allocate_t(__VA_ARGS__)
#define nfos_vector_borrow_mut(...) nfos_vector_borrow_mut_t(__VA_ARGS__)
#define nfos_vector_borrow(...) nfos_vector_borrow_t(__VA_ARGS__)
#else
#define nfos_vector_allocate(...) nfos_vector_allocate_debug_t(__VA_ARGS__, __FILE__, __LINE__)
#define nfos_vector_borrow_mut(...) nfos_vector_borrow_mut_debug_t(__VA_ARGS__)
#define nfos_vector_borrow(...) nfos_vector_borrow_debug_t(__VA_ARGS__)
#endif

//   Allocate memory and initialize a new vector.
//   @param elem_size - the size of vector element.
//   @param capacity - number of elements
//   @param init_elem - initialization function of an elem
//   @param vector_out - an output pointer that will hold the pointer to the newly
//                      allocated vector in the case of success.
//   @returns 0 if the allocation failed, and 1 if the allocation is successful.
int nfos_vector_allocate_t(int elem_size, unsigned capacity, 
            nfos_vector_init_elem_t init_elem, struct NfosVector **vector_out);
int nfos_vector_allocate_debug_t(int elem_size, unsigned capacity, 
            nfos_vector_init_elem_t init_elem, struct NfosVector **vector_out,
            const char *filename, int lineno);

//   Get a mutable reference to a vector element
//   @param vector - pointer to the vector
//   @param index  - index of the element
//   @param val_out - an output pointer that will hold the element reference.
int nfos_vector_borrow_mut_t(struct NfosVector *vector, int index, void **val_out);
int nfos_vector_borrow_mut_debug_t(struct NfosVector *vector, int index, void **val_out);

//   Get a immutable reference to a vector element
//   @param vector - pointer to the vector
//   @param index  - index of the element
//   @param val_out - an output pointer that will hold the element reference.
void nfos_vector_borrow_t(struct NfosVector *vector, int index, void **val_out);
void nfos_vector_borrow_debug_t(struct NfosVector *vector, int index, void **val_out);

// (Internal APIs, do not use)
void nfos_vector_borrow_unsafe(struct NfosVector *vector, int index, void **val_out);
int nfos_vector_get_elem_size(struct NfosVector *vector);
unsigned nfos_vector_get_capacity(struct NfosVector *vector);
