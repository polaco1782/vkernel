/*
 * vkernel userspace - hello world
 * Copyright (C) 2026 vkernel authors
 *
 * hello.c - Minimal freestanding ELF64 program for vkernel
 *
 * Build: see Makefile (Linux) or hello.vcxproj (Visual Studio).
 * Run:   vk> run hello.elf
 */

#include "../include/vk.h"

int _start(const vk_api_t* api) {
    vk_init(api);

    printf("+---------------------------------+\n");
    printf("|   Hello from vkernel userspace! |\n");
    printf("+---------------------------------+\n");
    printf("\n");
    printf("  Kernel API version : %llu\n", (unsigned long long)api->api_version);
    printf("  Architecture       : x86-64\n");
    printf("  Loader             : vkernel ELF64\n");
    printf("\n");

    /* Test memory allocation */
    printf("  Allocating 128 bytes... ");
    void* p = malloc(128);
    if (p) {
        printf("OK at %p\n", p);
        memset(p, 0xAB, 128);
        free(p);
        printf("  Freed.\n");
    } else {
        printf("FAILED\n");
    }

    FILE *f = fopen("hello.elf", "r");
    if (f) {
        printf("  fopen hello.elf: success\n");
        fclose(f);
    } else {
        printf("  fopen hello.elf: failed\n");
    }

    for(int i = 0; i < 10; i++) {
        printf("  Tick %d\n", i);
        vk_sleep(100); /* Sleep for 100 ticks (1 second) */
    }

    // execute a bad opcode to test error handling
    // Note: this will crash the process, but it's a good sanity check that the kernel doesn't just hang on invalid instructions.
    // printf("  Executing invalid instruction... ");
    ((void(*)())0)();
        
    printf("\nGoodbye!\n");
    return 0;
}
