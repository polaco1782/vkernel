/*
 * vkernel - UEFI Microkernel
 * Copyright (C) 2026 vkernel authors
 *
 * arp.cpp - Tiny ARP cache and blocking resolver
 */

#include "types.h"
#include "log.h"
#include "net.h"
#include "net_packet.h"
#include "net_wire.h"
#include "arp.h"
#include "ipv4.h"
#include "scheduler.h"

namespace vk::net::arp {
namespace {

constexpr u16 ARP_ETHERNET_HTYPE = 1;
constexpr u8 ARP_HLEN_ETHERNET = 6;
constexpr u8 ARP_PLEN_IPV4 = 4;
constexpr u64 ARP_RESOLVE_TIMEOUT_TICKS = 50;

struct arp_cache_entry {
    bool         valid = false;
    ipv4_address ip {};
    mac_address  mac {};
};

static arp_cache_entry s_cache[MAX_CACHE_ENTRIES];
static usize s_next_replace = 0;
static bool s_initialised = false;
static spinlock s_cache_lock;

struct parsed_arp_frame {
    ipv4_address sender_ip {};
    mac_address  sender_mac {};
    ipv4_address target_ip {};
    u16          operation = 0;
};

static void cache_insert_locked(ipv4_address ip, mac_address mac) {
    for (usize i = 0; i < MAX_CACHE_ENTRIES; ++i) {
        if (s_cache[i].valid && net::ipv4_equal(s_cache[i].ip, ip)) {
            s_cache[i].mac = mac;
            return;
        }
    }

    for (usize i = 0; i < MAX_CACHE_ENTRIES; ++i) {
        if (!s_cache[i].valid) {
            s_cache[i].valid = true;
            s_cache[i].ip = ip;
            s_cache[i].mac = mac;
            return;
        }
    }

    s_cache[s_next_replace].valid = true;
    s_cache[s_next_replace].ip = ip;
    s_cache[s_next_replace].mac = mac;
    s_next_replace = (s_next_replace + 1) % MAX_CACHE_ENTRIES;
}

static auto cache_lookup_locked(ipv4_address ip, mac_address* out_mac) -> bool {
    for (usize i = 0; i < MAX_CACHE_ENTRIES; ++i) {
        if (s_cache[i].valid && net::ipv4_equal(s_cache[i].ip, ip)) {
            if (out_mac != null) {
                *out_mac = s_cache[i].mac;
            }
            return true;
        }
    }
    return false;
}

static auto parse_arp_frame(const void* frame, u32 frame_length,
                            parsed_arp_frame* out) -> bool {
    if (frame == null ||
        out == null ||
        frame_length < wire::ETHERNET_HEADER_BYTES + wire::ARP_ETHERNET_IPV4_BYTES) {
        return false;
    }

    const auto* eth = static_cast<const wire::ethernet_header*>(frame);
    if (net::bswap16(eth->ether_type_be) != static_cast<u16>(packet::ether_type::arp)) {
        return false;
    }

    const auto* arp = reinterpret_cast<const wire::arp_ethernet_ipv4_packet*>(
        static_cast<const u8*>(frame) + sizeof(wire::ethernet_header));
    if (net::bswap16(arp->hardware_type_be) != ARP_ETHERNET_HTYPE ||
        net::bswap16(arp->protocol_type_be) != static_cast<u16>(packet::ether_type::ipv4) ||
        arp->hardware_len != ARP_HLEN_ETHERNET ||
        arp->protocol_len != ARP_PLEN_IPV4) {
        return false;
    }

    out->sender_ip = arp->sender_ip;
    out->sender_mac = arp->sender_mac;
    out->target_ip = arp->target_ip;
    out->operation = net::bswap16(arp->operation_be);
    return true;
}

} // namespace

void init() {
    if (s_initialised) return;
    for (usize i = 0; i < MAX_CACHE_ENTRIES; ++i) {
        s_cache[i] = {};
    }
    s_next_replace = 0;
    s_initialised = true;
}

bool lookup(ipv4_address ip, mac_address* out_mac) {
    if (!s_initialised) init();
    s_cache_lock.acquire();
    const bool found = cache_lookup_locked(ip, out_mac);
    s_cache_lock.release();
    return found;
}

void insert(ipv4_address ip, mac_address mac) {
    if (!s_initialised) init();
    s_cache_lock.acquire();
    cache_insert_locked(ip, mac);
    s_cache_lock.release();
}

auto cache_entry_count() -> usize {
    if (!s_initialised) init();

    usize count = 0;
    s_cache_lock.acquire();
    for (usize i = 0; i < MAX_CACHE_ENTRIES; ++i) {
        if (s_cache[i].valid) {
            ++count;
        }
    }
    s_cache_lock.release();
    return count;
}

bool cache_entry(usize index, cache_entry_info* out_entry) {
    if (out_entry == null) {
        return false;
    }
    if (!s_initialised) init();

    usize visible = 0;
    s_cache_lock.acquire();
    for (usize i = 0; i < MAX_CACHE_ENTRIES; ++i) {
        if (!s_cache[i].valid) {
            continue;
        }
        if (visible == index) {
            out_entry->ip = s_cache[i].ip;
            out_entry->mac = s_cache[i].mac;
            s_cache_lock.release();
            return true;
        }
        ++visible;
    }
    s_cache_lock.release();
    return false;
}

bool observe_frame(net_device* dev, const void* frame, u32 frame_length) {
    if (!s_initialised) init();

    parsed_arp_frame parsed {};
    if (!parse_arp_frame(frame, frame_length, &parsed)) {
        return false;
    }

    insert(parsed.sender_ip, parsed.sender_mac);
    if (dev != null &&
        parsed.operation == static_cast<u16>(packet::arp_operation::request) &&
        ipv4::owns_address(dev, parsed.target_ip)) {
        (void)packet::send_arp_reply(dev,
                                     parsed.sender_mac,
                                     parsed.target_ip,
                                     parsed.sender_ip);
    }
    return true;
}

bool resolve(net_device* dev,
             ipv4_address sender_ip,
             ipv4_address target_ip,
             mac_address* out_mac) {
    if (dev == null || out_mac == null) {
        return false;
    }
    if (!s_initialised) init();

    if (lookup(target_ip, out_mac)) {
        return true;
    }

    if (!packet::send_arp_request(dev, sender_ip, target_ip)) {
        return false;
    }

    const u64 start_tick = sched::tick_count();
    while (sched::tick_count() - start_tick < ARP_RESOLVE_TIMEOUT_TICKS) {
        if (lookup(target_ip, out_mac)) {
            return true;
        }
        sched::sleep(1);
    }

    log::warn() << "arp: resolution timed out on " << dev->name.c_str();
    return false;
}

bool resolve_default(ipv4_address sender_ip,
                     ipv4_address target_ip,
                     mac_address* out_mac) {
    auto* dev = net::primary_device();
    if (dev == null) {
        log::warn() << "arp: no default network device";
        return false;
    }
    return resolve(dev, sender_ip, target_ip, out_mac);
}

} // namespace vk::net::arp
