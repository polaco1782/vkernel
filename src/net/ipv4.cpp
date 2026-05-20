/*
 * vkernel - UEFI Microkernel
 * Copyright (C) 2026 vkernel authors
 *
 * net.cpp - Network device registry and helpers
 */

#include "types.h"
#include "log.h"
#include "net.h"
#include "net_packet.h"

namespace vk::net::packet {

#pragma pack(push, 1)
struct ipv4_header {
    u8  version_ihl;
    u8  dscp_ecn;
    u16 total_length_be;
    u16 identification_be;
    u16 flags_fragment_be;
    u8  ttl;
    u8  protocol;
    u16 header_checksum_be;
    u8  src_ip[4];
    u8  dst_ip[4];
};
#pragma pack(pop)

static_assert(sizeof(ipv4_header) == 20);

}