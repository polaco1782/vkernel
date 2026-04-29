/*
 * vkernel - UEFI Microkernel
 * Copyright (C) 2026 vkernel authors
 *
 * block.h - Block device subsystem
 */

#ifndef VKERNEL_BLOCK_H
#define VKERNEL_BLOCK_H

#include "types.h"

namespace vk {

/* ============================================================
 * Block driver/device interfaces
 * ============================================================ */

struct block_device;

struct block_ops {
    bool (*read_blocks)(block_device* dev, u64 lba, u32 count, void* buffer);
};

struct block_device {
    static_string<32> name;
    u64               block_count = 0;
    u32               block_size = 512;
    bool              removable = false;
    void*             driver_data = null;
    const block_ops*  ops = null;
};

struct block_driver_t {
    const char* name;
    bool (*init)();
    void (*shutdown)();
};

namespace block {

inline constexpr usize MAX_BLOCK_DEVICES = 16;

void init();
auto register_device(const block_device& dev) -> i32;
auto device_count() -> usize;
auto get_device(usize index) -> block_device*;
auto find(const char* name) -> block_device*;

bool read_blocks(block_device* dev, u64 lba, u32 count, void* buffer);
void list_devices();

} // namespace block
} // namespace vk

#endif /* VKERNEL_BLOCK_H */
