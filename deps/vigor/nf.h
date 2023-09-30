#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "libvig/verified/vigor-time.h"

#define FLOOD_FRAME ((uint16_t) -1)

struct nf_config;

bool nf_init(void);
void nf_expire_allocated_ind(vigor_time_t now, int partition);
int nf_process(uint16_t device, uint8_t* buffer, uint16_t buffer_length, vigor_time_t now,
               uint16_t partition);

extern struct nf_config config;
void nf_config_init(int argc, char **argv);
void nf_config_usage(void);
void nf_config_print(void);

#ifdef KLEE_VERIFICATION
void nf_loop_iteration_border(unsigned lcore_id, vigor_time_t time);
#endif
