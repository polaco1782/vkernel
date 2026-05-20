/*
 * vkernel - UEFI Microkernel
 * Copyright (C) 2026 vkernel authors
 *
 * net.cpp - Network device registry and helpers
 */

#include "config.h"
#include "types.h"
#include "console.h"
#include "log.h"
#include "memory.h"
#include "net.h"

namespace vk {
namespace net {

namespace {
    
static auto format_mac(const u8 mac[6], char* out, usize out_size) -> void {
    if (out == null || out_size < 18) {
        return;
    }

    static constexpr char hex_digits[] = "0123456789ABCDEF";
    usize pos = 0;
    for (usize i = 0; i < 6; ++i) {
        const u8 byte = mac[i];
        out[pos++] = hex_digits[byte >> 4];
        out[pos++] = hex_digits[byte & 0x0F];
        if (i + 1 < 6) {
            out[pos++] = ':';
        }
    }
    out[pos] = '\0';
}

} // namespace

static net_device s_devices[MAX_NET_DEVICES];
static usize      s_device_count = 0;
static bool       s_initialised = false;

void init() {
    if (s_initialised) return;
    memory::set(s_devices, 0, sizeof(s_devices));
    s_device_count = 0;
    s_initialised = true;
}

auto register_device(const net_device& dev) -> i32 {
    if (!s_initialised) init();
    if (s_device_count >= MAX_NET_DEVICES) {
        log::warn() << "net: registry full, cannot register " << dev.name.c_str();
        return -1;
    }
    if (dev.name.empty()) {
        log::warn() << "net: invalid device descriptor";
        return -1;
    }

    s_devices[s_device_count] = dev;

    char mac_buf[18];
    format_mac(s_devices[s_device_count].mac, mac_buf, sizeof(mac_buf));
    log::info() << "net: registered " << s_devices[s_device_count].name.c_str()
                << " mac=" << mac_buf
                << " mtu=" << s_devices[s_device_count].mtu
                << " link=" << (s_devices[s_device_count].link_up ? "up" : "down");

    ++s_device_count;
    return static_cast<i32>(s_device_count - 1);
}

auto device_count() -> usize {
    return s_device_count;
}

auto get_device(usize index) -> net_device* {
    if (index >= s_device_count) return null;
    return &s_devices[index];
}

auto find(const char* name) -> net_device* {
    string_view query(name);
    for (usize i = 0; i < s_device_count; ++i) {
        if (s_devices[i].name.view().equals(query)) {
            return &s_devices[i];
        }
    }
    return null;
}

auto primary_device() -> net_device* {
    net_device* first = null;
    for (usize i = 0; i < s_device_count; ++i) {
        auto* dev = &s_devices[i];
        if (first == null) {
            first = dev;
        }
        if (dev->link_up) {
            return dev;
        }
    }
    return first;
}

bool send_packet(net_device* dev, const void* packet, u32 length) {
    if (dev == null || packet == null || length == 0 || dev->ops == null ||
        dev->ops->send_packet == null) {
        return false;
    }
    return dev->ops->send_packet(dev, packet, length);
}

bool send_default(const void* packet, u32 length) {
    auto* dev = primary_device();
    if (dev == null) {
        log::warn() << "net: no network device available for packet send";
        return false;
    }
    return send_packet(dev, packet, length);
}

void list_devices() {
    log::info() << "Network devices:";
    if (s_device_count == 0) {
        log::info() << "  (none)";
        return;
    }

    for (usize i = 0; i < s_device_count; ++i) {
        auto& dev = s_devices[i];
        char mac_buf[18];
        format_mac(dev.mac, mac_buf, sizeof(mac_buf));
        log::info() << "  [" << i << "] " << dev.name.c_str()
                    << ": mac=" << mac_buf
                    << " mtu=" << dev.mtu
                    << " link=" << (dev.link_up ? "up" : "down");
    }
}

} // namespace net
} // namespace vk
