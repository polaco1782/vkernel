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

namespace net {

struct mac_address {
    u8 bytes[6];
};

struct ipv4_address {
    u8 bytes[4];
};

[[nodiscard]] constexpr auto make_mac(u8 b0, u8 b1, u8 b2,
                                      u8 b3, u8 b4, u8 b5) -> mac_address {
    return { { b0, b1, b2, b3, b4, b5 } };
}

[[nodiscard]] constexpr auto make_ipv4(u8 a, u8 b, u8 c, u8 d) -> ipv4_address {
    return { { a, b, c, d } };
}

[[nodiscard]] constexpr auto zero_ipv4() -> ipv4_address {
    return make_ipv4(0, 0, 0, 0);
}

[[nodiscard]] constexpr auto broadcast_mac() -> mac_address {
    return make_mac(0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF);
}

[[nodiscard]] constexpr auto zero_mac() -> mac_address {
    return make_mac(0, 0, 0, 0, 0, 0);
}

[[nodiscard]] constexpr auto bswap16(u16 value) -> u16 {
    return static_cast<u16>((value << 8) | (value >> 8));
}

[[nodiscard]] constexpr auto ipv4_equal(ipv4_address lhs, ipv4_address rhs) -> bool {
    for (usize i = 0; i < 4; ++i) {
        if (lhs.bytes[i] != rhs.bytes[i]) {
            return false;
        }
    }
    return true;
}

inline auto format_mac(mac_address mac, char* out, usize out_size) -> void {
    if (out == null || out_size < 18) {
        return;
    }

    static constexpr char hex_digits[] = "0123456789ABCDEF";
    usize pos = 0;
    for (usize i = 0; i < 6; ++i) {
        const u8 byte = mac.bytes[i];
        out[pos++] = hex_digits[byte >> 4];
        out[pos++] = hex_digits[byte & 0x0F];
        if (i + 1 < 6) {
            out[pos++] = ':';
        }
    }
    out[pos] = '\0';
}

inline auto mac2str(mac_address mac) -> static_string<18> {
    char buf[18];
    format_mac(mac, buf, sizeof(buf));
    return static_string<18>(buf);
}

inline auto format_ipv4(ipv4_address ip, char* out, usize out_size) -> void {
    if (out == null || out_size < 16) {
        return;
    }

    usize pos = 0;
    for (usize i = 0; i < 4; ++i) {
        u8 value = ip.bytes[i];
        if (value >= 100) {
            out[pos++] = static_cast<char>('0' + (value / 100));
            value = static_cast<u8>(value % 100);
            out[pos++] = static_cast<char>('0' + (value / 10));
            out[pos++] = static_cast<char>('0' + (value % 10));
        } else if (value >= 10) {
            out[pos++] = static_cast<char>('0' + (value / 10));
            out[pos++] = static_cast<char>('0' + (value % 10));
        } else {
            out[pos++] = static_cast<char>('0' + value);
        }

        if (i + 1 < 4) {
            out[pos++] = '.';
        }
    }
    out[pos] = '\0';
}

inline auto ipv42str(ipv4_address ip) -> static_string<16> {
    char buf[16];
    format_ipv4(ip, buf, sizeof(buf));
    return static_string<16>(buf);
}

[[nodiscard]] inline auto internet_checksum(const void* data, usize length) -> u16 {
    const auto* bytes = static_cast<const u8*>(data);
    u32 sum = 0;

    for (usize i = 0; i + 1 < length; i += 2) {
        const u16 word = static_cast<u16>(
            (static_cast<u16>(bytes[i]) << 8) |
            static_cast<u16>(bytes[i + 1]));
        sum += word;
        if (sum > 0xFFFFu) {
            sum = (sum & 0xFFFFu) + 1u;
        }
    }

    if ((length & 1u) != 0) {
        sum += static_cast<u16>(bytes[length - 1] << 8);
        if (sum > 0xFFFFu) {
            sum = (sum & 0xFFFFu) + 1u;
        }
    }

    return static_cast<u16>(~sum);
}

} // namespace net

struct net_ops {
    bool (*send_packet)(net_device* dev, const void* packet, u32 length);
    bool (*poll_packet)(net_device* dev, void* packet_out, u32 packet_capacity,
                        u32* packet_length_out);
};

struct net_device {
    static_string<32> name;
    net::mac_address  mac {};
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

[[nodiscard]] inline auto device_mac(const net_device& dev) -> mac_address {
    return dev.mac;
}

void init();
auto register_device(const net_device& dev) -> i32;
auto device_count() -> usize;
auto get_device(usize index) -> net_device*;
auto find(const char* name) -> net_device*;
auto primary_device() -> net_device*;
bool send_packet(net_device* dev, const void* packet, u32 length);
bool send_default(const void* packet, u32 length);
bool queue_packet(net_device* dev, const void* packet, u32 length);
bool queue_default(const void* packet, u32 length);
bool poll_packet(net_device* dev, void* packet_out, u32 packet_capacity,
                 u32* packet_length_out);
bool start_background_rx();
bool background_rx_running();
void list_devices();

} // namespace net
} // namespace vk

#endif /* VKERNEL_NET_H */
