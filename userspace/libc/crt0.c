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

#include "../../include/vkernel/vk.h"

/* Provided by the user program */
extern int main(int argc, char** argv);

/*
 * newlib constructor / destructor arrays.
 * __libc_init_array walks .preinit_array, .init, .init_array.
 * __libc_fini_array walks .fini_array, .fini.
 * These are defined in newlib's libc/misc/init.c and fini.c.
 */
extern void __libc_init_array(void);
extern void __libc_fini_array(void);

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
    vk_exit(ret);

    /* Never reached — silence compiler warnings. */
    return ret;
}
