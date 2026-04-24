/*
 * vkernel - UEFI Microkernel
 * Copyright (C) 2026 vkernel authors
 *
 * panic.cpp - Kernel panic handler
 */

#include "config.h"
#include "types.h"
#include "console.h"
#include "arch/x86_64/arch.h"

namespace vk {

/* Panic handler */
[[noreturn]] void vk_panic(const char* file, u32 line, const char* condition) {
    log::crash("\n*** KERNEL PANIC ***\nFile: %s\nLine: %u\nCondition: %s\n\nSystem halted.\n",
               file, line, condition);

    arch::disable_interrupts();
    while (true) {
        arch::cpu_halt();
    }
}

} // namespace vk
