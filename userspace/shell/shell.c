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
#pragma warning(disable: 4702) /* unreachable code (infinite shell loop) */
#endif

/* Freestanding: define what we need ourselves. */
#define VK_NULL ((void*)0)

/* -------------------------------------------------------------------------
 * Freestanding string helpers
 * ---------------------------------------------------------------------- */

static vk_usize vk_strlen(const char* s)
{
    vk_usize n = 0;
    while (s[n]) ++n;
    return n;
}

static int vk_strcmp(const char* a, const char* b)
{
    while (*a && *a == *b) { ++a; ++b; }
    return (unsigned char)*a - (unsigned char)*b;
}

/* Returns 1 if `s` starts with `prefix`, 0 otherwise. */
static int vk_has_prefix(const char* s, const char* prefix)
{
    while (*prefix) {
        if (*s++ != *prefix++)
            return 0;
    }
    return 1;
}

/* Skip leading whitespace. */
static const char* vk_skip_spaces(const char* s)
{
    while (*s == ' ' || *s == '\t') ++s;
    return s;
}

static int shell_has_framebuffer(void)
{
    vk_framebuffer_info_t fb;
    VK_CALL(framebuffer_info, &fb);
    return fb.valid != 0;
}

/* -------------------------------------------------------------------------
 * Console I/O
 * ---------------------------------------------------------------------- */

/*
 * Read a line from the console into `buf` (max `max` bytes including NUL).
 * Handles backspace/DEL and filters non-printable characters.
 * Returns the number of characters stored (not counting NUL).
 */
static vk_usize console_getline(char* buf, vk_usize max)
{
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

/* -------------------------------------------------------------------------
 * Command implementations
 * ---------------------------------------------------------------------- */

static void cmd_help(const char* arg)
{
    (void)arg;
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

static void cmd_version(const char* arg)
{
    (void)arg;
    VK_CALL(puts, "vkernel userspace shell\n");
    VK_CALL(puts, "  Loader : vkernel userspace\n");
}

static void cmd_mem(const char* arg)
{
    (void)arg;
    VK_CALL(dump_memory);
}

static void cmd_tasks(const char* arg)
{
    (void)arg;
    VK_CALL(dump_tasks);
}

static void cmd_ls(const char* arg)
{
    (void)arg;
    VK_CALL(puts, "TODO - Implement file listing\n");
}

static void cmd_cat(const char* arg)
{
    const char* path = vk_skip_spaces(arg);
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

    unsigned char buf[128];
    vk_usize n;
    while ((n = VK_CALL(file_read_handle, fh, buf, sizeof(buf))) > 0) {
        for (vk_usize i = 0; i < n; ++i)
            VK_CALL(putc, (char)buf[i]);
    }

    VK_CALL(file_close, fh);
    VK_CALL(puts, "\n");
}

static void cmd_clear(const char* arg)
{
    (void)arg;
    VK_CALL(clear);
}

static void cmd_uptime(const char* arg)
{
    (void)arg;
    vk_u64 ticks = VK_CALL(tick_count);
    VK_CALL(puts, "Uptime: ~");
    VK_CALL(put_dec, ticks / 100ULL);
    VK_CALL(puts, " seconds (");
    VK_CALL(put_dec, ticks);
    VK_CALL(puts, " ticks)\n");
}

static void cmd_reboot(const char* arg)
{
    (void)arg;
    VK_CALL(puts, "Rebooting...\n");
    VK_CALL(reboot);
}

static void cmd_idt(const char* arg)
{
    (void)arg;
    VK_CALL(dump_idt);
}

static void cmd_alloc(const char* arg)
{
    (void)arg;
    VK_CALL(puts, "Allocating 4096 bytes... ");
    void* ptr = VK_CALL(malloc, 4096);
    if (!ptr) {
        VK_CALL(puts, "FAILED\n");
        return;
    }
    VK_CALL(puts, "OK at ");
    VK_CALL(put_hex, (vk_u64)(unsigned long)ptr);
    VK_CALL(puts, "\n");
    VK_CALL(free, ptr);
    VK_CALL(puts, "Freed.\n");
}

static void cmd_run(const char* arg)
{
    const char* path = vk_skip_spaces(arg);
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

static void cmd_drvload(const char* arg)
{
    const char* name = vk_skip_spaces(arg);
    if (*name == '\0') {
        VK_CALL(puts, "Usage: drvload <driver_name>\n");
        VK_CALL(puts, "Example: drvload sb16.vko\n");
        return;
    }
    if (VK_CALL(drv_load, name) == 0)
        VK_CALL(puts, "Driver loaded successfully.\n");
    else {
        VK_CALL(puts, "Failed to load driver: ");
        VK_CALL(puts, name);
        VK_CALL(puts, "\n");
    }
}

static void cmd_drvunload(const char* arg)
{
    const char* name = vk_skip_spaces(arg);
    if (*name == '\0') {
        VK_CALL(puts, "Usage: drvunload <driver_name>\n");
        return;
    }
    if (VK_CALL(drv_unload, name) == 0)
        VK_CALL(puts, "Driver unloaded.\n");
    else {
        VK_CALL(puts, "Failed to unload driver: ");
        VK_CALL(puts, name);
        VK_CALL(puts, "\n");
    }
}

static void cmd_panic(const char* arg)
{
    (void)arg;
    VK_CALL(puts, "Triggering userspace fault...\n");
    ((void(*)())0)();
}

static void cmd_exit(const char* arg)
{
    (void)arg;
    VK_CALL(exit, 0);
}

/* -------------------------------------------------------------------------
 * Command dispatch
 *
 * Two separate, homogeneous tables — one for exact matches, one for prefix
 * commands. Keeping the tables homogeneous (every field always valid)
 * avoids any compiler trouble with partial zero-initialization of mixed
 * pointer/non-pointer struct fields in freestanding mode.
 * ---------------------------------------------------------------------- */

typedef void (*cmd_fn)(const char*);

typedef struct {
    const char* keyword;
    cmd_fn      fn;
} exact_cmd_t;

typedef struct {
    const char* prefix; /* includes trailing space, e.g. "cat " */
    vk_usize    skip;   /* bytes past the prefix to pass as arg  */
    cmd_fn      fn;
} prefix_cmd_t;

static const exact_cmd_t EXACT_CMDS[] = {
    { "help",    cmd_help     },
    { "?",       cmd_help     },
    { "version", cmd_version  },
    { "mem",     cmd_mem      },
    { "tasks",   cmd_tasks    },
    { "ls",      cmd_ls       },
    { "clear",   cmd_clear    },
    { "uptime",  cmd_uptime   },
    { "reboot",  cmd_reboot   },
    { "idt",     cmd_idt      },
    { "alloc",   cmd_alloc    },
    { "panic",   cmd_panic    },
    { "exit",    cmd_exit     },
};

static const prefix_cmd_t PREFIX_CMDS[] = {
    { "cat ",       4,  cmd_cat       },
    { "run ",       4,  cmd_run       },
    { "drvload ",   8,  cmd_drvload   },
    { "drvunload ", 10, cmd_drvunload },
};

#define ARRAY_LEN(a) (sizeof(a) / sizeof((a)[0]))

static int parse_cmdline(const char* cmdline)
{
    vk_usize i;

    for (i = 0; i < ARRAY_LEN(PREFIX_CMDS); ++i) {
        if (vk_has_prefix(cmdline, PREFIX_CMDS[i].prefix)) {
            PREFIX_CMDS[i].fn(cmdline + PREFIX_CMDS[i].skip);
            return 0;
        }
    }

    for (i = 0; i < ARRAY_LEN(EXACT_CMDS); ++i) {
        if (vk_strcmp(cmdline, EXACT_CMDS[i].keyword) == 0) {
            EXACT_CMDS[i].fn("");
            return 0;
        }
    }

    VK_CALL(puts, "Unknown command: ");
    VK_CALL(puts, cmdline);
    VK_CALL(puts, "\n");
    VK_CALL(puts, "Type 'help' for available commands.\n");
    return -1;
}

/* -------------------------------------------------------------------------
 * Startup script
 * ---------------------------------------------------------------------- */

/*
 * Read and execute "shell.txt" line by line using a byte accumulator.
 * Handles files that don't end with a trailing newline.
 */
static void read_startup_script(void)
{
    vk_file_handle_t fh = VK_CALL(file_open, "shell.txt", "r");
    if (fh == (vk_file_handle_t)0) {
        VK_CALL(puts, "No startup script found (shell.txt), skipping...\n");
        return;
    }

    char     chunk[256];
    char     line[256];
    vk_usize line_pos = 0;
    vk_usize n;
    vk_usize i;

    while ((n = VK_CALL(file_read_handle, fh, chunk, sizeof(chunk))) > 0) {
        for (i = 0; i < n; ++i) {
            char c = chunk[i];

            if (c == '\n') {
                line[line_pos] = '\0';
                line_pos = 0;

                const char* cmd = vk_skip_spaces(line);
                if (*cmd == '#' || *cmd == '\0')
                    continue;

                VK_CALL(puts, "vk> ");
                VK_CALL(puts, cmd);
                VK_CALL(puts, "\n");
                parse_cmdline(cmd);
                continue;
            }

            /* Silently truncate lines that exceed the buffer. */
            if (line_pos < sizeof(line) - 1)
                line[line_pos++] = c;
        }
    }

    /* Handle a final line with no trailing newline. */
    if (line_pos > 0) {
        line[line_pos] = '\0';
        const char* cmd = vk_skip_spaces(line);
        if (*cmd != '#' && *cmd != '\0') {
            VK_CALL(puts, "vk> ");
            VK_CALL(puts, cmd);
            VK_CALL(puts, "\n");
            parse_cmdline(cmd);
        }
    }

    VK_CALL(file_close, fh);
}

/* -------------------------------------------------------------------------
 * Entry point
 * ---------------------------------------------------------------------- */

int _start(const vk_api_t* api)
{
    vk_init(api);

    VK_CALL(puts, "\n\n");
    VK_CALL(puts, "+----------------------------------+\n");
    VK_CALL(puts, "|     vkernel userspace shell      |\n");
    VK_CALL(puts, "+----------------------------------+\n");
    VK_CALL(puts, "Type 'help' for available commands.\n\n");

    if (shell_has_framebuffer())
        read_startup_script();

    char line[256];
    for (;;) {
        VK_CALL(puts, "vk> ");
        vk_usize len = console_getline(line, sizeof(line));
        if (len == 0)
            continue;

        const char* cmd = vk_skip_spaces(line);
        if (*cmd == '\0')
            continue;

        parse_cmdline(cmd);
    }

    return 0;
}