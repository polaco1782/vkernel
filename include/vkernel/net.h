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

struct net_device {
    static_string<32> name;
    u8                mac[6] = {};
    u16               mtu = 1500;
    bool              link_up = false;
    void*             driver_data = null;
};

struct net_driver_t {
    const char* name;
    bool (*init)();
    void (*shutdown)();
};

namespace net {

inline constexpr usize MAX_NET_DEVICES = 8;

void init();
auto register_device(const net_device& dev) -> i32;
auto device_count() -> usize;
auto get_device(usize index) -> net_device*;
auto find(const char* name) -> net_device*;
void list_devices();

} // namespace net
} // namespace vk

#endif /* VKERNEL_NET_H */
