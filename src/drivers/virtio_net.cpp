/*
 * vkernel - UEFI Microkernel
 * Copyright (C) 2026 vkernel authors
 *
 * virtio_net.cpp - Legacy PCI virtio network driver
 */

#include "config.h"
#include "types.h"
#include "log.h"
#include "memory.h"
#include "net.h"
#include "driver.h"
#include "pci.h"
#include "arch/x86_64/arch.h"

namespace vk {
namespace {

constexpr u16 VIRTIO_PCI_DEVICE_FEATURES = 0x00;
constexpr u16 VIRTIO_PCI_DRIVER_FEATURES = 0x04;
constexpr u16 VIRTIO_PCI_QUEUE_PFN       = 0x08;
constexpr u16 VIRTIO_PCI_QUEUE_NUM       = 0x0C;
constexpr u16 VIRTIO_PCI_QUEUE_SEL       = 0x0E;
constexpr u16 VIRTIO_PCI_QUEUE_NOTIFY    = 0x10;
constexpr u16 VIRTIO_PCI_STATUS          = 0x12;
constexpr u16 VIRTIO_PCI_ISR             = 0x13;
constexpr u16 VIRTIO_PCI_CONFIG          = 0x14;

constexpr u8 VIRTIO_STATUS_ACKNOWLEDGE = 0x01;
constexpr u8 VIRTIO_STATUS_DRIVER      = 0x02;
constexpr u8 VIRTIO_STATUS_DRIVER_OK   = 0x04;
constexpr u8 VIRTIO_STATUS_FAILED      = 0x80;

constexpr u32 VIRTIO_NET_F_MAC    = 1u << 5;
constexpr u32 VIRTIO_NET_F_STATUS = 1u << 16;

constexpr u16 VIRTIO_NET_S_LINK_UP = 0x0001;

constexpr u16 VIRTIO_NET_QUEUE_RX = 0;
constexpr u16 VIRTIO_NET_QUEUE_TX = 1;

#pragma pack(push, 1)
struct vring_desc {
    u64 addr;
    u32 len;
    u16 flags;
    u16 next;
};

struct vring_avail {
    u16 flags;
    u16 idx;
    u16 ring[1];
};

struct vring_used_elem {
    u32 id;
    u32 len;
};

struct vring_used {
    u16 flags;
    u16 idx;
    vring_used_elem ring[1];
};
#pragma pack(pop)

static_assert(sizeof(vring_desc) == 16);
static_assert(sizeof(vring_used_elem) == 8);

struct virtqueue_state {
    u16         size = 0;
    u16         last_used_idx = 0;
    phys_addr   phys = 0;
    u32         pages = 0;
    u8*         mem = null;
    vring_desc* desc = null;
    vring_avail* avail = null;
    vring_used* used = null;
};

struct virtio_net_device {
    bool            present = false;
    pci_address     pci_addr = {};
    u16             io_base = 0;
    u32             negotiated_features = 0;
    u8              mac[6] = {};
    bool            mac_valid = false;
    u16             link_status = 0;
    u16             mtu = 1500;
    virtqueue_state rxq = {};
    virtqueue_state txq = {};
};

static virtio_net_device s_devices[net::MAX_NET_DEVICES];
static usize s_device_count = 0;

static inline auto virtio_read8(const virtio_net_device& dev, u16 reg) -> u8 {
    return arch::inb(static_cast<u16>(dev.io_base + reg));
}

static inline auto virtio_read16(const virtio_net_device& dev, u16 reg) -> u16 {
    return arch::inw(static_cast<u16>(dev.io_base + reg));
}

static inline auto virtio_read32(const virtio_net_device& dev, u16 reg) -> u32 {
    return arch::inl(static_cast<u16>(dev.io_base + reg));
}

static inline void virtio_write8(const virtio_net_device& dev, u16 reg, u8 value) {
    arch::outb(static_cast<u16>(dev.io_base + reg), value);
}

static inline void virtio_write16(const virtio_net_device& dev, u16 reg, u16 value) {
    arch::outw(static_cast<u16>(dev.io_base + reg), value);
}

static inline void virtio_write32(const virtio_net_device& dev, u16 reg, u32 value) {
    arch::outl(static_cast<u16>(dev.io_base + reg), value);
}

static inline auto virtio_config_read8(const virtio_net_device& dev, u16 offset) -> u8 {
    return virtio_read8(dev, static_cast<u16>(VIRTIO_PCI_CONFIG + offset));
}

static inline auto virtio_config_read16(const virtio_net_device& dev, u16 offset) -> u16 {
    return virtio_read16(dev, static_cast<u16>(VIRTIO_PCI_CONFIG + offset));
}

static auto vring_size(u16 queue_size) -> usize {
    const usize desc_bytes = sizeof(vring_desc) * queue_size;
    const usize avail_bytes = sizeof(u16) * (3 + queue_size);
    const usize used_offset = align_up(desc_bytes + avail_bytes, static_cast<usize>(PAGE_SIZE_4K));
    const usize used_bytes = sizeof(u16) * 3 + (sizeof(vring_used_elem) * queue_size);
    return align_up(used_offset + used_bytes, static_cast<usize>(PAGE_SIZE_4K));
}

static void cleanup_queue(virtqueue_state& queue) {
    if (queue.phys != 0 && queue.pages != 0) {
        g_phys_alloc.free_pages(queue.phys, queue.pages);
    }
    queue = {};
}

static auto setup_queue(virtio_net_device& dev, u16 queue_index, virtqueue_state& queue) -> bool {
    virtio_write16(dev, VIRTIO_PCI_QUEUE_SEL, queue_index);
    queue.size = virtio_read16(dev, VIRTIO_PCI_QUEUE_NUM);
    if (queue.size == 0) {
        return false;
    }

    const usize queue_bytes = vring_size(queue.size);
    queue.pages = static_cast<u32>(queue_bytes / PAGE_SIZE_4K);
    queue.phys = g_phys_alloc.allocate_pages(queue.pages, PAGE_SIZE_4K, 0x100000000ULL);
    if (queue.phys == 0) {
        queue.pages = 0;
        return false;
    }

    queue.mem = reinterpret_cast<u8*>(queue.phys);
    memory::set(queue.mem, 0, queue_bytes);
    arch::make_region_writable(queue.phys, queue_bytes);

    const usize desc_bytes = sizeof(vring_desc) * queue.size;
    const usize avail_bytes = sizeof(u16) * (3 + queue.size);
    const usize used_offset = align_up(desc_bytes + avail_bytes, static_cast<usize>(PAGE_SIZE_4K));

    queue.desc = reinterpret_cast<vring_desc*>(queue.mem);
    queue.avail = reinterpret_cast<vring_avail*>(queue.mem + desc_bytes);
    queue.used = reinterpret_cast<vring_used*>(queue.mem + used_offset);
    queue.last_used_idx = queue.used->idx;

    virtio_write32(dev, VIRTIO_PCI_QUEUE_PFN, static_cast<u32>(queue.phys / PAGE_SIZE_4K));
    return true;
}

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

static auto init_device(const pci_device& pci_dev, virtio_net_device& dev, usize net_index) -> bool {
    if ((pci_dev.bar[0] & 0x1u) == 0) {
        log::warn() << "virtio_net: BAR0 is not an I/O BAR";
        return false;
    }

    dev = {};
    dev.pci_addr = pci_dev.addr;
    dev.io_base = static_cast<u16>(pci_dev.bar[0] & ~0x3u);
    if (dev.io_base == 0) {
        return false;
    }

    pci::enable_bus_master(pci_dev.addr);

    virtio_write8(dev, VIRTIO_PCI_STATUS, 0);
    virtio_write8(dev, VIRTIO_PCI_STATUS, VIRTIO_STATUS_ACKNOWLEDGE);
    virtio_write8(dev, VIRTIO_PCI_STATUS, VIRTIO_STATUS_ACKNOWLEDGE | VIRTIO_STATUS_DRIVER);

    const u32 device_features = virtio_read32(dev, VIRTIO_PCI_DEVICE_FEATURES);
    dev.negotiated_features = 0;
    if ((device_features & VIRTIO_NET_F_MAC) != 0) {
        dev.negotiated_features |= VIRTIO_NET_F_MAC;
    }
    if ((device_features & VIRTIO_NET_F_STATUS) != 0) {
        dev.negotiated_features |= VIRTIO_NET_F_STATUS;
    }
    virtio_write32(dev, VIRTIO_PCI_DRIVER_FEATURES, dev.negotiated_features);

    if (!setup_queue(dev, VIRTIO_NET_QUEUE_RX, dev.rxq) ||
        !setup_queue(dev, VIRTIO_NET_QUEUE_TX, dev.txq)) {
        cleanup_queue(dev.rxq);
        cleanup_queue(dev.txq);
        virtio_write8(dev, VIRTIO_PCI_STATUS, VIRTIO_STATUS_FAILED);
        return false;
    }

    if ((dev.negotiated_features & VIRTIO_NET_F_MAC) != 0) {
        for (usize i = 0; i < 6; ++i) {
            dev.mac[i] = virtio_config_read8(dev, static_cast<u16>(i));
        }
        dev.mac_valid = true;
    }

    if ((dev.negotiated_features & VIRTIO_NET_F_STATUS) != 0) {
        dev.link_status = virtio_config_read16(dev, 6);
    }

    virtio_write8(dev, VIRTIO_PCI_STATUS,
                  VIRTIO_STATUS_ACKNOWLEDGE | VIRTIO_STATUS_DRIVER | VIRTIO_STATUS_DRIVER_OK);
    (void)virtio_read8(dev, VIRTIO_PCI_ISR);
    dev.present = true;

    net_device net_dev {};
    char name[16];
    name[0] = 'n';
    name[1] = 'e';
    name[2] = 't';
    name[3] = static_cast<char>('0' + net_index);
    name[4] = '\0';

    (void)net_dev.name.assign(name);
    if (dev.mac_valid) {
        memory::copy(net_dev.mac, dev.mac, sizeof(net_dev.mac));
    }
    net_dev.mtu = dev.mtu;
    net_dev.link_up = (dev.link_status & VIRTIO_NET_S_LINK_UP) != 0;
    net_dev.driver_data = &dev;

    if (net::register_device(net_dev) < 0) {
        cleanup_queue(dev.rxq);
        cleanup_queue(dev.txq);
        virtio_write8(dev, VIRTIO_PCI_STATUS, VIRTIO_STATUS_FAILED);
        dev = {};
        return false;
    }

    char mac_buf[18];
    format_mac(dev.mac, mac_buf, sizeof(mac_buf));
    log::info() << "virtio_net: registered " << net_dev.name.c_str()
                << " io_base=" << log::hex(static_cast<u64>(dev.io_base), 1, true, false)
                << " mac=" << mac_buf
                << " rxq=" << dev.rxq.size
                << " txq=" << dev.txq.size
                << " link=" << (net_dev.link_up ? "up" : "down");
    return true;
}

static bool virtio_net_init() {
    net::init();
    s_device_count = 0;

    usize found_count = 0;
    const usize pci_count = pci::device_count();
    for (usize i = 0; i < pci_count; ++i) {
        const auto* dev = pci::get_device(i);
        if (dev == null) {
            continue;
        }
        if (dev->vendor_id != pci_ids::VENDOR_VIRTIO ||
            dev->device_id != pci_ids::DEVICE_VIRTIO_NET_LEGACY) {
            continue;
        }
        if (found_count >= array_size(s_devices)) {
            log::warn() << "virtio_net: too many virtio-net devices, ignoring extra NICs";
            break;
        }
        if (init_device(*dev, s_devices[found_count], found_count)) {
            ++found_count;
        }
    }

    s_device_count = found_count;
    if (s_device_count == 0) {
        log::warn() << "virtio_net: no usable legacy virtio network PCI device found";
        return false;
    }

    net::list_devices();
    return true;
}

static void virtio_net_shutdown() {
    for (usize i = 0; i < s_device_count; ++i) {
        auto& dev = s_devices[i];
        if (dev.present) {
            virtio_write8(dev, VIRTIO_PCI_STATUS, 0);
        }
        cleanup_queue(dev.rxq);
        cleanup_queue(dev.txq);
        dev = {};
    }
    s_device_count = 0;
}

static const net_driver_t s_virtio_net_driver = {
    .name = "virtio_net",
    .init = virtio_net_init,
    .shutdown = virtio_net_shutdown,
};

static const driver_descriptor s_virtio_net_descriptor = {
    .name = "virtio_net",
    .type = driver_type::network,
    .sound = null,
    .block = null,
    .net = &s_virtio_net_driver,
};

} // namespace

namespace virtio_net_driver {

void register_builtin() {
    driver::register_driver(&s_virtio_net_descriptor);
}

} // namespace virtio_net_driver
} // namespace vk
