/*
 * vkernel - UEFI Microkernel
 * Copyright (C) 2026 vkernel authors
 *
 * pe.cpp - PE32+ (PE64) loader implementation
 *
 * Loads a freestanding MSVC-compiled PE32+ executable from a raw
 * in-memory buffer.  Only IMAGE_REL_BASED_DIR64 relocations are
 * processed; imports, TLS, and exception tables are not supported
 * (freestanding binaries compiled with /GS- /nodefaultlib /EHs-c-
 * do not generate them).
 */

#include "config.h"
#include "types.h"
#include "memory.h"
#include "console.h"
#include "pe.h"

namespace vk {
namespace pe {

/* Safe bounds check: verify [offset, offset+len) lies within [0, size) */
static bool range_ok(u64 size, u64 offset, u64 len) {
    if (offset > size) return false;
    if (len > size - offset) return false;
    return true;
}

auto load(const u8* file_data, usize file_size) -> load_result {
    load_result result{};
    result.error = pe_error::ok;

    /* ---- 1. DOS header ---- */
    if (file_size < sizeof(IMAGE_DOS_HEADER)) {
        result.error = pe_error::too_small;
        return result;
    }

    const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(file_data);
    if (dos->e_magic != IMAGE_DOS_SIGNATURE) {
        result.error = pe_error::bad_magic;
        return result;
    }

    /* ---- 2. NT headers ---- */
    if (dos->e_lfanew < 0 ||
        !range_ok(file_size,
                  static_cast<u64>(dos->e_lfanew),
                  sizeof(IMAGE_NT_HEADERS64))) {
        result.error = pe_error::too_small;
        return result;
    }

    const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS64*>(
        file_data + dos->e_lfanew);

    if (nt->Signature != IMAGE_NT_SIGNATURE) {
        result.error = pe_error::bad_magic;
        return result;
    }
    if (nt->FileHeader.Machine != IMAGE_FILE_MACHINE_AMD64) {
        result.error = pe_error::bad_machine;
        return result;
    }
    if (nt->OptionalHeader.Magic != IMAGE_OPTIONAL_HDR64_MAGIC) {
        result.error = pe_error::bad_type;
        return result;
    }

    const u32 size_of_image   = nt->OptionalHeader.SizeOfImage;
    const u32 size_of_headers = nt->OptionalHeader.SizeOfHeaders;
    const u64 image_base_link = nt->OptionalHeader.ImageBase;
    const u32 entry_rva       = nt->OptionalHeader.AddressOfEntryPoint;
    const u16 num_sections    = nt->FileHeader.NumberOfSections;

    if (!range_ok(file_size, 0, size_of_headers) ||
        size_of_headers > size_of_image) {
        result.error = pe_error::too_small;
        return result;
    }

    /* Validate section table fits in the file */
    const usize sections_offset =
        static_cast<usize>(dos->e_lfanew)
        + offsetof(IMAGE_NT_HEADERS64, OptionalHeader)
        + nt->FileHeader.SizeOfOptionalHeader;

    if (!range_ok(file_size, sections_offset,
                  static_cast<u64>(num_sections) *
                  sizeof(IMAGE_SECTION_HEADER))) {
        result.error = pe_error::too_small;
        return result;
    }

    /* ---- 3. Allocate zeroed image buffer ---- */
    auto* image = static_cast<u8*>(g_kernel_heap.allocate_zero(size_of_image));
    if (!image) {
        result.error = pe_error::no_memory;
        return result;
    }

    /* ---- 4. Copy mapped headers ---- */
    memory::memory_copy(image, file_data, size_of_headers);

    /* ---- 5. Copy sections ---- */
    const auto* sections = reinterpret_cast<const IMAGE_SECTION_HEADER*>(
        file_data + sections_offset);

    for (u16 i = 0; i < num_sections; ++i) {
        const auto& sec = sections[i];

        /* Skip sections with no on-disk data (e.g. .bss — already zeroed) */
        if (sec.SizeOfRawData == 0) continue;

        if (sec.VirtualAddress + sec.SizeOfRawData > size_of_image) {
            g_kernel_heap.free(image);
            result.error = pe_error::bad_reloc;
            return result;
        }
        if (!range_ok(file_size, sec.PointerToRawData, sec.SizeOfRawData)) {
            g_kernel_heap.free(image);
            result.error = pe_error::too_small;
            return result;
        }

        memory::memory_copy(image + sec.VirtualAddress,
                            file_data + sec.PointerToRawData,
                            sec.SizeOfRawData);
    }

    /* ---- 6. Apply base relocations ---- */
    const auto& reloc_dir =
        nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_BASERELOC];

    if (reloc_dir.Size > 0 && reloc_dir.VirtualAddress != 0) {
        const i64 delta =
            static_cast<i64>(reinterpret_cast<u64>(image)) -
            static_cast<i64>(image_base_link);

        if (delta != 0) {
            if (!range_ok(size_of_image,
                          reloc_dir.VirtualAddress, reloc_dir.Size)) {
                g_kernel_heap.free(image);
                result.error = pe_error::bad_reloc;
                return result;
            }

            const u8* blk     = image + reloc_dir.VirtualAddress;
            const u8* blk_end = blk + reloc_dir.Size;

            while (blk + sizeof(IMAGE_BASE_RELOCATION) <= blk_end) {
                const auto* hdr =
                    reinterpret_cast<const IMAGE_BASE_RELOCATION*>(blk);

                if (hdr->SizeOfBlock < sizeof(IMAGE_BASE_RELOCATION))
                    break;   /* Malformed — stop processing */

                const u32  nent =
                    (hdr->SizeOfBlock - sizeof(IMAGE_BASE_RELOCATION)) /
                    sizeof(u16);
                const u16* ent  =
                    reinterpret_cast<const u16*>(hdr + 1);

                for (u32 j = 0; j < nent; ++j) {
                    const u16 type   = ent[j] >> 12;
                    const u16 offset = ent[j] & 0x0FFFu;

                    if (type == IMAGE_REL_BASED_ABSOLUTE)
                        continue;   /* Padding entry */

                    if (type == IMAGE_REL_BASED_DIR64) {
                        const u32 rva = hdr->VirtualAddress + offset;
                        if (rva + sizeof(u64) > size_of_image) {
                            g_kernel_heap.free(image);
                            result.error = pe_error::bad_reloc;
                            return result;
                        }
                        auto* ptr = reinterpret_cast<u64*>(image + rva);
                        *ptr = static_cast<u64>(
                            static_cast<i64>(*ptr) + delta);
                    }
                }

                blk += hdr->SizeOfBlock;
            }
        }
    }

#if VK_DEBUG_LEVEL >= 3
    console::puts("[pe] loaded: base=0x");
    console::put_hex(reinterpret_cast<u64>(image));
    console::puts(" size=");
    console::put_dec(size_of_image);
    console::puts(" entry=0x");
    console::put_hex(reinterpret_cast<u64>(image) + entry_rva);
    console::puts("\n");
#endif

    result.entry      = reinterpret_cast<u64>(image) + entry_rva;
    result.image_base = image;
    result.image_size = size_of_image;
    return result;
}

auto error_string(pe_error err) -> const char* {
    switch (err) {
        case pe_error::ok:          return "ok";
        case pe_error::too_small:   return "file too small";
        case pe_error::bad_magic:   return "bad DOS/PE signature";
        case pe_error::bad_machine: return "not AMD64";
        case pe_error::bad_type:    return "not PE32+";
        case pe_error::no_memory:   return "out of memory";
        case pe_error::bad_reloc:   return "relocation out of range";
        default:                    return "unknown error";
    }
}

} // namespace pe
} // namespace vk
