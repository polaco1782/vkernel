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

using mac_address = vk::net::mac_address;
using ipv4_address = vk::net::ipv4_address;

enum class ether_type : u16 {
    ipv4 = 0x0800,
    arp  = 0x0806,
};

enum class arp_operation : u16 {
    request = 1,
    reply   = 2,
};

using vk::net::broadcast_mac;
using vk::net::device_mac;
using vk::net::make_ipv4;
using vk::net::make_mac;
using vk::net::zero_mac;
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
