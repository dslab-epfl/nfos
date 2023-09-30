#include "rand-gen.h"
#include "zipf.h"

#include <stdlib.h>
#include <stdbool.h>
#include <rte_lcore.h>
#include "rlu-wrapper.h"

typedef struct rand_seq {
  int id;
  int vals[RAND_SEQ_LEN];
} __attribute__ ((aligned (64))) rand_seq_t;

static rand_seq_t *rand_seqs;

static struct zipf_state zs;

void init_rand_seqs(double zipf_factor, int range) {
  int num_workers = rte_lcore_count() - 1;
  rand_seqs = malloc(sizeof(rand_seq_t) * num_workers);
  bool zipf = true;

  if (zipf_factor == (double)0)
    zipf = false;
  
  if (!zipf) {
    srand(42);
  } else {
    zipf_init(&zs, range, zipf_factor, 42);
  }

  for (int i = 0; i < num_workers; i++) {
    rand_seq_t *rand_seq = rand_seqs + i;
    rand_seq->id = 0;

    for (int j = 0; j < RAND_SEQ_LEN; j++) {
      if (!zipf) {
        rand_seq->vals[j] = rand() % range;
      } else {
        rand_seq->vals[j] = zipf_next(&zs) % range;
      }
    }
  }

}

int get_rand_num() {
  int thr_id = get_rlu_thread_id();
  rand_seq_t *rand_seq = rand_seqs + thr_id;
  int ret = rand_seq->vals[rand_seq->id];

  return ret;
}

// temp hack to avoid updating seq id if abort
void update_rand_num_id() {
  int thr_id = get_rlu_thread_id();
  rand_seq_t *rand_seq = rand_seqs + thr_id;
  rand_seq->id = (rand_seq->id + 1) % RAND_SEQ_LEN;
}
