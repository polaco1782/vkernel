/*
 * vkernel - UEFI Microkernel
 * Copyright (C) 2026 vkernel authors
 *
 * ramfs.cpp - In-memory fallback filesystem
 */

#include "config.h"
#include "console.h"
#include "fs/ramfs.h"
#include "log.h"
#include "memory.h"
#include "resource_ptr.h"
#include "types.h"

namespace vk {
namespace {

static file_entry g_files[RAMFS_MAX_FILES];
static usize g_file_count = 0;
static bool g_ready = false;

static auto is_separator(char ch) -> bool {
    return ch == '/' || ch == '\\';
}

static auto normalize_ramfs_path(string_view path) -> string_view {
    while (path.size() >= 2 && path[0] == '.' && is_separator(path[1])) {
        path.remove_prefix(2);
    }
    return path;
}

} // namespace

auto ramfs::init() -> status_code {
    g_file_count = 0;
    g_ready = true;
    memory::set(g_files, 0, sizeof(g_files));
    return status_code::success;
}

auto ramfs::is_ready() -> bool {
    return g_ready;
}

auto ramfs::add_file(string_view name, const u8* data, usize size) -> status_code {
    if (g_file_count >= RAMFS_MAX_FILES) return status_code::no_memory;
    if (name.data() == null || data == null) return status_code::invalid_param;

    const auto normalized = normalize_ramfs_path(name);
    if (normalized.empty()) return status_code::invalid_param;

    auto& file = g_files[g_file_count];
    if (!file.name.assign(normalized)) return status_code::invalid_param;

    kernel_heap_ptr<u8> copy(static_cast<u8*>(g_kernel_heap.allocate(size)));
    if (!copy) return status_code::no_memory;
    if (size > 0) {
        memory::copy(copy.get(), data, size);
    }

    file.data = copy.release();
    file.size = size;
    file.valid = true;
    ++g_file_count;

    log::debug() << "ramfs: added '" << file.name.c_str() << "' at heap="
                 << reinterpret_cast<const void*>(file.data)
                 << " (" << size << " bytes)";
    return status_code::success;
}

auto ramfs::add_file(const char* name, const u8* data, usize size) -> status_code {
    return add_file(string_view(name), data, size);
}

auto ramfs::add_file_nocopy(string_view name, u8* data, usize size) -> status_code {
    if (g_file_count >= RAMFS_MAX_FILES) return status_code::no_memory;
    if (name.data() == null || data == null) return status_code::invalid_param;

    const auto normalized = normalize_ramfs_path(name);
    if (normalized.empty()) return status_code::invalid_param;

    auto& file = g_files[g_file_count];
    if (!file.name.assign(normalized)) return status_code::invalid_param;

    file.data = data;
    file.size = size;
    file.valid = true;
    ++g_file_count;

    log::debug() << "ramfs: registered (nocopy) '" << file.name.c_str() << "' at "
                 << reinterpret_cast<const void*>(data)
                 << " (" << size << " bytes)";
    return status_code::success;
}

auto ramfs::find(string_view name) -> const file_entry* {
    name = normalize_ramfs_path(name);

    for (usize i = 0; i < g_file_count; ++i) {
        if (g_files[i].valid && g_files[i].name.view().compare(name)) {
            return &g_files[i];
        }
    }

    return null;
}

auto ramfs::find(const char* name) -> const file_entry* {
    return find(string_view(name));
}

auto ramfs::file_count() -> usize {
    return g_file_count;
}

auto ramfs::get_file(usize index) -> const file_entry* {
    if (index >= g_file_count) return null;
    return &g_files[index];
}

void ramfs::dump() {
    log::info() << "RAMFS: " << g_file_count << " file(s)";
    for (usize i = 0; i < g_file_count; ++i) {
        if (g_files[i].valid) {
            log::info() << "  [" << i << "] '" << g_files[i].name.c_str() << "' ("
                        << g_files[i].size << " bytes)";
        }
    }
}

} // namespace vk
