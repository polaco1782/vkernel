#pragma once

#include "../memory.h"
#include "../types.h"

namespace vk {

struct file_entry {
    static_string<128> name;
    u8* data;
    usize size;
    bool valid;
};

inline constexpr usize RAMFS_MAX_FILES = 32;

namespace ramfs {

auto init() -> status_code;
auto is_ready() -> bool;

auto add_file(string_view name, const u8* data, usize size) -> status_code;
auto add_file(const char* name, const u8* data, usize size) -> status_code;

auto add_file_nocopy(string_view name, u8* data, usize size) -> status_code;
auto add_file_nocopy(const char* name, u8* data, usize size) -> status_code;

auto find(string_view name) -> const file_entry*;
auto find(const char* name) -> const file_entry*;

auto file_count() -> usize;
auto get_file(usize index) -> const file_entry*;

void dump();

} // namespace ramfs
} // namespace vk
