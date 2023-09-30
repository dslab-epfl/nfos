/* !! Note: the part marked as "for NFOS dev" are internal comments for
 *    NFOS devs and are supposed to be invisible to the NF developers.
 * 
 */

/*
 * Note: you need to return -1 from the handlers immediately when any call to the
 * NFOS data structures (for access global state) returns -1
 * 
 * Note: ignore related_pkt_set stuff... Not needed anymore.
 */


#pragma once

#include <stdint.h>
#include <stdbool.h>

#include "vigor/nf-util.h"
#include "vigor/nf-log.h"
#include "vigor/libvig/verified/vigor-time.h"

// Type of a packet
typedef struct pkt {
  uint16_t len;
  struct rte_ether_hdr *ether_header;
  struct rte_ipv4_hdr *ipv4_header;
  struct tcpudp_hdr *tcpudp_header;
  uint8_t *payload;
  // Ugly. Todo: separate this from the headers
  uint8_t *raw;
} pkt_t;

/*
 * Opaque type for packet set id, you need to declare the actual type.
 * 
 * Declare this only if the NF has packet sets.
 */
typedef struct pkt_set_id pkt_set_id_t;

/*
 * Opaque type for packet set state, you need to declare the actual type.
 * 
 * Declare this only if the NF has packet sets.
 */
typedef struct pkt_set_state pkt_set_state_t;

/*
 * You need to declare the actual type of struct pkt_set_id here
 *
 * You need to declare the actual type of struct pkt_set_state here
 * 
 * You need to declare the following pkt set callbacks here:
 * pkt_set_id_eq, pkt_set_id_hash.
 * TODO: autogen these callbacks automatically from pkt_set_id def as in vigor
 * 
 * To NFOS dev:
 * NFOS distributes packet sets among data plane cores based on a hash computed
 * from the packet set id returned by the pkt_dispatcher(). It needs to know
 * the actual type of "struct pkt_set_id" to compute the hash, thus the actual type
 * of struct pkt_set_id needs to be included in the .c file where the hash computation
 * is implemented.
 */
#include <pkt_set.h>

#include <nf_nfos_config.h>

/* 
 * Opaque type for global state, you need to declare the actual type.
 * Note: you should use NFOS state interfaces to access the global state.
 */
typedef struct nf_state nf_state_t;


/* Handlers that you need to define for your NF */

/*
 * Function that initializes the global state of the NF.
 * 
 * Returns the initialized global state of the NF, NULL if intialization fails.
 * 
 * validity_duration_out: validity duration of packet sets (in us).
 * 
 * lcores_out: string of list of cores to run on.  The format is <c1>[-c2][,c3[-c4],...] where
 * c1, c2, etc are core indexes between 0 and 128.
 * 
 * has_related_pkt_sets_out: Temp hack to support NFs that allow two packet sets to map to the
 * same packet set state, e.g., NAT.
 * 
 * do_expiration_out: true if packet sets exist, false otherwise
 * 
 * has_pkt_sets_out: true if packet sets exist, false otherwise
 * 
 * Note: Hardcode the configurations of your NF in this function... We
 * will add support for passing configuration to NF in the future.
 */
nf_state_t *nf_init(vigor_time_t *validity_duration_out, char **lcores_out, bool *has_related_pkt_sets_out,
                    bool *do_expiration_out, bool *has_pkt_sets_out);

/* 
 * Pure function that parses the raw packet "buffer" to "pkt".
 *
 * Set a header/payload pointer in "pkt" to NULL if the NF does not need it.
 * E.g., set the ipv4/tcpudp/payload pointer to NULL if you are coding a bridge.
 * 
 * Return true if parsing succeeds, and false if the packet uses protocol not supported
 * by the NF. NFOS drops the packet if parsing fails.
 */
bool nf_pkt_parser(uint8_t *buffer, pkt_t *pkt);

/*
 * Pure function that maps a packet to its packet set or mark it as an orphan packet
 * based on the header contents and incoming device.
 * 
 * Returns index to the array of packet handlers.
 * 
 * The 3rd arg stores the computed packet set id, ignore it for orphan packet.
 * 
 * Set the 4th arg to false if the packet is orphan packet, and true if it belongs
 * to a packet set.
 * 
 * Note: The only global state you can access in this function is the category
 * of ethernet devices, e.g., WAN or LAN.
 * 
 * To NFOS dev: The global state is used for NFs that computes packet set ID
 * differently when the packet is received on different type of devices
 * (e.g., WAN vs. LAN). This does not prevent us from offloading the
 * distribution of packet sets among cores to the NIC since the computation
 * of packet set id is stateless for each single type of devices
 * 
 */
int nf_pkt_dispatcher(const pkt_t *pkt, uint16_t incoming_dev, pkt_set_id_t *pkt_set_id, bool *has_pkt_set_state, nf_state_t *global_state);


// TODO: make the following two symbols weak symbols and provide default impl in NFOS?
/*
 * Process a packet of unregistered packet set and possibly register the packet set.
 * 
 * The second arg of the function is pointer to the packet to be processed.
 * This handler should update the packet in place, i.e., the second arg points
 * to the processed packet when the handler finishes.
 * 
 * Ignore the argument global_state if the NF does not have global state.
 * 
 * Leave the handler as empty if the NF does not have packet sets.
 * 
 * Return -1 on abort, 0 on success 
 */
int nf_unknown_pkt_set_handler(nf_state_t *global_state, pkt_t *pkt, uint16_t incoming_dev, pkt_set_id_t *pkt_set_id);

/*
 * Handles an expired packet set, e.g., freeing resources
 * shared by all packet sets.
 * 
 * Ignore the argument global_state if the NF does not have global state.
 * 
 * Leave the handler as empty if the NF does not have packet sets.
 * 
 * Return -1 on abort, 0 on success 
 */
int nf_expired_pkt_set_handler(nf_state_t *global_state, pkt_set_state_t *local_state);

/*
 * Function type of handlers for (1) packets of a registered packet set or (2) orphan packets.
 *
 * You need to register the corresponding handlers (one for packet sets and/or one for orphan packets)
 * thru the register_pkt_handlers() NFOS API.
 * 
 * The second arg of the function is pointer to the packet to be processed.
 * This handler should update the packet in place, i.e., the second arg points
 * to the processed packet when the handler finishes.
 * 
 * Ignore the argument local_state & pkt_set_id if the handler is for orphan packets.
 * 
 * Ignore the argument global_state if the NF does not have global state.
 * 
 * Return -1 on abort, 0 on success 
 */
typedef int (*pkt_handler_t)(nf_state_t *global_state, pkt_t *pkt, uint16_t incoming_dev, pkt_set_state_t *local_state, pkt_set_id_t *pkt_set_id);


// Interface for sending a packet through a single ethernet port.
void send_pkt(pkt_t *pkt, uint16_t dev);

// Interface for flooding a packet to all ethernet device.
void flood_pkt(pkt_t *pkt);

// Interface for dropping a packet.
void drop_pkt(pkt_t *pkt);

// Interface for getting current time (in rdtsc cycles)
vigor_time_t get_curr_time();

typedef void (*periodic_handler_t)(nf_state_t *global_state);

/*
 * Interface for registering control plane handlers that
 * need to run periodically, e.g., backend expirator in Maglev.
 * 
 * period_len => period in micro-seconds.
 * 
 * Returns true if operation succeeds and false if it fails.
 */
bool register_periodic_handler(uint64_t period_len, periodic_handler_t handler);

/*
 * Interface for registering pkt handlers
 *
 * handlers => array of packet handlers (one for packet sets and/or one for orphan packets)
 * 
 * Returns true if operation succeeds and false if it fails.
 */
bool register_pkt_handlers(pkt_handler_t *handlers);

/*
 * Adds the mapping from the ID of a packet set to its packet set state.
 *
 * local_state points to the local state of the new packet set.
 * 
 * Returns true if operation succeeds and false if it fails.
 */
bool add_pkt_set(pkt_set_id_t *pkt_set_id, pkt_set_state_t *local_state,
                 pkt_set_id_t *related_pkt_set_id);
