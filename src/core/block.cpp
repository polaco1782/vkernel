/*
 * vkernel - UEFI Microkernel
 * Copyright (C) 2026 vkernel authors
 *
 * block.cpp - Block device registry and helpers
 */

#include "config.h"
#include "types.h"
#include "console.h"
#include "log.h"
#include "memory.h"
#include "block.h"

namespace vk {
namespace block {

static block_device s_devices[MAX_BLOCK_DEVICES];
static usize        s_device_count = 0;
static bool         s_initialised = false;

void init() {
    if (s_initialised) return;
    memory::set(s_devices, 0, sizeof(s_devices));
    s_device_count = 0;
    s_initialised = true;
}

auto register_device(const block_device& dev) -> i32 {
    if (!s_initialised) init();
    if (s_device_count >= MAX_BLOCK_DEVICES) {
        log::warn() << "block: registry full, cannot register " << dev.name.c_str();
        return -1;
    }
    if (dev.block_size == 0 || dev.block_count == 0 || dev.ops == null ||
        dev.ops->read_blocks == null) {
        log::warn() << "block: invalid device descriptor for " << dev.name.c_str();
        return -1;
    }

    s_devices[s_device_count] = dev;
    log::info() << "block: registered " << s_devices[s_device_count].name.c_str() << " (" << static_cast<unsigned long long>(s_devices[s_device_count].block_count) << " blocks x " << s_devices[s_device_count].block_size << " bytes)";
    ++s_device_count;
    return static_cast<i32>(s_device_count - 1);
}

auto device_count() -> usize {
    return s_device_count;
}

auto get_device(usize index) -> block_device* {
    if (index >= s_device_count) return null;
    return &s_devices[index];
}

auto find(const char* name) -> block_device* {
    string_view query(name);
    for (usize i = 0; i < s_device_count; ++i) {
        if (s_devices[i].name.view().equals(query)) {
            return &s_devices[i];
        }
    }
    return null;
}

bool read_blocks(block_device* dev, u64 lba, u32 count, void* buffer) {
    if (dev == null || dev->ops == null || dev->ops->read_blocks == null ||
        buffer == null || count == 0) {
        return false;
    }
    if (lba >= dev->block_count || count > dev->block_count - lba) {
        return false;
    }
    return dev->ops->read_blocks(dev, lba, count, buffer);
}

void list_devices() {
    log::info() << "Block devices:";
    if (s_device_count == 0) {
        log::info() << "  (none)";
        return;
    }

    for (usize i = 0; i < s_device_count; ++i) {
        auto& d = s_devices[i];
        log::info() << "  [" << i << "] " << d.name.c_str() << ": " << static_cast<unsigned long long>((d.block_count * d.block_size) / 1024) << " KiB (" << static_cast<unsigned long long>(d.block_count) << " blocks x " << d.block_size << ")";
    }
}

} // namespace block
} // namespace vk
