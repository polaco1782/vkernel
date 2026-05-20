/*
 * vkernel - UEFI Microkernel
 * Copyright (C) 2026 vkernel authors
 *
 * net.h - Network device subsystem
 */

#ifndef VKERNEL_NET_H
#define VKERNEL_NET_H

#include "types.h"

namespace vk {

struct net_device;

struct net_ops {
    bool (*send_packet)(net_device* dev, const void* packet, u32 length);
};

struct net_device {
    static_string<32> name;
    u8                mac[6] = {};
    u16               mtu = 1500;
    bool              link_up = false;
    void*             driver_data = null;
    const net_ops*    ops = null;
};

struct net_driver_t {
    const char* name;
    bool (*init)();
    void (*shutdown)();
};

namespace net {

inline constexpr usize MAX_NET_DEVICES = 8;

[[nodiscard]] constexpr auto bswap16(u16 value) -> u16;
void init();
auto register_device(const net_device& dev) -> i32;
auto device_count() -> usize;
auto get_device(usize index) -> net_device*;
auto find(const char* name) -> net_device*;
auto primary_device() -> net_device*;
bool send_packet(net_device* dev, const void* packet, u32 length);
bool send_default(const void* packet, u32 length);
void list_devices();

} // namespace net
} // namespace vk

#endif /* VKERNEL_NET_H */
