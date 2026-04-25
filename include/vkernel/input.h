/*
 * vkernel - UEFI Microkernel
 * Copyright (C) 2026 vkernel authors
 *
 * input.h - Kernel input subsystem
 *
 * Provides a unified character input interface backed by:
 *   - PS/2 keyboard (scan code set 1, 8042 controller)
 *   - COM1 serial port (0x3F8)
 *
 * Any task (shell, debugger, …) calls input::getc() to read a
 * character without knowing which physical device produced it.
 */

#ifndef VKERNEL_INPUT_H
#define VKERNEL_INPUT_H

#include "types.h"
#include "vk.h"

namespace vk {
namespace input {

/*
 * Initialize the input subsystem.
 * Must be called after ExitBootServices (uses direct I/O port access).
 * Initialises the PS/2 controller and COM1 serial receiver.
 */
auto init() -> status_code;

/*
 * Block until a printable/control character is available from any
 * input source, yielding to the scheduler while waiting.
 * Returns the ASCII character.
 */
auto getc() -> char;

/*
 * Non-blocking poll.  Returns '\0' if no character is available.
 */
auto try_getc() -> char;

/* Backend-specific helpers used by per-process console routing. */
auto getc_ps2() -> char;
auto try_getc_ps2() -> char;
auto getc_serial() -> char;
auto try_getc_serial() -> char;

/*
 * Non-blocking raw key event poll.
 * Returns true and fills ev if a PS/2 scancode is available.
 */
auto poll_key(vk_key_event_t& ev) -> bool;

/*
 * Initialise the PS/2 auxiliary (mouse) port.
 * Safe to call multiple times; initialises only once.
 */
auto mouse_init() -> status_code;

/*
 * Non-blocking PS/2 mouse event poll.
 * Returns true and fills ev if a complete 3-byte PS/2 packet is ready.
 * dy is normalised so that positive = down (screen coordinates).
 */
auto poll_mouse(vk_mouse_event_t& ev) -> bool;

} // namespace input
} // namespace vk

#endif /* VKERNEL_INPUT_H */
