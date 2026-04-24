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

// I'm too lazy to write a proper header for the userspace side, so this header is shared by both kernel and userspace.
// To avoid including kernel-only definitions in userspace, the API stubs and internal types are defined in kernel_api.cpp instead of here.
#define VK_CALL(fn, ...) (vk_get_api()->vk_##fn(__VA_ARGS__))

/* ============================================================
 * vkernel-specific APIs (not provided by any C standard library)
 * ============================================================ */

/* Block until the task with the given id exits. */
static inline void vk_wait_task(vk_i64 task_id) {
    if (vk_get_api()->vk_wait_task)
        vk_get_api()->vk_wait_task(task_id);
}

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* VK_USERSPACE_H */