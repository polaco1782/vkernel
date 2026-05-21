/*
 * vkernel - UEFI Microkernel
 * Copyright (C) 2026 vkernel authors
 *
 * net_wire.h - Shared network wire-format structs
 */

#ifndef VKERNEL_NET_WIRE_H
#define VKERNEL_NET_WIRE_H

#include "types.h"

namespace vk::net::wire {

constexpr u16 ETHERNET_HEADER_BYTES = 14;
constexpr u16 IPV4_HEADER_BYTES = 20;
constexpr u16 ARP_ETHERNET_IPV4_BYTES = 28;
constexpr u16 ICMP_HEADER_BYTES = 4;
constexpr u16 ICMP_ECHO_HEADER_BYTES = 8;

#pragma pack(push, 1)
struct ethernet_header {
    net::mac_address dst;
    net::mac_address src;
    u16 ether_type_be;
};

struct arp_ethernet_ipv4_packet {
    u16 hardware_type_be;
    u16 protocol_type_be;
    u8  hardware_len;
    u8  protocol_len;
    u16 operation_be;
    net::mac_address sender_mac;
    net::ipv4_address sender_ip;
    net::mac_address target_mac;
    net::ipv4_address target_ip;
};

struct ipv4_header {
    u8  version_ihl;
    u8  dscp_ecn;
    u16 total_length_be;
    u16 identification_be;
    u16 flags_fragment_be;
    u8  ttl;
    u8  protocol;
    u16 header_checksum_be;
    net::ipv4_address src_ip;
    net::ipv4_address dst_ip;
};

struct icmp_header {
    u8  type;
    u8  code;
    u16 checksum_be;
};

struct icmp_echo_header {
    u8  type;
    u8  code;
    u16 checksum_be;
    u16 identifier_be;
    u16 sequence_be;
};
#pragma pack(pop)

static_assert(sizeof(ethernet_header) == ETHERNET_HEADER_BYTES);
static_assert(sizeof(arp_ethernet_ipv4_packet) == ARP_ETHERNET_IPV4_BYTES);
static_assert(sizeof(ipv4_header) == IPV4_HEADER_BYTES);
static_assert(sizeof(icmp_header) == ICMP_HEADER_BYTES);
static_assert(sizeof(icmp_echo_header) == ICMP_ECHO_HEADER_BYTES);

} // namespace vk::net::wire

#endif /* VKERNEL_NET_WIRE_H */
