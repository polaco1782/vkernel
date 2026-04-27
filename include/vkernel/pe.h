/*
 * vkernel - UEFI Microkernel
 * Copyright (C) 2026 vkernel authors
 *
 * pe.h - PE32+ (PE64) loader (freestanding, x86-64)
 *
 * Loads freestanding PE32+ executables from an in-memory buffer into
 * the kernel heap, applies DIR64 base relocations, and returns the
 * resolved entry-point address for the caller to invoke.
 *
 * Calling convention note:
 *   PE32+ programs compiled with MSVC use the Microsoft x64 ABI.
 *   The kernel (also MSVC) calls the entry point with the first
 *   argument in RCX — an exact match with no ABI bridging needed.
 */

#ifndef VKERNEL_PE_H
#define VKERNEL_PE_H

#include "types.h"

namespace vk {
namespace pe {

/* ============================================================
 * PE magic and machine constants
 * ============================================================ */

inline constexpr u16 IMAGE_DOS_SIGNATURE        = 0x5A4D;    /* MZ      */
inline constexpr u32 IMAGE_NT_SIGNATURE         = 0x00004550; /* PE\0\0 */
inline constexpr u16 IMAGE_FILE_MACHINE_AMD64   = 0x8664;
inline constexpr u16 IMAGE_OPTIONAL_HDR64_MAGIC = 0x020B;    /* PE32+   */

inline constexpr u32 IMAGE_DIRECTORY_ENTRY_BASERELOC = 5;
inline constexpr u32 IMAGE_NUMBEROF_DIRECTORY_ENTRIES = 16;

inline constexpr u16 IMAGE_REL_BASED_ABSOLUTE = 0;   /* skip (padding) */
inline constexpr u16 IMAGE_REL_BASED_DIR64    = 10;  /* 64-bit absolute */

/* ============================================================
 * PE64 on-disk structures
 * ============================================================ */

#pragma pack(push, 1)

struct IMAGE_DOS_HEADER {
    u16 e_magic;
    u16 e_cblp, e_cp, e_crlc, e_cparhdr;
    u16 e_minalloc, e_maxalloc;
    u16 e_ss, e_sp, e_csum, e_ip, e_cs;
    u16 e_lfarlc, e_ovno;
    u16 e_res[4];
    u16 e_oemid, e_oeminfo;
    u16 e_res2[10];
    i32 e_lfanew;   /* Byte offset to IMAGE_NT_HEADERS64 */
};

struct IMAGE_FILE_HEADER {
    u16 Machine;
    u16 NumberOfSections;
    u32 TimeDateStamp;
    u32 PointerToSymbolTable;
    u32 NumberOfSymbols;
    u16 SizeOfOptionalHeader;
    u16 Characteristics;
};

struct IMAGE_DATA_DIRECTORY {
    u32 VirtualAddress;
    u32 Size;
};

struct IMAGE_OPTIONAL_HEADER64 {
    u16 Magic;
    u8  MajorLinkerVersion;
    u8  MinorLinkerVersion;
    u32 SizeOfCode;
    u32 SizeOfInitializedData;
    u32 SizeOfUninitializedData;
    u32 AddressOfEntryPoint;    /* RVA of entry point */
    u32 BaseOfCode;
    u64 ImageBase;
    u32 SectionAlignment;
    u32 FileAlignment;
    u16 MajorOperatingSystemVersion;
    u16 MinorOperatingSystemVersion;
    u16 MajorImageVersion;
    u16 MinorImageVersion;
    u16 MajorSubsystemVersion;
    u16 MinorSubsystemVersion;
    u32 Win32VersionValue;
    u32 SizeOfImage;
    u32 SizeOfHeaders;
    u32 CheckSum;
    u16 Subsystem;
    u16 DllCharacteristics;
    u64 SizeOfStackReserve;
    u64 SizeOfStackCommit;
    u64 SizeOfHeapReserve;
    u64 SizeOfHeapCommit;
    u32 LoaderFlags;
    u32 NumberOfRvaAndSizes;
    IMAGE_DATA_DIRECTORY DataDirectory[IMAGE_NUMBEROF_DIRECTORY_ENTRIES];
};

struct IMAGE_NT_HEADERS64 {
    u32                    Signature;
    IMAGE_FILE_HEADER      FileHeader;
    IMAGE_OPTIONAL_HEADER64 OptionalHeader;
};

struct IMAGE_SECTION_HEADER {
    u8  Name[8];
    u32 VirtualSize;         /* Bytes used in memory */
    u32 VirtualAddress;      /* RVA of the section   */
    u32 SizeOfRawData;       /* On-disk size (aligned to FileAlignment) */
    u32 PointerToRawData;    /* File offset of raw data */
    u32 PointerToRelocations;
    u32 PointerToLinenumbers;
    u16 NumberOfRelocations;
    u16 NumberOfLinenumbers;
    u32 Characteristics;
};

struct IMAGE_BASE_RELOCATION {
    u32 VirtualAddress; /* RVA of the 4 KB page this block covers */
    u32 SizeOfBlock;    /* Total byte size of this block (header + entries) */
    /* u16 TypeOffset[] entries follow inline */
};

#pragma pack(pop)

/* ============================================================
 * Loader result
 * ============================================================ */

enum class pe_error {
    ok = 0,
    too_small,      /* File too short to contain valid PE headers       */
    bad_magic,      /* DOS or PE signature mismatch                     */
    bad_machine,    /* Not AMD64                                         */
    bad_type,       /* Not PE32+ (64-bit optional header)               */
    no_memory,      /* Kernel heap allocation failed                     */
    bad_reloc,      /* Relocation block or section RVA out of range     */
};

struct load_result {
    pe_error error;
    u64      entry;       /* Heap address of entry point                */
    u8*      image_base;  /* Pointer to allocated, mapped image buffer  */
    u64      image_size;  /* SizeOfImage (total allocation size)        */
};

/* ============================================================
 * Public API
 * ============================================================ */

/*
 * Load a PE32+ binary from a raw byte buffer into the kernel heap.
 *
 * Steps performed:
 *  1. Validate DOS / NT / optional headers
 *  2. Allocate SizeOfImage zeroed bytes
 *  3. Copy SizeOfHeaders (mapped image headers)
 *  4. Copy each section from its PointerToRawData to its VirtualAddress
 *  5. Apply IMAGE_REL_BASED_DIR64 base relocations
 *  6. Return entry = alloc_base + AddressOfEntryPoint
 *
 * On success, result.error == pe_error::ok.
 * The caller is responsible for freeing result.image_base when done.
 */
auto load(const u8* file_data, usize file_size) -> load_result;

/* Human-readable error string */
auto error_string(pe_error err) -> const char*;

} // namespace pe
} // namespace vk

#endif /* VKERNEL_PE_H */
