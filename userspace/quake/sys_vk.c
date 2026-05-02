/*
 * sys_vk.c - Chocolate Quake system interface for vkernel
 * Replaces src/sys/src/sys.c
 */

#include "sys.h"
#include "client.h"
#include "cmd.h"
#include "config.h"
#include "end_screen.h"
#include "host.h"
#include "input.h"
#include "keys.h"
#include "menu.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>

#include "../include/vk.h"

qboolean isDedicated;

/* ---------------------------------------------------------------
 * File I/O — thin wrappers around standard fopen/fread/fwrite
 *
 * VK has a flat ramfs: files are stored as bare names ("pak0.pak").
 * Quake builds paths like "./id1/pak0.pak". We resolve by trying
 * the full path first, then falling back to the basename.
 * --------------------------------------------------------------- */

/* Return a pointer to the bare filename component (after last '/') */
static const char *vk_basename(const char *path)
{
    const char *s = strrchr(path, '/');
    return s ? s + 1 : path;
}

/* Try to open 'path'; if that fails, retry with just the basename. */
static FILE *vk_fopen(const char *path, const char *mode)
{
    FILE *f = fopen(path, mode);
    if (f) return f;
    const char *base = vk_basename(path);
    if (base != path)
        f = fopen(base, mode);
    return f;
}

#define MAX_HANDLES 10
static FILE *sys_handles[MAX_HANDLES];

static i32 findhandle(void)
{
    for (int i = 1; i < MAX_HANDLES; i++)
        if (!sys_handles[i])
            return i;
    Sys_Error("out of file handles");
    return -1;
}

static i32 filelength(FILE *f)
{
    long pos = ftell(f);
    fseek(f, 0, SEEK_END);
    long end = ftell(f);
    fseek(f, pos, SEEK_SET);
    return (i32)end;
}

i32 Sys_FileOpenRead(char *path, i32 *hndl)
{
    i32 i = findhandle();
    FILE *f = vk_fopen(path, "rb");
    if (!f) { *hndl = -1; return -1; }
    sys_handles[i] = f;
    *hndl = i;
    return filelength(f);
}

i32 Sys_FileOpenWrite(char *path)
{
    i32 i = findhandle();
    FILE *f = vk_fopen(path, "wb");
    if (!f)
        Sys_Error("Error opening %s: %s", path, strerror(errno));
    sys_handles[i] = f;
    return i;
}

void Sys_FileClose(i32 handle)
{
    fclose(sys_handles[handle]);
    sys_handles[handle] = NULL;
}

void Sys_FileSeek(i32 handle, i32 position)
{
    fseek(sys_handles[handle], position, SEEK_SET);
}

size_t Sys_FileRead(i32 handle, void *dest, i32 count)
{
    return fread(dest, 1, (size_t)count, sys_handles[handle]);
}

size_t Sys_FileWrite(i32 handle, void *data, i32 count)
{
    return fwrite(data, 1, (size_t)count, sys_handles[handle]);
}

i32 Sys_FileTime(char *path)
{
    FILE *f = vk_fopen(path, "rb");
    if (f) { fclose(f); return 1; }
    return -1;
}

void Sys_mkdir(char *path)
{
    (void)path; /* no mkdir on VK */
}

/* ---------------------------------------------------------------
 * Error / output
 * --------------------------------------------------------------- */

void Sys_Error(char *error, ...)
{
    va_list argptr;
    fprintf(stderr, "Sys_Error: ");
    va_start(argptr, error);
    vfprintf(stderr, error, argptr);
    va_end(argptr);
    fprintf(stderr, "\n");

    Host_Shutdown();
    VK_CALL(exit, 1);
    __builtin_unreachable();
}

void Sys_Printf(char *fmt, ...)
{
    va_list argptr;
    va_start(argptr, fmt);
    vprintf(fmt, argptr);
    va_end(argptr);
}

void Sys_Quit(void)
{
    Host_Shutdown();
    ES_DisplayScreen();
    VK_CALL(exit, 0);
    __builtin_unreachable();
}

/* ---------------------------------------------------------------
 * Timing
 * --------------------------------------------------------------- */

double Sys_FloatTime(void)
{
    static double freq = 0.0;
    static vk_u64 start = 0;

    if (freq == 0.0) {
        freq  = (double)VK_CALL(ticks_per_sec);
        start = VK_CALL(tick_count);
        return 0.0;
    }
    vk_u64 now = VK_CALL(tick_count);
    return (double)(now - start) / freq;
}

char *Sys_ConsoleInput(void)
{
    return NULL;
}

/* ---------------------------------------------------------------
 * PS/2 scancode → Quake key mapping
 * --------------------------------------------------------------- */
static byte ps2_to_qkey[256];

static void build_key_table(void)
{
    static int done = 0;
    if (done) return;
    done = 1;

    memset(ps2_to_qkey, 0, sizeof(ps2_to_qkey));

    /* letters (scan set 1 make codes) */
    ps2_to_qkey[0x1E] = 'a'; ps2_to_qkey[0x30] = 'b';
    ps2_to_qkey[0x2E] = 'c'; ps2_to_qkey[0x20] = 'd';
    ps2_to_qkey[0x12] = 'e'; ps2_to_qkey[0x21] = 'f';
    ps2_to_qkey[0x22] = 'g'; ps2_to_qkey[0x23] = 'h';
    ps2_to_qkey[0x17] = 'i'; ps2_to_qkey[0x24] = 'j';
    ps2_to_qkey[0x25] = 'k'; ps2_to_qkey[0x26] = 'l';
    ps2_to_qkey[0x32] = 'm'; ps2_to_qkey[0x31] = 'n';
    ps2_to_qkey[0x18] = 'o'; ps2_to_qkey[0x19] = 'p';
    ps2_to_qkey[0x10] = 'q'; ps2_to_qkey[0x13] = 'r';
    ps2_to_qkey[0x1F] = 's'; ps2_to_qkey[0x14] = 't';
    ps2_to_qkey[0x16] = 'u'; ps2_to_qkey[0x2F] = 'v';
    ps2_to_qkey[0x11] = 'w'; ps2_to_qkey[0x2D] = 'x';
    ps2_to_qkey[0x15] = 'y'; ps2_to_qkey[0x2C] = 'z';

    /* digits */
    ps2_to_qkey[0x02] = '1'; ps2_to_qkey[0x03] = '2';
    ps2_to_qkey[0x04] = '3'; ps2_to_qkey[0x05] = '4';
    ps2_to_qkey[0x06] = '5'; ps2_to_qkey[0x07] = '6';
    ps2_to_qkey[0x08] = '7'; ps2_to_qkey[0x09] = '8';
    ps2_to_qkey[0x0A] = '9'; ps2_to_qkey[0x0B] = '0';

    /* special */
    ps2_to_qkey[0x01] = K_ESCAPE;
    ps2_to_qkey[0x1C] = K_ENTER;
    ps2_to_qkey[0x0E] = K_BACKSPACE;
    ps2_to_qkey[0x0F] = K_TAB;
    ps2_to_qkey[0x39] = K_SPACE;
    ps2_to_qkey[0x3B] = K_F1;  ps2_to_qkey[0x3C] = K_F2;
    ps2_to_qkey[0x3D] = K_F3;  ps2_to_qkey[0x3E] = K_F4;
    ps2_to_qkey[0x3F] = K_F5;  ps2_to_qkey[0x40] = K_F6;
    ps2_to_qkey[0x41] = K_F7;  ps2_to_qkey[0x42] = K_F8;
    ps2_to_qkey[0x43] = K_F9;  ps2_to_qkey[0x44] = K_F10;
    ps2_to_qkey[0x57] = K_F11; ps2_to_qkey[0x58] = K_F12;

    /* arrow keys — extended (0xE0 prefix), kernel delivers make|0x80 */
    ps2_to_qkey[0xC8] = K_UPARROW;
    ps2_to_qkey[0xD0] = K_DOWNARROW;
    ps2_to_qkey[0xCB] = K_LEFTARROW;
    ps2_to_qkey[0xCD] = K_RIGHTARROW;
    /* also non-prefixed numpad arrows */
    ps2_to_qkey[0x48] = K_UPARROW;
    ps2_to_qkey[0x50] = K_DOWNARROW;
    ps2_to_qkey[0x4B] = K_LEFTARROW;
    ps2_to_qkey[0x4D] = K_RIGHTARROW;

    /* modifiers */
    ps2_to_qkey[0x2A] = K_SHIFT; ps2_to_qkey[0x36] = K_SHIFT;
    ps2_to_qkey[0x1D] = K_CTRL;  ps2_to_qkey[0x9D] = K_CTRL;
    ps2_to_qkey[0x38] = K_ALT;   ps2_to_qkey[0xB8] = K_ALT;

    /* nav */
    ps2_to_qkey[0x49] = K_PGUP; ps2_to_qkey[0xC9] = K_PGUP;
    ps2_to_qkey[0x51] = K_PGDN; ps2_to_qkey[0xD1] = K_PGDN;
    ps2_to_qkey[0x47] = K_HOME; ps2_to_qkey[0xC7] = K_HOME;
    ps2_to_qkey[0x4F] = K_END;  ps2_to_qkey[0xCF] = K_END;
    ps2_to_qkey[0x52] = K_INS;  ps2_to_qkey[0xD2] = K_INS;
    ps2_to_qkey[0x53] = K_DEL;  ps2_to_qkey[0xD3] = K_DEL;
    ps2_to_qkey[0x46] = K_PAUSE;

    /* punctuation */
    ps2_to_qkey[0x0C] = '-'; ps2_to_qkey[0x0D] = '=';
    ps2_to_qkey[0x1A] = '['; ps2_to_qkey[0x1B] = ']';
    ps2_to_qkey[0x2B] = '\\';
    ps2_to_qkey[0x27] = ';'; ps2_to_qkey[0x28] = '\'';
    ps2_to_qkey[0x29] = '`';
    ps2_to_qkey[0x33] = ','; ps2_to_qkey[0x34] = '.';
    ps2_to_qkey[0x35] = '/';
}

/* Accumulated mouse delta — read by IN_Move in in_vk.c */
int vk_mouse_acc_dx = 0;
int vk_mouse_acc_dy = 0;

void Sys_SendKeyEvents(void)
{
    build_key_table();

    /* keyboard events */
    vk_key_event_t kev;
    while (VK_CALL(poll_key, &kev)) {
        byte qkey = 0;
        if (kev.scancode < 256)
            qkey = ps2_to_qkey[kev.scancode];
        if (qkey)
            Key_Event((int)qkey, kev.pressed ? true : false);
    }

    /* mouse events — accumulate movement, dispatch button changes */
    vk_mouse_event_t mev;
    static vk_u32 prev_buttons = 0;
    while (VK_CALL(poll_mouse, &mev)) {
        vk_mouse_acc_dx += mev.dx;
        vk_mouse_acc_dy += mev.dy;

        vk_u32 changed = prev_buttons ^ mev.buttons;
        if (changed & 1) Key_Event(K_MOUSE1, (mev.buttons & 1) ? true : false);
        if (changed & 2) Key_Event(K_MOUSE2, (mev.buttons & 2) ? true : false);
        if (changed & 4) Key_Event(K_MOUSE3, (mev.buttons & 4) ? true : false);
        prev_buttons = mev.buttons;
    }
}

void Sys_HighFPPrecision(void) {}
void Sys_LowFPPrecision(void)  {}

/* ---------------------------------------------------------------
 * Initialisation
 * --------------------------------------------------------------- */

quakeparms_t *Sys_Init(i32 argc, char *argv[])
{
    static quakeparms_t parms;

    /* Try progressively smaller heap sizes, like Chocolate Doom does.
     * Kernel heap is 64 MiB total (shared); start at 32 MiB and step
     * down to MINIMUM_MEMORY (0x550000 ≈ 5.3 MiB) in 2 MiB steps. */
    parms.membase = NULL;
    for (parms.memsize = 32 * 1024 * 1024;
         parms.memsize >= 0x550000;
         parms.memsize -= 2 * 1024 * 1024) {
        parms.membase = malloc((size_t)parms.memsize);
        if (parms.membase) break;
    }
    if (!parms.membase) {
        fprintf(stderr, "Sys_Init: out of memory\n");
        VK_CALL(exit, 1);
    }

    parms.basedir = ".";
    COM_InitArgv(argc, argv);
    parms.argc = com_argc;
    parms.argv = com_argv;
    return &parms;
}
