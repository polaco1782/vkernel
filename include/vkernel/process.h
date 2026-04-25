/*
 * vkernel - UEFI Microkernel
 * Copyright (C) 2026 vkernel authors
 *
 * process.h - ELF process loader and execution
 *
 * Loads an ELF or PE binary from ramfs and invokes its entry point.
 */

#ifndef VKERNEL_PROCESS_H
#define VKERNEL_PROCESS_H

#include "types.h"

namespace vk {
namespace process {

enum class console_interface : u8 {
	graphical = 0,
	serial = 1,
};

/*
 * Load the named file from ramfs as an ELF64 binary, populate the
 * kernel API table, and call its entry point using the selected
 * console interface routing.
 *
 * Prints progress and errors to the console.
 * Returns the spawned task ID, or -1 on load error.
 */
auto run(const char* filename) -> i64;
auto run(const char* filename, console_interface interface) -> i64;

} // namespace process
} // namespace vk

#endif /* VKERNEL_PROCESS_H */
