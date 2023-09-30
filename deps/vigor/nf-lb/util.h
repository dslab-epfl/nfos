#ifndef NF_LB_UTIL_H
#define NF_LB_UTIL_H

#include <stdint.h>

#include <rte_ethdev.h>
#include <rte_flow.h>

/* convenient funcs for NIC xstats */
struct nf_xstats {
    struct rte_eth_xstat_name* names;
    uint64_t* ids;
    uint64_t* prev_stats;
    uint64_t* stats;
    int len;
    uint16_t port_id;
};

void nf_init_xstats(struct nf_xstats* xstats, uint16_t port_id,
                    char** raw_names, int len);

void nf_destroy_xstats(struct nf_xstats* xstats);

void nf_update_xstats(struct nf_xstats* xstats);

void nf_show_xstats(struct nf_xstats* xstats, uint64_t duration);

void nf_show_xstats_abs(struct nf_xstats* xstats, uint64_t duration);


// Set up a flow rule using dst-udp-port-related matching pattern
//
// More generic framework for creating a flow rule could simply be taken
// from DPDK's testpmd
struct rte_flow* generate_dst_udp_flow(uint16_t port_id, uint8_t priority,
                                       uint16_t dst_udp_spec, uint16_t dst_udp_mask,
                                       uint16_t rx_q);

// Generate RSS flow
struct rte_flow* generate_rss_flow(uint16_t port_id, uint8_t priority,
                                   uint16_t* reta, uint32_t reta_sz);

struct rte_flow* generate_jump_flow(uint16_t port_id);

// Delete arbitrary flow
int delete_flow(uint16_t port_id, struct rte_flow* flow);

#endif