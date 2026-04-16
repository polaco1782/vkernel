/*
 * vkernel - UEFI Microkernel
 * Copyright (C) 2026 vkernel authors
 *
 * process.h - ELF process loader and execution
 *
 * Owns the kernel API table (vk_api_t), loads an ELF binary from
 * ramfs, wires the API stubs, and invokes the entry point.
 */

#ifndef VKERNEL_PROCESS_H
#define VKERNEL_PROCESS_H

#include "types.h"
#include "userapi.h"

namespace vk {
namespace process {

/*
 * Load the named file from ramfs as an ELF64 binary, populate the
 * kernel API table, and call its entry point.
 *
 * Prints progress and errors to the console.
 * Returns the spawned task ID, or -1 on load error.
 */
auto run(const char* filename) -> i64;

/*
 * Return a pointer to the global kernel API table.
 * Valid after the first call to run(), or after init().
 */
auto get_api() -> const vk_api_t*;

/*
 * Initialise the kernel API table without running anything.
 * Called once during kernel startup so the table is always ready.
 */
void init();

} // namespace process
} // namespace vk

#endif /* VKERNEL_PROCESS_H */
