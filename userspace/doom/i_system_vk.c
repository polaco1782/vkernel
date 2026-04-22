/*
 * i_system_vk.c - Chocolate Doom system interface for vkernel
 *
 * Replaces i_system.c.
 */

#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <stdarg.h>

#include "config.h"
#include "doomtype.h"
#include "deh_str.h"
#include "m_argv.h"
#include "m_config.h"
#include "m_misc.h"
#include "i_input.h"
#include "i_joystick.h"
#include "i_sound.h"
#include "i_timer.h"
#include "i_video.h"
#include "i_system.h"
#include "w_wad.h"
#include "z_zone.h"

#include "../../include/vkernel/vk.h"

#define DEFAULT_RAM 16 /* MiB */
#define MIN_RAM     4  /* MiB */

/* ---- atexit list ---- */
typedef struct atexit_listentry_s atexit_listentry_t;
struct atexit_listentry_s {
    atexit_func_t func;
    boolean run_on_error;
    atexit_listentry_t *next;
};

static atexit_listentry_t *exit_funcs = NULL;

void I_AtExit(atexit_func_t func, boolean run_on_error)
{
    atexit_listentry_t *entry = malloc(sizeof(*entry));
    entry->func = func;
    entry->run_on_error = run_on_error;
    entry->next = exit_funcs;
    exit_funcs = entry;
}

void I_Tactile(int on, int off, int total)
{
    (void)on; (void)off; (void)total;
}

static byte *AutoAllocMemory(int *size, int default_ram, int min_ram)
{
    byte *zonemem = NULL;
    while (zonemem == NULL) {
        if (default_ram < min_ram)
            I_Error("Unable to allocate %i MiB of RAM for zone", default_ram);
        *size = default_ram * 1024 * 1024;
        zonemem = malloc(*size);
        if (zonemem == NULL)
            default_ram -= 1;
    }
    return zonemem;
}

byte *I_ZoneBase(int *size)
{
    int min_ram, default_ram;
    int p = M_CheckParmWithArgs("-mb", 1);

    if (p > 0) {
        default_ram = atoi(myargv[p + 1]);
        min_ram = default_ram;
    } else {
        default_ram = (sizeof(void *) == 8) ? DEFAULT_RAM * 2 : DEFAULT_RAM;
        min_ram = MIN_RAM;
    }

    byte *zonemem = AutoAllocMemory(size, default_ram, min_ram);
    printf("zone memory: %p, %x allocated for zone\n", zonemem, *size);
    return zonemem;
}

void I_PrintBanner(const char *msg)
{
    int spaces = 35 - (int)(strlen(msg) / 2);
    for (int i = 0; i < spaces; ++i) putchar(' ');
    puts(msg);
}

void I_PrintDivider(void)
{
    for (int i = 0; i < 75; ++i) putchar('=');
    putchar('\n');
}

void I_PrintStartupBanner(const char *gamedescription)
{
    I_PrintDivider();
    I_PrintBanner(gamedescription);
    I_PrintDivider();
    printf(
    " " PACKAGE_NAME " is free software, covered by the GNU General Public\n"
    " License.  There is NO warranty; not even for MERCHANTABILITY or\n"
    " FITNESS FOR A PARTICULAR PURPOSE.\n");
    I_PrintDivider();
}

boolean I_ConsoleStdout(void)
{
    return true;
}

void I_BindVariables(void)
{
    I_BindInputVariables();
    I_BindVideoVariables();
    I_BindJoystickVariables();
    I_BindSoundVariables();
}

void I_Quit(void)
{
    atexit_listentry_t *entry = exit_funcs;
    while (entry != NULL) {
        entry->func();
        entry = entry->next;
    }
    VK_CALL(exit, 0);
    __builtin_unreachable();
}

static boolean already_quitting = false;

void I_Error(const char *error, ...)
{
    char msgbuf[512];
    va_list argptr;

    if (already_quitting) {
        fprintf(stderr, "Warning: recursive call to I_Error detected.\n");
        VK_CALL(exit, -1);
        __builtin_unreachable();
    }
    already_quitting = true;

    va_start(argptr, error);
    vfprintf(stderr, error, argptr);
    fprintf(stderr, "\n\n");
    va_end(argptr);

    va_start(argptr, error);
    memset(msgbuf, 0, sizeof(msgbuf));
    M_vsnprintf(msgbuf, sizeof(msgbuf), error, argptr);
    va_end(argptr);

    /* Also print to vkernel console */
    VK_CALL(puts, "\nDOOM ERROR: ");
    VK_CALL(puts, msgbuf);
    VK_CALL(puts, "\n");

    atexit_listentry_t *entry = exit_funcs;
    while (entry != NULL) {
        if (entry->run_on_error)
            entry->func();
        entry = entry->next;
    }

    VK_CALL(exit, -1);
    __builtin_unreachable();
}

void *I_Realloc(void *ptr, size_t size)
{
    void *new_ptr = realloc(ptr, size);
    if (size != 0 && new_ptr == NULL)
        I_Error("I_Realloc: failed on reallocation of %zu bytes", size);
    return new_ptr;
}

/* Read access violation emulation for demo compatibility */
#define DOS_MEM_DUMP_SIZE 10
static const unsigned char mem_dump_dos622[DOS_MEM_DUMP_SIZE] = {
    0x57, 0x92, 0x19, 0x00, 0xF4, 0x06, 0x70, 0x00, 0x16, 0x00 };
static const unsigned char *dos_mem_dump = mem_dump_dos622;

boolean I_GetMemoryValue(unsigned int offset, void *value, int size)
{
    if (offset + (unsigned)size > DOS_MEM_DUMP_SIZE)
        return false;

    switch (size) {
    case 1:
        *(unsigned char *)value = dos_mem_dump[offset];
        return true;
    case 2:
        *(unsigned short *)value = dos_mem_dump[offset]
                                 | (dos_mem_dump[offset + 1] << 8);
        return true;
    case 4:
        *(unsigned int *)value = dos_mem_dump[offset]
                               | (dos_mem_dump[offset + 1] << 8)
                               | (dos_mem_dump[offset + 2] << 16)
                               | (dos_mem_dump[offset + 3] << 24);
        return true;
    }
    return false;
}
