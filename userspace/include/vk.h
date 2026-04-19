/*
 * vkernel userspace - kernel API header
 * Copyright (C) 2026 vkernel authors
 *
 * vk.h - Thin wrapper around the canonical kernel ABI header.
 *
 * With newlib providing the standard C library, this header only
 * exposes vkernel-specific APIs that have no C standard equivalent.
 * Standard functions (printf, malloc, memcpy, strlen, …) come from
 * newlib's <stdio.h>, <stdlib.h>, <string.h>, etc.
 */

#ifndef VK_USERSPACE_H
#define VK_USERSPACE_H

#include "../../include/vkernel/vk.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ============================================================
 * vkernel-specific APIs (not provided by any C standard library)
 * ============================================================ */

/* Block until the task with the given id exits. */
static inline void vk_wait_task(vk_i64 task_id) {
    if (vk_get_api()->vk_wait_task)
        vk_get_api()->vk_wait_task(task_id);
}

/* ============================================================
 * Convenience: read a line from the console into buf[].
 * Returns the number of characters stored (excluding NUL).
 * ============================================================ */
static inline int vk_getline(char* buf, int capacity) {
    int i = 0;
    while (i < capacity - 1) {
        char c = vk_getc();
        if (c == '\r' || c == '\n') break;
        if (c == '\b' || c == 127) { if (i > 0) --i; continue; }
        buf[i++] = c;
    }
    buf[i] = '\0';
    return i;
}

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* VK_USERSPACE_H */