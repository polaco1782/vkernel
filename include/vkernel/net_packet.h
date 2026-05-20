/*
 * vkernel - UEFI Microkernel
 * Copyright (C) 2026 vkernel authors
 *
 * net_packet.h - Small network packet construction helpers
 */

#ifndef VKERNEL_NET_PACKET_H
#define VKERNEL_NET_PACKET_H

#include "types.h"
#include "net.h"

namespace vk::net::packet {

struct mac_address {
    u8 bytes[6];
};

struct ipv4_address {
    u8 bytes[4];
};

enum class ether_type : u16 {
    ipv4 = 0x0800,
    arp  = 0x0806,
};

enum class arp_operation : u16 {
    request = 1,
    reply   = 2,
};

[[nodiscard]] constexpr auto make_mac(u8 b0, u8 b1, u8 b2,
                                      u8 b3, u8 b4, u8 b5) -> mac_address {
    return { { b0, b1, b2, b3, b4, b5 } };
}

[[nodiscard]] constexpr auto make_ipv4(u8 a, u8 b, u8 c, u8 d) -> ipv4_address {
    return { { a, b, c, d } };
}

[[nodiscard]] constexpr auto broadcast_mac() -> mac_address {
    return make_mac(0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF);
}

[[nodiscard]] constexpr auto zero_mac() -> mac_address {
    return make_mac(0, 0, 0, 0, 0, 0);
}

[[nodiscard]] auto device_mac(const net_device& dev) -> mac_address;
[[nodiscard]] bool send_ethernet(net_device* dev, mac_address dst,
                                 ether_type type,
                                 const void* payload, u32 payload_length);
[[nodiscard]] bool send_ethernet_default(mac_address dst, ether_type type,
                                         const void* payload, u32 payload_length);
[[nodiscard]] bool send_arp(net_device* dev, arp_operation op,
                            mac_address target_mac,
                            ipv4_address sender_ip,
                            ipv4_address target_ip);
[[nodiscard]] bool send_arp_request(net_device* dev,
                                    ipv4_address sender_ip,
                                    ipv4_address target_ip);
[[nodiscard]] bool send_arp_reply(net_device* dev,
                                  mac_address target_mac,
                                  ipv4_address sender_ip,
                                  ipv4_address target_ip);

} // namespace vk::net::packet

#endif /* VKERNEL_NET_PACKET_H */
