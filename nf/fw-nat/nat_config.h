#define LAN_DEVICE 0
#define WAN_DEVICE 1
#define EXTERNAL_ADDR 0xdeadbeef
// Todo: parameterize this
#define NUM_EXTERNAL_ADDRS 57 // this is where we cut corners
#define MAX_NUM_SESSIONS 8388608 // can be reduced, but doesn't matter here since it will be round to next pow of two

#ifndef EXPIRATION_TIME
#define EXPIRATION_TIME 1200000 // 1.2 sec
#endif

#define ENDPOINT_MAC {0x01, 0x23, 0x45, 0x56, 0x78, 0x9a}

#ifndef LCORES
#define LCORES "8,10,12,14,16"
#endif

#define EXTERNAL_PORT_LOW 1024
#define EXTERNAL_PORT_HIGH 65535

#define EI_NAT_SECOND 3000000000LL

#define FLOW_SIZE_THRESHOLD UINT32_MAX // effectively avoid dropping flows
