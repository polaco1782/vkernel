/*
 * vkernel - UEFI Microkernel
 * Copyright (C) 2026 vkernel authors
 *
 * vk.h - Userspace C runtime for vkernel
 *
 * Include this single header and call vk_init(api) at the top of
 * _start().  After that, use the vk_* functions exactly like their
 * POSIX / libc counterparts.
 *
 * Example:
 *
 *   #include <vk.h>
 *
 *   int _start(const vk_api_t* api) {
 *       vk_init(api);
 *       vk_puts("Hello, world!\n");
 *       void* p = vk_malloc(64);
 *       vk_free(p);
 *       return 0;
 *   }
 */

#ifndef VK_H
#define VK_H

#include "vkernel/userapi.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ============================================================
 * Internal: stored API pointer
 * ============================================================ */

static const vk_api_t* __vk_api;

static inline void vk_init(const vk_api_t* api) {
    __vk_api = api;
}

/* ============================================================
 * Console output
 * ============================================================ */

static inline void vk_puts(const char* s) {
    __vk_api->puts(s);
}

static inline void vk_putc(char c) {
    __vk_api->putc(c);
}

static inline void vk_put_hex(vk_u64 v) {
    __vk_api->put_hex(v);
}

static inline void vk_put_dec(vk_u64 v) {
    __vk_api->put_dec(v);
}

static inline void vk_clear(void) {
    __vk_api->clear();
}

/* ============================================================
 * Console input
 * ============================================================ */

/* Block until a character is available */
static inline char vk_getc(void) {
    return __vk_api->getc();
}

/* Non-blocking: returns '\0' if nothing available */
static inline char vk_try_getc(void) {
    return __vk_api->try_getc();
}

/* ============================================================
 * Memory
 * ============================================================ */

static inline void* vk_malloc(vk_usize size) {
    return __vk_api->malloc(size);
}

static inline void vk_free(void* ptr) {
    __vk_api->free(ptr);
}

static inline void* vk_memset(void* dest, int c, vk_usize n) {
    return __vk_api->memset(dest, c, n);
}

static inline void* vk_memcpy(void* dest, const void* src, vk_usize n) {
    return __vk_api->memcpy(dest, src, n);
}

/* ============================================================
 * Filesystem (ramfs)
 * ============================================================ */

/* Returns non-zero if the file exists in ramfs */
static inline int vk_file_exists(const char* name) {
    return __vk_api->file_exists(name);
}

/* Returns the file size in bytes, or 0 if not found */
static inline vk_usize vk_file_size(const char* name) {
    return __vk_api->file_size(name);
}

/*
 * Read the entire file into buf (up to buf_size bytes).
 * Returns the number of bytes actually read, or 0 on failure.
 */
static inline vk_usize vk_file_read(const char* name, void* buf, vk_usize buf_size) {
    return __vk_api->file_read(name, buf, buf_size);
}

/* ============================================================
 * Process control
 * ============================================================ */

/* Terminate immediately with exit code */
static inline void vk_exit(int code) {
    __vk_api->exit(code);
    /* Should never return, but prevent compiler warnings */
    __builtin_unreachable();
}

/* Yield the CPU to other tasks */
static inline void vk_yield(void) {
    __vk_api->yield();
}

/* ============================================================
 * Convenience helpers (built on top of the primitives above)
 * ============================================================ */

/* Print a signed integer */
static inline void vk_print_int(int n) {
    if (n < 0) {
        vk_putc('-');
        n = -n;
    }
    vk_put_dec((vk_u64)(unsigned int)n);
}

/* Simple strlen */
static inline vk_usize vk_strlen(const char* s) {
    vk_usize len = 0;
    while (s[len]) ++len;
    return len;
}

/* Read a line into buf (up to max-1 chars). Returns length. */
static inline vk_usize vk_getline(char* buf, vk_usize max) {
    vk_usize pos = 0;
    while (pos < max - 1) {
        char c = vk_getc();
        if (c == '\r' || c == '\n') {
            vk_putc('\n');
            break;
        }
        if ((c == 0x7F || c == '\b') && pos > 0) {
            --pos;
            vk_puts("\b \b");
            continue;
        }
        if (c >= ' ' && c < 0x7F) {
            buf[pos++] = c;
            vk_putc(c);
        }
    }
    buf[pos] = '\0';
    return pos;
}

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* VK_H */
