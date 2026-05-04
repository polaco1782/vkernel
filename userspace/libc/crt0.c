/*
 * vkernel userspace - C runtime startup
 * Copyright (C) 2026 vkernel authors
 *
 * crt0.c - Entry point that bridges the vkernel ABI to standard main().
 *
 * The kernel calls _start(const vk_api_t* api).  We store the API
 * pointer, perform minimal C runtime initialization (newlib init_array),
 * then call the user's main().
 */

#include "../include/vk.h"

/* Provided by the user program */
extern int main(int argc, char** argv);

/*
 * newlib constructor / destructor arrays.
 * __libc_init_array walks .preinit_array, .init, .init_array.
 * __libc_fini_array walks .fini_array, .fini.
 * These are defined in newlib's libc/misc/init.c and fini.c.
 *
 * When building against a bare-metal newlib (x86_64-elf) that does not
 * ship crtbegin/crtend objects, these symbols may be absent from libc.a.
 * We provide our own implementations that walk the ELF sections directly.
 */

typedef void (*_func_ptr)(void);

extern _func_ptr __preinit_array_start[] __attribute__((weak));
extern _func_ptr __preinit_array_end[]   __attribute__((weak));
extern _func_ptr __init_array_start[]    __attribute__((weak));
extern _func_ptr __init_array_end[]      __attribute__((weak));
extern _func_ptr __fini_array_start[]    __attribute__((weak));
extern _func_ptr __fini_array_end[]      __attribute__((weak));

#define VK_CMDLINE_MAX 256
#define VK_ARGV_MAX    32

static int is_ascii_space(char ch)
{
    return ch == ' ' || ch == '\t' || ch == '\r' || ch == '\n';
}

static int parse_argv(char* cmdline, char** argv, int max_args)
{
    int argc = 0;
    char* read = cmdline;

    while (*read != '\0' && argc + 1 < max_args) {
        while (is_ascii_space(*read))
            ++read;
        if (*read == '\0')
            break;

        char* token = read;
        char* write = read;
        char quote = '\0';

        while (*read != '\0') {
            char ch = *read;

            if (quote != '\0') {
                if (ch == quote) {
                    quote = '\0';
                    ++read;
                    continue;
                }

                if (ch == '\\' && read[1] == quote) {
                    *write++ = quote;
                    read += 2;
                    continue;
                }

                *write++ = *read++;
                continue;
            }

            if (ch == '"' || ch == '\'') {
                quote = ch;
                ++read;
                continue;
            }

            if (is_ascii_space(ch))
                break;

            *write++ = *read++;
        }

        *write = '\0';
        argv[argc++] = token;

        while (is_ascii_space(*read))
            ++read;
    }

    argv[argc] = (char*)0;
    return argc;
}

void __libc_init_array(void)
{
    if (__preinit_array_start) {
        for (_func_ptr *f = __preinit_array_start; f < __preinit_array_end; ++f)
            (*f)();
    }
    if (__init_array_start) {
        for (_func_ptr *f = __init_array_start; f < __init_array_end; ++f)
            (*f)();
    }
}

void __libc_fini_array(void)
{
    if (__fini_array_start) {
        for (_func_ptr *f = __fini_array_end - 1; f >= __fini_array_start; --f)
            (*f)();
    }
}

/*
 * _start — true entry point for every vkernel userspace binary.
 *
 * The kernel passes the API table pointer in the first argument
 * register (RDI on System V, RCX on MSVC x64).
 */
int _start(const vk_api_t* api)
{
    char cmdline[VK_CMDLINE_MAX] = {0};
    char* argv[VK_ARGV_MAX] = {0};
    int argc = 0;

    /* 1. Store the kernel API pointer for all translation units. */
    _vk_api_ptr = api;

    /* 2. Run global constructors (C++ static init, newlib internals). */
    __libc_init_array();

    /* 3. Build argc/argv from the command line provided by the kernel. */
    if (api != (const vk_api_t*)0 && api->vk_get_cmdline != 0) {
        api->vk_get_cmdline(cmdline, (vk_usize)sizeof(cmdline));
        argc = parse_argv(cmdline, argv, VK_ARGV_MAX);
    }

    int ret = main(argc, argv);

    /* 4. Run global destructors. */
    __libc_fini_array();

    /* 5. Terminate the process via the kernel. */
    VK_CALL(exit, ret);

    /* Never reached — silence compiler warnings. */
    return ret;
}
