/*
 * vkernel - UEFI Microkernel
 * Copyright (C) 2026 vkernel authors
 *
 * process_debug.cpp - Optional per-process debug metadata.
 *
 * Keeps ELF symbol parsing and symbol lookup out of the main process
 * lifecycle code so process.cpp can stay focused on load/exec/cleanup.
 */

#include "config.h"
#include "types.h"
#include "memory.h"
#include "elf.h"
#include "fs.h"
#include "log.h"
#include "process_debug.h"
#include "process_internal.h"

namespace vk {
namespace process {
namespace {

static auto range_ok(usize file_size, u64 offset, u64 len) -> bool {
    if (offset > file_size) {
        return false;
    }
    if (len > file_size - offset) {
        return false;
    }
    return true;
}

static auto hex_value(char ch) -> u8 {
    if (ch >= '0' && ch <= '9') {
        return static_cast<u8>(ch - '0');
    }
    if (ch >= 'a' && ch <= 'f') {
        return static_cast<u8>(10 + (ch - 'a'));
    }
    if (ch >= 'A' && ch <= 'F') {
        return static_cast<u8>(10 + (ch - 'A'));
    }
    return 0xFF;
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
        const u8 digit = hex_value(*p);
        if (digit > 0x0F) {
            return false;
        }
        value = (value << 4) | digit;
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

static auto next_line(const char*& cursor, const char* end, string_view* out) -> bool {
    if (out != null) {
        *out = {};
    }

    if (cursor == null || end == null || cursor >= end) {
        return false;
    }

    const char* start = cursor;
    while (cursor < end && *cursor != '\n') {
        ++cursor;
    }

    const char* line_end = cursor;
    if (line_end > start && line_end[-1] == '\r') {
        --line_end;
    }
    if (cursor < end) {
        ++cursor;
    }

    if (out != null) {
        *out = string_view(start, static_cast<usize>(line_end - start));
    }
    return true;
}

static auto parse_line_record(string_view line,
                              u64* linked_address,
                              u32* line_number,
                              string_view* file_path) -> bool {
    if (linked_address != null) {
        *linked_address = 0;
    }
    if (line_number != null) {
        *line_number = 0;
    }
    if (file_path != null) {
        *file_path = {};
    }

    const char* start = line.data();
    const char* end = start + line.size();
    const char* tab1 = find_char(start, end, '\t');
    if (tab1 == null) {
        return false;
    }
    const char* tab2 = find_char(tab1 + 1, end, '\t');
    if (tab2 == null || tab2 + 1 >= end) {
        return false;
    }

    u64 parsed_address = 0;
    u32 parsed_line = 0;
    if (!parse_hex_u64(start, tab1, &parsed_address) ||
        !parse_dec_u32(tab1 + 1, tab2, &parsed_line)) {
        return false;
    }

    if (linked_address != null) {
        *linked_address = parsed_address;
    }
    if (line_number != null) {
        *line_number = parsed_line;
    }
    if (file_path != null) {
        *file_path = string_view(tab2 + 1, static_cast<usize>(end - (tab2 + 1)));
    }
    return true;
}

static auto compute_elf_load_bias(const process_task_context* ctx,
                                  const u8* file_data,
                                  usize file_size,
                                  i64* out) -> bool {
    if (out != null) {
        *out = 0;
    }

    if (ctx == null || file_data == null || ctx->image_base == null ||
        file_size < sizeof(elf::Elf64_Ehdr)) {
        return false;
    }

    const auto* ehdr = reinterpret_cast<const elf::Elf64_Ehdr*>(file_data);
    if (ehdr->e_ident[elf::EI_MAG0] != elf::ELFMAG0 ||
        ehdr->e_ident[elf::EI_MAG1] != elf::ELFMAG1 ||
        ehdr->e_ident[elf::EI_MAG2] != elf::ELFMAG2 ||
        ehdr->e_ident[elf::EI_MAG3] != elf::ELFMAG3) {
        return false;
    }

    u64 vaddr_min = ~u64{0};
    bool have_load_segment = false;
    if (ehdr->e_phoff != 0 && ehdr->e_phnum > 0 &&
        ehdr->e_phentsize == sizeof(elf::Elf64_Phdr) &&
        range_ok(file_size,
                 ehdr->e_phoff,
                 static_cast<u64>(ehdr->e_phnum) * sizeof(elf::Elf64_Phdr))) {
        const auto* phdrs =
            reinterpret_cast<const elf::Elf64_Phdr*>(file_data + ehdr->e_phoff);
        for (u16 i = 0; i < ehdr->e_phnum; ++i) {
            if (phdrs[i].p_type != elf::PT_LOAD) {
                continue;
            }
            if (!have_load_segment || phdrs[i].p_vaddr < vaddr_min) {
                vaddr_min = phdrs[i].p_vaddr;
            }
            have_load_segment = true;
        }
    }

    if (!have_load_segment) {
        return false;
    }

    if (out != null) {
        *out = static_cast<i64>(reinterpret_cast<u64>(ctx->image_base))
             - static_cast<i64>(vaddr_min);
    }
    return true;
}

static void sort_symbols_by_address(process_symbol* entries, usize count) {
    for (usize i = 1; i < count; ++i) {
        process_symbol key = entries[i];
        usize j = i;
        while (j > 0 && entries[j - 1].address > key.address) {
            entries[j] = entries[j - 1];
            --j;
        }
        entries[j] = key;
    }
}

static void sort_lines_by_address(process_line* entries, usize count) {
    for (usize i = 1; i < count; ++i) {
        process_line key = entries[i];
        usize j = i;
        while (j > 0 && entries[j - 1].address > key.address) {
            entries[j] = entries[j - 1];
            --j;
        }
        entries[j] = key;
    }
}

static auto build_line_map_path(string_view program_path,
                                static_string<256>& out_path) -> bool {
    constexpr string_view k_line_map_directory("/data/debug/lines/");
    constexpr string_view k_suffix(".lines");
    usize program_name_offset = 0;
    for (usize i = 0; i < program_path.size(); ++i) {
        if (program_path[i] == '/') {
            program_name_offset = i + 1;
        }
    }
    const string_view program_name(program_path.data() + program_name_offset,
                                   program_path.size() - program_name_offset);
    if (k_line_map_directory.size() + program_name.size() + k_suffix.size() > out_path.capacity()) {
        out_path.clear();
        return false;
    }

    char buffer[256];
    usize offset = 0;
    for (usize i = 0; i < k_line_map_directory.size(); ++i) {
        buffer[offset++] = k_line_map_directory[i];
    }
    for (usize i = 0; i < program_name.size(); ++i) {
        buffer[offset++] = program_name[i];
    }
    for (usize i = 0; i < k_suffix.size(); ++i) {
        buffer[offset++] = k_suffix[i];
    }

    return out_path.assign(string_view(buffer, offset));
}

} // namespace

void init_debug_metadata(process_task_context* ctx) {
    if (ctx == null) {
        return;
    }

    ctx->symbols = null;
    ctx->symbol_count = 0;
    ctx->symbol_strings = null;
    ctx->symbol_storage = null;
    ctx->lines = null;
    ctx->line_count = 0;
    ctx->line_files = null;
    ctx->line_storage = null;
}

auto attach_symbols_from_elf(process_task_context* ctx,
                             const u8* file_data,
                             usize file_size) -> bool {
    if (ctx == null || file_data == null || ctx->image_base == null ||
        file_size < sizeof(elf::Elf64_Ehdr)) {
        return false;
    }

    i64 load_bias = 0;
    if (!compute_elf_load_bias(ctx, file_data, file_size, &load_bias)) {
        return false;
    }

    const auto* ehdr = reinterpret_cast<const elf::Elf64_Ehdr*>(file_data);
    if (ehdr->e_shoff == 0 || ehdr->e_shnum == 0 ||
        ehdr->e_shentsize != sizeof(elf::Elf64_Shdr)) {
        return false;
    }

    if (!range_ok(file_size,
                  ehdr->e_shoff,
                  static_cast<u64>(ehdr->e_shnum) * sizeof(elf::Elf64_Shdr))) {
        return false;
    }

    const auto* shdrs =
        reinterpret_cast<const elf::Elf64_Shdr*>(file_data + ehdr->e_shoff);

    const elf::Elf64_Shdr* symtab_shdr = null;
    const elf::Elf64_Shdr* strtab_shdr = null;
    for (u16 i = 0; i < ehdr->e_shnum; ++i) {
        if (shdrs[i].sh_type != elf::SHT_SYMTAB) {
            continue;
        }
        if (shdrs[i].sh_entsize != sizeof(elf::Elf64_Sym) ||
            shdrs[i].sh_link >= ehdr->e_shnum) {
            continue;
        }
        const auto& linked = shdrs[shdrs[i].sh_link];
        if (linked.sh_type != elf::SHT_STRTAB) {
            continue;
        }
        if (!range_ok(file_size, shdrs[i].sh_offset, shdrs[i].sh_size) ||
            !range_ok(file_size, linked.sh_offset, linked.sh_size)) {
            continue;
        }
        symtab_shdr = &shdrs[i];
        strtab_shdr = &linked;
        break;
    }

    if (symtab_shdr == null || strtab_shdr == null || symtab_shdr->sh_size == 0) {
        return false;
    }

    const auto* symtab =
        reinterpret_cast<const elf::Elf64_Sym*>(file_data + symtab_shdr->sh_offset);
    const usize sym_count =
        static_cast<usize>(symtab_shdr->sh_size / sizeof(elf::Elf64_Sym));
    const auto* strtab =
        reinterpret_cast<const char*>(file_data + strtab_shdr->sh_offset);
    const usize strtab_size = static_cast<usize>(strtab_shdr->sh_size);
    const u64 image_start = reinterpret_cast<u64>(ctx->image_base);
    const u64 image_end = image_start + ctx->image_size;

    usize kept_count = 0;
    usize string_bytes = 0;
    for (usize i = 0; i < sym_count; ++i) {
        const auto& sym = symtab[i];
        if (sym.st_shndx == elf::SHN_UNDEF || sym.st_name == 0) {
            continue;
        }
        if (elf::elf64_st_type(sym.st_info) != elf::STT_FUNC ||
            sym.st_name >= strtab_size) {
            continue;
        }

        const u64 runtime_address =
            static_cast<u64>(static_cast<i64>(sym.st_value) + load_bias);
        if (runtime_address < image_start || runtime_address >= image_end) {
            continue;
        }

        const char* name = strtab + sym.st_name;
        usize name_len = 0;
        while (sym.st_name + name_len < strtab_size && name[name_len] != '\0') {
            ++name_len;
        }
        if (sym.st_name + name_len >= strtab_size || name_len == 0) {
            continue;
        }

        ++kept_count;
        string_bytes += name_len + 1;
    }

    if (kept_count == 0 || string_bytes == 0) {
        return false;
    }

    const usize entries_bytes = kept_count * sizeof(process_symbol);
    const usize total_bytes = entries_bytes + string_bytes;
    auto* storage = static_cast<u8*>(g_kernel_heap.allocate(total_bytes));
    if (storage == null) {
        return false;
    }

    auto* entries = reinterpret_cast<process_symbol*>(storage);
    auto* strings = reinterpret_cast<char*>(storage + entries_bytes);
    usize next_entry = 0;
    usize next_string = 0;

    for (usize i = 0; i < sym_count; ++i) {
        const auto& sym = symtab[i];
        if (sym.st_shndx == elf::SHN_UNDEF || sym.st_name == 0) {
            continue;
        }
        if (elf::elf64_st_type(sym.st_info) != elf::STT_FUNC ||
            sym.st_name >= strtab_size) {
            continue;
        }

        const u64 runtime_address =
            static_cast<u64>(static_cast<i64>(sym.st_value) + load_bias);
        if (runtime_address < image_start || runtime_address >= image_end) {
            continue;
        }

        const char* name = strtab + sym.st_name;
        usize name_len = 0;
        while (sym.st_name + name_len < strtab_size && name[name_len] != '\0') {
            ++name_len;
        }
        if (sym.st_name + name_len >= strtab_size || name_len == 0) {
            continue;
        }

        process_symbol entry {};
        entry.address = runtime_address;
        entry.size = sym.st_size > 0xFFFF'FFFFULL
            ? 0xFFFF'FFFFu
            : static_cast<u32>(sym.st_size);
        entry.name_offset = static_cast<u32>(next_string);

        entries[next_entry++] = entry;
        memory::copy(strings + next_string, name, name_len);
        strings[next_string + name_len] = '\0';
        next_string += name_len + 1;
    }

    sort_symbols_by_address(entries, next_entry);

    ctx->symbols = entries;
    ctx->symbol_count = next_entry;
    ctx->symbol_strings = strings;
    ctx->symbol_storage = storage;
    return next_entry > 0;
}

auto attach_lines_from_map(process_task_context* ctx,
                           string_view program_path,
                           const u8* file_data,
                           usize file_size) -> bool {
    if (ctx == null || program_path.empty()) {
        return false;
    }

    i64 load_bias = 0;
    if (!compute_elf_load_bias(ctx, file_data, file_size, &load_bias)) {
        return false;
    }

    static_string<256> line_map_path;
    if (!build_line_map_path(program_path, line_map_path)) {
        return false;
    }

    kernel_heap_ptr<u8> owned_map;
    usize map_size = 0;
    const u8* map_data = fs::load_file(line_map_path.view(), owned_map, map_size);
    if (map_data == null || map_size == 0) {
        return false;
    }

    const u64 image_start = reinterpret_cast<u64>(ctx->image_base);
    const u64 image_end = image_start + ctx->image_size;
    const char* cursor = reinterpret_cast<const char*>(map_data);
    const char* map_end = cursor + map_size;

    string_view header;
    if (!next_line(cursor, map_end, &header) || !header.compare(string_view("vklines1"))) {
        return false;
    }

    usize kept_count = 0;
    usize file_bytes = 0;
    while (cursor < map_end) {
        string_view line;
        if (!next_line(cursor, map_end, &line) || line.empty()) {
            continue;
        }

        u64 linked_address = 0;
        u32 line_number = 0;
        string_view file_path;
        if (!parse_line_record(line, &linked_address, &line_number, &file_path) ||
            file_path.empty()) {
            continue;
        }

        const i64 runtime_address_i64 =
            static_cast<i64>(linked_address) + load_bias;
        if (runtime_address_i64 < 0) {
            continue;
        }

        const u64 runtime_address = static_cast<u64>(runtime_address_i64);
        if (runtime_address < image_start || runtime_address >= image_end) {
            continue;
        }

        ++kept_count;
        file_bytes += file_path.size() + 1;
    }

    if (kept_count == 0 || file_bytes == 0) {
        return false;
    }

    const usize entries_bytes = kept_count * sizeof(process_line);
    const usize total_bytes = entries_bytes + file_bytes;
    auto* storage = static_cast<u8*>(g_kernel_heap.allocate(total_bytes));
    if (storage == null) {
        return false;
    }

    auto* entries = reinterpret_cast<process_line*>(storage);
    auto* files = reinterpret_cast<char*>(storage + entries_bytes);
    usize next_entry = 0;
    usize next_file = 0;

    cursor = reinterpret_cast<const char*>(map_data);
    if (!next_line(cursor, map_end, &header)) {
        g_kernel_heap.free(storage);
        return false;
    }

    while (cursor < map_end) {
        string_view line;
        if (!next_line(cursor, map_end, &line) || line.empty()) {
            continue;
        }

        u64 linked_address = 0;
        u32 line_number = 0;
        string_view file_path;
        if (!parse_line_record(line, &linked_address, &line_number, &file_path) ||
            file_path.empty()) {
            continue;
        }

        const i64 runtime_address_i64 =
            static_cast<i64>(linked_address) + load_bias;
        if (runtime_address_i64 < 0) {
            continue;
        }

        const u64 runtime_address = static_cast<u64>(runtime_address_i64);
        if (runtime_address < image_start || runtime_address >= image_end) {
            continue;
        }

        process_line entry {};
        entry.address = runtime_address;
        entry.line = line_number;
        entry.file_offset = static_cast<u32>(next_file);

        entries[next_entry++] = entry;
        memory::copy(files + next_file, file_path.data(), file_path.size());
        files[next_file + file_path.size()] = '\0';
        next_file += file_path.size() + 1;
    }

    sort_lines_by_address(entries, next_entry);

    ctx->lines = entries;
    ctx->line_count = next_entry;
    ctx->line_files = files;
    ctx->line_storage = storage;
    return next_entry > 0;
}

void cleanup_debug_metadata(process_task_context* ctx) {
    if (ctx == null) {
        return;
    }

    if (ctx->symbol_storage != null) {
        g_kernel_heap.free(ctx->symbol_storage);
    }
    if (ctx->line_storage != null) {
        g_kernel_heap.free(ctx->line_storage);
    }

    init_debug_metadata(ctx);
}

auto lookup_symbol(const process_task_context* ctx,
                   u64 address,
                   resolved_symbol* out) -> bool {
    if (out != null) {
        out->name = null;
        out->address = 0;
        out->offset = 0;
        out->size = 0;
    }

    if (ctx == null || ctx->symbols == null || ctx->symbol_strings == null ||
        ctx->symbol_count == 0) {
        return false;
    }

    usize left = 0;
    usize right = ctx->symbol_count;
    while (left < right) {
        const usize mid = left + (right - left) / 2;
        if (ctx->symbols[mid].address <= address) {
            left = mid + 1;
        } else {
            right = mid;
        }
    }

    if (left == 0) {
        return false;
    }

    const auto& entry = ctx->symbols[left - 1];
    if (address < entry.address) {
        return false;
    }

    if (out != null) {
        out->name = ctx->symbol_strings + entry.name_offset;
        out->address = entry.address;
        out->offset = address - entry.address;
        out->size = entry.size;
    }
    return true;
}

auto lookup_source_location(const process_task_context* ctx,
                            u64 address,
                            resolved_source_location* out) -> bool {
    if (out != null) {
        out->file_path = null;
        out->address = 0;
        out->offset = 0;
        out->line = 0;
    }

    if (ctx == null || ctx->lines == null || ctx->line_files == null ||
        ctx->line_count == 0) {
        return false;
    }

    usize left = 0;
    usize right = ctx->line_count;
    while (left < right) {
        const usize mid = left + (right - left) / 2;
        if (ctx->lines[mid].address <= address) {
            left = mid + 1;
        } else {
            right = mid;
        }
    }

    if (left == 0) {
        return false;
    }

    const auto& entry = ctx->lines[left - 1];
    if (address < entry.address) {
        return false;
    }

    if (out != null) {
        out->file_path = ctx->line_files + entry.file_offset;
        out->address = entry.address;
        out->offset = address - entry.address;
        out->line = entry.line;
    }
    return true;
}

} // namespace process
} // namespace vk
