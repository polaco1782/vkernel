/*
 * vkernel - UEFI Microkernel
 * Copyright (C) 2026 vkernel authors
 *
 * net_packet.cpp - Small network packet construction helpers
 */

#include "types.h"
#include "log.h"
#include "memory.h"
#include "net.h"
#include "net_packet.h"
#include "net_wire.h"

namespace vk::net::packet {
namespace {

constexpr u16 ETHERNET_MIN_FRAME_BYTES = 60;
constexpr u16 ARP_ETHERNET_HTYPE = 1;
constexpr u8 ARP_HLEN_ETHERNET = 6;
constexpr u8 ARP_PLEN_IPV4 = 4;

} // namespace

bool send_ethernet(net_device* dev, mac_address dst, ether_type type,
                   const void* payload, u32 payload_length) {
    if (dev == null) {
        return false;
    }
    if (payload_length > 0 && payload == null) {
        return false;
    }
    if (payload_length > dev->mtu) {
        log::warn() << "net_packet: payload exceeds MTU for "
                    << dev->name.c_str()
                    << " payload=" << static_cast<unsigned long long>(payload_length)
                    << " mtu=" << dev->mtu;
        return false;
    }

    const u32 unpadded_frame_length = wire::ETHERNET_HEADER_BYTES + payload_length;
    const u32 frame_length = unpadded_frame_length < ETHERNET_MIN_FRAME_BYTES
        ? ETHERNET_MIN_FRAME_BYTES
        : unpadded_frame_length;

    auto* frame = static_cast<u8*>(g_kernel_heap.allocate(frame_length));
    if (frame == null) {
        log::warn() << "net_packet: failed to allocate frame buffer (" << frame_length << " bytes)";
        return false;
    }

    memory::set(frame, 0, frame_length);

    auto* header = reinterpret_cast<wire::ethernet_header*>(frame);
    header->dst = dst;
    header->src = net::device_mac(*dev);
    header->ether_type_be = net::bswap16(static_cast<u16>(type));

    if (payload_length > 0) {
        memory::copy(frame + sizeof(wire::ethernet_header), payload, payload_length);
    }

    const bool ok = net::send_packet(dev, frame, frame_length);
    g_kernel_heap.free(frame);
    return ok;
}

bool send_ethernet_default(mac_address dst, ether_type type,
                           const void* payload, u32 payload_length) {
    auto* dev = net::primary_device();
    if (dev == null) {
        log::warn() << "net_packet: no default network device";
        return false;
    }
    return send_ethernet(dev, dst, type, payload, payload_length);
}

bool send_arp(net_device* dev, arp_operation op, mac_address target_mac,
              ipv4_address sender_ip, ipv4_address target_ip) {
    if (dev == null) {
        return false;
    }

    wire::arp_ethernet_ipv4_packet packet {};
    packet.hardware_type_be = net::bswap16(ARP_ETHERNET_HTYPE);
    packet.protocol_type_be = net::bswap16(static_cast<u16>(ether_type::ipv4));
    packet.hardware_len = ARP_HLEN_ETHERNET;
    packet.protocol_len = ARP_PLEN_IPV4;
    packet.operation_be = net::bswap16(static_cast<u16>(op));
    packet.sender_mac = net::device_mac(*dev);
    packet.sender_ip = sender_ip;
    packet.target_mac = target_mac;
    packet.target_ip = target_ip;

    const mac_address eth_dst = (op == arp_operation::request)
        ? broadcast_mac()
        : target_mac;
    return send_ethernet(dev, eth_dst, ether_type::arp,
                         &packet, sizeof(packet));
}

bool send_arp_request(net_device* dev, ipv4_address sender_ip,
                      ipv4_address target_ip) {
    return send_arp(dev, arp_operation::request,
                    zero_mac(), sender_ip, target_ip);
}

bool send_arp_reply(net_device* dev, mac_address target_mac,
                    ipv4_address sender_ip, ipv4_address target_ip) {
    return send_arp(dev, arp_operation::reply,
                    target_mac, sender_ip, target_ip);
}

} // namespace vk::net::packet
