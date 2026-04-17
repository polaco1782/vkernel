/*
 * vkernel - UEFI Microkernel
 * Copyright (C) 2026 vkernel authors
 *
 * shell.cpp - Built-in kernel shell
 *
 * Reads characters from the COM1 serial port and executes
 * simple built-in commands. Runs as a scheduler task.
 */

#include "config.h"
#include "types.h"
#include "console.h"
#include "memory.h"
#include "fs.h"
#include "scheduler.h"
#include "input.h"
#include "shell.h"
#include "panic.h"
#include "arch/x86_64/arch.h"
#include "elf.h"
#include "vk.h"
#include "process.h"

namespace vk {
namespace shell {

/* ============================================================
 * String helpers
 * ============================================================ */

static bool str_equal(const char* a, const char* b) {
    while (*a && *b) {
        if (*a != *b) return false;
        ++a; ++b;
    }
    return *a == *b;
}

static bool str_starts_with(const char* str, const char* prefix) {
    while (*prefix) {
        if (*str != *prefix) return false;
        ++str; ++prefix;
    }
    return true;
}

/* ============================================================
 * Built-in commands
 * ============================================================ */

static void cmd_help() {
    console::puts("Available commands:\n");
    console::puts("  help    - Show this message\n");
    console::puts("  version - Show kernel version\n");
    console::puts("  mem     - Show memory info\n");
    console::puts("  tasks   - Show running tasks\n");
    console::puts("  ls      - List files in ramfs\n");
    console::puts("  cat <f> - Display file contents\n");
    console::puts("  clear   - Clear screen\n");
    console::puts("  uptime  - Show tick count\n");
    console::puts("  idt     - Dump interrupt descriptor table\n");
    console::puts("  run <f> - Load and execute an ELF64 binary from ramfs\n");
}

static void cmd_version() {
    console::puts("vkernel ");
    console::puts(config::version_string);
    console::puts(" (");
    console::puts(config::arch_name);
    console::puts(", ");
    console::puts(config::compiler_name);
    console::puts(", ");
    console::puts(config::build_type);
    console::puts(")\n");
}

static void cmd_mem() {
    console::puts("Physical allocator:\n");
    console::puts("  Total pages: ");
    console::put_dec(g_phys_alloc.total_pages());
    console::puts("\n  Free pages:  ");
    console::put_dec(g_phys_alloc.free_pages());
    console::puts("\n  Used pages:  ");
    console::put_dec(g_phys_alloc.used_pages());
    console::puts("\n  Total RAM:   ");
    console::put_dec((g_phys_alloc.total_pages() * PAGE_SIZE_4K) / (1024 * 1024));
    console::puts(" MB\n");
    console::puts("\n");
    
    memory::dump_heap();
}

static void cmd_ls() {
    ramfs::dump();
}

static void cmd_cat(const char* arg) {
    /* Skip leading spaces */
    while (*arg == ' ') ++arg;
    if (*arg == '\0') {
        console::puts("Usage: cat <filename>\n");
        return;
    }
    auto* f = ramfs::find(arg);
    if (f == null) {
        console::puts("File not found: ");
        console::puts(arg);
        console::puts("\n");
        return;
    }
    /* Print file contents (assuming text) */
    for (usize i = 0; i < f->size; ++i) {
        console::putc(static_cast<char>(f->data[i]));
    }
    console::puts("\n");
}

/* Simple volatile tick counter (incremented by preempt calls) */
static volatile u64 s_tick_count = 0;

static void cmd_uptime() {
    console::puts("Uptime: ~");
    console::put_dec(s_tick_count / 100);
    console::puts(" seconds (");
    console::put_dec(s_tick_count);
    console::puts(" ticks)\n");
}

static void cmd_run(const char* arg) {
    /* Skip leading spaces */
    while (*arg == ' ') ++arg;
    if (*arg == '\0') {
        console::puts("Usage: run <filename>\n");
        return;
    }
    i64 task_id = process::run(arg);
    if (task_id >= 0) {
        console::puts("run: spawned task ");
        console::put_dec(static_cast<u64>(task_id));
        console::puts("\n");
    }
}

/* ============================================================
 * Shell main loop
 * ============================================================ */

static constexpr usize CMD_BUF_SIZE = 256;

void shell_main() {
    console::puts("\n");
    console::puts("  _  __                    _   ____  _          _ _ \n");
    console::puts(" | |/ /___ _ __ _ __   _  | | / ___|| |__   ___| | |\n");
    console::puts(" | ' // _ \\ '__| '_ \\ / _ \\ | \\___ \\| '_ \\ / _ \\ | |\n");
    console::puts(" | . \\  __/ |  | | | |  __/ |  ___) | | | |  __/ | |\n");
    console::puts(" |_|\\_\\___|_|  |_| |_|\\___|_| |____/|_| |_|\\___|_|_|\n");
    console::puts("\n");
    console::puts("Type 'help' for available commands.\n\n");

    char cmd_buf[CMD_BUF_SIZE];

    while (true) {
        /* Print prompt */
        console::puts("vk> ");

        /* Read a line */
        usize pos = 0;
        while (pos < CMD_BUF_SIZE - 1) {
            char c = input::getc();
            s_tick_count = s_tick_count + 1; /* rough tick tracking */

            if (c == '\r' || c == '\n') {
                console::putc('\n');
                break;
            }
            if (c == 0x7F || c == '\b') {
                /* Backspace */
                if (pos > 0) {
                    --pos;
                    console::puts("\b \b");
                }
                continue;
            }
            if (c >= ' ' && c < 0x7F) {
                cmd_buf[pos++] = c;
                console::putc(c);
            }
        }
        cmd_buf[pos] = '\0';

        /* Skip empty input */
        if (pos == 0) continue;

        /* Dispatch */
        if (str_equal(cmd_buf, "help") || str_equal(cmd_buf, "?")) {
            cmd_help();
        } else if (str_equal(cmd_buf, "version")) {
            cmd_version();
        } else if (str_equal(cmd_buf, "mem")) {
            cmd_mem();
        } else if (str_equal(cmd_buf, "tasks")) {
            sched::dump_tasks();
        } else if (str_equal(cmd_buf, "ls")) {
            cmd_ls();
        } else if (str_starts_with(cmd_buf, "cat ")) {
            cmd_cat(cmd_buf + 4);
        } else if (str_equal(cmd_buf, "clear")) {
            console::clear();
        } else if (str_equal(cmd_buf, "uptime")) {
            cmd_uptime();
        } else if (str_equal(cmd_buf, "reboot")) {
            console::puts("Rebooting...\n");
            arch::reboot();
        } else if (str_equal(cmd_buf, "idt")) {
            arch::dump_idt();
        } else if (str_equal(cmd_buf, "alloc")) {
            /* Test command to allocate some memory and show it in 'mem' */
            void* p = g_kernel_heap.allocate(4096);
            if (p) {
                console::puts("Allocated 4096 bytes at 0x");
                console::put_hex(reinterpret_cast<u64>(p));
                console::puts("\n");
            } else {
                console::puts("Allocation failed\n");
            }
        } else if (str_starts_with(cmd_buf, "run ")) {
            cmd_run(cmd_buf + 4);
        } else if (str_equal(cmd_buf, "panic")) {
            vk_panic(__FILE__, __LINE__, "Manual panic triggered by 'panic' command");
        } else {
            console::puts("Unknown command: ");
            console::puts(cmd_buf);
            console::puts("\nType 'help' for available commands.\n");
        }
    }
}

} // namespace shell
} // namespace vk
