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

static void shell_put_spaces(vk_usize count)
{
    while (count-- > 0)
        VK_CALL(putc, ' ');
}

static void shell_put_padded(const char* s, vk_usize width)
{
    vk_usize len = vk_strlen(s);
    VK_CALL(puts, s);
    if (len < width)
        shell_put_spaces(width - len);
}

static void shell_put_dec_width(vk_u64 value, vk_usize width)
{
    char buf[21];
    vk_usize len = 0;

    if (value == 0) {
        buf[len++] = '0';
    } else {
        while (value > 0 && len < sizeof(buf)) {
            buf[len++] = (char)('0' + (value % 10));
            value /= 10;
        }
    }

    if (len < width)
        shell_put_spaces(width - len);

    while (len > 0)
        VK_CALL(putc, buf[--len]);
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
    VK_CALL(puts, "  top          - Show live CPU usage per task\n");
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

#define TOP_MAX_TASKS 64
#define TOP_MAX_ROWS  18

static const char* task_state_name(vk_u32 state)
{
    switch (state) {
        case 0: return "ready";
        case 1: return "run";
        case 2: return "sleep";
        case 3: return "done";
        default: return "?";
    }
}

static vk_u64 top_find_previous_ticks(const vk_task_info_t* tasks,
                                      vk_usize count,
                                      vk_u64 id)
{
    for (vk_usize i = 0; i < count; ++i) {
        if (tasks[i].id == id)
            return tasks[i].cpu_ticks;
    }
    return 0;
}

static vk_u64 top_cpu_delta(const vk_task_info_t* task,
                            const vk_task_info_t* prev,
                            vk_usize prev_count)
{
    vk_u64 old_ticks = top_find_previous_ticks(prev, prev_count, task->id);
    return task->cpu_ticks >= old_ticks ? task->cpu_ticks - old_ticks : 0;
}

static void top_print_percent(vk_u64 cpu_delta, vk_u64 elapsed_ticks)
{
    vk_u64 tenths;
    if (elapsed_ticks == 0)
        elapsed_ticks = 1;

    tenths = (cpu_delta * 1000ULL + elapsed_ticks / 2ULL) / elapsed_ticks;
    shell_put_dec_width(tenths / 10ULL, 3);
    VK_CALL(putc, '.');
    VK_CALL(putc, (char)('0' + (tenths % 10ULL)));
}

static void top_print_row(const vk_task_info_t* task,
                          vk_u64 cpu_delta,
                          vk_u64 elapsed_ticks)
{
    shell_put_dec_width(task->id, 3);
    VK_CALL(puts, "  ");
    top_print_percent(cpu_delta, elapsed_ticks);
    VK_CALL(puts, "  ");
    shell_put_dec_width(task->cpu_ticks, 8);
    VK_CALL(puts, "  ");
    shell_put_padded(task_state_name(task->state), 7);
    VK_CALL(puts, "  ");
    VK_CALL(puts, task->name);
    VK_CALL(putc, '\n');
}

static void top_render(const vk_task_info_t* prev,
                       vk_usize prev_count,
                       const vk_task_info_t* now,
                       vk_usize now_count,
                       vk_usize total_tasks,
                       vk_u64 elapsed_ticks)
{
    int printed[TOP_MAX_TASKS];
    vk_usize rows = now_count < TOP_MAX_ROWS ? now_count : TOP_MAX_ROWS;

    for (vk_usize i = 0; i < TOP_MAX_TASKS; ++i)
        printed[i] = 0;

    VK_CALL(clear);
    VK_CALL(puts, "vkernel top - press q to quit\n");
    VK_CALL(puts, "Tasks: ");
    VK_CALL(put_dec, (vk_u64)total_tasks);
    VK_CALL(puts, "   Sample: ");
    VK_CALL(put_dec, elapsed_ticks);
    VK_CALL(puts, " ticks\n\n");
    VK_CALL(puts, "PID  CPU%   CPU TICKS  STATE    NAME\n");

    for (vk_usize row = 0; row < rows; ++row) {
        vk_usize best = TOP_MAX_TASKS;
        vk_u64 best_delta = 0;

        for (vk_usize i = 0; i < now_count; ++i) {
            vk_u64 delta;
            if (printed[i])
                continue;

            delta = top_cpu_delta(&now[i], prev, prev_count);
            if (best == TOP_MAX_TASKS || delta > best_delta ||
                (delta == best_delta && now[i].id < now[best].id)) {
                best = i;
                best_delta = delta;
            }
        }

        if (best == TOP_MAX_TASKS)
            break;

        printed[best] = 1;
        top_print_row(&now[best], best_delta, elapsed_ticks);
    }

    if (total_tasks > now_count) {
        VK_CALL(puts, "\n");
        VK_CALL(put_dec, (vk_u64)(total_tasks - now_count));
        VK_CALL(puts, " more task(s) not shown.\n");
    }
}

static void cmd_top(const char* arg)
{
    vk_task_info_t prev[TOP_MAX_TASKS];
    vk_task_info_t now[TOP_MAX_TASKS];
    vk_usize prev_total;
    vk_usize prev_count;
    int once = 0;

    arg = vk_skip_spaces(arg);
    if (vk_strcmp(arg, "once") == 0)
        once = 1;

    prev_total = VK_CALL(task_snapshot, prev, TOP_MAX_TASKS);
    prev_count = prev_total < TOP_MAX_TASKS ? prev_total : TOP_MAX_TASKS;

    for (;;) {
        vk_u64 start = VK_CALL(tick_count);
        vk_u64 end;
        vk_u64 elapsed;
        vk_usize total;
        vk_usize count;

        VK_CALL(sleep, VK_CALL(ticks_per_sec));
        end = VK_CALL(tick_count);
        elapsed = end >= start ? end - start : 1;

        total = VK_CALL(task_snapshot, now, TOP_MAX_TASKS);
        count = total < TOP_MAX_TASKS ? total : TOP_MAX_TASKS;
        top_render(prev, prev_count, now, count, total, elapsed);

        for (vk_usize i = 0; i < count; ++i)
            prev[i] = now[i];
        prev_count = count;

        if (once)
            break;

        char c = VK_CALL(try_getc);
        if (c == 'q' || c == 'Q' || c == 27 || c == '\r' || c == '\n') {
            VK_CALL(puts, "\n");
            break;
        }
    }
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
    { "top",     cmd_top      },
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
    { "top ",       4,  cmd_top       },
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
