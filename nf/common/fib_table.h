#pragma once
#ifndef __FIB_TABLE_H__
#define __FIB_TABLE_H__
#include "mtrie.h"
#include "vector.h"
// WARNING: using nfos_vector_borrow_unsafe instead of nfos_vector_borrow
// since the latter will cause SEG_FAULT if called in the nf_init when the threads
// are not initialized.
typedef struct ip4_fib_t_
{
    /*Forwarding FIB Table (organized in mtrie)*/
    ip4_fib_mtrie_t mtrie;
    /*The index of the FIB table. Each device should have 1 FIB table*/
    uint32_t index;
} ip4_fib_t;
extern struct NfosVector *ip4_fibs;
extern uint32_t fib_table_index;

static inline ip4_fib_t*
ip4_fib_get(uint32_t fib_index){
    ip4_fib_t* fib_table;
    nfos_vector_borrow_unsafe(ip4_fibs, fib_index, (void **)(&fib_table));
    return fib_table;
}
#endif
