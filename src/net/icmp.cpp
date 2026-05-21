/*
 * vkernel - UEFI Microkernel
 * Copyright (C) 2026 vkernel authors
 *
 * icmp.cpp - Minimal ICMPv4 helpers and handler
 */

#include "types.h"
#include "log.h"
#include "memory.h"
#include "net.h"
#include "net_wire.h"
#include "ipv4.h"
#include "icmp.h"

namespace vk::net::icmp {
namespace {

[[nodiscard]] static bool send_echo(net_device* dev,
                                    type icmp_type,
                                    ipv4_address src_ip,
                                    ipv4_address dst_ip,
                                    u16 identifier,
                                    u16 sequence,
                                    const void* payload,
                                    u16 payload_length) {
    if (dev == null) {
        return false;
    }
    if (payload_length > 0 && payload == null) {
        return false;
    }

    const u16 packet_length =
        static_cast<u16>(wire::ICMP_ECHO_HEADER_BYTES + payload_length);
    auto* packet = static_cast<u8*>(g_kernel_heap.allocate(packet_length));
    if (packet == null) {
        log::warn() << "icmp: failed to allocate packet buffer (" << packet_length
                    << " bytes)";
        return false;
    }

    auto* header = reinterpret_cast<wire::icmp_echo_header*>(packet);
    header->type = static_cast<u8>(icmp_type);
    header->code = 0;
    header->checksum_be = 0;
    header->identifier_be = net::bswap16(identifier);
    header->sequence_be = net::bswap16(sequence);

    if (payload_length > 0) {
        memory::copy(packet + sizeof(*header), payload, payload_length);
    }

    header->checksum_be = net::bswap16(
        net::internet_checksum(packet, packet_length));

    const ipv4::send_params params {
        .src_ip = src_ip,
        .dst_ip = dst_ip,
        .proto = ipv4::protocol::icmp,
        .ttl = 64,
    };
    const bool ok = ipv4::send(dev, params, packet, packet_length);
    g_kernel_heap.free(packet);
    return ok;
}

} // namespace

bool send_echo_request(net_device* dev,
                       ipv4_address dst_ip,
                       u16 identifier,
                       u16 sequence,
                       const void* payload,
                       u16 payload_length) {
    return send_echo(dev,
                     type::echo_request,
                     net::zero_ipv4(),
                     dst_ip,
                     identifier,
                     sequence,
                     payload,
                     payload_length);
}

bool send_echo_request_default(ipv4_address dst_ip,
                               u16 identifier,
                               u16 sequence,
                               const void* payload,
                               u16 payload_length) {
    auto* dev = net::primary_device();
    if (dev == null) {
        log::warn() << "icmp: no default network device";
        return false;
    }
    return send_echo_request(dev, dst_ip, identifier, sequence,
                             payload, payload_length);
}

bool observe_packet(net_device* dev,
                    ipv4_address src_ip,
                    ipv4_address dst_ip,
                    const void* packet,
                    u16 packet_length) {
    if (dev == null || packet == null || packet_length < wire::ICMP_HEADER_BYTES) {
        return false;
    }

    const auto* header = static_cast<const wire::icmp_header*>(packet);
    if (net::internet_checksum(packet, packet_length) != 0) {
        log::warn() << "icmp: dropped packet with bad checksum on "
                    << dev->name.c_str();
        return true;
    }

    char src_ip_buf[16];
    char dst_ip_buf[16];
    net::format_ipv4(src_ip, src_ip_buf, sizeof(src_ip_buf));
    net::format_ipv4(dst_ip, dst_ip_buf, sizeof(dst_ip_buf));

    switch (static_cast<type>(header->type)) {
        case type::echo_request: {
            if (header->code != 0 || packet_length < wire::ICMP_ECHO_HEADER_BYTES) {
                return true;
            }

            const auto* echo =
                static_cast<const wire::icmp_echo_header*>(packet);
            const void* echo_payload =
                static_cast<const u8*>(packet) + sizeof(*echo);
            const u16 echo_payload_length =
                static_cast<u16>(packet_length - sizeof(*echo));

            log::info() << "icmp: echo request from " << src_ip_buf
                        << " to " << dst_ip_buf
                        << " id=" << net::bswap16(echo->identifier_be)
                        << " seq=" << net::bswap16(echo->sequence_be)
                        << " payload=" << echo_payload_length;

            (void)send_echo(dev,
                            type::echo_reply,
                            dst_ip,
                            src_ip,
                            net::bswap16(echo->identifier_be),
                            net::bswap16(echo->sequence_be),
                            echo_payload,
                            echo_payload_length);
            return true;
        }
        case type::echo_reply: {
            if (header->code != 0 || packet_length < wire::ICMP_ECHO_HEADER_BYTES) {
                return true;
            }

            const auto* echo =
                static_cast<const wire::icmp_echo_header*>(packet);
            log::info() << "icmp: echo reply from " << src_ip_buf
                        << " to " << dst_ip_buf
                        << " id=" << net::bswap16(echo->identifier_be)
                        << " seq=" << net::bswap16(echo->sequence_be)
                        << " payload="
                        << static_cast<unsigned long long>(
                            packet_length - sizeof(*echo));
            return true;
        }
        default:
            log::debug() << "icmp: received type="
                         << static_cast<unsigned long long>(header->type)
                         << " code="
                         << static_cast<unsigned long long>(header->code)
                         << " from " << src_ip_buf;
            return true;
    }
}

} // namespace vk::net::icmp
