#define LAN_DEVICE 0
#define WAN_DEVICE 1
// maximum 2m active flows
#define MAX_NUM_FLOWS 2097152U

#ifndef EXPIRATION_TIME
#define EXPIRATION_TIME 1200000 // 1.2 sec
#endif

#define ENDPOINT_MAC {0x01, 0x23, 0x45, 0x56, 0x78, 0x9a}
#define LB_CAPACITY 2
#define IP4_PLY_POOL_CAPACITY 5
#define N_FIB_TABLE 2
#ifndef LCORES
#define LCORES "8,10,12,14,16"
#endif
