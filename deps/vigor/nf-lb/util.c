#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

#include <rte_ethdev.h>
#include <rte_common.h>
#include <rte_flow.h>
#include <rte_byteorder.h>

#include "nf-lb/util.h"

#define MAX_PATTERN_NUM		4
#define MAX_ACTION_NUM		4

/* convenient funcs for NIC xstats */

void nf_init_xstats(struct nf_xstats* xstats, uint16_t port_id,
                    char** raw_names, int len)
{
    int ret, actual_len;
    uint64_t *stats, *prev_stats, *ids;
    struct rte_eth_xstat_name* names;

    // get all xstats if len == 0
    // actual_len => hack variable used to alloc memory for names & ids
    if (!len) {
        // get num of xstats
        actual_len = rte_eth_xstats_get(port_id, NULL, 0);
        if (actual_len < 0)
            rte_exit(EXIT_FAILURE,
                    "rte_eth_xstats_get(%u) failed: %d", port_id,
                    actual_len);
    } else {
        actual_len = len;
    }
    // init names & ids of xstats
    names = calloc(actual_len, sizeof(*names));
    if (names == NULL) {
        rte_exit(EXIT_FAILURE,
                "Failed to calloc memory for names");
    }
    ids = calloc(actual_len, sizeof(*ids));


    // retrieve all stats if len == 0
    if (!len) {
        len = actual_len;

        ret = rte_eth_xstats_get_names(port_id, names, len);
        if (ret < 0 || ret > len) {
            free(names);
            rte_exit(EXIT_FAILURE,
                    "rte_eth_xstats_get_names(%u) len%i failed: %d",
                    port_id, len, ret);
        }

        for (int i = 0; i < len; i++)
            ids[i] = i;

    } else {
        int ind = 0;
        for (int i = 0; i < len; i++) {
            if (strlen(raw_names[i]) < RTE_ETH_XSTATS_NAME_SIZE) {
                uint64_t id;
                if (rte_eth_xstats_get_id_by_name(port_id, raw_names[i], &id)) {
                    printf("stats %s not available on port %d\n",
                            raw_names[i], port_id);
                } else {
                    strcpy(names[ind].name, raw_names[i]);
                    ids[ind] = id;
                    ind++;
                }
            }
        }
        len = ind;
    }

    // get initial xstats
    stats = calloc(len, sizeof(*stats));
    if (stats == NULL)
        rte_exit(EXIT_FAILURE,
                "Failed to calloc memory for stats");
    ret = rte_eth_xstats_get_by_id(port_id, ids, stats, len);
    if (ret < 0 || ret > len) {
        free(stats);
        rte_exit(EXIT_FAILURE,
                "rte_eth_xstats_get_by_id(%u) len%i failed: %d",
                port_id, len, ret);
    }

    // copy xstats to prev_stats
    prev_stats = calloc(len, sizeof(*prev_stats));
    if (prev_stats == NULL)
        rte_exit(EXIT_FAILURE,
                "Failed to calloc memory for prev_stats");
    memcpy(prev_stats, stats, len * sizeof(*stats));

    xstats->names = names;
    xstats->ids = ids;
    xstats->stats = stats;
    xstats->prev_stats = prev_stats;
    xstats->len = len;
    xstats->port_id = port_id;
}

void nf_destroy_xstats(struct nf_xstats* xstats)
{
    free(xstats->names);
    free(xstats->ids);
    free(xstats->stats);
    free(xstats->prev_stats);
}

void nf_update_xstats(struct nf_xstats* xstats)
{
    int ret;

    memcpy(xstats->prev_stats, xstats->stats, xstats->len * sizeof(*(xstats->stats)));
    ret = rte_eth_xstats_get_by_id(xstats->port_id, xstats->ids,
                                   xstats->stats, xstats->len);
    if (ret < 0 || ret > xstats->len) {
        rte_exit(EXIT_FAILURE,
                "rte_eth_xstats_get_by_id(%u) len%i failed: %d",
                xstats->port_id, xstats->len, ret);
    }
}

void nf_show_xstats(struct nf_xstats* xstats, uint64_t duration)
{
    static const char* stats_border = "_______";

    printf("PORT STATISTICS:\n================\n");
    for (int i = 0; i < xstats->len; i++) {
        printf("Port %u: %s %s:\t\t%"PRIu64"\n",
                xstats->port_id, stats_border,
                (xstats->names)[i].name,
                ((xstats->stats)[i] - (xstats->prev_stats)[i]) / duration);
    }
    fflush(stdout);
}

void nf_show_xstats_abs(struct nf_xstats* xstats, uint64_t duration)
{
    static const char* stats_border = "_______";

    printf("PORT STATISTICS ABS:\n================\n");
    for (int i = 0; i < xstats->len; i++) {
        printf("Port %u: %s %s:\t\t%"PRIu64"\n",
                xstats->port_id, stats_border,
                (xstats->names)[i].name,
                (xstats->stats)[i]);
    }
    fflush(stdout);
}

/* rte_flow utils */

// Set up a flow rule using dst-udp-port-related matching pattern
//
// More generic framework for creating a flow rule could simply be taken
// from DPDK's testpmd
struct rte_flow*
generate_dst_udp_flow(uint16_t port_id, uint8_t priority,
                      uint16_t dst_udp_spec, uint16_t dst_udp_mask,
                      uint16_t rx_q)
{
    struct rte_flow_attr attr;
    struct rte_flow_item pattern[MAX_PATTERN_NUM];
    struct rte_flow_action action[MAX_ACTION_NUM];
    struct rte_flow *flow = NULL;
    struct rte_flow_action_queue queue = { .index = rx_q };
    struct rte_flow_item_udp udp_spec;
    struct rte_flow_item_udp udp_mask;
    memset(pattern, 0, sizeof(pattern));
    memset(action, 0, sizeof(action));

    /*
     * set the rule attribute.
     * in this case only ingress packets will be checked.
     */
    memset(&attr, 0, sizeof(struct rte_flow_attr));
    attr.ingress = 1;
#ifdef GROUP_ONE
    attr.group = 1;
#else
    attr.group = 0;
#endif
    attr.priority = priority;

    /*
     * create the action sequence.
     * one action only,  move packet to queue
     */
    action[0].type = RTE_FLOW_ACTION_TYPE_QUEUE;
    action[0].conf = &queue;
    action[1].type = RTE_FLOW_ACTION_TYPE_END;

    /*
     * set the first level of the pattern (ETH).
     * since in this example we just want to get the
     * ipv4 we set this level to allow all.
     */
    pattern[0].type = RTE_FLOW_ITEM_TYPE_ETH;

    /*
     * second level (IP)
     * only accept ipv4 packets
     */
    pattern[1].type = RTE_FLOW_ITEM_TYPE_IPV4;

    /*
     * Third level (UDP)
     * 
     */
    memset(&udp_spec, 0, sizeof(struct rte_flow_item_udp));
    memset(&udp_mask, 0, sizeof(struct rte_flow_item_udp));
    udp_spec.hdr.dst_port = rte_cpu_to_be_16(dst_udp_spec);
    udp_mask.hdr.dst_port = rte_cpu_to_be_16(dst_udp_mask);
    pattern[2].type = RTE_FLOW_ITEM_TYPE_UDP;
    pattern[2].spec = &udp_spec;
    pattern[2].mask = &udp_mask;

    /* the final level must be always type end */
    pattern[3].type = RTE_FLOW_ITEM_TYPE_END;

    struct rte_flow_error error;
    // int res = rte_flow_validate(port_id, &attr, pattern, action, &error);
    // if(!res)
    flow = rte_flow_create(port_id, &attr, pattern, action, &error);

	  if (!flow) {
	  	  printf("UDP flow can't be created %d message: %s\n",
	  		       error.type,
	  		       error.message ? error.message : "(no stated reason)");
	  	  rte_exit(EXIT_FAILURE, "error in creating flow");
	  } else {
        // printf("UDP flow created! priority:%d  dst. port spec:%d  "
        //        "dst. port mask:%d  rxq:%d\n", priority, dst_udp_spec,
        //         dst_udp_mask, rx_q);
    }

    return flow;
}

// Generate RSS flow
struct rte_flow* generate_rss_flow(uint16_t port_id, uint8_t priority,
                                   uint16_t* reta, uint32_t reta_sz) {
    struct rte_flow_attr attr;
    struct rte_flow_item pattern[2];
    struct rte_flow_action action[2];
    struct rte_flow *flow = NULL;
    struct rte_flow_action_rss rss;
    struct rte_eth_dev_info dev_info;
    // rte_eth_dev_info_get(port_id, &dev_info);

    memset(pattern, 0, sizeof(pattern));
    memset(action, 0, sizeof(action));

    /*
     * set the rule attribute.
     * in this case only ingress packets will be checked.
     */
    memset(&attr, 0, sizeof(struct rte_flow_attr));
    attr.ingress = 1;
#ifdef GROUP_ONE
    attr.group = 1;
#else
    attr.group = 0;
#endif
    attr.priority = priority;

    /*
     * create the action sequence.
     * one action only,  spread packet using RSS
     */
    memset(&rss, 0, sizeof(rss));
    // rss.types = ETH_RSS_UDP & dev_info.flow_type_rss_offloads;
    rss.types = ETH_RSS_UDP;

    rss.key_len = 40;
    uint8_t rss_keys[40];
    for (int i = 0; i < 40; i++)
        rss_keys[i] = i;
    rss.key = rss_keys;

    rss.queue_num = reta_sz;
    rss.queue = reta;

    rss.level = 0;
    rss.func = RTE_ETH_HASH_FUNCTION_DEFAULT;

    action[0].type = RTE_FLOW_ACTION_TYPE_RSS;
    action[0].conf = &rss;
    action[1].type = RTE_FLOW_ACTION_TYPE_END;

    /*
     * set the first level of the pattern (ETH).
     * since in this example we just want to get the
     * ipv4 we set this level to allow all.
     */
    pattern[0].type = RTE_FLOW_ITEM_TYPE_ETH;

    /* the final level must be always type end */
    pattern[1].type = RTE_FLOW_ITEM_TYPE_END;

    struct rte_flow_error error;
    // int res = rte_flow_validate(port_id, &attr, pattern, action, &error);

    // uint64_t timer_start = rte_get_tsc_cycles();
    // if(!res)
    flow = rte_flow_create(port_id, &attr, pattern, action, &error);

    // uint64_t timer_val = rte_get_tsc_cycles() - timer_start;
    // printf("[BENCH] Create RSS flow takes %ld us\n",
    //         timer_val * 1000000 / rte_get_tsc_hz());

	  if (!flow) {
        printf("RSS flow can't be created %d message: %s\n",
	  		       error.type,
	  		       error.message ? error.message : "(no stated reason)");
	  	  rte_exit(EXIT_FAILURE, "error in creating flow");
	  } else {
        // printf("RSS flow created! priority:%d", priority);
    }

    return flow;
}

struct rte_flow* generate_jump_flow(uint16_t port_id) {
    struct rte_flow_attr attr;
    struct rte_flow_item pattern[2];
    struct rte_flow_action action[2];
    struct rte_flow *flow = NULL;
    struct rte_flow_action_jump jump_action;

    memset(pattern, 0, sizeof(pattern));
    memset(action, 0, sizeof(action));

    /*
     * set the rule attribute.
     * in this case only ingress packets will be checked.
     */
    memset(&attr, 0, sizeof(struct rte_flow_attr));
    attr.ingress = 1;
    attr.group = 0;

    /*
     * create the action sequence.
     * one action only,  spread packet using RSS
     */
    jump_action = (struct rte_flow_action_jump) {
        .group = 1
    };
    action[0].type = RTE_FLOW_ACTION_TYPE_JUMP;
    action[0].conf = &jump_action;
    action[1].type = RTE_FLOW_ACTION_TYPE_END;

    /*
     * set the first level of the pattern (ETH).
     * since in this example we just want to get the
     * ipv4 we set this level to allow all.
     */
    pattern[0].type = RTE_FLOW_ITEM_TYPE_ETH;

    /* the final level must be always type end */
    pattern[1].type = RTE_FLOW_ITEM_TYPE_END;

    struct rte_flow_error error;
    int res = rte_flow_validate(port_id, &attr, pattern, action, &error);

    uint64_t timer_start = rte_get_tsc_cycles();
    if(!res)
        flow = rte_flow_create(port_id, &attr, pattern, action, &error);
    uint64_t timer_val = rte_get_tsc_cycles() - timer_start;
    printf("[BENCH] Create jump flow takes %ld us\n",
           timer_val * 1000000 / rte_get_tsc_hz());

	  if (!flow) {
        printf("Jump flow can't be created %d message: %s\n",
	  		       error.type,
	  		       error.message ? error.message : "(no stated reason)");
	  	  rte_exit(EXIT_FAILURE, "error in creating flow");
	  } else {
        // printf("Jump flow created!");
    }

    return flow;
}

// Delete arbitrary flow
int delete_flow(uint16_t port_id, struct rte_flow* flow) {
    struct rte_flow_error error;
    if (rte_flow_destroy(port_id, flow, &error)) {
        printf("Flow can't be deleted %d message: %s\n",
               error.type,
               error.message ? error.message : "(no stated reason)");
        rte_exit(EXIT_FAILURE, "error in deleting flow");
    }
    return 0;
}
