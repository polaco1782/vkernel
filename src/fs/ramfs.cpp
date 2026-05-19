/*
 * vkernel - UEFI Microkernel
 * Copyright (C) 2026 vkernel authors
 *
 * ramfs.cpp - Flat in-memory fallback filesystem
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

    auto& file = g_files[g_file_count];
    if (!file.name.assign(name)) return status_code::invalid_param;

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

    auto& file = g_files[g_file_count];
    if (!file.name.assign(name)) return status_code::invalid_param;

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
    if (name.size() >= 2 && name[0] == '.' && name[1] == '/') {
        name.remove_prefix(2);
    }

    for (usize i = 0; i < g_file_count; ++i) {
        if (g_files[i].valid && g_files[i].name.view().equals(name)) {
            return &g_files[i];
        }
    }

    const char* last_slash = null;
    for (usize i = 0; i < name.size(); ++i) {
        if (name[i] == '/' || name[i] == '\\') {
            last_slash = name.data() + i;
        }
    }
    if (last_slash != null) {
        string_view base(last_slash + 1,
                         name.size() - static_cast<usize>(last_slash + 1 - name.data()));
        for (usize i = 0; i < g_file_count; ++i) {
            if (g_files[i].valid && g_files[i].name.view().equals(base)) {
                return &g_files[i];
            }
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
