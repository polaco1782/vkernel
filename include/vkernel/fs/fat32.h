#pragma once

#include "driver.h"

#include "../block.h"
#include "../resource_ptr.h"
#include "../types.h"

namespace vk {
namespace fat32 {

struct mount_info {
    bool mounted = false;
    bool writable = false;
    static_string<16> filesystem_name;
    static_string<32> block_device;
    static_string<64> logical_root_path;
    u64 partition_start_lba = 0;
    u32 total_sectors = 0;
    u32 bytes_per_sector = 0;
    u8 sectors_per_cluster = 0;
    u8 fat_count = 0;
    u16 reserved_sector_count = 0;
    u32 sectors_per_fat = 0;
    u32 first_data_sector = 0;
    u32 root_cluster = 0;
    u32 logical_root_cluster = 0;
    u32 fs_info_sector = 0;
};

struct file_descriptor {
    bool valid = false;
    u32 first_cluster = 0;
    usize size = 0;
    mutable bool cluster_cache_valid = false;
    mutable usize cluster_cache_index = 0;
    mutable u32 cluster_cache_value = 0;
};

struct directory_entry_info {
    static_string<256> name;
    bool is_directory = false;
    usize size = 0;
    u32 first_cluster = 0;
};

using directory_visit_callback = bool (*)(const directory_entry_info& entry, void* context);

void init();
auto mount_partition(block_device* device, u64 start_lba) -> status_code;
auto is_mounted() -> bool;
auto info() -> mount_info;

auto file_exists(string_view path) -> bool;
auto file_size(string_view path) -> usize;
auto directory_exists(string_view path) -> bool;
auto list_directory(string_view path, directory_visit_callback callback, void* context) -> bool;
auto open_file(string_view path, file_descriptor& file_out) -> bool;
auto read_file(file_descriptor& file, usize offset, void* buffer, usize size, usize& size_out) -> bool;
auto read_file(string_view path, kernel_heap_ptr<u8>& owned_buffer, usize& size_out) -> const u8*;
auto write_file(string_view path, const u8* data, usize size) -> bool;
auto remove_file(string_view path) -> bool;
auto driver() -> const fs::filesystem_driver&;

} // namespace fat32
} // namespace vk
