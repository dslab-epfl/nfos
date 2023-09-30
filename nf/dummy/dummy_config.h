#define LAN_DEVICE 0
#define WAN_DEVICE 1
// maximum 2m active flows
#define MAX_NUM_FLOWS 2097152U

#ifndef EXPIRATION_TIME
#define EXPIRATION_TIME 1200000 // 1.2 sec
#endif

#define DATA_SIZE 10000
#define DATA_ZIPF_FACTOR 0

#define READ_ONLY
//#define WRITE_PER_FLOW
//#define WRITE_PER_PACKET

#define ENDPOINT_MAC {0x01, 0x23, 0x45, 0x56, 0x78, 0x9a}
#ifndef LCORES
#define LCORES "8,10,12,14,16"
#endif
