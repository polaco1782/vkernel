/*
 * vkernel - UEFI Microkernel
 * Copyright (C) 2026 vkernel authors
 *
 * driver.h - Filesystem backend driver contract
 */

#pragma once

#include "../block.h"
#include "../fs.h"

namespace vk {
namespace fs {

inline constexpr usize MAX_FILESYSTEM_FILE_STORAGE = 64;
inline constexpr usize MAX_DIRECT_FILESYSTEM_READ_BYTES = 128 * 1024;

struct filesystem_file_descriptor {
    alignas(u64) u8 storage[MAX_FILESYSTEM_FILE_STORAGE] {};
};

struct filesystem_driver {
    const char* name = "unknown";
    void (*init)() = null;
    auto (*mount_partition)(block_device* device, u64 start_lba) -> status_code = null;
    auto (*is_mounted)() -> bool = null;
    void (*fill_runtime_info)(runtime_info& info) = null;
    auto (*file_exists)(string_view path) -> bool = null;
    auto (*file_size)(string_view path) -> usize = null;
    auto (*directory_exists)(string_view path) -> bool = null;
    auto (*list_directory)(string_view path, directory_visit_callback callback, void* context) -> bool = null;
    auto (*open_file)(string_view path, filesystem_file_descriptor& file_out) -> bool = null;
    auto (*read_file_range)(filesystem_file_descriptor& file, usize offset, void* buffer, usize size, usize& size_out) -> bool = null;
    auto (*read_file)(string_view path, kernel_heap_ptr<u8>& owned_buffer, usize& size_out) -> const u8* = null;
    auto (*write_file)(string_view path, const u8* data, usize size) -> bool = null;
    auto (*remove_file)(string_view path) -> bool = null;
    usize max_direct_read_bytes = MAX_DIRECT_FILESYSTEM_READ_BYTES;
};

} // namespace fs
} // namespace vk
