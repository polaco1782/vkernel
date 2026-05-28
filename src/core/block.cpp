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

namespace {

static constexpr bool TRACE_BLOCK_REQUESTS = false;
[[maybe_unused]] static constexpr u32 TRACE_SMALL_REQUEST_MAX_BLOCKS = 16;
[[maybe_unused]] static constexpr u32 TRACE_MEDIUM_REQUEST_MAX_BLOCKS = 256;

[[maybe_unused]] static auto sample_lba(u64 lba, u32 count, u32 numerator, u32 denominator) -> u64 {
    if (count <= 1) {
        return lba;
    }
    return lba + ((static_cast<u64>(count - 1) * numerator) / denominator);
}

[[maybe_unused]] static void trace_request(const char* operation, const block_device* dev, u64 lba, u32 count, const void* buffer) {
    if (dev == null || count == 0) {
        return;
    }

    const u64 end_lba = lba + static_cast<u64>(count - 1);
    const u64 total_bytes = static_cast<u64>(count) * dev->block_size;

    if (count <= TRACE_SMALL_REQUEST_MAX_BLOCKS) {
        log::debug() << "block: " << operation << " "
                     << static_cast<unsigned long long>(count) << " block(s) from "
                     << dev->name.c_str() << " at LBA "
                     << static_cast<unsigned long long>(lba) << " into/from buffer "
                     << buffer;
        return;
    }

    if (count <= TRACE_MEDIUM_REQUEST_MAX_BLOCKS) {
        log::debug() << "block: " << operation << " "
                     << static_cast<unsigned long long>(count) << " block(s) from "
                     << dev->name.c_str() << " range=["
                     << static_cast<unsigned long long>(lba) << ".."
                     << static_cast<unsigned long long>(end_lba) << "] bytes="
                     << static_cast<unsigned long long>(total_bytes) << " buffer="
                     << buffer;
        return;
    }

    log::debug() << "block: " << operation << " sampled large range from "
                 << dev->name.c_str() << " blocks="
                 << static_cast<unsigned long long>(count) << " lba=["
                 << static_cast<unsigned long long>(lba) << ".."
                 << static_cast<unsigned long long>(end_lba) << "] sample_lba={"
                 << static_cast<unsigned long long>(lba) << ","
                 << static_cast<unsigned long long>(sample_lba(lba, count, 1, 4)) << ","
                 << static_cast<unsigned long long>(sample_lba(lba, count, 2, 4)) << ","
                 << static_cast<unsigned long long>(sample_lba(lba, count, 3, 4)) << ","
                 << static_cast<unsigned long long>(end_lba) << "} bytes="
                 << static_cast<unsigned long long>(total_bytes) << " buffer="
                 << buffer;
}

} // namespace

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
        if (s_devices[i].name.view().compare(query)) {
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

    if constexpr (TRACE_BLOCK_REQUESTS) {
        trace_request("reading", dev, lba, count, buffer);
    }

    return dev->ops->read_blocks(dev, lba, count, buffer);
}

bool write_blocks(block_device* dev, u64 lba, u32 count, const void* buffer) {
    if (dev == null || dev->ops == null || dev->ops->write_blocks == null ||
        buffer == null || count == 0) {
        return false;
    }
    if (lba >= dev->block_count || count > dev->block_count - lba) {
        return false;
    }

    if constexpr (TRACE_BLOCK_REQUESTS) {
        trace_request("writing", dev, lba, count, buffer);
    }

    return dev->ops->write_blocks(dev, lba, count, buffer);
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
