/*
 * vkernel - UEFI Microkernel
 * Copyright (C) 2026 vkernel authors
 *
 * fs.h - Virtual filesystem + in-memory ramfs (freestanding C++26)
 */

#ifndef VKERNEL_FS_H
#define VKERNEL_FS_H

#include "types.h"
#include "memory.h"

namespace vk {

/* ============================================================
 * File entry — an in-memory blob loaded from the ESP
 * ============================================================ */

struct file_entry {
    char     name[128];     /* Null-terminated ASCII path (e.g. "shell.bin") */
    u8*      data;          /* Pointer into kernel heap / static buffer */
    usize    size;          /* Byte count */
    bool     valid;
};

/* ============================================================
 * Ramfs — flat array of file_entry (no directories)
 * Files are loaded from the ESP before ExitBootServices and
 * remain accessible forever in kernel memory.
 * ============================================================ */

inline constexpr usize RAMFS_MAX_FILES = 32;

namespace ramfs {

auto init() -> status_code;

/* Register a file blob (called during ESP load phase) */
auto add_file(const char* name, const u8* data, usize size) -> status_code;

/* Look up a file by name */
auto find(const char* name) -> const file_entry*;

/* Iterate */
auto file_count() -> usize;
auto get_file(usize index) -> const file_entry*;

void dump();

} // namespace ramfs

/* ============================================================
 * UEFI ESP loader — uses Simple File System Protocol
 * Must be called BEFORE ExitBootServices.
 * ============================================================ */

namespace loader {

/* Load a single file from the ESP into kernel memory.
 * Returns null on failure; caller owns the buffer. */
struct loaded_file {
    u8*   data;
    usize size;
};

auto load_file_from_esp(const char* path) -> loaded_file;

/* Load a list of well-known files into the ramfs */
auto load_initrd() -> status_code;

} // namespace loader

} // namespace vk

#endif /* VKERNEL_FS_H */
