#ifndef _DOUBLE_MAP_STUB_CONTROL_H_INCLUDED_
#define _DOUBLE_MAP_STUB_CONTROL_H_INCLUDED_
#include "libvig/models/str-descr.h"

typedef int entry_condition(void *key_a, void *key_b, int index, void *value,
                            void *state);

struct DoubleMap;

void dmap_set_entry_condition(struct DoubleMap *map, entry_condition *cond,
                              void *state);
//@ requires true;
//@ ensures true;

void dmap_set_layout(struct DoubleMap *map,
                     struct str_field_descr *key_a_fields, int key_a_count,
                     struct str_field_descr *key_b_fields, int key_b_count,
                     struct str_field_descr *value_fields, int value_count,
                     struct nested_field_descr *value_nested_fields,
                     int val_nests_count);
void dmap_reset(struct DoubleMap *map, int capacity);

#endif //_DOUBLE_MAP_STUB_CONTROL_H_INCLUDED_
