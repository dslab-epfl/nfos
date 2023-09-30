#include "vector.h"

#include <stdlib.h>

#include <rte_malloc.h>

#include "rlu-wrapper.h"

struct NfosVector {
  void **elems;
  int elem_size;
  unsigned capacity;
};

int nfos_vector_get_elem_size(struct NfosVector *vector){
  return vector->elem_size;
}

unsigned nfos_vector_get_capacity(struct NfosVector *vector){
  return vector->capacity;
}

int nfos_vector_allocate_t(int elem_size, unsigned capacity, 
            nfos_vector_init_elem_t init_elem, struct NfosVector **vector_out) {
  struct NfosVector *vector = (struct NfosVector *) malloc(sizeof(struct NfosVector));
  if (!vector) return 0;

  vector->elems = (void **) rte_malloc(NULL, sizeof(void *) * (uint64_t)capacity, 0);
  if (!vector->elems) {
    free(vector);
    return 0;
  }
  vector->elem_size = elem_size;
  vector->capacity = capacity;

  for (int i = 0; i < capacity; i++) {
    vector->elems[i] = RLU_ALLOC(elem_size);
    init_elem(vector->elems[i]);
  }

  *vector_out = vector;
  return 1;
}

int nfos_vector_allocate_debug_t(int elem_size, unsigned capacity, 
            nfos_vector_init_elem_t init_elem, struct NfosVector **vector_out,
            const char *filename, int lineno) {
  int ret = nfos_vector_allocate_t(elem_size, capacity, init_elem, vector_out);
  profiler_add_ds_inst((void *)(*vector_out), filename, lineno);
  return ret;
}

int nfos_vector_borrow_mut_t(struct NfosVector *vector, int index, void **val_out) {
  rlu_thread_data_t *rlu_data = get_rlu_thread_data();
  void *elem = vector->elems[index];
  if (!_mvrlu_try_lock(rlu_data, (void **)&elem, vector->elem_size)) {
    return ABORT_HANDLER;
  } else {
    *val_out = elem;
    return 1;
  }
}

int nfos_vector_borrow_mut_debug_t(struct NfosVector *vector, int index, void **val_out){
  profiler_add_ds_op_info(vector, 3, 0, &index, sizeof(index));

  int ret = nfos_vector_borrow_mut_t(vector, index, val_out);

  profiler_inc_curr_op();
  return ret;
}

void nfos_vector_borrow_t(struct NfosVector *vector, int index, void **val_out) {
  rlu_thread_data_t *rlu_data = get_rlu_thread_data();
  void *elem = vector->elems[index];
  elem = RLU_DEREF(rlu_data, elem);
  *val_out = elem;
}

void nfos_vector_borrow_debug_t(struct NfosVector *vector, int index, void **val_out) {
  profiler_add_ds_op_info(vector, 3, 1, &index, sizeof(index));

  nfos_vector_borrow_t(vector, index, val_out);

  profiler_inc_curr_op();
}

void nfos_vector_borrow_unsafe(struct NfosVector *vector, int index, void **val_out) {
  void *elem = vector->elems[index];
  *val_out = elem;
}
