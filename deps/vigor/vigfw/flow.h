#include <stdint.h>

struct FlowId {
  uint16_t src_port;
  uint16_t dst_port;
  uint32_t src_ip;
  uint32_t dst_ip;
  uint8_t protocol;
};
