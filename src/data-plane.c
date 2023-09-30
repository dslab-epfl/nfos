#include <stdbool.h>
#include <string.h>

#include <rte_lcore.h>

#include "vigor/libvig/verified/vigor-time.h"
#include "vigor/nf-util.h"

#include "nf.h"
#include "data-plane.h"
#include "pkt-set-manager.h"
#include "rlu-wrapper.h"

#include "nf-log.h"

RTE_DEFINE_PER_LCORE(uint16_t, dst_device);
static pkt_handler_t *pkt_handlers;

bool _register_pkt_handlers(pkt_handler_t *handlers) {
  pkt_handlers = handlers;
  return true;
}

#define MAX_BATCH 32

/*
 * Utils for rolling back packet set state upon aborts
 */
#ifdef MUTABLE_PKT_SET_STATE
#ifdef PKT_PROCESS_BATCHING
typedef struct {
  uint16_t num_logged;
  pkt_set_state_t states[MAX_BATCH];
  pkt_set_state_t *states_main_copy[MAX_BATCH];
} pkt_set_state_log_t;

RTE_DEFINE_PER_LCORE(pkt_set_state_log_t, pkt_set_state_log);

static inline void reset_log_pkt_set_state() {
  pkt_set_state_log_t *log = &RTE_PER_LCORE(pkt_set_state_log);
  log->num_logged = 0;
}

static inline void log_one_pkt_set_state(pkt_set_state_t *state) {
  pkt_set_state_log_t *log = &RTE_PER_LCORE(pkt_set_state_log);
  uint16_t num_logged = log->num_logged;
  log->num_logged++;
  pkt_set_state_t *log_elem = log->states + num_logged;
  log->states_main_copy[num_logged] = state;
  memcpy(log_elem, state, sizeof(pkt_set_state_t));
}

static inline void rollback_pkt_set_states() {
  pkt_set_state_log_t *log = &RTE_PER_LCORE(pkt_set_state_log);
  uint16_t num_logged = log->num_logged;
  for (uint16_t i = 0; i < num_logged; i++) {
    pkt_set_state_t *logged = log->states + i;
    pkt_set_state_t *main_copy = log->states_main_copy[i];
    memcpy(main_copy, logged, sizeof(pkt_set_state_t));
  }
}

#else

RTE_DEFINE_PER_LCORE(pkt_set_state_t, pkt_set_state_log);

static inline void log_pkt_set_state(pkt_set_state_t *state) {
  pkt_set_state_t *log = &RTE_PER_LCORE(pkt_set_state_log);
  memcpy(log, state, sizeof(pkt_set_state_t));
}

static inline void rollback_pkt_set_state(pkt_set_state_t *state) {
  pkt_set_state_t *log = &RTE_PER_LCORE(pkt_set_state_log);
  memcpy(state, log, sizeof(pkt_set_state_t));
}
#endif
#endif


/*
 * Packet processing
 */
#ifdef PKT_PROCESS_BATCHING
uint16_t process_pkt(struct rte_mbuf **mbufs, uint16_t *dst_devices, uint16_t batch_size,
                     vigor_time_t now, uint16_t pkt_set_partition,
                     nf_state_t *non_pkt_set_state) {

  pkt_t packet[MAX_BATCH]; 
  pkt_set_id_t pkt_set_id[MAX_BATCH];
  bool parse_res[MAX_BATCH];
  bool has_pkt_set_state[MAX_BATCH];
  int pkt_class[MAX_BATCH];

  // All packets in a batch comes from the same device
  uint16_t device = mbufs[0]->port;

  // TODO: Stateless processing, should be able to vectorize it
  for (int i = 0; i < batch_size; i++) {
    dst_devices[i] = device;

    uint8_t *buffer = rte_pktmbuf_mtod(mbufs[i], uint8_t*);
    uint32_t pkt_len = (uint32_t)(mbufs[i]->data_len);
    packet_state_total_length(buffer, &pkt_len);
    // TODO: pass pkt_len explicitly to nf_pkt_parser
    packet[i].len = pkt_len;
    parse_res[i] = nf_pkt_parser(buffer, &packet[i]);
    nf_return_all_chunks(buffer);
    pkt_class[i] = nf_pkt_dispatcher(&packet[i], device, &pkt_set_id[i],
                                   &has_pkt_set_state[i], non_pkt_set_state);
  }

  // Stateful processing

  // Assumption here is that all packets in a batch either has pkt set state or not
  // Temp hack for Bridge/Maglev to do transaction chopping...
  if (!has_pkt_set_state[0]) {
    rlu_thread_data_t *rlu_data = get_rlu_thread_data();
no_pkt_set_restart:
    RLU_READER_LOCK(rlu_data);
    for (int i = 0; i < batch_size; i++) {
      if (parse_res[i]) {
        RTE_PER_LCORE(dst_device) = dst_devices[i];
        pkt_handler_t pkt_handler = pkt_handlers[pkt_class[i]];
        int handler_res = pkt_handler(non_pkt_set_state, &packet[i], device, NULL, &pkt_set_id[i]);
        if (handler_res == ABORT_HANDLER) {
          nfos_abort_txn(rlu_data);
          NF_DEBUG("ABORT: no_pkt_set handlers\n");
          goto no_pkt_set_restart;
        }
        dst_devices[i] = RTE_PER_LCORE(dst_device);
      }
    }
    if (!RLU_READER_UNLOCK(rlu_data)) {
      nfos_abort_txn(rlu_data);
      NF_DEBUG("ABORT: read validation\n");
      goto no_pkt_set_restart;
    }

  // Ensure batching is only applied to registered pkt sets.
  // unknown_pkt_set_handler is only safe to rerun if not batched
  } else {
    pkt_set_state_t *pkt_set_state[MAX_BATCH];
    int n = 0;
    bool reg_pkt_set;

    // get the first packet without parsing error
    while ( (n < batch_size) && (!parse_res[n]) ) { n++; }
    if (n < batch_size) {
      reg_pkt_set = get_pkt_set_state(&pkt_set_id[n], &pkt_set_state[n], pkt_set_partition, now);
      __builtin_prefetch(pkt_set_state[n]);
    }

    // For now assume none of the handlers use try_lock/_const,
    // we need to enable nested try-locking again later.
    rlu_thread_data_t *rlu_data = get_rlu_thread_data();
    while (n < batch_size) {
      if (!reg_pkt_set) {
        // reset dst_device to drop a pkt by default
        RTE_PER_LCORE(dst_device) = device;
unknown_restart:
        RLU_READER_LOCK(rlu_data);

        add_pkt_set_log_clear();
        int handler_res = nf_unknown_pkt_set_handler(non_pkt_set_state, &packet[n], device, &pkt_set_id[n]);
        if (handler_res == ABORT_HANDLER) {
          nfos_abort_txn(rlu_data);
          NF_DEBUG("ABORT: handlers\n");
          goto unknown_restart;
        }
        if (!RLU_READER_UNLOCK(rlu_data)) {
          nfos_abort_txn(rlu_data);
          NF_DEBUG("ABORT: read validation\n");
          goto unknown_restart;
        }
        add_pkt_set_commit(&pkt_set_id[n], pkt_set_partition, now);

        dst_devices[n] = RTE_PER_LCORE(dst_device);

        // get the next packet without parsing error
        do { n++; } while ( (n < batch_size) && (!parse_res[n]) );
        if (n < batch_size) {
          reg_pkt_set = get_pkt_set_state(&pkt_set_id[n], &pkt_set_state[n], pkt_set_partition, now);
          __builtin_prefetch(pkt_set_state[n]);
        }


      } else {
        // Determine the index range of next pkts belonging to registered pkt sets
        int start_n = n;
        do {
          // get the next packet without parsing error
          do { n++; } while ( (n < batch_size) && (!parse_res[n]) );
          if (n < batch_size) {
            reg_pkt_set = get_pkt_set_state(&pkt_set_id[n], &pkt_set_state[n], pkt_set_partition, now);
            __builtin_prefetch(pkt_set_state[n]);
          } else {
            reg_pkt_set = false;
          }
        // finish the critical section for pkt_handlers when
        // - next pkt belongs to unknown pkt sets
        // or
        // - the last pkt in the batch is processed
        } while (reg_pkt_set);
        int end_n = n;

        // Run the pkt_handlers for this range
restart:
        n = start_n;
#ifdef MUTABLE_PKT_SET_STATE
        reset_log_pkt_set_state();
#endif
        RLU_READER_LOCK(rlu_data);
        do {
#ifdef MUTABLE_PKT_SET_STATE
          log_one_pkt_set_state(pkt_set_state[n]);
#endif
          // reset dst_device to drop a pkt by default
          RTE_PER_LCORE(dst_device) = device;
          pkt_handler_t pkt_handler = pkt_handlers[pkt_class[n]];
          int handler_res = pkt_handler(non_pkt_set_state, &packet[n], device, pkt_set_state[n], &pkt_set_id[n]);
          if (handler_res == ABORT_HANDLER) {
            nfos_abort_txn(rlu_data);
#ifdef MUTABLE_PKT_SET_STATE
            rollback_pkt_set_states();
#endif
            NF_DEBUG("ABORT: handlers\n");
            goto restart;
          }
          dst_devices[n] = RTE_PER_LCORE(dst_device);

          // get the next packet without parsing error
          do { n++; } while ( (n < batch_size) && (!parse_res[n]) );
        } while (n < end_n);
        if (!RLU_READER_UNLOCK(rlu_data)) {
          nfos_abort_txn(rlu_data);
#ifdef MUTABLE_PKT_SET_STATE
          rollback_pkt_set_states();
#endif
          NF_DEBUG("ABORT: read validation\n");
          goto restart;
        }

      }

    }

  }

}

#else

uint16_t process_pkt(uint16_t device, uint8_t* buffer, uint16_t buffer_length,
                  vigor_time_t now, uint16_t pkt_set_partition,
                  nf_state_t *non_pkt_set_state) {
  
  RTE_PER_LCORE(dst_device) = device;
  pkt_t packet;
  pkt_set_id_t pkt_set_id;

  rlu_thread_data_t *rlu_data = get_rlu_thread_data();

  // temp hack
  uint32_t buffer_length_u32 = buffer_length;
  packet_state_total_length(buffer, &buffer_length_u32);
  // TODO: pass pkt_len explicitly to nf_pkt_parser
  packet.len = buffer_length;
  bool parse_res = nf_pkt_parser(buffer, &packet);
  nf_return_all_chunks(buffer);

  if (parse_res) {
    bool has_pkt_set_state;
    int pkt_class = nf_pkt_dispatcher(&packet, device, &pkt_set_id,
                                   &has_pkt_set_state, non_pkt_set_state);
    pkt_handler_t pkt_handler = pkt_handlers[pkt_class];

    if (!has_pkt_set_state) {
no_pkt_set_restart:
      RLU_READER_LOCK(rlu_data);
      if (pkt_handler(non_pkt_set_state, &packet, device, NULL, &pkt_set_id) == ABORT_HANDLER) {
        nfos_abort_txn(rlu_data);
        NF_DEBUG("ABORT: pkt_handler\n");
        goto no_pkt_set_restart;
      }
      if (!RLU_READER_UNLOCK(rlu_data)) {
        nfos_abort_txn(rlu_data);
        NF_DEBUG("ABORT: read validation\n");
        goto no_pkt_set_restart;
      }

    } else {
      pkt_set_state_t *pkt_set_state;
      if (get_pkt_set_state(&pkt_set_id, &pkt_set_state, pkt_set_partition, now)) {
        __builtin_prefetch(pkt_set_state);

restart_second:
#ifdef MUTABLE_PKT_SET_STATE
        log_pkt_set_state(pkt_set_state);
#endif
	      RLU_READER_LOCK(rlu_data);
        if (pkt_handler(non_pkt_set_state, &packet, device, pkt_set_state, &pkt_set_id) == ABORT_HANDLER) {
          nfos_abort_txn(rlu_data);
#ifdef MUTABLE_PKT_SET_STATE
          // TODO: merging this into nfos_abort_txn
          rollback_pkt_set_state(pkt_set_state);
#endif
          NF_DEBUG("ABORT: pkt_handler\n");
          goto restart_second;
        }
        if (!RLU_READER_UNLOCK(rlu_data)) {
          nfos_abort_txn(rlu_data);
#ifdef MUTABLE_PKT_SET_STATE
          rollback_pkt_set_state(pkt_set_state);
#endif
          NF_DEBUG("ABORT: read validation\n");
          goto restart_second;
        }

      } else {

restart_third:
	      RLU_READER_LOCK(rlu_data);

        add_pkt_set_log_clear();
        if (nf_unknown_pkt_set_handler(non_pkt_set_state, &packet, device, &pkt_set_id) == ABORT_HANDLER) {
          nfos_abort_txn(rlu_data);
          NF_DEBUG("ABORT: nf_unknown_pkt_set_handler\n");
          goto restart_third;
        }
        if (!RLU_READER_UNLOCK(rlu_data)) {
          nfos_abort_txn(rlu_data);
          NF_DEBUG("ABORT: read validation\n");
          goto restart_third;
        }
        add_pkt_set_commit(&pkt_set_id, pkt_set_partition, now);

      }
    }

  }

  return RTE_PER_LCORE(dst_device);
}
#endif

void send_pkt(pkt_t *pkt, uint16_t dev) {
  RTE_PER_LCORE(dst_device) = dev;
}

void flood_pkt(pkt_t *pkt) {
  RTE_PER_LCORE(dst_device) = FLOOD_PORT;
}

void drop_pkt(pkt_t *pkt) {}
