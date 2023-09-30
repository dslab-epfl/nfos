#pragma once

#include <stdbool.h>

#define RAND_SEQ_LEN 10000

void init_rand_seqs(double zipf_factor, int range); 
int get_rand_num();
void update_rand_num_id();
