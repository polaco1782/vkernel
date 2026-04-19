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
    printf("Available commands:\n");
    printf("  help         - Show this message\n");
    printf("  version      - Show API version\n");
    printf("  mem          - Show memory info\n");
    printf("  tasks        - Show scheduler tasks\n");
    printf("  ls           - Show staged files\n");
    printf("  cat <f>      - Print a ramfs file\n");
    printf("  clear        - Clear the screen\n");
    printf("  uptime       - Show tick count\n");
    printf("  reboot       - Reboot the machine\n");
    printf("  idt          - Dump interrupt descriptor table\n");
    printf("  alloc        - Allocate and free a test block\n");
    printf("  run <f>      - Launch a userspace program\n");
    printf("  drvload <d>  - Load a driver (e.g. drvload sb16.vko)\n");
    printf("  drvunload <d>- Unload a driver\n");
    printf("  panic        - Trigger a userspace fault\n");
    printf("  exit         - Exit the shell\n");
}

static void print_version(const vk_api_t* api) {
    printf("vkernel userspace shell\n");
    printf("  Kernel API version : %llu\n", (unsigned long long)api->api_version);
    printf("  Loader             : vkernel userspace\n");
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

    printf("Staged files:\n");
    for (unsigned int i = 0; i < sizeof(k_files) / sizeof(k_files[0]); ++i) {
        const char* name = k_files[i];
        if (vk_file_exists(name)) {
            printf("  %s (%llu bytes)\n", name, (unsigned long long)vk_file_size(name));
        }
    }
}

static void cat_file(const char* arg) {
    const char* path = skip_spaces(arg);
    if (*path == '\0') {
        printf("Usage: cat <filename>\n");
        return;
    }

    FILE* file = fopen(path, "r");
    if (file == NULL) {
        printf("cat: file not found: %s\n", path);
        return;
    }

    unsigned char buffer[128];
    size_t read_count;
    while ((read_count = fread(buffer, 1, sizeof(buffer), file)) > 0) {
        fwrite(buffer, 1, read_count, stdout);
    }
    fclose(file);
    printf("\n");
}

static void run_program(const char* arg) {
    const char* path = skip_spaces(arg);
    if (*path == '\0') {
        printf("Usage: run <filename>\n");
        return;
    }

    vk_i64 task_id = vk_run(path);
    if (task_id < 0) {
        printf("run: failed to launch %s\n", path);
    } else {
        printf("run: spawned task %lld\n", (long long)task_id);
        vk_wait_task(task_id);
    }
}

static void print_memory_info(void) {
    vk_dump_memory();
}

static void do_drvload(const char* arg) {
    const char* name = skip_spaces(arg);
    if (*name == '\0') {
        printf("Usage: drvload <driver_name>\n");
        printf("Example: drvload sb16.vko\n");
        return;
    }
    int result = vk_drv_load(name);
    if (result == 0) {
        printf("Driver loaded successfully.\n");
    } else {
        printf("Failed to load driver: %s\n", name);
    }
}

static void do_drvunload(const char* arg) {
    const char* name = skip_spaces(arg);
    if (*name == '\0') {
        printf("Usage: drvunload <driver_name>\n");
        return;
    }
    int result = vk_drv_unload(name);
    if (result == 0) {
        printf("Driver unloaded.\n");
    } else {
        printf("Failed to unload driver: %s\n", name);
    }
}

static void print_tasks(void) {
    vk_dump_tasks();
}

static void print_uptime(void) {
    vk_u64 ticks = vk_tick_count();
    printf("Uptime: ~%llu seconds (%llu ticks)\n", (unsigned long long)(ticks / 100ULL), (unsigned long long)ticks);
}

static void do_reboot(void) {
    printf("Rebooting...\n");
    vk_reboot();
}

static void do_idt_dump(void) {
    vk_dump_idt();
}

static void do_alloc_test(void) {
    printf("Allocating 4096 bytes... ");
    void* ptr = malloc(4096);
    if (ptr == NULL) {
        printf("FAILED\n");
        return;
    }

    printf("OK at %p\n", ptr);
    free(ptr);
    printf("Freed.\n");
}

static void do_panic_test(void) {
    printf("Triggering userspace fault...\n");
    ((void(*)())0)();
}

int _start(const vk_api_t* api) {
    vk_init(api);

    printf("\n\n");
    printf("+----------------------------------+\n");
    printf("|   vkernel userspace shell        |\n");
    printf("+----------------------------------+\n");
    printf("Type 'help' for available commands.\n\n");

    char line[256];

    for (;;) {
        printf("vk> ");
        vk_usize len = vk_getline(line, sizeof(line));
        if (len == 0) {
            continue;
        }

        const char* command = skip_spaces(line);
        if (*command == '\0') {
            continue;
        }

        if (strcmp(command, "help") == 0 || strcmp(command, "?") == 0) {
            print_help();
        } else if (strcmp(command, "version") == 0) {
            print_version(api);
        } else if (strcmp(command, "mem") == 0) {
            print_memory_info();
        } else if (strcmp(command, "tasks") == 0) {
            print_tasks();
        } else if (strcmp(command, "ls") == 0) {
            print_known_files();
        } else if (has_prefix(command, "cat ")) {
            cat_file(command + 4);
        } else if (strcmp(command, "clear") == 0) {
            vk_clear();
        } else if (strcmp(command, "uptime") == 0) {
            print_uptime();
        } else if (strcmp(command, "reboot") == 0) {
            do_reboot();
        } else if (strcmp(command, "idt") == 0) {
            do_idt_dump();
        } else if (strcmp(command, "alloc") == 0) {
            do_alloc_test();
        } else if (has_prefix(command, "run ")) {
            run_program(command + 4);
        } else if (has_prefix(command, "drvload ")) {
            do_drvload(command + 8);
        } else if (has_prefix(command, "drvunload ")) {
            do_drvunload(command + 10);
        } else if (strcmp(command, "panic") == 0) {
            do_panic_test();
        } else if (strcmp(command, "exit") == 0) {
            vk_exit(0);
        } else {
            printf("Unknown command: %s\n", command);
            printf("Type 'help' for available commands.\n");
        }
    }

    return 0;
}
