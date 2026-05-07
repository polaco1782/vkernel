/*
 * vkernel - UEFI Microkernel
 * Copyright (C) 2026 vkernel authors
 *
 * fs.cpp - Filesystem facade and kernel stream table
 */

#include "fs.h"

#include "fs/fat32.h"
#include "fs/ramfs.h"

#include "log.h"
#include "memory.h"
#include "scheduler.h"

namespace vk {
namespace {

static u8 s_empty_file_marker = 0;

struct open_mode {
    bool valid = false;
    bool readable = false;
    bool writable = false;
    bool create = false;
    bool truncate = false;
    bool append = false;
};

enum class stream_backend : u8 {
    none,
    fat32,
};

static auto stream_backend_name(stream_backend backend) -> const char* {
    switch (backend) {
        case stream_backend::none:
            return "none";
        case stream_backend::fat32:
            return "fat32";
    }
    return "unknown";
}

struct kernel_stream {
    stream_backend backend = stream_backend::none;
    fat32::file_descriptor fat_file {};
    u8* owned_buffer = null;
    static_string<256> path;
    u64 owner_task_id = 0;
    usize size = 0;
    usize capacity = 0;
    usize position = 0;
    bool readable = false;
    bool writable = false;
    bool append = false;
    bool dirty = false;
    bool direct_fat = false;
    bool eof = false;
    bool error = false;
    bool in_use = false;
};

static constexpr usize MAX_STREAMS = 64;
static constexpr usize MAX_DIRECT_FAT_READ_BYTES = 128 * 1024;
static kernel_stream s_streams[MAX_STREAMS];
static bool s_initialised = false;

static void reset_stream(kernel_stream& stream) {
    if (stream.owned_buffer != null) {
        g_kernel_heap.free(stream.owned_buffer);
        stream.owned_buffer = null;
    }
    stream.backend = stream_backend::none;
    stream.fat_file = {};
    stream.path.clear();
    stream.owner_task_id = 0;
    stream.size = 0;
    stream.capacity = 0;
    stream.position = 0;
    stream.readable = false;
    stream.writable = false;
    stream.append = false;
    stream.dirty = false;
    stream.direct_fat = false;
    stream.eof = false;
    stream.error = false;
    stream.in_use = false;
}

static auto parse_mode(const char* mode_string) -> open_mode {
    open_mode mode {};
    if (mode_string == null || mode_string[0] == '\0') {
        return mode;
    }

    char first = '\0';
    bool plus = false;
    for (usize i = 0; mode_string[i] != '\0'; ++i) {
        const char ch = mode_string[i];
        if (first == '\0' && (ch == 'r' || ch == 'w' || ch == 'a')) {
            first = ch;
            continue;
        }
        if (ch == '+') {
            plus = true;
            continue;
        }
        if (ch == 'b' || ch == 't') {
            continue;
        }
        if (ch == 'r' || ch == 'w' || ch == 'a') {
            return mode;
        }
    }

    switch (first) {
        case 'r':
            mode.valid = true;
            mode.readable = true;
            mode.writable = plus;
            break;
        case 'w':
            mode.valid = true;
            mode.readable = plus;
            mode.writable = true;
            mode.create = true;
            mode.truncate = true;
            break;
        case 'a':
            mode.valid = true;
            mode.readable = plus;
            mode.writable = true;
            mode.create = true;
            mode.append = true;
            break;
        default:
            break;
    }

    return mode;
}

static auto is_separator(char ch) -> bool {
    return ch == '/' || ch == '\\';
}

static auto is_absolute_path(string_view path) -> bool {
    return path.size() > 0 && is_separator(path[0]);
}

static auto normalize_path(string_view path) -> string_view {
    while (path.size() >= 2 && path[0] == '.' && is_separator(path[1])) {
        path.remove_prefix(2);
    }
    return path;
}

static auto basename_view(string_view path) -> string_view {
    usize start = 0;
    for (usize i = 0; i < path.size(); ++i) {
        if (is_separator(path[i])) {
            start = i + 1;
        }
    }
    return string_view(path.data() + start, path.size() - start);
}

static auto stream_data(const kernel_stream& stream) -> const u8* {
    if (stream.direct_fat) {
        return null;
    }
    if (stream.size == 0) {
        return &s_empty_file_marker;
    }
    return stream.owned_buffer;
}

static auto ensure_capacity(kernel_stream& stream, usize required) -> bool {
    if (required <= stream.capacity) {
        return true;
    }

    usize new_capacity = stream.capacity > 0 ? stream.capacity : 256;
    while (new_capacity < required) {
        const usize next_capacity = new_capacity * 2;
        if (next_capacity <= new_capacity) {
            new_capacity = required;
            break;
        }
        new_capacity = next_capacity;
    }

    auto* new_buffer = static_cast<u8*>(g_kernel_heap.allocate(new_capacity));
    if (new_buffer == null) {
        return false;
    }

    if (stream.size > 0) {
        const u8* data = stream_data(stream);
        if (data != null) {
            memory::copy(new_buffer, data, stream.size);
        }
    }

    if (stream.owned_buffer != null) {
        g_kernel_heap.free(stream.owned_buffer);
    }
    stream.owned_buffer = new_buffer;
    stream.capacity = new_capacity;
    stream.backend = stream_backend::fat32;
    stream.direct_fat = false;
    return true;
}

static auto stream_accessible_to_current_task(const kernel_stream& stream) -> bool {
    const u64 current_task_id = sched::current_task_id();
    return stream.owner_task_id == 0 || current_task_id == 0 || stream.owner_task_id == current_task_id;
}

static auto handle_from_id(fs::file_handle handle) -> kernel_stream* {
    if (handle == 0 || handle > MAX_STREAMS) {
        return null;
    }
    auto& stream = s_streams[static_cast<usize>(handle - 1)];
    if (!stream.in_use || !stream_accessible_to_current_task(stream)) {
        return null;
    }
    return &stream;
}

static auto try_fat_file_exists(string_view path) -> bool {
    if (!fat32::is_mounted()) {
        return false;
    }

    if (fat32::file_exists(path)) {
        return true;
    }

    const auto base = basename_view(path);
    if (!is_absolute_path(path) && !base.equals(path) && fat32::file_exists(base)) {
        log::debug() << "fs: basename fallback exists '" << path << "' -> '" << base << "'";
        return true;
    }

    return false;
}

static auto try_fat_file_size(string_view path) -> usize {
    if (!fat32::is_mounted()) {
        return 0;
    }

    usize size = fat32::file_size(path);
    if (size > 0 || fat32::file_exists(path)) {
        return size;
    }

    const auto base = basename_view(path);
    if (!is_absolute_path(path) && !base.equals(path)) {
        size = fat32::file_size(base);
        if (size > 0 || fat32::file_exists(base)) {
            log::debug() << "fs: basename fallback size '" << path << "' -> '" << base
                         << "' size=" << static_cast<unsigned long long>(size);
            return size;
        }
    }

    return 0;
}

static auto try_fat_open_file(string_view path, fat32::file_descriptor& file_out) -> bool {
    if (!fat32::is_mounted()) {
        return false;
    }

    if (fat32::open_file(path, file_out)) {
        return true;
    }

    const auto base = basename_view(path);
    if (!is_absolute_path(path) && !base.equals(path) && fat32::open_file(base, file_out)) {
        log::debug() << "fs: basename fallback open '" << path << "' -> '" << base
                     << "' size=" << static_cast<unsigned long long>(file_out.size);
        return true;
    }

    return false;
}

static auto try_fat_read_file(string_view path, kernel_heap_ptr<u8>& owned_buffer, usize& size_out) -> const u8* {
    if (!fat32::is_mounted()) {
        return null;
    }

    owned_buffer.reset();
    const u8* data = fat32::read_file(path, owned_buffer, size_out);
    if (data != null) {
        return data;
    }

    const auto base = basename_view(path);
    if (!is_absolute_path(path) && !base.equals(path)) {
        owned_buffer.reset();
        data = fat32::read_file(base, owned_buffer, size_out);
        if (data != null) {
            log::debug() << "fs: basename fallback read '" << path << "' -> '" << base
                         << "' size=" << static_cast<unsigned long long>(size_out);
            return data;
        }
    }

    return null;
}

static auto try_fat_write_file(string_view path, const u8* data, usize size) -> bool {
    if (!fat32::is_mounted()) {
        return false;
    }

    if (fat32::write_file(path, data, size)) {
        return true;
    }

    const auto base = basename_view(path);
    if (!is_absolute_path(path) && !base.equals(path) && fat32::write_file(base, data, size)) {
        log::debug() << "fs: basename fallback write '" << path << "' -> '" << base
                     << "' size=" << static_cast<unsigned long long>(size);
        return true;
    }

    return false;
}

static auto try_fat_remove_file(string_view path) -> bool {
    if (!fat32::is_mounted()) {
        return false;
    }

    if (fat32::remove_file(path)) {
        return true;
    }

    const auto base = basename_view(path);
    if (!is_absolute_path(path) && !base.equals(path) && fat32::remove_file(base)) {
        log::debug() << "fs: basename fallback remove '" << path << "' -> '" << base << "'";
        return true;
    }

    return false;
}

static auto is_backup_shell_request(string_view path) -> bool {
    return basename_view(path).equals(string_view("shell.vbin"));
}

static auto find_backup_shell(string_view path) -> const file_entry* {
    if (!is_backup_shell_request(path)) {
        return null;
    }
    return ramfs::find(string_view("shell.vbin"));
}

static auto close_stream(fs::file_handle handle, kernel_stream& stream, const char* reason) -> int {
    log::debug() << "fs: file_close handle=" << static_cast<unsigned long long>(handle)
                 << " backend=" << stream_backend_name(stream.backend)
                 << " owner=" << static_cast<unsigned long long>(stream.owner_task_id)
                 << " path='" << stream.path.view() << "' dirty="
                 << (stream.dirty ? "yes" : "no") << " size="
                 << static_cast<unsigned long long>(stream.size)
                 << " reason=" << reason;

    if (stream.backend == stream_backend::fat32 && stream.dirty) {
        const u8* data = stream.size > 0 ? stream_data(stream) : null;
        if (!try_fat_write_file(stream.path.view(), data, stream.size)) {
            stream.error = true;
            log::debug() << "fs: file_close flush failed handle=" << static_cast<unsigned long long>(handle)
                         << " path='" << stream.path.view() << "'";
            return -1;
        }
        log::debug() << "fs: file_close flushed fat32 file path='" << stream.path.view()
                     << "' size=" << static_cast<unsigned long long>(stream.size);
    }

    reset_stream(stream);
    return 0;
}

} // namespace

void fs::init() {
    if (s_initialised) {
        return;
    }

    for (auto& stream : s_streams) {
        reset_stream(stream);
    }
    fat32::init();
    s_initialised = true;
}

auto fs::mount_boot_filesystem() -> status_code {
    if (!s_initialised) {
        init();
    }

    const auto rc = fat32::mount_first_available();
    if (rc == status_code::success) {
        const auto mounted = fat32::info();
        log::info() << "fs: mounted " << mounted.filesystem_name.c_str()
                    << " on " << mounted.block_device.c_str()
                    << " root=" << mounted.logical_root_path.c_str();
    } else {
        log::warn() << "fs: FAT32 mount failed; only RAMFS backup shell remains available";
        log::debug() << "fs: FAT32 mount error code: " << static_cast<u32>(rc);
    }

    return rc;
}

auto fs::query_info() -> runtime_info {
    runtime_info info {};
    info.fallback_ready = ramfs::is_ready();

    if (fat32::is_mounted()) {
        const auto mounted = fat32::info();
        info.fat32_mounted = true;
        info.writable = mounted.writable;
        (void)info.active_backend.assign(mounted.filesystem_name.view());
        (void)info.block_device.assign(mounted.block_device.view());
        (void)info.logical_root_path.assign(mounted.logical_root_path.view());
        info.bytes_per_sector = mounted.bytes_per_sector;
        info.sectors_per_cluster = mounted.sectors_per_cluster;
        info.cluster_size = mounted.bytes_per_sector * static_cast<u32>(mounted.sectors_per_cluster);
        info.first_data_sector = mounted.first_data_sector;
        info.root_cluster = mounted.logical_root_cluster;
        return info;
    }

    (void)info.active_backend.assign("none");

    return info;
}

auto fs::file_exists(const char* path) -> bool {
    if (path == null) {
        return false;
    }

    const auto normalized = normalize_path(string_view(path));
    if (try_fat_file_exists(normalized)) {
        return true;
    }
    return false;
}

auto fs::file_size(const char* path) -> usize {
    if (path == null) {
        return 0;
    }

    const auto normalized = normalize_path(string_view(path));
    const usize fat_size = try_fat_file_size(normalized);
    if (fat_size > 0 || try_fat_file_exists(normalized)) {
        return fat_size;
    }

    return 0;
}

auto fs::load_file(string_view path, kernel_heap_ptr<u8>& owned_buffer, usize& size_out) -> const u8* {
    const auto normalized = normalize_path(path);
    log::debug() << "fs: loading file '" << normalized << "'";
    const u8* fat_data = try_fat_read_file(normalized, owned_buffer, size_out);
    if (fat_data != null) {
        log::debug() << "fs: load_file '" << normalized << "' resolved via fat32 size="
                     << static_cast<unsigned long long>(size_out);
        return fat_data;
    }

    owned_buffer.reset();
    const auto* entry = find_backup_shell(normalized);
    if (entry == null) {
        size_out = 0;
        log::debug() << "fs: load_file miss '" << normalized << "'";
        return null;
    }

    size_out = entry->size;
    log::debug() << "fs: load_file '" << normalized << "' resolved via backup shell size="
                 << static_cast<unsigned long long>(size_out);
    return entry->size > 0 ? entry->data : &s_empty_file_marker;
}

auto fs::file_open(const char* path, const char* mode_string) -> file_handle {
    if (!s_initialised) {
        init();
    }

    const auto mode = parse_mode(mode_string);
    if (!mode.valid || path == null) {
        log::debug() << "fs: file_open rejected invalid request path="
                     << (path != null ? path : "<null>") << " mode="
                     << (mode_string != null ? mode_string : "<null>");
        return 0;
    }

    const auto normalized = normalize_path(string_view(path));
    if (normalized.empty()) {
        log::debug() << "fs: file_open rejected empty normalized path from '" << path << "'";
        return 0;
    }

    log::debug() << "fs: file_open path='" << normalized << "' mode='" << mode_string
                 << "' readable=" << (mode.readable ? "yes" : "no")
                 << " writable=" << (mode.writable ? "yes" : "no")
                 << " create=" << (mode.create ? "yes" : "no")
                 << " truncate=" << (mode.truncate ? "yes" : "no")
                 << " append=" << (mode.append ? "yes" : "no");

    for (usize i = 0; i < MAX_STREAMS; ++i) {
        auto& stream = s_streams[i];
        if (stream.in_use) {
            continue;
        }

        reset_stream(stream);
        stream.in_use = true;
        stream.owner_task_id = sched::current_task_id();
        stream.readable = mode.readable;
        stream.writable = mode.writable;
        stream.append = mode.append;
        stream.eof = false;
        stream.error = false;
        if (!stream.path.assign(normalized)) {
            reset_stream(stream);
            return 0;
        }

        if (mode.writable) {
            if (!fat32::is_mounted()) {
                log::debug() << "fs: file_open denied writable open without mounted fat32 path='"
                             << normalized << "'";
                reset_stream(stream);
                return 0;
            }

            stream.backend = stream_backend::fat32;
            kernel_heap_ptr<u8> owned_buffer;
            usize size = 0;
            const u8* file_data = try_fat_read_file(normalized, owned_buffer, size);
            const bool file_exists = file_data != null;

            if (!file_exists && !mode.create) {
                log::debug() << "fs: file_open miss for existing writable path='" << normalized << "'";
                reset_stream(stream);
                return 0;
            }

            if (file_exists && !mode.truncate) {
                stream.owned_buffer = owned_buffer.release();
                stream.size = size;
                stream.capacity = size;
                if (stream.size > 0 && stream.owned_buffer == null) {
                    if (!ensure_capacity(stream, stream.size)) {
                        reset_stream(stream);
                        return 0;
                    }
                    memory::copy(stream.owned_buffer, file_data, stream.size);
                }
            }

            if (mode.truncate) {
                if (stream.owned_buffer != null) {
                    g_kernel_heap.free(stream.owned_buffer);
                    stream.owned_buffer = null;
                }
                stream.size = 0;
                stream.capacity = 0;
            }

            stream.position = mode.append ? stream.size : 0;
            log::debug() << "fs: file_open handle=" << static_cast<unsigned long long>(i + 1)
                         << " backend=" << stream_backend_name(stream.backend)
                         << " path='" << normalized << "' size="
                         << static_cast<unsigned long long>(stream.size)
                         << " position=" << static_cast<unsigned long long>(stream.position);
            return static_cast<file_handle>(i + 1);
        }

        fat32::file_descriptor fat_file {};
        if (try_fat_open_file(normalized, fat_file)) {
            stream.backend = stream_backend::fat32;
            stream.fat_file = fat_file;
            stream.direct_fat = true;
            stream.size = fat_file.size;
            stream.capacity = fat_file.size;
            stream.position = 0;
            stream.eof = stream.size == 0;
            log::debug() << "fs: file_open handle=" << static_cast<unsigned long long>(i + 1)
                         << " backend=" << stream_backend_name(stream.backend)
                         << " path='" << normalized << "' size="
                         << static_cast<unsigned long long>(stream.size);
            return static_cast<file_handle>(i + 1);
        }

        log::debug() << "fs: file_open miss path='" << normalized << "'";
        reset_stream(stream);
        return 0;
    }

    log::debug() << "fs: file_open failed, no free stream for path='" << normalized << "'";
    return 0;
}

auto fs::file_close(file_handle handle) -> int {
    auto* stream = handle_from_id(handle);
    if (stream == null) {
        log::debug() << "fs: file_close invalid handle=" << static_cast<unsigned long long>(handle);
        return -1;
    }

    return close_stream(handle, *stream, "explicit");
}

void fs::close_all_for_task(u64 task_id) {
    if (task_id == 0) {
        return;
    }

    usize closed_count = 0;
    for (usize i = 0; i < MAX_STREAMS; ++i) {
        auto& stream = s_streams[i];
        if (!stream.in_use || stream.owner_task_id != task_id) {
            continue;
        }

        if (close_stream(static_cast<file_handle>(i + 1), stream, "task-exit") != 0) {
            reset_stream(stream);
        }
        ++closed_count;
    }

    if (closed_count > 0) {
        log::debug() << "fs: closed " << static_cast<unsigned long long>(closed_count)
                     << " leaked stream(s) for task "
                     << static_cast<unsigned long long>(task_id);
    }
}

auto fs::file_read(file_handle handle, void* buf, usize buf_size) -> usize {
    auto* stream = handle_from_id(handle);
    if (stream == null || buf == null || !stream->readable) {
        return 0;
    }
    if (stream->position >= stream->size) {
        stream->eof = true;
        return 0;
    }

    const usize remaining = stream->size - stream->position;
    usize to_copy = remaining < buf_size ? remaining : buf_size;
    if (stream->backend == stream_backend::fat32 && stream->direct_fat) {
        if (to_copy > MAX_DIRECT_FAT_READ_BYTES) {
            to_copy = MAX_DIRECT_FAT_READ_BYTES;
        }
        usize read_count = 0;
        if (!fat32::read_file(stream->fat_file, stream->position, buf, to_copy, read_count)) {
            stream->error = true;
            return 0;
        }
        stream->position += read_count;
        stream->eof = stream->position >= stream->size;
        return read_count;
    }

    const u8* data = stream_data(*stream);
    if (to_copy > 0 && data != null) {
        memory::copy(buf, data + stream->position, to_copy);
    }
    stream->position += to_copy;
    stream->eof = stream->position >= stream->size;
    return to_copy;
}

auto fs::file_write(file_handle handle, const void* buf, usize buf_size) -> usize {
    auto* stream = handle_from_id(handle);
    if (stream == null || buf == null || !stream->writable) {
        return 0;
    }

    if (stream->append) {
        stream->position = stream->size;
    }

    if (stream->position > stream->size) {
        if (!ensure_capacity(*stream, stream->position)) {
            stream->error = true;
            return 0;
        }
        memory::set(stream->owned_buffer + stream->size, 0, stream->position - stream->size);
        stream->size = stream->position;
    }

    const usize end_position = stream->position + buf_size;
    if (end_position < stream->position || !ensure_capacity(*stream, end_position)) {
        stream->error = true;
        return 0;
    }

    memory::copy(stream->owned_buffer + stream->position, buf, buf_size);
    stream->position = end_position;
    if (stream->position > stream->size) {
        stream->size = stream->position;
    }
    stream->dirty = true;
    stream->eof = stream->position >= stream->size;
    return buf_size;
}

auto fs::file_seek(file_handle handle, i64 offset, int whence) -> int {
    auto* stream = handle_from_id(handle);
    if (stream == null) {
        log::debug() << "fs: file_seek invalid handle=" << static_cast<unsigned long long>(handle);
        return -1;
    }

    i64 base = 0;
    switch (whence) {
        case 0:
            base = 0;
            break;
        case 1:
            base = static_cast<i64>(stream->position);
            break;
        case 2:
            base = static_cast<i64>(stream->size);
            break;
        default:
            stream->error = true;
            return -1;
    }

    const i64 next = base + offset;
    if (next < 0) {
        stream->error = true;
        log::debug() << "fs: file_seek rejected negative target handle="
                     << static_cast<unsigned long long>(handle) << " base=" << base
                     << " offset=" << offset;
        return -1;
    }

    stream->position = static_cast<usize>(next);
    stream->eof = stream->position >= stream->size;
    log::debug() << "fs: file_seek handle=" << static_cast<unsigned long long>(handle)
                 << " path='" << stream->path.view() << "' whence=" << whence
                 << " offset=" << offset << " -> position="
                 << static_cast<unsigned long long>(stream->position);
    return 0;
}

auto fs::file_tell(file_handle handle) -> i64 {
    auto* stream = handle_from_id(handle);
    if (stream == null) {
        return -1;
    }
    return static_cast<i64>(stream->position);
}

auto fs::file_remove(const char* path) -> int {
    if (path == null) {
        log::debug() << "fs: file_remove rejected null path";
        return -1;
    }

    const auto normalized = normalize_path(string_view(path));
    const bool removed = try_fat_remove_file(normalized);
    log::debug() << "fs: file_remove path='" << normalized << "' result="
                 << (removed ? "removed" : "not-found-or-failed");
    return removed ? 0 : -1;
}

} // namespace vk
