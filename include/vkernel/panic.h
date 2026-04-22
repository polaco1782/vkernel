/*
 * vkernel - UEFI Microkernel
 * Copyright (C) 2026 vkernel authors
 *
 * panic.h - Kernel panic handler
 */

#ifndef VKERNEL_PANIC_H
#define VKERNEL_PANIC_H

#include "types.h"

namespace vk {

/* Kernel panic handler — displays error info and halts the system */
VK_NORETURN void vk_panic(const char* file, u32 line, const char* condition);

} // namespace vk

#endif /* VKERNEL_PANIC_H */
