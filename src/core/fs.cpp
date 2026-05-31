/*
 * vkernel - UEFI Microkernel
 * Copyright (C) 2026 vkernel authors
 *
 * fs.cpp - Filesystem facade and kernel stream table
 */

#include "fs.h"

#include "fs/driver.h"
#include "fs/fat32.h"
#include "fs/mounts.h"
#include "fs/ramfs.h"

#include "log.h"
#include "memory.h"
#include "scheduler.h"

namespace vk {
namespace {

using filesystem_driver = fs::filesystem_driver;
using filesystem_file_descriptor = fs::filesystem_file_descriptor;
using filesystem_driver_getter = auto (*)() -> const filesystem_driver&;

static u8 s_empty_file_marker = 0;

struct open_mode {
    bool valid = false;
    bool readable = false;
    bool writable = false;
    bool create = false;
    bool truncate = false;
    bool append = false;
};

static auto filesystem_driver_name(const filesystem_driver* driver) -> const char* {
    return driver != null ? driver->name : "none";
}

struct kernel_stream {
    const filesystem_driver* driver = null;
    filesystem_file_descriptor backend_file {};
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
    bool direct_backend = false;
    bool eof = false;
    bool error = false;
    bool in_use = false;
};

static constexpr usize MAX_STREAMS = 64;
static kernel_stream s_streams[MAX_STREAMS];
static bool s_initialised = false;
static const filesystem_driver* s_active_driver = null;
static const filesystem_driver_getter s_filesystem_driver_getters[] = {
    fat32::driver,
};

static auto mounted_filesystem_driver() -> const filesystem_driver* {
    if (s_active_driver == null || s_active_driver->is_mounted == null || !s_active_driver->is_mounted()) {
        return null;
    }
    return s_active_driver;
}

static auto mount_supported_partition(const fs::mounts::partition_view& partition, void*) -> bool {
    for (const auto getter : s_filesystem_driver_getters) {
        if (getter == null) {
            continue;
        }
        const auto* driver = &getter();
        if (driver->mount_partition == null) {
            continue;
        }
        if (driver->mount_partition(partition.device, partition.start_lba) != status_code::success) {
            continue;
        }

        s_active_driver = driver;
        return true;
    }

    return false;
}

static void reset_stream(kernel_stream& stream) {
    if (stream.owned_buffer != null) {
        g_kernel_heap.free(stream.owned_buffer);
        stream.owned_buffer = null;
    }
    stream.driver = null;
    stream.backend_file = {};
    stream.path.clear();
    stream.owner_task_id = 0;
    stream.size = 0;
    stream.capacity = 0;
    stream.position = 0;
    stream.readable = false;
    stream.writable = false;
    stream.append = false;
    stream.dirty = false;
    stream.direct_backend = false;
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

static auto normalize_path(string_view path) -> string_view {
    while (path.size() >= 2 && path[0] == '.' && is_separator(path[1])) {
        path.remove_prefix(2);
    }
    return path;
}

static auto stream_data(const kernel_stream& stream) -> const u8* {
    if (stream.direct_backend) {
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
    stream.direct_backend = false;
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

static auto try_backend_file_exists(string_view path) -> bool {
    const auto* driver = mounted_filesystem_driver();
    if (driver == null || driver->file_exists == null) {
        return false;
    }

    return driver->file_exists(path);
}

static auto try_backend_file_size(string_view path) -> usize {
    const auto* driver = mounted_filesystem_driver();
    if (driver == null || driver->file_size == null) {
        return 0;
    }

    return driver->file_size(path);
}

static auto try_backend_directory_exists(string_view path) -> bool {
    const auto* driver = mounted_filesystem_driver();
    if (driver == null || driver->directory_exists == null) {
        return false;
    }

    return driver->directory_exists(path);
}

static auto try_backend_open_file(string_view path, filesystem_file_descriptor& file_out) -> bool {
    const auto* driver = mounted_filesystem_driver();
    if (driver == null || driver->open_file == null) {
        return false;
    }

    return driver->open_file(path, file_out);
}

static auto try_backend_read_file(string_view path, kernel_heap_ptr<u8>& owned_buffer, usize& size_out) -> const u8* {
    const auto* driver = mounted_filesystem_driver();
    if (driver == null || driver->read_file == null) {
        return null;
    }

    owned_buffer.reset();
    return driver->read_file(path, owned_buffer, size_out);
}

static auto try_backend_remove_file(string_view path) -> bool {
    const auto* driver = mounted_filesystem_driver();
    if (driver == null || driver->remove_file == null) {
        return false;
    }

    return driver->remove_file(path);
}
static auto try_backend_list_directory(string_view path, fs::directory_visit_callback callback, void* context) -> bool {
    const auto* driver = mounted_filesystem_driver();
    if (driver == null || driver->list_directory == null || callback == null) {
        return false;
    }

    return driver->list_directory(path, callback, context);
}

static auto is_backup_shell_request(string_view path) -> bool {
    return path.compare(string_view("/bin/shell.vbin"));
}

static auto find_backup_shell(string_view path) -> const file_entry* {
    if (!is_backup_shell_request(path)) {
        return null;
    }
    return ramfs::find(string_view("/bin/shell.vbin"));
}

static auto close_stream(fs::file_handle handle, kernel_stream& stream, const char* reason) -> int {
    log::debug() << "fs: file_close handle=" << static_cast<unsigned long long>(handle)
                 << " backend=" << filesystem_driver_name(stream.driver)
                 << " owner=" << static_cast<unsigned long long>(stream.owner_task_id)
                 << " path='" << stream.path.view() << "' dirty="
                 << (stream.dirty ? "yes" : "no") << " size="
                 << static_cast<unsigned long long>(stream.size)
                 << " reason=" << reason;

    if (stream.driver != null && stream.dirty) {
        const u8* data = stream.size > 0 ? stream_data(stream) : null;
        if (stream.driver->write_file == null || !stream.driver->write_file(stream.path.view(), data, stream.size)) {
            stream.error = true;
            log::debug() << "fs: file_close flush failed handle=" << static_cast<unsigned long long>(handle)
                         << " path='" << stream.path.view() << "'";
            return -1;
        }
        log::debug() << "fs: file_close flushed file path='" << stream.path.view()
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
    for (const auto getter : s_filesystem_driver_getters) {
        if (getter == null) {
            continue;
        }
        const auto* driver = &getter();
        if (driver->init != null) {
            driver->init();
        }
    }
    s_active_driver = null;
    s_initialised = true;
}

auto fs::mount_boot_filesystem() -> status_code {
    if (!s_initialised) {
        init();
    }

    s_active_driver = null;
    const auto rc = fs::mounts::mount_gpt_partition_index(1, mount_supported_partition, null);
    if (rc == status_code::success) {
        const auto mounted = query_info();
        log::info() << "fs: mounted " << mounted.active_backend.c_str()
                    << " on " << mounted.block_device.c_str()
                    << " root=" << mounted.logical_root_path.c_str();
    } else {
        log::warn() << "fs: boot filesystem mount failed for hardcoded GPT data partition";
        log::debug() << "fs: mount error code: " << static_cast<u32>(rc);
    }

    return rc;
}

auto fs::query_info() -> runtime_info {
    runtime_info info {};
    info.fallback_ready = ramfs::is_ready();

    const auto* driver = mounted_filesystem_driver();
    if (driver != null && driver->fill_runtime_info != null) {
        driver->fill_runtime_info(info);
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
    if (try_backend_file_exists(normalized)) {
        return true;
    }
    return false;
}

auto fs::file_size(const char* path) -> usize {
    if (path == null) {
        return 0;
    }

    const auto normalized = normalize_path(string_view(path));
    const usize size = try_backend_file_size(normalized);
    if (size > 0 || try_backend_file_exists(normalized)) {
        return size;
    }

    return 0;
}

auto fs::directory_exists(const char* path) -> bool {
    if (path == null) {
        return false;
    }

    const auto normalized = normalize_path(string_view(path));
    return try_backend_directory_exists(normalized);
}

auto fs::list_directory(const char* path, directory_visit_callback callback, void* context) -> bool {
    if (!s_initialised) {
        init();
    }
    if (path == null || callback == null) {
        return false;
    }

    const auto normalized = normalize_path(string_view(path));
    const bool ok = try_backend_list_directory(normalized, callback, context);
    log::debug() << "fs: list_directory path='" << normalized << "' result=" << (ok ? "ok" : "fail");
    return ok;
}

auto fs::load_file(string_view path, kernel_heap_ptr<u8>& owned_buffer, usize& size_out) -> const u8* {
    const auto normalized = normalize_path(path);
    log::debug() << "fs: loading file '" << normalized << "'";
    const u8* backend_data = try_backend_read_file(normalized, owned_buffer, size_out);
    if (backend_data != null) {
        const auto* driver = mounted_filesystem_driver();
        log::debug() << "fs: load_file '" << normalized << "' resolved via "
                     << filesystem_driver_name(driver) << " size="
                     << static_cast<unsigned long long>(size_out);
        return backend_data;
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
            const auto* driver = mounted_filesystem_driver();
            if (driver == null) {
                log::debug() << "fs: file_open denied writable open without mounted filesystem path='"
                             << normalized << "'";
                reset_stream(stream);
                return 0;
            }

            stream.driver = driver;
            kernel_heap_ptr<u8> owned_buffer;
            usize size = 0;
            const u8* file_data = try_backend_read_file(normalized, owned_buffer, size);
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
                         << " backend=" << filesystem_driver_name(stream.driver)
                         << " path='" << normalized << "' size="
                         << static_cast<unsigned long long>(stream.size)
                         << " position=" << static_cast<unsigned long long>(stream.position);
            return static_cast<file_handle>(i + 1);
        }

        filesystem_file_descriptor backend_file {};
        if (try_backend_open_file(normalized, backend_file)) {
            stream.driver = mounted_filesystem_driver();
            stream.backend_file = backend_file;
            stream.direct_backend = true;
            stream.size = try_backend_file_size(normalized);
            stream.capacity = stream.size;
            stream.position = 0;
            stream.eof = stream.size == 0;
            log::debug() << "fs: file_open handle=" << static_cast<unsigned long long>(i + 1)
                         << " backend=" << filesystem_driver_name(stream.driver)
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
    if (stream->driver != null && stream->direct_backend) {
        if (to_copy > stream->driver->max_direct_read_bytes) {
            to_copy = stream->driver->max_direct_read_bytes;
        }
        usize read_count = 0;
        if (stream->driver->read_file_range == null
            || !stream->driver->read_file_range(stream->backend_file, stream->position, buf, to_copy, read_count)) {
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

auto fs::file_truncate(file_handle handle, i64 length) -> int {
    auto* stream = handle_from_id(handle);
    if (stream == null || !stream->writable || length < 0) {
        return -1;
    }

    const usize next_size = static_cast<usize>(length);
    if (static_cast<i64>(next_size) != length) {
        stream->error = true;
        return -1;
    }

    if (next_size > stream->size) {
        if (!ensure_capacity(*stream, next_size)) {
            stream->error = true;
            return -1;
        }
        memory::set(stream->owned_buffer + stream->size, 0, next_size - stream->size);
    }

    stream->size = next_size;
    if (stream->position > stream->size) {
        stream->position = stream->size;
    }
    stream->dirty = true;
    stream->eof = stream->position >= stream->size;
    log::debug() << "fs: file_truncate handle=" << static_cast<unsigned long long>(handle)
                 << " path='" << stream->path.view() << "' length="
                 << static_cast<unsigned long long>(stream->size);
    return 0;
}

auto fs::file_remove(const char* path) -> int {
    if (path == null) {
        log::debug() << "fs: file_remove rejected null path";
        return -1;
    }

    const auto normalized = normalize_path(string_view(path));
    const bool removed = try_backend_remove_file(normalized);
    log::debug() << "fs: file_remove path='" << normalized << "' result="
                 << (removed ? "removed" : "not-found-or-failed");
    return removed ? 0 : -1;
}

} // namespace vk
