/*
 * vkernel - UEFI Microkernel
 * Copyright (C) 2026 vkernel authors
 *
 * virtio_blk.cpp - Legacy PCI virtio block driver
 */

#include "config.h"
#include "types.h"
#include "log.h"
#include "memory.h"
#include "block.h"
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

constexpr u16 VRING_DESC_F_NEXT  = 0x01;
constexpr u16 VRING_DESC_F_WRITE = 0x02;

constexpr u32 VIRTIO_BLK_T_IN  = 0;
constexpr u32 VIRTIO_BLK_T_OUT = 1;
constexpr u8 VIRTIO_BLK_S_OK   = 0;

constexpr u32 VIRTIO_BLK_SECTOR_SIZE = 512;
constexpr u32 VIRTIO_BLK_MAX_SECTORS_PER_REQUEST = 128;
constexpr u32 VIRTIO_BLK_BOUNCE_BYTES =
    VIRTIO_BLK_SECTOR_SIZE * VIRTIO_BLK_MAX_SECTORS_PER_REQUEST;

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

struct virtio_blk_req_header {
    u32 type;
    u32 reserved;
    u64 sector;
};
#pragma pack(pop)

static_assert(sizeof(vring_desc) == 16);
static_assert(sizeof(vring_used_elem) == 8);
static_assert(sizeof(virtio_blk_req_header) == 16);

struct virtio_blk_device {
    bool present = false;
    u16 io_base = 0;
    u16 queue_size = 0;
    u16 last_used_idx = 0;
    u64 sectors = 0;
    phys_addr queue_phys = 0;
    u32 queue_pages = 0;
    u8* queue_mem = null;
    vring_desc* desc = null;
    vring_avail* avail = null;
    vring_used* used = null;
    virtio_blk_req_header request_header {};
    u8 request_status = 0xFF;
    phys_addr bounce_phys = 0;
    u8* bounce_mem = null;
};

static virtio_blk_device s_device;

struct atomic_lock {
    volatile u64 locked = 0;

    void acquire() {
        while (!arch::atomic_cmpxchg(&locked, 0, 1)) {
            arch::cpu_pause();
        }
        arch::memory_barrier();
    }

    void release() {
        arch::memory_barrier();
        locked = 0;
    }
};

struct atomic_lock_guard {
    atomic_lock& lock;

    explicit atomic_lock_guard(atomic_lock& lock_in)
        : lock(lock_in) {
        lock.acquire();
    }

    ~atomic_lock_guard() {
        lock.release();
    }
};

static atomic_lock s_request_lock;

static inline auto virtio_read8(const virtio_blk_device& dev, u16 reg) -> u8 {
    return arch::inb(static_cast<u16>(dev.io_base + reg));
}

static inline auto virtio_read16(const virtio_blk_device& dev, u16 reg) -> u16 {
    return arch::inw(static_cast<u16>(dev.io_base + reg));
}

static inline auto virtio_read32(const virtio_blk_device& dev, u16 reg) -> u32 {
    return arch::inl(static_cast<u16>(dev.io_base + reg));
}

static inline void virtio_write8(const virtio_blk_device& dev, u16 reg, u8 value) {
    arch::outb(static_cast<u16>(dev.io_base + reg), value);
}

static inline void virtio_write16(const virtio_blk_device& dev, u16 reg, u16 value) {
    arch::outw(static_cast<u16>(dev.io_base + reg), value);
}

static inline void virtio_write32(const virtio_blk_device& dev, u16 reg, u32 value) {
    arch::outl(static_cast<u16>(dev.io_base + reg), value);
}

static auto virtio_config_read64(const virtio_blk_device& dev, u16 offset) -> u64 {
    const u32 low = virtio_read32(dev, static_cast<u16>(VIRTIO_PCI_CONFIG + offset));
    const u32 high = virtio_read32(dev, static_cast<u16>(VIRTIO_PCI_CONFIG + offset + 4));
    return static_cast<u64>(low) | (static_cast<u64>(high) << 32);
}

static auto vring_size(u16 queue_size) -> usize {
    const usize desc_bytes = sizeof(vring_desc) * queue_size;
    const usize avail_bytes = sizeof(u16) * (3 + queue_size);
    const usize used_offset = align_up(desc_bytes + avail_bytes, static_cast<usize>(PAGE_SIZE_4K));
    const usize used_bytes = sizeof(u16) * 3 + (sizeof(vring_used_elem) * queue_size);
    return align_up(used_offset + used_bytes, static_cast<usize>(PAGE_SIZE_4K));
}

static auto setup_queue(virtio_blk_device& dev) -> bool {
    virtio_write16(dev, VIRTIO_PCI_QUEUE_SEL, 0);
    dev.queue_size = virtio_read16(dev, VIRTIO_PCI_QUEUE_NUM);
    if (dev.queue_size < 3) {
        return false;
    }

    const usize queue_bytes = vring_size(dev.queue_size);
    dev.queue_pages = static_cast<u32>(queue_bytes / PAGE_SIZE_4K);
    dev.queue_phys = g_phys_alloc.allocate_pages(dev.queue_pages, PAGE_SIZE_4K, 0x100000000ULL);
    if (dev.queue_phys == 0) {
        return false;
    }

    dev.queue_mem = reinterpret_cast<u8*>(dev.queue_phys);
    memory::set(dev.queue_mem, 0, queue_bytes);
    arch::make_region_writable(dev.queue_phys, queue_bytes);

    dev.bounce_phys = g_phys_alloc.allocate_pages(
        VIRTIO_BLK_BOUNCE_BYTES / PAGE_SIZE_4K,
        PAGE_SIZE_4K,
        0x100000000ULL);
    if (dev.bounce_phys == 0) {
        g_phys_alloc.free_pages(dev.queue_phys, dev.queue_pages);
        dev.queue_phys = 0;
        dev.queue_pages = 0;
        dev.queue_mem = null;
        return false;
    }
    arch::make_region_writable(dev.bounce_phys, VIRTIO_BLK_BOUNCE_BYTES);
    dev.bounce_mem = reinterpret_cast<u8*>(dev.bounce_phys);
    memory::set(dev.bounce_mem, 0, VIRTIO_BLK_BOUNCE_BYTES);

    const usize desc_bytes = sizeof(vring_desc) * dev.queue_size;
    const usize avail_bytes = sizeof(u16) * (3 + dev.queue_size);
    const usize used_offset = align_up(desc_bytes + avail_bytes, static_cast<usize>(PAGE_SIZE_4K));

    dev.desc = reinterpret_cast<vring_desc*>(dev.queue_mem);
    dev.avail = reinterpret_cast<vring_avail*>(dev.queue_mem + desc_bytes);
    dev.used = reinterpret_cast<vring_used*>(dev.queue_mem + used_offset);
    dev.last_used_idx = dev.used->idx;

    virtio_write32(dev, VIRTIO_PCI_QUEUE_PFN, static_cast<u32>(dev.queue_phys / PAGE_SIZE_4K));
    return true;
}

static auto submit_request(virtio_blk_device& dev, u32 type, u64 sector, void* data, u32 bytes) -> bool {
    if (data == null || bytes == 0 || (bytes % VIRTIO_BLK_SECTOR_SIZE) != 0) {
        return false;
    }

    dev.request_header.type = type;
    dev.request_header.reserved = 0;
    dev.request_header.sector = sector;
    dev.request_status = 0xFF;

    dev.desc[0].addr = reinterpret_cast<u64>(&dev.request_header);
    dev.desc[0].len = sizeof(dev.request_header);
    dev.desc[0].flags = VRING_DESC_F_NEXT;
    dev.desc[0].next = 1;

    dev.desc[1].addr = reinterpret_cast<u64>(data);
    dev.desc[1].len = bytes;
    dev.desc[1].flags = VRING_DESC_F_NEXT | (type == VIRTIO_BLK_T_IN ? VRING_DESC_F_WRITE : 0);
    dev.desc[1].next = 2;

    dev.desc[2].addr = reinterpret_cast<u64>(&dev.request_status);
    dev.desc[2].len = sizeof(dev.request_status);
    dev.desc[2].flags = VRING_DESC_F_WRITE;
    dev.desc[2].next = 0;

    const u16 avail_slot = dev.avail->idx % dev.queue_size;
    dev.avail->ring[avail_slot] = 0;
    arch::memory_barrier();
    dev.avail->idx = static_cast<u16>(dev.avail->idx + 1);
    arch::memory_barrier();

    virtio_write16(dev, VIRTIO_PCI_QUEUE_NOTIFY, 0);

    constexpr u32 VIRTIO_BLK_TIMEOUT_SPINS = 10000000;
    for (u32 spin = 0; spin < VIRTIO_BLK_TIMEOUT_SPINS; ++spin) {
        arch::memory_barrier();
        if (dev.used->idx != dev.last_used_idx) {
            const u16 used_slot = dev.last_used_idx % dev.queue_size;
            const auto used = dev.used->ring[used_slot];
            dev.last_used_idx = static_cast<u16>(dev.last_used_idx + 1);
            (void)virtio_read8(dev, VIRTIO_PCI_ISR);
            return used.id == 0 && dev.request_status == VIRTIO_BLK_S_OK;
        }
        arch::cpu_pause();
    }

    return false;
}

static auto virtio_blk_transfer(block_device* block_dev, u64 lba, u32 count, void* buffer, bool write) -> bool {
    if (block_dev == null || buffer == null || count == 0) {
        return false;
    }

    auto* dev = static_cast<virtio_blk_device*>(block_dev->driver_data);
    if (dev == null || !dev->present) {
        return false;
    }

    atomic_lock_guard guard(s_request_lock);

    auto* current_buffer = static_cast<u8*>(buffer);
    u64 current_lba = lba;
    u32 remaining = count;
    while (remaining > 0) {
        const u32 chunk = remaining > VIRTIO_BLK_MAX_SECTORS_PER_REQUEST
            ? VIRTIO_BLK_MAX_SECTORS_PER_REQUEST
            : remaining;
        const u32 bytes = chunk * VIRTIO_BLK_SECTOR_SIZE;
        if (write) {
            memory::copy(dev->bounce_mem, current_buffer, bytes);
        }

        if (!submit_request(*dev,
                            write ? VIRTIO_BLK_T_OUT : VIRTIO_BLK_T_IN,
                            current_lba,
                            dev->bounce_mem,
                            bytes)) {
            log::warn() << "virtio_blk: submit failed "
                         << (write ? "write" : "read")
                         << " caller=" << reinterpret_cast<const void*>(current_buffer)
                         << " bounce_phys=" << reinterpret_cast<const void*>(static_cast<usize>(dev->bounce_phys))
                         << " lba=" << static_cast<unsigned long long>(current_lba)
                         << " bytes=" << static_cast<unsigned long long>(bytes);
            return false;
        }

        if (!write) {
            memory::copy(current_buffer, dev->bounce_mem, bytes);
        }

        current_lba += chunk;
        current_buffer += bytes;
        remaining -= chunk;
    }

    return true;
}

static bool virtio_blk_read(block_device* dev, u64 lba, u32 count, void* buffer) {
    return virtio_blk_transfer(dev, lba, count, buffer, false);
}

static bool virtio_blk_write(block_device* dev, u64 lba, u32 count, const void* buffer) {
    return virtio_blk_transfer(dev, lba, count, const_cast<void*>(buffer), true);
}

static const block_ops s_virtio_blk_ops = {
    .read_blocks = virtio_blk_read,
    .write_blocks = virtio_blk_write,
};

static auto init_device(const pci_device& pci_dev) -> bool {
    if ((pci_dev.bar[0] & 0x1u) == 0) {
        log::warn() << "virtio_blk: BAR0 is not an I/O BAR";
        return false;
    }

    s_device = {};
    s_device.io_base = static_cast<u16>(pci_dev.bar[0] & ~0x3u);
    if (s_device.io_base == 0) {
        return false;
    }

    pci::enable_bus_master(pci_dev.addr);

    virtio_write8(s_device, VIRTIO_PCI_STATUS, 0);
    virtio_write8(s_device, VIRTIO_PCI_STATUS, VIRTIO_STATUS_ACKNOWLEDGE);
    virtio_write8(s_device, VIRTIO_PCI_STATUS, VIRTIO_STATUS_ACKNOWLEDGE | VIRTIO_STATUS_DRIVER);

    (void)virtio_read32(s_device, VIRTIO_PCI_DEVICE_FEATURES);
    virtio_write32(s_device, VIRTIO_PCI_DRIVER_FEATURES, 0);

    if (!setup_queue(s_device)) {
        virtio_write8(s_device, VIRTIO_PCI_STATUS, VIRTIO_STATUS_FAILED);
        return false;
    }

    s_device.sectors = virtio_config_read64(s_device, 0);
    if (s_device.sectors == 0) {
        virtio_write8(s_device, VIRTIO_PCI_STATUS, VIRTIO_STATUS_FAILED);
        return false;
    }

    virtio_write8(s_device, VIRTIO_PCI_STATUS,
                  VIRTIO_STATUS_ACKNOWLEDGE | VIRTIO_STATUS_DRIVER | VIRTIO_STATUS_DRIVER_OK);
    s_device.present = true;

    block_device block {};
    char name[16];
    name[0] = 'b';
    name[1] = 'l';
    name[2] = 'o';
    name[3] = 'c';
    name[4] = 'k';
    name[5] = static_cast<char>('0' + block::device_count());
    name[6] = '\0';

    (void)block.name.assign(name);
    block.block_count = s_device.sectors;
    block.block_size = VIRTIO_BLK_SECTOR_SIZE;
    block.removable = false;
    block.driver_data = &s_device;
    block.ops = &s_virtio_blk_ops;

    if (block::register_device(block) < 0) {
        return false;
    }

    log::info() << "virtio_blk: registered " << block.name.c_str()
                << " io_base=" << log::hex(static_cast<u64>(s_device.io_base), 1, true, false)
                << " sectors=" << static_cast<unsigned long long>(s_device.sectors)
                << " queue_size=" << static_cast<unsigned long long>(s_device.queue_size);
    return true;
}

static bool virtio_blk_init() {
    block::init();

    const auto* dev = pci::find_device(pci_ids::VENDOR_VIRTIO, pci_ids::DEVICE_VIRTIO_BLK_LEGACY);
    if (dev == null) {
        log::warn() << "virtio_blk: no legacy virtio block PCI device found";
        return false;
    }

    if (!init_device(*dev)) {
        return false;
    }

    block::list_devices();
    return true;
}

static void virtio_blk_shutdown() {
    if (s_device.present) {
        virtio_write8(s_device, VIRTIO_PCI_STATUS, 0);
    }
    if (s_device.queue_phys != 0 && s_device.queue_pages != 0) {
        g_phys_alloc.free_pages(s_device.queue_phys, s_device.queue_pages);
    }
    if (s_device.bounce_phys != 0) {
        g_phys_alloc.free_pages(s_device.bounce_phys,
                                VIRTIO_BLK_BOUNCE_BYTES / PAGE_SIZE_4K);
    }
    s_device = {};
}

static const block_driver_t s_virtio_blk_driver = {
    .name = "virtio_blk",
    .init = virtio_blk_init,
    .shutdown = virtio_blk_shutdown,
};

static const driver_descriptor s_virtio_blk_descriptor = {
    .name = "virtio_blk",
    .type = driver_type::block,
    .sound = null,
    .block = &s_virtio_blk_driver,
};

} // namespace

namespace virtio_blk_driver {

void register_builtin() {
    driver::register_driver(&s_virtio_blk_descriptor);
}

} // namespace virtio_blk_driver
} // namespace vk
