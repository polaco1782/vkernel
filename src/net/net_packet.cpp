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

namespace vk::net::packet {
namespace {

constexpr u16 ETHERNET_HEADER_BYTES = 14;
constexpr u16 ETHERNET_MIN_FRAME_BYTES = 60;
constexpr u16 ARP_ETHERNET_HTYPE = 1;
constexpr u8 ARP_HLEN_ETHERNET = 6;
constexpr u8 ARP_PLEN_IPV4 = 4;

#pragma pack(push, 1)
struct ethernet_header {
    u8  dst[6];
    u8  src[6];
    u16 ether_type_be;
};

struct arp_packet {
    u16 hardware_type_be;
    u16 protocol_type_be;
    u8  hardware_len;
    u8  protocol_len;
    u16 operation_be;
    u8  sender_mac[6];
    u8  sender_ip[4];
    u8  target_mac[6];
    u8  target_ip[4];
};
#pragma pack(pop)

static_assert(sizeof(ethernet_header) == ETHERNET_HEADER_BYTES);
static_assert(sizeof(arp_packet) == 28);

[[nodiscard]] constexpr auto bswap16(u16 value) -> u16 {
    return static_cast<u16>((value << 8) | (value >> 8));
}

static void copy_mac_bytes(u8 dst[6], mac_address src) {
    for (usize i = 0; i < 6; ++i) {
        dst[i] = src.bytes[i];
    }
}

static void copy_ipv4_bytes(u8 dst[4], ipv4_address src) {
    for (usize i = 0; i < 4; ++i) {
        dst[i] = src.bytes[i];
    }
}

} // namespace

auto device_mac(const net_device& dev) -> mac_address {
    mac_address mac {};
    for (usize i = 0; i < 6; ++i) {
        mac.bytes[i] = dev.mac[i];
    }
    return mac;
}

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

    const u32 unpadded_frame_length = ETHERNET_HEADER_BYTES + payload_length;
    const u32 frame_length = unpadded_frame_length < ETHERNET_MIN_FRAME_BYTES
        ? ETHERNET_MIN_FRAME_BYTES
        : unpadded_frame_length;

    auto* frame = static_cast<u8*>(g_kernel_heap.allocate(frame_length));
    if (frame == null) {
        log::warn() << "net_packet: failed to allocate frame buffer (" << frame_length << " bytes)";
        return false;
    }

    memory::set(frame, 0, frame_length);

    auto* header = reinterpret_cast<ethernet_header*>(frame);
    copy_mac_bytes(header->dst, dst);
    copy_mac_bytes(header->src, device_mac(*dev));
    header->ether_type_be = bswap16(static_cast<u16>(type));

    if (payload_length > 0) {
        memory::copy(frame + sizeof(ethernet_header), payload, payload_length);
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

    arp_packet packet {};
    packet.hardware_type_be = bswap16(ARP_ETHERNET_HTYPE);
    packet.protocol_type_be = bswap16(static_cast<u16>(ether_type::ipv4));
    packet.hardware_len = ARP_HLEN_ETHERNET;
    packet.protocol_len = ARP_PLEN_IPV4;
    packet.operation_be = bswap16(static_cast<u16>(op));
    copy_mac_bytes(packet.sender_mac, device_mac(*dev));
    copy_ipv4_bytes(packet.sender_ip, sender_ip);
    copy_mac_bytes(packet.target_mac, target_mac);
    copy_ipv4_bytes(packet.target_ip, target_ip);

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
