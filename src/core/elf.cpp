/*
 * vkernel - UEFI Microkernel
 * Copyright (C) 2026 vkernel authors
 *
 * elf.cpp - ELF64 loader implementation
 *
 * Loads statically-linked ET_EXEC and position-independent ET_DYN
 * binaries from an in-memory buffer into the kernel heap, then
 * returns the resolved entry-point address for the caller to invoke.
 */

#include "config.h"
#include "types.h"
#include "memory.h"
#include "console.h"
#include "elf.h"

namespace vk {
namespace elf {

/* ============================================================
 * Internal helpers
 * ============================================================ */

/* Align a value up to the nearest multiple of align (must be power-of-2) */
static constexpr auto align_up(u64 value, u64 align) -> u64 {
    if (align == 0) return value;
    return (value + align - 1) & ~(align - 1);
}

/* Safe read: verify [offset, offset+len) lies within [0, file_size) */
static bool range_ok(usize file_size, u64 offset, u64 len) {
    if (offset > file_size) return false;
    if (len > file_size - offset) return false;
    return true;
}

/* ============================================================
 * load()
 * ============================================================ */

auto load(const u8* file_data, usize file_size) -> load_result {
    load_result result{};
    result.error          = elf_error::ok;
    result.entry          = 0;
    result.image_base     = null;
    result.image_size     = 0;
    result.image_from_phys = false;

    /* ---- 1. Basic size check ---- */
    if (file_size < sizeof(Elf64_Ehdr)) {
        result.error = elf_error::too_small;
        return result;
    }

    const auto* ehdr = reinterpret_cast<const Elf64_Ehdr*>(file_data);

    /* ---- 2. Magic ---- */
    if (ehdr->e_ident[EI_MAG0] != ELFMAG0 ||
        ehdr->e_ident[EI_MAG1] != ELFMAG1 ||
        ehdr->e_ident[EI_MAG2] != ELFMAG2 ||
        ehdr->e_ident[EI_MAG3] != ELFMAG3) {
        result.error = elf_error::bad_magic;
        return result;
    }

    /* ---- 3. Class / endian / version ---- */
    if (ehdr->e_ident[EI_CLASS] != ELFCLASS64) {
        result.error = elf_error::bad_class;
        return result;
    }
    if (ehdr->e_ident[EI_DATA] != ELFDATA2LSB) {
        result.error = elf_error::bad_endian;
        return result;
    }
    if (ehdr->e_ident[EI_VERSION] != EV_CURRENT) {
        result.error = elf_error::bad_version;
        return result;
    }

    /* ---- 4. Machine / type ---- */
    if (ehdr->e_machine != EM_X86_64) {
        result.error = elf_error::bad_machine;
        return result;
    }
    if (ehdr->e_type != ET_EXEC && ehdr->e_type != ET_DYN) {
        result.error = elf_error::bad_type;
        return result;
    }

    /* ---- 5. Validate program header table ---- */
    if (!range_ok(file_size, ehdr->e_phoff,
                  static_cast<u64>(ehdr->e_phnum) * sizeof(Elf64_Phdr))) {
        result.error = elf_error::too_small;
        return result;
    }

    const auto* phdrs = reinterpret_cast<const Elf64_Phdr*>(
        file_data + ehdr->e_phoff);

    /* ---- 6. Compute the virtual address span of all PT_LOAD segments ---- */
    u64 vaddr_min = ~u64{0};
    u64 vaddr_max = 0;
    u32 load_count = 0;

    for (u16 i = 0; i < ehdr->e_phnum; ++i) {
        const auto& ph = phdrs[i];
        if (ph.p_type != PT_LOAD) continue;

        if (!range_ok(file_size, ph.p_offset, ph.p_filesz)) {
            result.error = elf_error::segment_overflow;
            return result;
        }

        u64 seg_start = ph.p_vaddr;
        u64 seg_end   = ph.p_vaddr + ph.p_memsz;

        if (seg_start < vaddr_min) vaddr_min = seg_start;
        if (seg_end   > vaddr_max) vaddr_max = seg_end;

        ++load_count;
    }

    if (load_count == 0) {
        result.error = elf_error::no_load_segments;
        return result;
    }

    /* ---- 7. Allocate contiguous image buffer ---- */
    /*
     * For ET_DYN (PIE) we use the virtual addresses relative to vaddr_min
     * as offsets into the allocation (load bias = image_base - vaddr_min).
     * For ET_EXEC the kernel is identity-mapped, so we still allocate a
     * heap buffer and copy segments there; the load bias shifts the entry
     * point accordingly.  This works for simple programs that do not rely
     * on hard-coded absolute virtual addresses for data (e.g. no global
     * pointer tables without relocation).  A full user-mode environment
     * would map these at their requested virtual addresses via paging.
     */
    u64 image_size = align_up(vaddr_max - vaddr_min, 4096ULL);
    auto* image_base = static_cast<u8*>(g_kernel_heap.allocate_zero(image_size));

    console::puts("[elf] vaddr range ");
    console::put_hex(vaddr_min);
    console::puts(" - ");
    console::put_hex(vaddr_max);
    console::puts(" (size ");
    console::put_hex(image_size);
    console::puts(" bytes)\n");

    if (image_base == null) {
        /* Heap too small — fall back to the physical allocator.  The
         * physical allocator works in pages; cast the phys_addr to a
         * pointer (valid because the kernel runs identity-mapped). */
        console::puts("[elf] heap allocation failed, trying physical allocator...\n");
        u32 page_count = static_cast<u32>(
            (image_size + PAGE_SIZE_4K - 1) / PAGE_SIZE_4K);
        phys_addr phys = g_phys_alloc.allocate_pages(
            page_count, static_cast<u32>(PAGE_SIZE_4K), 0);
        if (phys == 0) {
            console::puts("[elf] physical allocation failed\n");
            result.error = elf_error::no_memory;
            return result;
        }
        image_base = reinterpret_cast<u8*>(phys);
        /* Physical pages are not guaranteed to be zeroed — clear them now. */
        memory::memory_set(image_base, 0, image_size);
        result.image_from_phys = true;
    }

    /* load_bias: value to add to a vaddr to get the host pointer */
    i64 load_bias = static_cast<i64>(
        reinterpret_cast<u64>(image_base)) - static_cast<i64>(vaddr_min);

    /* ---- 8. Copy PT_LOAD segments ---- */
    for (u16 i = 0; i < ehdr->e_phnum; ++i) {
        const auto& ph = phdrs[i];
        if (ph.p_type != PT_LOAD) continue;

        u8* dest = reinterpret_cast<u8*>(
            static_cast<i64>(ph.p_vaddr) + load_bias);

        /* Copy file image */
        if (ph.p_filesz > 0) {
            memory::memory_copy(dest, file_data + ph.p_offset, ph.p_filesz);
        }
        /* Zero-fill BSS region (p_memsz > p_filesz) — already zeroed by
         * allocate_zero, but be explicit for clarity */
        if (ph.p_memsz > ph.p_filesz) {
            memory::memory_set(dest + ph.p_filesz, 0,
                               ph.p_memsz - ph.p_filesz);
        }
    }

    /* ---- 9. Process dynamic relocations (ET_DYN / PIE only) ---- */
    /*
     * Walk PT_DYNAMIC to find DT_RELA / DT_RELASZ, then apply every
     * R_X86_64_RELATIVE entry:  *loc += load_bias
     * This fixes up any absolute pointer slots (vtables, fn-pointer tables,
     * initialised pointer globals) that the static linker baked in at
     * link-time-zero-base addresses.
     */
    if (ehdr->e_type == ET_DYN) {
        u64 rela_vaddr = 0;
        u64 rela_size  = 0;

        for (u16 i = 0; i < ehdr->e_phnum; ++i) {
            const auto& ph = phdrs[i];
            if (ph.p_type != PT_DYNAMIC) continue;

            const auto* dyn = reinterpret_cast<const Elf64_Dyn*>(
                reinterpret_cast<u8*>(static_cast<i64>(ph.p_vaddr) + load_bias));

            for (; dyn->d_tag != DT_NULL; ++dyn) {
                if      (dyn->d_tag == DT_RELA)   rela_vaddr = dyn->d_val;
                else if (dyn->d_tag == DT_RELASZ)  rela_size  = dyn->d_val;
            }
            break;
        }

        if (rela_vaddr != 0 && rela_size >= sizeof(Elf64_Rela)) {
            const auto* rela = reinterpret_cast<const Elf64_Rela*>(
                reinterpret_cast<u8*>(static_cast<i64>(rela_vaddr) + load_bias));
            const usize count = rela_size / sizeof(Elf64_Rela);

            for (usize i = 0; i < count; ++i) {
                if (elf64_r_type(rela[i].r_info) != R_X86_64_RELATIVE) continue;
                auto* loc = reinterpret_cast<u64*>(
                    static_cast<i64>(rela[i].r_offset) + load_bias);
                *loc = static_cast<u64>(load_bias + rela[i].r_addend);
            }
        }
    }

    /* ---- 10. Resolve entry point ---- */
    result.entry = static_cast<u64>(
        static_cast<i64>(ehdr->e_entry) + load_bias);
    result.image_base = image_base;
    result.image_size = image_size;
    result.error      = elf_error::ok;

#if VK_DEBUG_LEVEL >= 3
    console::puts("[elf] loaded: image_base=0x");
    console::put_hex(reinterpret_cast<u64>(image_base));
    console::puts(" size=");
    console::put_dec(image_size);
    console::puts(" entry=0x");
    console::put_hex(result.entry);
    console::puts("\n");
#endif

    return result;
}

/* ============================================================
 * error_string()
 * ============================================================ */

auto error_string(elf_error err) -> const char* {
    switch (err) {
        case elf_error::ok:               return "ok";
        case elf_error::too_small:        return "file too small";
        case elf_error::bad_magic:        return "bad ELF magic";
        case elf_error::bad_class:        return "not a 64-bit ELF";
        case elf_error::bad_endian:       return "not little-endian";
        case elf_error::bad_version:      return "bad ELF version";
        case elf_error::bad_machine:      return "not x86-64";
        case elf_error::bad_type:         return "not ET_EXEC or ET_DYN";
        case elf_error::no_load_segments: return "no PT_LOAD segments";
        case elf_error::no_memory:        return "out of memory";
        case elf_error::segment_overflow: return "segment exceeds file";
        default:                          return "unknown error";
    }
}

} // namespace elf
} // namespace vk
