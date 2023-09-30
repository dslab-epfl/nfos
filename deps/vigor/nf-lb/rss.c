#include <stdint.h>

#include <rte_ethdev.h>

#include "nf-log.h"
#include "rss.h"

#define RSS_HASH_KEY_LENGTH 40

// Keys for working around 82599/e810's limited choices
// of headers for RSS
// Required in policer
// Only the WAN IP is considered for hashing with these keys
static uint8_t hash_key_lan[RSS_HASH_KEY_LENGTH] = {
	0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
	0x41, 0x67, 0x25, 0x3d, 0x43, 0xA3, 0x8F, 0xB0,
	0xD0, 0xCA, 0x2B, 0xCB, 0xAE, 0x7B, 0x30, 0xB4,
	0x77, 0xCB, 0x2D, 0xA3, 0x80, 0x30, 0xF2, 0x0C,
	0x6A, 0x42, 0xB7, 0x3B, 0xBE, 0xAC, 0x01, 0xFA,
};

// Default hash key (allows symmetric hashing)
static uint8_t hash_key_symmetric[RSS_HASH_KEY_LENGTH] = {
  0x6D, 0x5A, 0x6D, 0x5A, 0x6D, 0x5A, 0x6D, 0x5A,
  0x6D, 0x5A, 0x6D, 0x5A, 0x6D, 0x5A, 0x6D, 0x5A,
  0x6D, 0x5A, 0x6D, 0x5A, 0x6D, 0x5A, 0x6D, 0x5A,
  0x6D, 0x5A, 0x6D, 0x5A, 0x6D, 0x5A, 0x6D, 0x5A,
  0x6D, 0x5A, 0x6D, 0x5A, 0x6D, 0x5A, 0x6D, 0x5A
};

// Configure RSS to distribute packets based on IP
int rss_init(struct rte_eth_conf* dev_conf, uint16_t port, int nf_name) {
  unsigned num_rxqueues = rte_lcore_count() - 1;
  if (num_rxqueues > 1) {
    dev_conf->rxmode.mq_mode = ETH_MQ_RX_RSS;

    struct rte_eth_dev_info dev_info;

    rte_eth_dev_info_get(port, &dev_info);

    // temp hack, hardcode configuration for policer
    if (nf_name == POL && 0 == port) {
      // temp hack, eventually should pass nf config to here
      // assuming port 0 is LAN
      dev_conf->rx_adv_conf.rss_conf.rss_key = hash_key_lan;
		  dev_conf->rx_adv_conf.rss_conf.rss_hf =
        ETH_RSS_IP & dev_info.flow_type_rss_offloads;

    } else {
      if (nf_name == FW)
        dev_conf->rx_adv_conf.rss_conf.rss_key = hash_key_symmetric;
      else
        dev_conf->rx_adv_conf.rss_conf.rss_key = NULL;

		  dev_conf->rx_adv_conf.rss_conf.rss_hf =
        (ETH_RSS_IP | ETH_RSS_TCP | ETH_RSS_UDP) & dev_info.flow_type_rss_offloads;

    }
    dev_conf->rx_adv_conf.rss_conf.rss_key_len = RSS_HASH_KEY_LENGTH;
  }
  return 0;
}

uint16_t get_rss_reta_size(uint16_t port) {
  struct rte_eth_dev_info dev_info;
  int reta_size;

	rte_eth_dev_info_get(port, &dev_info);
	return dev_info.reta_size;
}

int get_rss_reta(uint16_t port, uint16_t* reta) {

  uint16_t reta_sz = get_rss_reta_size(port);
  uint16_t num_reta_grps = reta_sz / RTE_RETA_GROUP_SIZE;
  struct rte_eth_rss_reta_entry64 reta_conf[num_reta_grps];

  for (uint16_t i = 0; i < num_reta_grps; i++)
			reta_conf[i].mask = UINT64_MAX;

  int status = rte_eth_dev_rss_reta_query(port, reta_conf, reta_sz);
  if (status != 0) {
	  NF_INFO("Could not query RETA for port %d (size %d) ! Error %d", port,
            reta_sz, status);
	  return status;
  }

  for (uint16_t i = 0; i < num_reta_grps; i++) {
    uint16_t base = i * RTE_RETA_GROUP_SIZE;
	  for (uint16_t j = 0; j < RTE_RETA_GROUP_SIZE; j++)
	  	reta[base + j] = reta_conf[i].reta[j];
  }

	return 0;
}

int set_rss_reta(uint16_t port, uint16_t* reta, uint16_t reta_sz) {

  uint16_t num_reta_grps = reta_sz / RTE_RETA_GROUP_SIZE;

  struct rte_eth_rss_reta_entry64 reta_conf[num_reta_grps];

  for (uint16_t i = 0; i < num_reta_grps; i++) {
		reta_conf[i].mask = UINT64_MAX;

    uint16_t base = i * RTE_RETA_GROUP_SIZE;
	  for (uint16_t j = 0; j < RTE_RETA_GROUP_SIZE; j++)
	  	reta_conf[i].reta[j] = reta[base + j];
  }

  return rte_eth_dev_rss_reta_update(port, reta_conf, reta_sz);
}

// set the default rss reta
int set_default_rss_reta(uint16_t port, int num_queues) {
  NF_DEBUG("set default rss reta on port %d", port);
  if (num_queues > 1) {
    int reta_sz = get_rss_reta_size(port);
    NF_DEBUG("reta_sz: %d", reta_sz);
    uint16_t reta[reta_sz];
    for (int i = 0; i < reta_sz; i++)
      reta[i] = i % num_queues;
    set_rss_reta(port, reta, reta_sz);

    NF_DEBUG("initial reta:");
    get_rss_reta(port, reta);
    for (int i = 0; i < reta_sz; i++)
      NF_DEBUG("reta[%d]: %d", i, reta[i]);
  }
}


