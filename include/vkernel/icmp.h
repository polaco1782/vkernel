/*
 * vkernel - UEFI Microkernel
 * Copyright (C) 2026 vkernel authors
 *
 * icmp.h - Minimal ICMPv4 helpers and handler
 */

#ifndef VKERNEL_ICMP_H
#define VKERNEL_ICMP_H

#include "types.h"
#include "net.h"

namespace vk::net::icmp {

enum class type : u8 {
    echo_reply   = 0,
    echo_request = 8,
};

[[nodiscard]] bool send_echo_request(net_device* dev,
                                     ipv4_address dst_ip,
                                     u16 identifier,
                                     u16 sequence,
                                     const void* payload,
                                     u16 payload_length);
[[nodiscard]] bool send_echo_request_default(ipv4_address dst_ip,
                                             u16 identifier,
                                             u16 sequence,
                                             const void* payload,
                                             u16 payload_length);
bool observe_packet(net_device* dev,
                    ipv4_address src_ip,
                    ipv4_address dst_ip,
                    const void* packet,
                    u16 packet_length);

} // namespace vk::net::icmp

#endif /* VKERNEL_ICMP_H */
