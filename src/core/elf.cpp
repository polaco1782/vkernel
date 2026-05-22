/*
 * vkernel - UEFI Microkernel
 * Copyright (C) 2026 vkernel authors
 *
 * elf.cpp - ELF64 loader implementation
 */

#include "config.h"
#include "types.h"
#include "memory.h"
#include "console.h"
#include "log.h"
#include "elf.h"
#include "resource_ptr.h"
#include "virtual_memory.h"

namespace vk {
namespace elf {

/* ============================================================
 * Internal helpers
 * ============================================================ */

/* align must be a power of two. */
static constexpr auto align_up(u64 value, u64 align) -> u64 {
    if (align == 0) return value;
    return (value + align - 1) & ~(align - 1);
}

static bool range_ok(usize file_size, u64 offset, u64 len) {
    if (offset > file_size) return false;
    if (len > file_size - offset) return false;
    return true;
}

/* ============================================================
 * load()
 * ============================================================ */

static auto load_impl(const u8* file_data,
                      usize file_size,
                      vm::address_space* as,
                      virt_addr preferred_base) -> load_result {
    load_result result{};
    result.error          = elf_error::ok;
    result.entry          = 0;
    result.image_base     = null;
    result.image_size     = 0;
    result.image_from_phys = false;
    result.image_phys     = 0;
    result.image_vm_mapped = false;

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
    u64 max_align  = 4096ULL;
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
        if (ph.p_align > max_align) max_align = ph.p_align;

        ++load_count;
    }

    if (load_count == 0) {
        result.error = elf_error::no_load_segments;
        return result;
    }

    /* ---- 7. Allocate contiguous image buffer ---- */
    /* ET_DYN uses a normal load bias; ET_EXEC is staged the same way here. */
    u64 image_size = align_up(vaddr_max - vaddr_min, 4096ULL);
    kernel_allocation_ptr<u8> image_owner;
    u8* image_runtime_base = null;
    u8* image_write_base = null;
    phys_addr image_phys = 0;
    bool image_vm_mapped = false;

    const virt_addr vm_image_base = align_up(preferred_base, max_align);
    if (as != null) {
        if (!vm::allocate_user_pages(as,
                                     vm_image_base,
                                     static_cast<usize>(image_size),
                                     vm::MAP_WRITABLE | vm::MAP_USER | vm::MAP_EXECUTABLE,
                                     &image_phys)) {
            log::error() << "elf: virtual image allocation failed";
            result.error = elf_error::no_memory;
            return result;
        }

        image_runtime_base = reinterpret_cast<u8*>(static_cast<usize>(vm_image_base));
        image_write_base = reinterpret_cast<u8*>(static_cast<usize>(image_phys));
        image_vm_mapped = true;
    } else {
        image_owner = kernel_allocation_ptr<u8>(
            static_cast<u8*>(g_kernel_heap.allocate_zero_aligned(image_size, max_align)),
            kernel_allocation_deleter {
                .size = static_cast<usize>(image_size),
                .from_phys = false,
            });
        image_runtime_base = image_owner.get();
        image_write_base = image_owner.get();
    }

    log::debug() << "elf: vaddr range " << log::hex(static_cast<u64>(static_cast<unsigned long long>(vaddr_min)), 1, true, false) << " - " << log::hex(static_cast<u64>(static_cast<unsigned long long>(vaddr_max)), 1, true, false) << " (size " << static_cast<unsigned long long>(image_size) << " bytes)";

    if (image_write_base == null) {
        if (as != null) {
            result.error = elf_error::no_memory;
            return result;
        }

        /* Fall back to page allocation when the heap cannot fit the image. */
        log::warn() << "elf: heap allocation failed, trying physical allocator";
        u32 page_count = static_cast<u32>(
            (image_size + PAGE_SIZE_4K - 1) / PAGE_SIZE_4K);

        // phys_addr phys = g_phys_alloc.allocate_pages(
        //     page_count, static_cast<u32>(PAGE_SIZE_4K), 0);

        phys_addr phys = g_phys_alloc.allocate_pages(
            page_count, static_cast<u32>(max(static_cast<u32>(PAGE_SIZE_4K), static_cast<u32>(max_align))),
            0);

        if (phys == 0) {
            log::error() << "elf: physical allocation failed";
            result.error = elf_error::no_memory;
            return result;
        }
        image_owner.reset(reinterpret_cast<u8*>(phys));
        image_owner = kernel_allocation_ptr<u8>(
            image_owner.release(),
            kernel_allocation_deleter {
                .size = static_cast<usize>(image_size),
                .from_phys = true,
            });
        memory::set(image_owner.get(), 0, image_size);
        image_runtime_base = image_owner.get();
        image_write_base = image_owner.get();
        result.image_from_phys = true;
    }

    /* VM-backed loads write through a physical alias but relocate to virtual VAs. */
    i64 load_bias = static_cast<i64>(
        reinterpret_cast<u64>(image_runtime_base)) - static_cast<i64>(vaddr_min);
    i64 write_bias = static_cast<i64>(
        reinterpret_cast<u64>(image_write_base)) - static_cast<i64>(vaddr_min);

    /* ---- 8. Copy PT_LOAD segments ---- */
    for (u16 i = 0; i < ehdr->e_phnum; ++i) {
        const auto& ph = phdrs[i];
        if (ph.p_type != PT_LOAD) continue;

        u8* dest = reinterpret_cast<u8*>(
            static_cast<i64>(ph.p_vaddr) + write_bias);

        /* Copy file image */
        if (ph.p_filesz > 0) {
            memory::copy(dest, file_data + ph.p_offset, ph.p_filesz);
        }
        /* Be explicit about BSS even when the allocation already started zeroed. */
        if (ph.p_memsz > ph.p_filesz) {
            memory::set(dest + ph.p_filesz, 0,
                               ph.p_memsz - ph.p_filesz);
        }
    }

    /* ---- 9. Process dynamic relocations (ET_DYN / PIE only) ---- */
    /* Apply R_X86_64_RELATIVE relocations from PT_DYNAMIC when present. */
    if (ehdr->e_type == ET_DYN) {
        u64 rela_vaddr = 0;
        u64 rela_size  = 0;

        for (u16 i = 0; i < ehdr->e_phnum; ++i) {
            const auto& ph = phdrs[i];
            if (ph.p_type != PT_DYNAMIC) continue;

            const auto* dyn = reinterpret_cast<const Elf64_Dyn*>(
                reinterpret_cast<u8*>(static_cast<i64>(ph.p_vaddr) + write_bias));

            for (; dyn->d_tag != DT_NULL; ++dyn) {
                if      (dyn->d_tag == DT_RELA)   rela_vaddr = dyn->d_val;
                else if (dyn->d_tag == DT_RELASZ)  rela_size  = dyn->d_val;
            }
            break;
        }

        if (rela_vaddr != 0 && rela_size >= sizeof(Elf64_Rela)) {
            const auto* rela = reinterpret_cast<const Elf64_Rela*>(
                reinterpret_cast<u8*>(static_cast<i64>(rela_vaddr) + write_bias));
            const usize count = rela_size / sizeof(Elf64_Rela);

            for (usize i = 0; i < count; ++i) {
                if (elf64_r_type(rela[i].r_info) != R_X86_64_RELATIVE) continue;
                auto* loc = reinterpret_cast<u64*>(
                    static_cast<i64>(rela[i].r_offset) + write_bias);
                *loc = static_cast<u64>(load_bias + rela[i].r_addend);
            }
        }
    }

    /* ---- 10. Resolve entry point ---- */
    result.entry = static_cast<u64>(
        static_cast<i64>(ehdr->e_entry) + load_bias);
    result.image_base = image_vm_mapped ? image_runtime_base : image_owner.release();
    result.image_size = image_size;
    result.image_phys = image_phys;
    result.image_vm_mapped = image_vm_mapped;
    result.error      = elf_error::ok;

    log::debug() << "elf: loaded image_base=" << reinterpret_cast<const void*>(result.image_base)
                 << " image_phys=" << reinterpret_cast<const void*>(static_cast<usize>(result.image_phys))
                 << " vm_mapped=" << (result.image_vm_mapped ? "yes" : "no")
                 << " size=" << static_cast<unsigned long long>(image_size)
                 << " entry=" << log::hex(static_cast<u64>(static_cast<unsigned long long>(result.entry)), 1, true, false);

    return result;
}

auto load(const u8* file_data, usize file_size) -> load_result {
    return load_impl(file_data, file_size, null, 0);
}

auto load_into_address_space(const u8* file_data,
                             usize file_size,
                             vm::address_space* as,
                             virt_addr preferred_base) -> load_result {
    return load_impl(file_data, file_size, as, preferred_base);
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
