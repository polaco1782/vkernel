/*
 * vkernel - UEFI Microkernel
 * Copyright (C) 2026 vkernel authors
 *
 * arp.h - Tiny ARP cache and blocking resolver
 */

#ifndef VKERNEL_ARP_H
#define VKERNEL_ARP_H

#include "types.h"
#include "net.h"

namespace vk::net::arp {

inline constexpr usize MAX_CACHE_ENTRIES = 16;

struct cache_entry_info {
    ipv4_address ip {};
    mac_address  mac {};
};

void init();
bool lookup(ipv4_address ip, mac_address* out_mac);
void insert(ipv4_address ip, mac_address mac);
auto cache_entry_count() -> usize;
bool cache_entry(usize index, cache_entry_info* out_entry);
bool observe_frame(net_device* dev, const void* frame, u32 frame_length);
bool resolve(net_device* dev,
             ipv4_address sender_ip,
             ipv4_address target_ip,
             mac_address* out_mac);
bool resolve_default(ipv4_address sender_ip,
                     ipv4_address target_ip,
                     mac_address* out_mac);

} // namespace vk::net::arp

#endif /* VKERNEL_ARP_H */
