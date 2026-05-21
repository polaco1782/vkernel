/*
 * vkernel - UEFI Microkernel
 * Copyright (C) 2026 vkernel authors
 *
 * ipv4.h - Minimal IPv4 transmit helpers
 */

#ifndef VKERNEL_IPV4_H
#define VKERNEL_IPV4_H

#include "types.h"
#include "net.h"
#include "net_packet.h"

namespace vk::net::ipv4 {

enum class protocol : u8 {
    icmp = 1,
    tcp  = 6,
    udp  = 17,
};

struct send_params {
    packet::ipv4_address src_ip;
    packet::ipv4_address dst_ip;
    protocol             proto = protocol::udp;
    u8                   ttl = 64;
};

bool configure_device(net_device* dev, packet::ipv4_address local_ip);
bool configure_default(packet::ipv4_address local_ip);
bool configured_address(net_device* dev, packet::ipv4_address* out_local_ip);
bool owns_address(net_device* dev, packet::ipv4_address ip);
bool observe_frame(net_device* dev, const void* frame, u32 frame_length);

[[nodiscard]] bool send(net_device* dev, const send_params& params,
                        const void* payload, u16 payload_length);
[[nodiscard]] bool send_default(const send_params& params,
                                const void* payload, u16 payload_length);

} // namespace vk::net::ipv4

#endif /* VKERNEL_IPV4_H */
