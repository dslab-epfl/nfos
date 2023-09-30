/* RSS-based dynamic flow partitioning engine */

#ifndef RSS_H
#define RSS_H

#include <stdint.h>

#include <rte_ethdev.h>

#define OTHER_NF 0
#define POL 1
#define FW 2

int rss_init(struct rte_eth_conf* dev_conf, uint16_t port, int nf_name);
uint16_t get_rss_reta_size(uint16_t port);
// Note: size of the reta buffer should be larger than the val returned
// by get_rss_reta_size
// TODO: mv this check into the func
int get_rss_reta(uint16_t port, uint16_t* reta);
// Note: reta_sz should equal the val returned by get_rss_reta_size
// TODO: mv this check into the func
int set_rss_reta(uint16_t port, uint16_t* reta, uint16_t reta_sz);

// set the default rss reta
int set_default_rss_reta(uint16_t port, int num_queues);

#endif
