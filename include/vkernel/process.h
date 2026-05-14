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
#include "vk.h"

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
auto run(string_view filename) -> i64;
auto run(const char* filename) -> i64;
auto run(string_view filename, console_interface interface) -> i64;
auto run(const char* filename, console_interface interface) -> i64;
auto run(string_view filename, console_interface interface, const vk_framebuffer_info_t* fb_override) -> i64;
auto run(const char* filename, console_interface interface, const vk_framebuffer_info_t* fb_override) -> i64;

/*
 * Launch a binary from a command line string.
 * The first token is treated as the binary path; the full command line is
 * recorded for userspace argv parsing.
 */
auto run_command_line(string_view command_line) -> i64;
auto run_command_line(const char* command_line) -> i64;
auto run_command_line(string_view command_line, console_interface interface) -> i64;
auto run_command_line(const char* command_line, console_interface interface) -> i64;
auto run_command_line(string_view command_line, console_interface interface, const vk_framebuffer_info_t* fb_override) -> i64;
auto run_command_line(const char* command_line, console_interface interface, const vk_framebuffer_info_t* fb_override) -> i64;

/*
 * Replace the current task's process image with a new binary.
 * Returns -1 on load/setup error. On success this does not return.
 */
auto exec(string_view filename) -> int;
auto exec(const char* filename) -> int;
auto exec(string_view filename, console_interface interface) -> int;
auto exec(const char* filename, console_interface interface) -> int;
auto exec(string_view filename, console_interface interface, const vk_framebuffer_info_t* fb_override) -> int;
auto exec(const char* filename, console_interface interface, const vk_framebuffer_info_t* fb_override) -> int;

auto exec_command_line(string_view command_line) -> int;
auto exec_command_line(const char* command_line) -> int;
auto exec_command_line(string_view command_line, console_interface interface) -> int;
auto exec_command_line(const char* command_line, console_interface interface) -> int;
auto exec_command_line(string_view command_line, console_interface interface, const vk_framebuffer_info_t* fb_override) -> int;
auto exec_command_line(const char* command_line, console_interface interface, const vk_framebuffer_info_t* fb_override) -> int;

} // namespace process
} // namespace vk

#endif /* VKERNEL_PROCESS_H */
