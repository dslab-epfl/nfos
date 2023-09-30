#pragma once
#ifndef __TCP_STATE_H__
#define __TCP_STATE_H__
#include "nf.h"

/*TCP Flags*/
#define TCP_FLAG_FIN    0x01
#define TCP_FLAG_SYN    0x02
#define TCP_FLAG_RST    0x04
#define TCP_FLAG_PUSH   0x08
#define TCP_FLAG_ACK    0x10
#define TCP_FLAG_URG    0x20
#define TCP_FLAG_ECE    0x40
#define TCP_FLAG_CWR    0x80
#define TCP_FLAGS_RSTFINACKSYN (TCP_FLAG_RST + TCP_FLAG_FIN + TCP_FLAG_SYN + TCP_FLAG_ACK)
#define TCP_FLAGS_ACKSYN (TCP_FLAG_SYN + TCP_FLAG_ACK)
#define PROTOCOL_TCP    6
union tcp_flag_t
{
    uint8_t as_u8[2];
    uint16_t as_u16;
};

/*Simple help function to track tcp flags from 2 sides*/
static inline void track_session(int is_input, union tcp_flag_t *flag, uint8_t tcp_flag){
  uint8_t old_flags = (*flag).as_u8[is_input];
  uint8_t new_flags = old_flags | tcp_flag;
  (*flag).as_u8[is_input] = (old_flags != new_flags) ? new_flags : old_flags;
}

#endif
