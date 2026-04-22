/*
 * vkernel userspace - shell
 * Copyright (C) 2026 vkernel authors
 *
 * shell.c - Freestanding userspace shell for vkernel
 *
 * Build: see Makefile (Linux) or shell.vcxproj (Visual Studio).
 * Run:   launched automatically by the kernel as shell.elf / shell.exe
 */

#include "../include/vk.h"

#if defined(_MSC_VER)
// Disable "unreachable code" warnings since the shell runs an infinite loop.
#pragma warning(disable: 4702)
#endif

static inline vk_usize console_getline(char* buf, vk_usize max) {
    vk_usize pos = 0;
    while (pos < max - 1) {
        char c = VK_CALL(getc);
        if (c == '\r' || c == '\n') {
            VK_CALL(putc, '\n');
            break;
        }
        if ((c == 0x7F || c == '\b') && pos > 0) {
            --pos;
            VK_CALL(puts, "\b \b");
            continue;
        }
        if (c >= ' ' && c < 0x7F) {
            buf[pos++] = c;
            VK_CALL(putc, c);
        }
    }
    buf[pos] = '\0';
    return pos;
}

static const char* skip_spaces(const char* text) {
    while (*text == ' ' || *text == '\t') {
        ++text;
    }
    return text;
}

static int has_prefix(const char* text, const char* prefix) {
    while (*prefix != '\0') {
        if (*text != *prefix) {
            return 0;
        }
        ++text;
        ++prefix;
    }
    return 1;
}

static void print_help(void) {
    VK_CALL(puts, "Available commands:\n");
    VK_CALL(puts, "  help         - Show this message\n");
    VK_CALL(puts, "  version      - Show API version\n");
    VK_CALL(puts, "  mem          - Show memory info\n");
    VK_CALL(puts, "  tasks        - Show scheduler tasks\n");
    VK_CALL(puts, "  ls           - Show staged files\n");
    VK_CALL(puts, "  cat <f>      - Print a ramfs file\n");
    VK_CALL(puts, "  clear        - Clear the screen\n");
    VK_CALL(puts, "  uptime       - Show tick count\n");
    VK_CALL(puts, "  reboot       - Reboot the machine\n");
    VK_CALL(puts, "  idt          - Dump interrupt descriptor table\n");
    VK_CALL(puts, "  alloc        - Allocate and free a test block\n");
    VK_CALL(puts, "  run <f>      - Launch a userspace program\n");
    VK_CALL(puts, "  drvload <d>  - Load a driver (e.g. drvload sb16.vko)\n");
    VK_CALL(puts, "  drvunload <d>- Unload a driver\n");
    VK_CALL(puts, "  panic        - Trigger a userspace fault\n");
    VK_CALL(puts, "  exit         - Exit the shell\n");
}

static void print_version(const vk_api_t* api) {
    VK_CALL(puts, "vkernel userspace shell\n");
    VK_CALL(puts, "  Kernel API version : ");
    VK_CALL(put_dec, (vk_u64)api->api_version);
    VK_CALL(puts, "\n");
    VK_CALL(puts, "  Loader             : vkernel userspace\n");
}

static void print_known_files(void) {
    static const char* k_files[] = {
        "shell.vbin",
        "hello.vbin",
        "doom.vbin",
        "framebuffer.vbin",
        "framebuffer_text.vbin",
        "raytracer.vbin",
        "ramfs_reader.vbin",
        "motd.txt",
        "hello.txt",
        "shell.txt",
    };

    VK_CALL(puts, "Staged files:\n");
    for (unsigned int i = 0; i < sizeof(k_files) / sizeof(k_files[0]); ++i) {
        const char* name = k_files[i];
        if (VK_CALL(file_exists, name)) {
            VK_CALL(puts, "  ");
            VK_CALL(puts, name);
            VK_CALL(puts, " (");
            VK_CALL(put_dec, (vk_u64)VK_CALL(file_size, name));
            VK_CALL(puts, " bytes)\n");
        }
    }
}

static void cat_file(const char* arg) {
    const char* path = skip_spaces(arg);
    if (*path == '\0') {
        VK_CALL(puts, "Usage: cat <filename>\n");
        return;
    }

    vk_file_handle_t fh = VK_CALL(file_open, path, "r");
    
    if (fh == (vk_file_handle_t)0) {
        VK_CALL(puts, "cat: file not found: ");
        VK_CALL(puts, path);
        VK_CALL(puts, "\n");
        return;
    }

    unsigned char buffer[128];
    vk_usize read_count;
    while ((read_count = VK_CALL(file_read_handle, fh, buffer, sizeof(buffer))) > 0) {
        for (vk_usize i = 0; i < read_count; ++i) {
            VK_CALL(putc, (char)buffer[i]);
        }
    }
    VK_CALL(file_close, fh);
    VK_CALL(puts, "\n");
}

static void run_program(const char* arg) {
    const char* path = skip_spaces(arg);
    if (*path == '\0') {
        VK_CALL(puts, "Usage: run <filename>\n");
        return;
    }

    vk_i64 task_id = VK_CALL(run, path);
    if (task_id < 0) {
        VK_CALL(puts, "run: failed to launch ");
        VK_CALL(puts, path);
        VK_CALL(puts, "\n");
    } else {
        VK_CALL(puts, "run: spawned task ");
        VK_CALL(put_dec, (vk_u64)task_id);
        VK_CALL(puts, "\n");
        VK_CALL(wait_task, task_id);
    }
}

static void print_memory_info(void) {
    VK_CALL(dump_memory);
}

static void do_drvload(const char* arg) {
    const char* name = skip_spaces(arg);
    if (*name == '\0') {
        VK_CALL(puts, "Usage: drvload <driver_name>\n");
        VK_CALL(puts, "Example: drvload sb16.vko\n");
        return;
    }
    int result = VK_CALL(drv_load, name);
    if (result == 0) {
        VK_CALL(puts, "Driver loaded successfully.\n");
    } else {
        VK_CALL(puts, "Failed to load driver: ");
        VK_CALL(puts, name);
        VK_CALL(puts, "\n");
    }
}

static void do_drvunload(const char* arg) {
    const char* name = skip_spaces(arg);
    if (*name == '\0') {
        VK_CALL(puts, "Usage: drvunload <driver_name>\n");
        return;
    }
    int result = VK_CALL(drv_unload, name);
    if (result == 0) {
        VK_CALL(puts, "Driver unloaded.\n");
    } else {
        VK_CALL(puts, "Failed to unload driver: ");
        VK_CALL(puts, name);
        VK_CALL(puts, "\n");
    }
}

static void print_tasks(void) {
    VK_CALL(dump_tasks);
}

static void print_uptime(void) {
    vk_u64 ticks = VK_CALL(tick_count);
    VK_CALL(puts, "Uptime: ~");
    VK_CALL(put_dec, (vk_u64)(ticks / 100ULL));
    VK_CALL(puts, " seconds (");
    VK_CALL(put_dec, (vk_u64)ticks);
    VK_CALL(puts, " ticks)\n");
}

static void do_reboot(void) {
    VK_CALL(puts, "Rebooting...\n");
    VK_CALL(reboot);
}

static void do_idt_dump(void) {
    VK_CALL(dump_idt);
}

static void do_alloc_test(void) {
    VK_CALL(puts, "Allocating 4096 bytes... ");
    void* ptr = VK_CALL(malloc, 4096);
    if (ptr == 0) {
        VK_CALL(puts, "FAILED\n");
        return;
    }
    VK_CALL(puts, "OK at ");
    VK_CALL(put_hex, (vk_u64)(unsigned long)ptr);
    VK_CALL(puts, "\n");
    VK_CALL(free, ptr);
    VK_CALL(puts, "Freed.\n");
}

static void do_panic_test(void) {
    VK_CALL(puts, "Triggering userspace fault...\n");
    ((void(*)())0)();
}

// ugly but it works ¯\_(ツ)_/¯
#define CMP(x) VK_CALL(memcmp, cmdline, x, sizeof(x) - 1) == 0
int parse_cmdline(const char *cmdline) {
    if (CMP("help") || CMP("?")) {
        print_help();
    } else if (CMP("mem")) {
        print_memory_info();
    } else if (CMP("tasks")) {
        print_tasks();
    } else if (CMP("ls")) {
        print_known_files();
    } else if (has_prefix(cmdline, "cat ")) {
        cat_file(cmdline + 4);
    } else if (CMP("clear")) {
        VK_CALL(clear);
    } else if (CMP("uptime")) {
        print_uptime();
    } else if (CMP("reboot")) {
        do_reboot();
    } else if (CMP("idt")) {
        do_idt_dump();
    } else if (CMP("alloc")) {
        do_alloc_test();
    } else if (has_prefix(cmdline, "run ")) {
        run_program(cmdline + 4);
    } else if (has_prefix(cmdline, "drvload ")) {
        do_drvload(cmdline + 8);
    } else if (has_prefix(cmdline, "drvunload ")) {
        do_drvunload(cmdline + 10);
    } else if (CMP("panic")) {
        do_panic_test();
    } else if (CMP("exit")) {
        VK_CALL(exit, 0);
    } else {
        VK_CALL(puts, "Unknown command: ");
        VK_CALL(puts, cmdline);
        VK_CALL(puts, "\n");
        VK_CALL(puts, "Type 'help' for available commands.\n");
    }
}

static void read_startup_script(void) {
    vk_file_handle_t fh = VK_CALL(file_open, "shell.txt", "r");
    if (fh == (vk_file_handle_t)0) {
        VK_CALL(puts, "No startup script found (shell.txt), skipping...\n");
        return;
    }

    char line[128];
    char buffer[256];
    vk_usize read_count;
    while ((read_count = VK_CALL(file_read_handle, fh, buffer, sizeof(buffer))) > 0) {
        for (vk_usize i = 0; i < read_count; ++i) {
            if (buffer[i] == '\n') {
                buffer[i] = '\0';
                const char* command = skip_spaces(buffer);
                if (*command != '\0') {
                    VK_CALL(puts, "vk> ");
                    VK_CALL(puts, command);
                    VK_CALL(puts, "\n");
                    parse_cmdline(command);
                }
                // Move remaining data to the start of the buffer
                vk_usize remaining = read_count - i - 1;
                if (remaining > 0) {
                    VK_CALL(memmove, buffer, buffer + i + 1, remaining);
                }
                read_count = remaining;
                i = -1; // Reset to start of buffer for next iteration
            }
        }
    }

    VK_CALL(file_close, fh);
}

int _start(const vk_api_t* api) {
    vk_init(api);

    VK_CALL(puts, "\n\n");
    VK_CALL(puts, "+----------------------------------+\n");
    VK_CALL(puts, "|   vkernel userspace shell        |\n");
    VK_CALL(puts, "+----------------------------------+\n");
    VK_CALL(puts, "Type 'help' for available commands.\n\n");

    char line[256];

    read_startup_script();

    for (;;) {
        VK_CALL(puts, "vk> ");
        vk_usize len = console_getline(line, sizeof(line));
        if (len == 0) {
            continue;
        }

        const char* command = skip_spaces(line);
        if (*command == '\0') {
            continue;
        }

        int ret = parse_cmdline(command);
    }

    return 0;
}
