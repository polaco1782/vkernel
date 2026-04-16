/*
 * vkernel userspace - hello world
 * Copyright (C) 2026 vkernel authors
 *
 * hello.c - Minimal freestanding ELF64 program for vkernel
 *
 * Build: see Makefile in this directory.
 * Run:   vk> run hello.elf
 */

#include <vk.h>

int _start(const vk_api_t* api) {
    vk_init(api);

    vk_puts("+---------------------------------+\n");
    vk_puts("|   Hello from vkernel userspace! |\n");
    vk_puts("+---------------------------------+\n");
    vk_puts("\n");
    vk_puts("  Kernel API version : ");
    vk_print_int((int)__vk_api->api_version);
    vk_puts("\n");
    vk_puts("  Architecture       : x86-64\n");
    vk_puts("  Loader             : vkernel ELF64\n");
    vk_puts("\n");

    /* Test memory allocation */
    vk_puts("  Allocating 128 bytes... ");
    void* p = vk_malloc(128);
    if (p) {
        vk_puts("OK at 0x");
        vk_put_hex((vk_u64)(unsigned long)p);
        vk_puts("\n");
        vk_memset(p, 0xAB, 128);
        vk_free(p);
        vk_puts("  Freed.\n");
    } else {
        vk_puts("FAILED\n");
    }

    /* Test file access */
    vk_puts("  Checking hello.elf in ramfs... ");
    if (vk_file_exists("hello.elf")) {
        vk_puts("found, ");
        vk_put_dec(vk_file_size("hello.elf"));
        vk_puts(" bytes\n");
    } else {
        vk_puts("not found\n");
    }

    vk_puts("\nGoodbye!\n");
    return 0;
}
