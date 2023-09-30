#pragma once

#include "nf.h"
#include "vigor/libvig/verified/vigor-time.h"

struct concurrent_dchain_cell {
    int prev;
    int next;
    int list_ind; // ind of corresponding al_list, -1 means the cell is free
    vigor_time_t time;
    pkt_set_id_t id;
    int map_chain_next; // index of next cell in the same map chain, -1 means map chain tail
};

// Separate free/alloc list head cells with 7 (2^3 - 1) padding cells
#define LIST_HEAD_PADDING 3

// Requires the array dchain_cell, large enough to fit all the range of
// possible 'index' values + 2 special values.
// Forms a two closed linked lists inside the array.
// First list represents the "free" cells. It is a single linked list.
// Initially the whole array
// (except 2 special cells holding metadata) added to the "free" list.
// Second list represents the "occupied" cells and it is double-linked,
// the order matters.
// It is supposed to store the ordered sequence, and support moving any
// element to the top.
//
// The lists are organized as follows:
//              +----+   +---+   +-------------------+   +-----
//              |    V   |   V   |                   V   |
//  [. + .][    .]  {    .} {    .} {. + .} {. + .} {    .} ....
//   ^   ^                           ^   ^   ^   ^
//   |   |                           |   |   |   |
//   |   +---------------------------+   +---+   +-------------
//   +---------------------------------------------------------
//
// Where {    .} is an "free" list cell, and {. + .} is an "alloc" list cell,
// and dots represent prev/next fields.
// [] - denote the special cells - the ones that are always kept in the
// corresponding lists.
// Empty "alloc" and "free" lists look like this:
//
//   +---+   +---+
//   V   V   V   |
//  [. + .] [    .]
//
// , i.e. cells[0].next == 0 && cells[0].prev == 0 for the "alloc" list, and
// cells[1].next == 1 for the free list.
// For any cell in the "alloc" list, 'prev' and 'next' fields must be different.
// Any cell in the "free" list, in contrast, have 'prev' and 'next' equal;
// After initialization, any cell is allways on one and only one of these lists.

// #define NUM_AL_LISTS (128)
// #define DCHAIN_RESERVED (NUM_AL_LISTS + 1)


void concurrent_dchain_impl_init(struct concurrent_dchain_cell *cells, int index_range, int num_cores);

int concurrent_dchain_impl_has_free_indexes(struct concurrent_dchain_cell *cells, int core_id);

int concurrent_dchain_impl_allocate_new_index(struct concurrent_dchain_cell *cells, int *index, int core_id, vigor_time_t time);

int concurrent_dchain_impl_allocate_new_index_global(struct concurrent_dchain_cell *cells, int *index, int core_id, vigor_time_t time);

int concurrent_dchain_impl_free_index(struct concurrent_dchain_cell *cells, int index, int core_id);

int concurrent_dchain_impl_get_oldest_index(struct concurrent_dchain_cell *cells, int *index, int core_id);

int concurrent_dchain_impl_rejuvenate_index(struct concurrent_dchain_cell *cells, int index, int core_id, vigor_time_t time);

int concurrent_dchain_impl_is_index_allocated(struct concurrent_dchain_cell *cells, int index, int core_id);

struct concurrent_dchain_cell *concurrent_dchain_impl_cell_out(struct concurrent_dchain_cell *cells, int index);
