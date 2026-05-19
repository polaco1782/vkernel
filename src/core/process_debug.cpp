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

} // namespace

void init_debug_metadata(process_task_context* ctx) {
    if (ctx == null) {
        return;
    }

    ctx->symbols = null;
    ctx->symbol_count = 0;
    ctx->symbol_strings = null;
    ctx->symbol_storage = null;
}

auto attach_symbols_from_elf(process_task_context* ctx,
                             const u8* file_data,
                             usize file_size) -> bool {
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

    const i64 load_bias = static_cast<i64>(reinterpret_cast<u64>(ctx->image_base))
        - static_cast<i64>(vaddr_min);

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

void cleanup_debug_metadata(process_task_context* ctx) {
    if (ctx == null) {
        return;
    }

    if (ctx->symbol_storage != null) {
        g_kernel_heap.free(ctx->symbol_storage);
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

} // namespace process
} // namespace vk
