/*
 * vkernel - UEFI Microkernel
 * Copyright (C) 2026 vkernel authors
 *
 * fs.h - Filesystem facade + RAMFS fallback (freestanding C++26)
 */

#ifndef VKERNEL_FS_H
#define VKERNEL_FS_H

#include "types.h"
#include "memory.h"
#include "resource_ptr.h"

namespace vk {

/* ============================================================
 * UEFI ESP loader — uses Simple File System Protocol
 * Must be called BEFORE ExitBootServices.
 * ============================================================ */

namespace loader {

struct loaded_file {
    u8* data;
    usize size;
};

auto load_file_from_esp(const char* path) -> loaded_file;
auto load_initrd() -> status_code;

} // namespace loader

namespace fs {

using file_handle = u64;

struct runtime_info {
    bool fallback_ready = false;
    bool fat32_mounted = false;
    bool writable = false;
    static_string<16> active_backend;
    static_string<32> block_device;
    static_string<64> logical_root_path;
    u32 bytes_per_sector = 0;
    u8 sectors_per_cluster = 0;
    u32 cluster_size = 0;
    u32 first_data_sector = 0;
    u32 root_cluster = 0;
};

void init();
auto mount_boot_filesystem() -> status_code;
auto query_info() -> runtime_info;

auto file_exists(const char* path) -> bool;
auto file_size(const char* path) -> usize;

auto file_open(const char* path, const char* mode) -> file_handle;
auto file_close(file_handle handle) -> int;
auto file_read(file_handle handle, void* buf, usize buf_size) -> usize;
auto file_write(file_handle handle, const void* buf, usize buf_size) -> usize;
auto file_seek(file_handle handle, i64 offset, int whence) -> int;
auto file_tell(file_handle handle) -> i64;
auto file_remove(const char* path) -> int;
void close_all_for_task(u64 task_id);

auto load_file(string_view path, kernel_heap_ptr<u8>& owned_buffer, usize& size_out) -> const u8*;

} // namespace fs

} // namespace vk

#endif /* VKERNEL_FS_H */
