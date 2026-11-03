/* Shared declarations of existing public wire headers. Layout is unchanged. */
#ifndef GNB_ROUTE_FRAME_H
#define GNB_ROUTE_FRAME_H
#include <stdint.h>
#include "gnb_def.h"
#pragma pack(push, 1)
typedef struct _gnb_route_frame_head_t {
    unsigned char magic[2];
    unsigned char pf_type_bits;
    uint8_t ttl;
    gnb_uuid_t src_uuid64;
    gnb_uuid_t dst_uuid64;
    unsigned char verifycode[4];
} __attribute__ ((__packed__)) gnb_route_frame_head_t;
typedef struct _gnb_unified_forwarding_frame_foot_t {
    gnb_uuid_t src_nodeid;
    gnb_uuid_t dst_nodeid;
    gnb_uuid_t unified_forwarding_nodeid;
    uint64_t unified_forwarding_seq;
} __attribute__ ((__packed__)) gnb_unified_forwarding_frame_foot_t;
#pragma pack(pop)
#endif
