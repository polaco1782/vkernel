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
#include "net_packet.h"
#include "net_wire.h"
#include "arp.h"
#include "ipv4.h"
#include "scheduler.h"
#include "spinlock.h"

namespace vk {
namespace net {

namespace {

constexpr u32 NET_RX_FRAME_BUFFER_BYTES = 2048;
constexpr usize NET_TX_QUEUE_CAPACITY = 64;

struct queued_packet {
    net_device* dev = null;
    u8*         buffer = null;
    u32         length = 0;
};

static bool s_background_rx_started = false;
static bool s_background_tx_started = false;
static queued_packet s_tx_queue[NET_TX_QUEUE_CAPACITY];
static usize s_tx_queue_head = 0;
static usize s_tx_queue_tail = 0;
static spinlock s_tx_queue_lock;

[[nodiscard]] static bool tx_queue_empty() {
    return s_tx_queue_head == s_tx_queue_tail;
}

[[nodiscard]] static bool tx_queue_full() {
    return ((s_tx_queue_head + 1) % NET_TX_QUEUE_CAPACITY) == s_tx_queue_tail;
}

static void dispatch_frame(net_device* dev, const void* frame, u32 frame_length) {
    if (dev == null || frame == null || frame_length < wire::ETHERNET_HEADER_BYTES) {
        return;
    }

    const auto* eth = static_cast<const wire::ethernet_header*>(frame);
    switch (net::bswap16(eth->ether_type_be)) {
        case static_cast<u16>(packet::ether_type::arp):
            (void)arp::observe_frame(dev, frame, frame_length);
            break;
        case static_cast<u16>(packet::ether_type::ipv4):
            (void)ipv4::observe_frame(dev, frame, frame_length);
            break;
        default:
            break;
    }
}

static void background_rx_task(void*) {
    u8 frame_buffer[NET_RX_FRAME_BUFFER_BYTES];

    while (true) {
        bool any_packets = false;

        const usize count = device_count();
        for (usize i = 0; i < count; ++i) {
            auto* dev = get_device(i);
            if (dev == null) {
                continue;
            }

            while (true) {
                u32 frame_length = 0;
                if (!poll_packet(dev, frame_buffer, sizeof(frame_buffer), &frame_length)) {
                    break;
                }
                any_packets = true;
                dispatch_frame(dev, frame_buffer, frame_length);

                {
                    auto* eth = reinterpret_cast<const wire::ethernet_header*>(frame_buffer);

                    log::debug() << "net: received packet on " << dev->name.c_str()
                                << " length=" << frame_length
                                << " mac_src=" << mac2str(eth->src)
                                << " mac_dst=" << mac2str(eth->dst)
                                << " ether_type="
                                << log::hex(
                                    net::bswap16(eth->ether_type_be), 4, true, false);
                }
            }
        }

        if (!any_packets) {
            sched::sleep(1);
        }
    }
}

static void background_tx_task(void*) {
    while (true) {
        queued_packet entry {};

        s_tx_queue_lock.acquire();
        if (!tx_queue_empty()) {
            entry = s_tx_queue[s_tx_queue_tail];
            s_tx_queue[s_tx_queue_tail] = {};
            s_tx_queue_tail = (s_tx_queue_tail + 1) % NET_TX_QUEUE_CAPACITY;
        }
        s_tx_queue_lock.release();

        if (entry.buffer == null) {
            sched::sleep(1);
            continue;
        }

        if (!send_packet(entry.dev, entry.buffer, entry.length) &&
            entry.dev != null) {
            log::warn() << "net: TX worker failed to send packet on "
                        << entry.dev->name.c_str()
                        << " length=" << entry.length;
        }
        g_kernel_heap.free(entry.buffer);
    }
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
        if (s_devices[i].name.view().compare(query)) {
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

bool queue_packet(net_device* dev, const void* packet, u32 length) {
    if (dev == null || packet == null || length == 0) {
        return false;
    }

    if (!s_background_tx_started) {
        return send_packet(dev, packet, length);
    }

    auto* buffer = static_cast<u8*>(g_kernel_heap.allocate(length));
    if (buffer == null) {
        log::warn() << "net: failed to allocate TX queue buffer (" << length
                    << " bytes)";
        return false;
    }
    memory::copy(buffer, packet, length);

    s_tx_queue_lock.acquire();
    if (tx_queue_full()) {
        s_tx_queue_lock.release();
        g_kernel_heap.free(buffer);
        log::warn() << "net: TX queue full, dropping packet on "
                    << dev->name.c_str()
                    << " length=" << length;
        return false;
    }

    s_tx_queue[s_tx_queue_head].dev = dev;
    s_tx_queue[s_tx_queue_head].buffer = buffer;
    s_tx_queue[s_tx_queue_head].length = length;
    s_tx_queue_head = (s_tx_queue_head + 1) % NET_TX_QUEUE_CAPACITY;
    s_tx_queue_lock.release();
    return true;
}

bool queue_default(const void* packet, u32 length) {
    auto* dev = primary_device();
    if (dev == null) {
        log::warn() << "net: no network device available for packet queue";
        return false;
    }
    return queue_packet(dev, packet, length);
}

bool poll_packet(net_device* dev, void* packet_out, u32 packet_capacity,
                 u32* packet_length_out) {
    if (dev == null || packet_out == null || packet_capacity == 0 ||
        packet_length_out == null || dev->ops == null ||
        dev->ops->poll_packet == null) {
        return false;
    }
    return dev->ops->poll_packet(dev, packet_out, packet_capacity, packet_length_out);
}

bool start_background_worker() {
    if (device_count() == 0) {
        return false;
    }

    if (!s_background_tx_started) {
        const i64 tx_task_id = sched::create_task("net_tx", background_tx_task, null);
        if (tx_task_id < 0) {
            log::warn() << "net: failed to create background TX task";
            return false;
        }

        s_background_tx_started = true;
        log::info() << "net: background TX task started";
    }

    if (!s_background_rx_started) {
        const i64 rx_task_id = sched::create_task("net_rx", background_rx_task, null);
        if (rx_task_id < 0) {
            log::warn() << "net: failed to create background RX task";
            return false;
        }

        s_background_rx_started = true;
        log::info() << "net: background RX task started";
    }

    return s_background_rx_started && s_background_tx_started;
}

bool background_rx_running() {
    return s_background_rx_started;
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
