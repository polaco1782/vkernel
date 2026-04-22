/*
 * vkernel userspace - ramfs reader demo (C++ / newlib runtime)
 * Copyright (C) 2026 vkernel authors
 *
 * ramfs_reader.cpp - Reads a file from ramfs using newlib stdio.
 * I/O is backed by the libvksys syscall glue (open/read/write/close).
 */

#include <stdio.h>
#include <stdint.h>

class File {
    private:
        FILE* f;
       
    public:
        File(const char* path) : f(nullptr) {
            f = fopen(path, "rb");
            if (!f) {
                fprintf(stderr, "Error: cannot open '%s'\n", path);
            }
        }
};

int main() {
    puts("Ramfs reader demo (C++ + newlib)\n");
    puts("  Opening hello.elf from ramfs...\n");

    FILE* f = fopen("hello.elf", "rb");
    if (!f) {
        puts("  fopen failed\n");
        return 1;
    }

    uint8_t header[16];
    const size_t bytes_read = fread(header, 1, sizeof(header), f);
    fclose(f);

    printf("  Read %zu bytes\n", bytes_read);
    printf("  ELF magic:");
    for (size_t i = 0; i < bytes_read && i < 4; ++i)
        printf(" %02x", header[i]);
    puts("\n");

    if (bytes_read >= 4 &&
        header[0] == 0x7F &&
        header[1] == 'E'  &&
        header[2] == 'L'  &&
        header[3] == 'F') {
        puts("  Looks like a valid ELF binary.\n");
    } else {
        puts("  Unexpected file contents.\n");
    }

    return 0;
}
