/*
 * vkernel - UEFI Microkernel
 * Copyright (C) 2026 vkernel authors
 *
 * kernel_debug.cpp - Optional kernel symbol lookup for crash diagnostics.
 */

#include "config.h"
#include "types.h"
#include "memory.h"
#include "fs.h"
#include "fs/ramfs.h"
#include "kernel_debug.h"
#include "resource_ptr.h"
#include "spinlock.h"
#include "arch/x86_64/arch.h"

namespace vk {
namespace kernel_debug {
namespace {

struct kernel_symbol_entry {
    u64 address;
    u32 name_offset;
};

struct kernel_line_entry {
    u64 address;
    u32 line;
    u32 file_offset;
};

struct symbol_table_state {
    kernel_symbol_entry* entries = null;
    usize count = 0;
    const char* strings = null;
    void* storage = null;
    spinlock lock;
};

struct line_table_state {
    kernel_line_entry* entries = null;
    usize count = 0;
    const char* files = null;
    void* storage = null;
    spinlock lock;
};

static symbol_table_state s_symbols;
static line_table_state s_lines;
static constexpr const char* KERNEL_SYMBOL_MAP_PATH = "/boot/vkernel.elf.map";
static constexpr const char* KERNEL_LINE_MAP_PATH = "/boot/vkernel.elf.lines";

static auto is_space(char ch) -> bool {
    return ch == ' ' || ch == '\t' || ch == '\r';
}

static auto is_hex_digit(char ch) -> bool {
    return (ch >= '0' && ch <= '9')
        || (ch >= 'a' && ch <= 'f')
        || (ch >= 'A' && ch <= 'F');
}

static auto is_text_symbol_type(char ch) -> bool {
    return ch == 'T' || ch == 't' || ch == 'W' || ch == 'w';
}

static auto hex_value(char ch) -> u8 {
    if (ch >= '0' && ch <= '9') {
        return static_cast<u8>(ch - '0');
    }
    if (ch >= 'a' && ch <= 'f') {
        return static_cast<u8>(10 + (ch - 'a'));
    }
    return static_cast<u8>(10 + (ch - 'A'));
}

static void sort_symbols_by_address(kernel_symbol_entry* entries, usize count) {
    for (usize i = 1; i < count; ++i) {
        kernel_symbol_entry key = entries[i];
        usize j = i;
        while (j > 0 && entries[j - 1].address > key.address) {
            entries[j] = entries[j - 1];
            --j;
        }
        entries[j] = key;
    }
}

static auto parse_hex_u64(const char* start, const char* end, u64* out) -> bool {
    if (out != null) {
        *out = 0;
    }
    if (start == null || end == null || start >= end) {
        return false;
    }

    u64 value = 0;
    for (const char* p = start; p < end; ++p) {
        if (!is_hex_digit(*p)) {
            return false;
        }
        value = (value << 4) | hex_value(*p);
    }

    if (out != null) {
        *out = value;
    }
    return true;
}

static auto parse_dec_u32(const char* start, const char* end, u32* out) -> bool {
    if (out != null) {
        *out = 0;
    }
    if (start == null || end == null || start >= end) {
        return false;
    }

    u64 value = 0;
    for (const char* p = start; p < end; ++p) {
        if (*p < '0' || *p > '9') {
            return false;
        }
        value = (value * 10) + static_cast<u64>(*p - '0');
        if (value > 0xFFFF'FFFFULL) {
            return false;
        }
    }

    if (out != null) {
        *out = static_cast<u32>(value);
    }
    return true;
}

static auto find_line_end(const char* cursor, const char* end) -> const char* {
    const char* line_end = cursor;
    while (line_end < end && *line_end != '\n') {
        ++line_end;
    }
    return line_end;
}

static auto find_char(const char* start, const char* end, char ch) -> const char* {
    if (start == null || end == null) {
        return null;
    }

    for (const char* p = start; p < end; ++p) {
        if (*p == ch) {
            return p;
        }
    }
    return null;
}

static auto count_symbols_in_map(const char* data,
                                 usize size,
                                 usize* out_count,
                                 usize* out_string_bytes) -> bool {
    if (out_count != null) {
        *out_count = 0;
    }
    if (out_string_bytes != null) {
        *out_string_bytes = 0;
    }
    if (data == null || size == 0) {
        return false;
    }

    const char* cursor = data;
    const char* end = data + size;
    usize count = 0;
    usize string_bytes = 0;

    while (cursor < end) {
        const char* line_end = find_line_end(cursor, end);
        const char* p = cursor;
        while (p < line_end && is_space(*p)) {
            ++p;
        }

        const char* hex_start = p;
        while (p < line_end && is_hex_digit(*p)) {
            ++p;
        }
        if (hex_start == p) {
            cursor = line_end < end ? line_end + 1 : end;
            continue;
        }

        while (p < line_end && is_space(*p)) {
            ++p;
        }
        if (p >= line_end || !is_text_symbol_type(*p)) {
            cursor = line_end < end ? line_end + 1 : end;
            continue;
        }
        ++p;

        while (p < line_end && is_space(*p)) {
            ++p;
        }
        if (p >= line_end) {
            cursor = line_end < end ? line_end + 1 : end;
            continue;
        }

        const char* name_end = line_end;
        while (name_end > p
               && (name_end[-1] == '\r'
                   || name_end[-1] == ' '
                   || name_end[-1] == '\t')) {
            --name_end;
        }
        if (name_end <= p) {
            cursor = line_end < end ? line_end + 1 : end;
            continue;
        }

        ++count;
        string_bytes += static_cast<usize>(name_end - p) + 1;
        cursor = line_end < end ? line_end + 1 : end;
    }

    if (out_count != null) {
        *out_count = count;
    }
    if (out_string_bytes != null) {
        *out_string_bytes = string_bytes;
    }
    return count > 0 && string_bytes > 0;
}

static auto parse_line_record(const char* start,
                              const char* end,
                              u64* out_address,
                              u32* out_line,
                              const char** out_path,
                              usize* out_path_len) -> bool {
    if (out_address != null) {
        *out_address = 0;
    }
    if (out_line != null) {
        *out_line = 0;
    }
    if (out_path != null) {
        *out_path = null;
    }
    if (out_path_len != null) {
        *out_path_len = 0;
    }

    if (start == null || end == null || start >= end) {
        return false;
    }

    const char* tab1 = find_char(start, end, '\t');
    if (tab1 == null) {
        return false;
    }
    const char* tab2 = find_char(tab1 + 1, end, '\t');
    if (tab2 == null || tab2 + 1 >= end) {
        return false;
    }

    u64 address = 0;
    u32 line = 0;
    if (!parse_hex_u64(start, tab1, &address) ||
        !parse_dec_u32(tab1 + 1, tab2, &line)) {
        return false;
    }

    const char* path = tab2 + 1;
    const usize path_len = static_cast<usize>(end - path);
    if (path_len == 0) {
        return false;
    }

    if (out_address != null) {
        *out_address = address;
    }
    if (out_line != null) {
        *out_line = line;
    }
    if (out_path != null) {
        *out_path = path;
    }
    if (out_path_len != null) {
        *out_path_len = path_len;
    }
    return true;
}

static auto build_symbol_table_from_map(const char* data, usize size) -> bool {
    usize count = 0;
    usize string_bytes = 0;
    if (!count_symbols_in_map(data, size, &count, &string_bytes)) {
        return false;
    }

    const usize entries_bytes = count * sizeof(kernel_symbol_entry);
    const usize total_bytes = entries_bytes + string_bytes;
    auto* storage = static_cast<u8*>(g_kernel_heap.allocate(total_bytes));
    if (storage == null) {
        return false;
    }

    auto* entries = reinterpret_cast<kernel_symbol_entry*>(storage);
    auto* strings = reinterpret_cast<char*>(storage + entries_bytes);
    usize next_entry = 0;
    usize next_string = 0;

    const char* cursor = data;
    const char* end = data + size;
    while (cursor < end) {
        const char* line_end = find_line_end(cursor, end);
        const char* p = cursor;
        while (p < line_end && is_space(*p)) {
            ++p;
        }

        const char* hex_start = p;
        while (p < line_end && is_hex_digit(*p)) {
            ++p;
        }

        u64 address = 0;
        if (!parse_hex_u64(hex_start, p, &address)) {
            cursor = line_end < end ? line_end + 1 : end;
            continue;
        }

        while (p < line_end && is_space(*p)) {
            ++p;
        }
        if (p >= line_end || !is_text_symbol_type(*p)) {
            cursor = line_end < end ? line_end + 1 : end;
            continue;
        }
        ++p;

        while (p < line_end && is_space(*p)) {
            ++p;
        }
        if (p >= line_end) {
            cursor = line_end < end ? line_end + 1 : end;
            continue;
        }

        const char* name_end = line_end;
        while (name_end > p
               && (name_end[-1] == '\r'
                   || name_end[-1] == ' '
                   || name_end[-1] == '\t')) {
            --name_end;
        }
        if (name_end <= p) {
            cursor = line_end < end ? line_end + 1 : end;
            continue;
        }

        entries[next_entry].address = address;
        entries[next_entry].name_offset = static_cast<u32>(next_string);
        memory::copy(strings + next_string, p, static_cast<usize>(name_end - p));
        strings[next_string + static_cast<usize>(name_end - p)] = '\0';
        next_string += static_cast<usize>(name_end - p) + 1;
        ++next_entry;
        cursor = line_end < end ? line_end + 1 : end;
    }

    sort_symbols_by_address(entries, next_entry);
    s_symbols.entries = entries;
    s_symbols.count = next_entry;
    s_symbols.strings = strings;
    s_symbols.storage = storage;
    return next_entry > 0;
}

static void sort_lines_by_address(kernel_line_entry* entries, usize count) {
    for (usize i = 1; i < count; ++i) {
        kernel_line_entry key = entries[i];
        usize j = i;
        while (j > 0 && entries[j - 1].address > key.address) {
            entries[j] = entries[j - 1];
            --j;
        }
        entries[j] = key;
    }
}

static auto count_lines_in_map(const char* data,
                               usize size,
                               usize* out_count,
                               usize* out_file_bytes) -> bool {
    if (out_count != null) {
        *out_count = 0;
    }
    if (out_file_bytes != null) {
        *out_file_bytes = 0;
    }
    if (data == null || size == 0) {
        return false;
    }

    const char* cursor = data;
    const char* end = data + size;
    const char* line_end = find_line_end(cursor, end);
    if (static_cast<usize>(line_end - cursor) != 8
        || !string_view(cursor, 8).compare(string_view("vklines1"))) {
        return false;
    }
    cursor = line_end < end ? line_end + 1 : end;

    usize count = 0;
    usize file_bytes = 0;
    while (cursor < end) {
        line_end = find_line_end(cursor, end);
        u64 address = 0;
        u32 line = 0;
        const char* path = null;
        usize path_len = 0;
        if (parse_line_record(cursor, line_end, &address, &line, &path, &path_len)) {
            ++count;
            file_bytes += path_len + 1;
        }
        cursor = line_end < end ? line_end + 1 : end;
    }

    if (out_count != null) {
        *out_count = count;
    }
    if (out_file_bytes != null) {
        *out_file_bytes = file_bytes;
    }
    return count > 0 && file_bytes > 0;
}

static auto build_line_table_from_map(const char* data, usize size) -> bool {
    usize count = 0;
    usize file_bytes = 0;
    if (!count_lines_in_map(data, size, &count, &file_bytes)) {
        return false;
    }

    const usize entries_bytes = count * sizeof(kernel_line_entry);
    const usize total_bytes = entries_bytes + file_bytes;
    auto* storage = static_cast<u8*>(g_kernel_heap.allocate(total_bytes));
    if (storage == null) {
        return false;
    }

    auto* entries = reinterpret_cast<kernel_line_entry*>(storage);
    auto* files = reinterpret_cast<char*>(storage + entries_bytes);
    usize next_entry = 0;
    usize next_file = 0;

    const char* cursor = data;
    const char* end = data + size;
    const char* line_end = find_line_end(cursor, end);
    cursor = line_end < end ? line_end + 1 : end;

    while (cursor < end) {
        line_end = find_line_end(cursor, end);
        u64 address = 0;
        u32 line = 0;
        const char* path = null;
        usize path_len = 0;
        if (parse_line_record(cursor, line_end, &address, &line, &path, &path_len)) {
            entries[next_entry].address = address;
            entries[next_entry].line = line;
            entries[next_entry].file_offset = static_cast<u32>(next_file);
            memory::copy(files + next_file, path, path_len);
            files[next_file + path_len] = '\0';
            next_file += path_len + 1;
            ++next_entry;
        }
        cursor = line_end < end ? line_end + 1 : end;
    }

    sort_lines_by_address(entries, next_entry);
    s_lines.entries = entries;
    s_lines.count = next_entry;
    s_lines.files = files;
    s_lines.storage = storage;
    return next_entry > 0;
}

static auto load_map_from_ramfs(const char* path, usize* out_size) -> const char* {
    if (out_size != null) {
        *out_size = 0;
    }

    const auto* entry = ramfs::find(path);
    if (entry == null || entry->data == null || entry->size == 0) {
        return null;
    }

    if (out_size != null) {
        *out_size = entry->size;
    }
    return reinterpret_cast<const char*>(entry->data);
}

static auto ensure_symbols_loaded() -> bool {
    if (s_symbols.entries != null && s_symbols.count > 0) {
        return true;
    }

    s_symbols.lock.acquire();
    if (s_symbols.entries != null && s_symbols.count > 0) {
        s_symbols.lock.release();
        return true;
    }

    usize map_size = 0;
    const char* map_data = load_map_from_ramfs(KERNEL_SYMBOL_MAP_PATH, &map_size);
    kernel_heap_ptr<u8> owned_buffer;
    if (map_data == null) {
        usize fs_size = 0;
        const u8* fs_data = fs::load_file(string_view(KERNEL_SYMBOL_MAP_PATH),
                                          owned_buffer,
                                          fs_size);
        if (fs_data != null && fs_size > 0) {
            map_data = reinterpret_cast<const char*>(fs_data);
            map_size = fs_size;
        }
    }

    const bool ok = map_data != null && map_size > 0
        && build_symbol_table_from_map(map_data, map_size);
    s_symbols.lock.release();
    return ok;
}

static auto ensure_lines_loaded() -> bool {
    if (s_lines.entries != null && s_lines.count > 0) {
        return true;
    }

    s_lines.lock.acquire();
    if (s_lines.entries != null && s_lines.count > 0) {
        s_lines.lock.release();
        return true;
    }

    usize map_size = 0;
    const char* map_data = load_map_from_ramfs(KERNEL_LINE_MAP_PATH, &map_size);
    kernel_heap_ptr<u8> owned_buffer;
    if (map_data == null) {
        usize fs_size = 0;
        const u8* fs_data = fs::load_file(string_view(KERNEL_LINE_MAP_PATH),
                                          owned_buffer,
                                          fs_size);
        if (fs_data != null && fs_size > 0) {
            map_data = reinterpret_cast<const char*>(fs_data);
            map_size = fs_size;
        }
    }

    const bool ok = map_data != null && map_size > 0
        && build_line_table_from_map(map_data, map_size);
    s_lines.lock.release();
    return ok;
}

} // namespace

auto lookup_symbol(u64 address, resolved_symbol* out) -> bool {
    if (out != null) {
        out->name = null;
        out->address = 0;
        out->offset = 0;
    }

    if (!ensure_symbols_loaded()) {
        return false;
    }

    const u64 image_base = arch::image_base();
    if (address < image_base) {
        return false;
    }
    const u64 relative_address = address - image_base;

    usize left = 0;
    usize right = s_symbols.count;
    while (left < right) {
        const usize mid = left + (right - left) / 2;
        if (s_symbols.entries[mid].address <= relative_address) {
            left = mid + 1;
        } else {
            right = mid;
        }
    }

    if (left == 0) {
        return false;
    }

    const auto& entry = s_symbols.entries[left - 1];
    if (relative_address < entry.address) {
        return false;
    }

    if (out != null) {
        out->name = s_symbols.strings + entry.name_offset;
        out->address = image_base + entry.address;
        out->offset = relative_address - entry.address;
    }
    return true;
}

auto lookup_source_location(u64 address, resolved_source_location* out) -> bool {
    if (out != null) {
        out->file_path = null;
        out->address = 0;
        out->offset = 0;
        out->line = 0;
    }

    if (!ensure_lines_loaded()) {
        return false;
    }

    const u64 image_base = arch::image_base();
    if (address < image_base) {
        return false;
    }
    const u64 relative_address = address - image_base;

    usize left = 0;
    usize right = s_lines.count;
    while (left < right) {
        const usize mid = left + (right - left) / 2;
        if (s_lines.entries[mid].address <= relative_address) {
            left = mid + 1;
        } else {
            right = mid;
        }
    }

    if (left == 0) {
        return false;
    }

    const auto& entry = s_lines.entries[left - 1];
    if (relative_address < entry.address) {
        return false;
    }

    if (out != null) {
        out->file_path = s_lines.files + entry.file_offset;
        out->address = image_base + entry.address;
        out->offset = relative_address - entry.address;
        out->line = entry.line;
    }
    return true;
}

} // namespace kernel_debug
} // namespace vk
