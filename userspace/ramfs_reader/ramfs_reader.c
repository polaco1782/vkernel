/*
 * vkernel userspace - ramfs reader demo
 * Copyright (C) 2026 vkernel authors
 *
 * ramfs_reader.c - Freestanding file-read example
 *
 * Build: see Makefile (Linux) or ramfs_reader.vcxproj (Visual Studio).
 * Run:   vk> run ramfs_reader.elf
 */

#include "../include/vk.h"

int _start(const vk_api_t* api) {
    vk_init(api);

    printf("Ramfs reader demo\n");
    printf("  Kernel API version : %llu\n", (unsigned long long)api->api_version);
    printf("  Opening hello.elf from ramfs...\n");

    FILE* file = fopen("hello.elf", "rb");
    if (file == NULL) {
        printf("  fopen failed\n");
        return 1;
    }

    unsigned char header[16];
    size_t bytes_read = fread(header, 1, sizeof(header), file);
    fclose(file);

    printf("  Read %zu bytes\n", bytes_read);
    printf("  ELF magic: ");
    for (size_t i = 0; i < bytes_read && i < 4; ++i) {
        printf("%X ", header[i]);
    }
    printf("\n");

    if (bytes_read >= 4 &&
        header[0] == 0x7F &&
        header[1] == 'E' &&
        header[2] == 'L' &&
        header[3] == 'F') {
        printf("  Looks like a valid ELF binary.\n");
    } else {
        printf("  Unexpected file contents.\n");
    }

    return 0;
}
