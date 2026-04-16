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
VK_NORETURN void vk_panic(const char* file, u32 line, const char* condition) {
    console::set_color(console_color::white, console_color::red);
    console::puts("\n*** KERNEL PANIC ***\n");
    console::puts("File: ");
    console::puts(file);
    console::puts("\nLine: ");
    
    /* Simple number conversion for line */
    char line_buf[16];
    i32 tmp = static_cast<i32>(line);
    i32 i = 0;
    if (tmp == 0) {
        line_buf[i++] = '0';
    } else {
        char num_buf[16];
        i32 j = 0;
        while (tmp > 0 && j < 15) {
            num_buf[j++] = static_cast<char>('0' + (tmp % 10));
            tmp /= 10;
        }
        while (j > 0) {
            line_buf[i++] = num_buf[--j];
        }
    }
    line_buf[i] = '\0';
    console::puts(line_buf);
    
    console::puts("\nCondition: ");
    console::puts(condition);
    console::puts("\n\nSystem halted.\n");
    console::set_color(console_color::white, console_color::black);
    
    arch::disable_interrupts();
    while (true) {
        arch::cpu_halt();
    }
}

} // namespace vk
