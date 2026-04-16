/*
 * vkernel - UEFI Microkernel
 * Copyright (C) 2026 vkernel authors
 *
 * elf.h - ELF64 loader (freestanding, x86-64)
 *
 * Supports statically-linked ET_EXEC binaries and position-independent
 * ET_DYN executables with base-address relocation.
 */

#ifndef VKERNEL_ELF_H
#define VKERNEL_ELF_H

#include "types.h"

namespace vk {
namespace elf {

/* ============================================================
 * ELF64 header & program-header constants
 * ============================================================ */

/* ELF magic */
inline constexpr u8 ELFMAG0 = 0x7F;
inline constexpr u8 ELFMAG1 = 'E';
inline constexpr u8 ELFMAG2 = 'L';
inline constexpr u8 ELFMAG3 = 'F';

/* e_ident indices */
inline constexpr u32 EI_MAG0    = 0;
inline constexpr u32 EI_MAG1    = 1;
inline constexpr u32 EI_MAG2    = 2;
inline constexpr u32 EI_MAG3    = 3;
inline constexpr u32 EI_CLASS   = 4;   /* 2 = ELFCLASS64 */
inline constexpr u32 EI_DATA    = 5;   /* 1 = ELFDATA2LSB */
inline constexpr u32 EI_VERSION = 6;
inline constexpr u32 EI_NIDENT  = 16;

inline constexpr u8 ELFCLASS64  = 2;
inline constexpr u8 ELFDATA2LSB = 1;
inline constexpr u8 EV_CURRENT  = 1;

/* Object file types */
inline constexpr u16 ET_EXEC = 2;   /* Executable */
inline constexpr u16 ET_DYN  = 3;   /* Position-independent / shared */

/* Machine type */
inline constexpr u16 EM_X86_64 = 62;

/* Program header types */
inline constexpr u32 PT_NULL    = 0;
inline constexpr u32 PT_LOAD    = 1;
inline constexpr u32 PT_DYNAMIC = 2;
inline constexpr u32 PT_INTERP  = 3;

/* Program header flags */
inline constexpr u32 PF_X = 0x1;   /* Execute */
inline constexpr u32 PF_W = 0x2;   /* Write   */
inline constexpr u32 PF_R = 0x4;   /* Read    */

/* Dynamic section tags */
inline constexpr i64 DT_NULL    = 0;
inline constexpr i64 DT_RELA    = 7;
inline constexpr i64 DT_RELASZ  = 8;
inline constexpr i64 DT_RELAENT = 9;

/* Relocation type */
inline constexpr u32 R_X86_64_RELATIVE = 8;

/* ============================================================
 * ELF64 structures (packed to match the file format)
 * ============================================================ */

struct Elf64_Ehdr {
    u8  e_ident[EI_NIDENT];
    u16 e_type;
    u16 e_machine;
    u32 e_version;
    u64 e_entry;        /* Virtual entry point */
    u64 e_phoff;        /* Program header table file offset */
    u64 e_shoff;
    u32 e_flags;
    u16 e_ehsize;
    u16 e_phentsize;
    u16 e_phnum;
    u16 e_shentsize;
    u16 e_shnum;
    u16 e_shstrndx;
} __attribute__((packed));

struct Elf64_Phdr {
    u32 p_type;
    u32 p_flags;
    u64 p_offset;   /* Offset in file */
    u64 p_vaddr;    /* Virtual address in memory */
    u64 p_paddr;
    u64 p_filesz;   /* Bytes in file image */
    u64 p_memsz;    /* Bytes in memory (>= filesz; excess is zero-filled) */
    u64 p_align;
} __attribute__((packed));

struct Elf64_Dyn {
    i64 d_tag;
    u64 d_val;      /* Value or pointer (union — we use as u64 for both) */
} __attribute__((packed));

struct Elf64_Rela {
    u64 r_offset;   /* Address of the location to patch */
    u64 r_info;     /* Symbol index (high 32) + relocation type (low 32) */
    i64 r_addend;   /* Constant addend */
} __attribute__((packed));

/* Extract relocation type from r_info */
constexpr auto elf64_r_type(u64 info) -> u32 { return static_cast<u32>(info); }

/* ============================================================
 * Loader result
 * ============================================================ */

enum class elf_error {
    ok = 0,
    too_small,          /* File too short to be a valid ELF */
    bad_magic,          /* Not an ELF file */
    bad_class,          /* Not 64-bit */
    bad_endian,         /* Not little-endian */
    bad_version,        /* ELF version mismatch */
    bad_machine,        /* Not x86-64 */
    bad_type,           /* Not ET_EXEC or ET_DYN */
    no_load_segments,   /* No PT_LOAD segments found */
    no_memory,          /* Heap allocation failed */
    segment_overflow,   /* File offset + filesz exceeds file */
};

struct load_result {
    elf_error   error;
    u64         entry;      /* Resolved virtual entry point */
    u8*         image_base; /* Pointer to allocated image (heap) */
    u64         image_size; /* Total size of allocated region */
};

/* ============================================================
 * Public API
 * ============================================================ */

/*
 * Load an ELF64 binary from a raw byte buffer into the kernel heap.
 *
 * For ET_EXEC: virtual addresses are used as-is (must be in the
 *              kernel-accessible range; no paging setup needed for
 *              identity-mapped or direct-mapped kernels).
 * For ET_DYN:  segments are placed at a heap-allocated base and the
 *              entry point is adjusted by the load bias.
 *
 * On success, result.error == elf_error::ok and result.entry contains
 * the address to jump to.
 */
auto load(const u8* file_data, usize file_size) -> load_result;

/* Human-readable error string */
auto error_string(elf_error err) -> const char*;

} // namespace elf
} // namespace vk

#endif /* VKERNEL_ELF_H */
