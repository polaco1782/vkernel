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
    /* 1. Store the kernel API pointer for all translation units. */
    _vk_api_ptr = api;

    /* 2. Run global constructors (C++ static init, newlib internals). */
    __libc_init_array();

    /* 3. Call the user program.  No argc/argv support yet. */
    int ret = main(0, (char**)0);

    /* 4. Run global destructors. */
    __libc_fini_array();

    /* 5. Terminate the process via the kernel. */
    VK_CALL(exit, ret);

    /* Never reached — silence compiler warnings. */
    return ret;
}
