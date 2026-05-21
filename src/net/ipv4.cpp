/*
 * vkernel - UEFI Microkernel
 * Copyright (C) 2026 vkernel authors
 *
 * ipv4.cpp - Minimal IPv4 transmit helpers
 */

#include "types.h"
#include "log.h"
#include "memory.h"
#include "net.h"
#include "net_packet.h"
#include "net_wire.h"
#include "arp.h"
#include "ipv4.h"
#include "icmp.h"
#include "spinlock.h"

namespace vk::net::ipv4 {
namespace {

constexpr u8 IPV4_VERSION = 4;
constexpr u8 IPV4_IHL_WORDS = 5;
constexpr u16 IPV4_FLAG_DF = 0x4000;
constexpr u16 IPV4_MAX_HEADER_BYTES = 60;
constexpr usize MAX_IPV4_DEVICE_CONFIGS = net::MAX_NET_DEVICES;

struct device_ipv4_config {
    net_device*   dev = null;
    bool          valid = false;
    ipv4_address  local_ip {};
};

static spinlock s_ipv4_id_lock;
static spinlock s_config_lock;
static u16 s_next_identification = 1;
static device_ipv4_config s_device_configs[MAX_IPV4_DEVICE_CONFIGS];
static bool s_configs_initialised = false;

[[nodiscard]] static auto next_identification() -> u16 {
    s_ipv4_id_lock.acquire();
    const u16 id = s_next_identification++;
    if (s_next_identification == 0) {
        s_next_identification = 1;
    }
    s_ipv4_id_lock.release();
    return id;
}

static void init_configs() {
    if (s_configs_initialised) {
        return;
    }

    for (usize i = 0; i < MAX_IPV4_DEVICE_CONFIGS; ++i) {
        s_device_configs[i] = {};
    }
    s_configs_initialised = true;
}

[[nodiscard]] static auto configured_address_locked(net_device* dev,
                                                    ipv4_address* out_local_ip) -> bool {
    if (dev == null) {
        return false;
    }

    for (usize i = 0; i < MAX_IPV4_DEVICE_CONFIGS; ++i) {
        const auto& config = s_device_configs[i];
        if (config.valid && config.dev == dev) {
            if (out_local_ip != null) {
                *out_local_ip = config.local_ip;
            }
            return true;
        }
    }

    return false;
}

[[nodiscard]] static auto header_words(const wire::ipv4_header* header) -> u16 {
    return static_cast<u16>(header->version_ihl & 0x0F);
}

} // namespace

bool configure_device(net_device* dev, ipv4_address local_ip) {
    if (dev == null) {
        return false;
    }

    init_configs();
    char ip_buf[16];
    net::format_ipv4(local_ip, ip_buf, sizeof(ip_buf));

    s_config_lock.acquire();
    for (usize i = 0; i < MAX_IPV4_DEVICE_CONFIGS; ++i) {
        auto& config = s_device_configs[i];
        if (config.valid && config.dev == dev) {
            config.local_ip = local_ip;
            s_config_lock.release();
            log::info() << "ipv4: configured " << dev->name.c_str()
                        << " address=" << ip_buf;
            return true;
        }
    }

    for (usize i = 0; i < MAX_IPV4_DEVICE_CONFIGS; ++i) {
        auto& config = s_device_configs[i];
        if (!config.valid) {
            config.dev = dev;
            config.valid = true;
            config.local_ip = local_ip;
            s_config_lock.release();
            log::info() << "ipv4: configured " << dev->name.c_str()
                        << " address=" << ip_buf;
            return true;
        }
    }

    s_config_lock.release();
    log::warn() << "ipv4: configuration table full, cannot configure "
                << dev->name.c_str();
    return false;
}

bool configure_default(ipv4_address local_ip) {
    auto* dev = net::primary_device();
    if (dev == null) {
        log::warn() << "ipv4: no default network device";
        return false;
    }
    return configure_device(dev, local_ip);
}

bool configured_address(net_device* dev, ipv4_address* out_local_ip) {
    init_configs();

    s_config_lock.acquire();
    const bool found = configured_address_locked(dev, out_local_ip);
    s_config_lock.release();
    return found;
}

bool owns_address(net_device* dev, ipv4_address ip) {
    ipv4_address local_ip {};
    if (!configured_address(dev, &local_ip)) {
        return false;
    }
    return net::ipv4_equal(local_ip, ip);
}

bool observe_frame(net_device* dev, const void* frame, u32 frame_length) {
    if (dev == null || frame == null ||
        frame_length < wire::ETHERNET_HEADER_BYTES + wire::IPV4_HEADER_BYTES) {
        return false;
    }

    const auto* eth = static_cast<const wire::ethernet_header*>(frame);
    if (net::bswap16(eth->ether_type_be) != static_cast<u16>(packet::ether_type::ipv4)) {
        return false;
    }

    const auto* header = reinterpret_cast<const wire::ipv4_header*>(
        static_cast<const u8*>(frame) + wire::ETHERNET_HEADER_BYTES);
    const u8 version = static_cast<u8>(header->version_ihl >> 4);
    const u8 ihl_words = static_cast<u8>(header_words(header));
    if (version != IPV4_VERSION || ihl_words < IPV4_IHL_WORDS) {
        return false;
    }

    const u16 header_bytes = static_cast<u16>(ihl_words * 4);
    if (header_bytes > IPV4_MAX_HEADER_BYTES) {
        return false;
    }
    const u16 total_length = net::bswap16(header->total_length_be);
    const u32 frame_header_bytes =
        static_cast<u32>(wire::ETHERNET_HEADER_BYTES) + header_bytes;
    const u32 frame_total_bytes =
        static_cast<u32>(wire::ETHERNET_HEADER_BYTES) + total_length;
    if (frame_length < frame_header_bytes ||
        total_length < header_bytes ||
        frame_length < frame_total_bytes) {
        return false;
    }

    ipv4_address local_ip {};
    if (!configured_address(dev, &local_ip) ||
        !net::ipv4_equal(local_ip, header->dst_ip)) {
        return false;
    }

    const u16 header_checksum = net::bswap16(header->header_checksum_be);
    u8 header_copy[IPV4_MAX_HEADER_BYTES];
    memory::copy(header_copy, header, header_bytes);
    auto* header_copy_typed = reinterpret_cast<wire::ipv4_header*>(header_copy);
    header_copy_typed->header_checksum_be = 0;
    const u16 computed_checksum = net::internet_checksum(header_copy, header_bytes);
    if (computed_checksum != header_checksum) {
        log::warn() << "ipv4: dropped packet with bad header checksum on "
                    << dev->name.c_str();
        return true;
    }

    arp::insert(header->src_ip, eth->src);

    const auto* payload =
        static_cast<const u8*>(static_cast<const void*>(header)) + header_bytes;
    const u16 payload_length = static_cast<u16>(total_length - header_bytes);

    switch (static_cast<protocol>(header->protocol)) {
        case protocol::icmp:
            if (icmp::observe_packet(dev,
                                     header->src_ip,
                                     header->dst_ip,
                                     payload,
                                     payload_length)) {
                return true;
            }
            break;
        default:
            break;
    }

    char src_ip_buf[16];
    char dst_ip_buf[16];
    net::format_ipv4(header->src_ip, src_ip_buf, sizeof(src_ip_buf));
    net::format_ipv4(header->dst_ip, dst_ip_buf, sizeof(dst_ip_buf));
    log::debug() << "ipv4: received packet on " << dev->name.c_str()
                 << " src_ip=" << src_ip_buf
                 << " dst_ip=" << dst_ip_buf
                 << " proto=" << static_cast<unsigned long long>(header->protocol)
                 << " payload=" << payload_length;
    return true;
}

bool send(net_device* dev, const send_params& params,
          const void* payload, u16 payload_length) {
    if (dev == null) {
        return false;
    }
    if (payload_length > 0 && payload == null) {
        return false;
    }
    if (wire::IPV4_HEADER_BYTES + payload_length > dev->mtu) {
        log::warn() << "ipv4: payload exceeds MTU for "
                    << dev->name.c_str()
                    << " total=" << static_cast<unsigned long long>(wire::IPV4_HEADER_BYTES + payload_length)
                    << " mtu=" << dev->mtu;
        return false;
    }

    const u16 total_length = static_cast<u16>(wire::IPV4_HEADER_BYTES + payload_length);
    auto* packet_buf = static_cast<u8*>(g_kernel_heap.allocate(total_length));
    if (packet_buf == null) {
        log::warn() << "ipv4: failed to allocate packet buffer (" << total_length << " bytes)";
        return false;
    }

    ipv4_address src_ip = params.src_ip;
    if (net::ipv4_equal(src_ip, net::zero_ipv4())) {
        if (!configured_address(dev, &src_ip)) {
            log::warn() << "ipv4: no configured source address for "
                        << dev->name.c_str();
            g_kernel_heap.free(packet_buf);
            return false;
        }
    }

    net::mac_address dst_mac {};
    if (!arp::resolve(dev, src_ip, params.dst_ip, &dst_mac)) {
        g_kernel_heap.free(packet_buf);
        return false;
    }

    auto* header = reinterpret_cast<wire::ipv4_header*>(packet_buf);
    memory::set(header, 0, sizeof(*header));

    header->version_ihl = static_cast<u8>((IPV4_VERSION << 4) | IPV4_IHL_WORDS);
    header->dscp_ecn = 0;
    header->total_length_be = net::bswap16(total_length);
    header->identification_be = net::bswap16(next_identification());
    header->flags_fragment_be = net::bswap16(IPV4_FLAG_DF);
    header->ttl = params.ttl;
    header->protocol = static_cast<u8>(params.proto);
    header->src_ip = src_ip;
    header->dst_ip = params.dst_ip;
    header->header_checksum_be = 0;
    header->header_checksum_be = net::bswap16(
        net::internet_checksum(header, sizeof(*header)));

    if (payload_length > 0) {
        memory::copy(packet_buf + sizeof(*header), payload, payload_length);
    }

    const bool ok = packet::send_ethernet(dev,
                                          dst_mac,
                                          packet::ether_type::ipv4,
                                          packet_buf,
                                          total_length);
    g_kernel_heap.free(packet_buf);
    return ok;
}

bool send_default(const send_params& params,
                  const void* payload, u16 payload_length) {
    auto* dev = net::primary_device();
    if (dev == null) {
        log::warn() << "ipv4: no default network device";
        return false;
    }
    return send(dev, params, payload, payload_length);
}

} // namespace vk::net::ipv4
