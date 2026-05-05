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

static inline int vk_terminate_task(vk_u64 task_id) {
    if (vk_get_api()->vk_terminate_task)
        return vk_get_api()->vk_terminate_task(task_id);
    return 0;
}

static inline vk_usize vk_json_copy_escaped(char* out, vk_usize out_cap, vk_usize pos, const char* s) {
    vk_usize i = 0;
    if (!s) return pos;
    while (s[i] && pos + 1 < out_cap) {
        char ch = s[i++];
        if (ch == '\"' || ch == '\\') {
            if (pos + 2 >= out_cap) break;
            out[pos++] = '\\';
            out[pos++] = ch;
        } else if (ch == '\n') {
            if (pos + 2 >= out_cap) break;
            out[pos++] = '\\';
            out[pos++] = 'n';
        } else if (ch == '\r') {
            if (pos + 2 >= out_cap) break;
            out[pos++] = '\\';
            out[pos++] = 'r';
        } else if (ch == '\t') {
            if (pos + 2 >= out_cap) break;
            out[pos++] = '\\';
            out[pos++] = 't';
        } else {
            out[pos++] = ch;
        }
    }
    out[pos] = '\0';
    return pos;
}

static inline vk_usize vk_kobj_rpc_json(const char* req_json, char* out, vk_usize out_cap) {
    if (vk_get_api()->vk_kobj_rpc)
        return vk_get_api()->vk_kobj_rpc(req_json, out, out_cap);
    if (out && out_cap > 0)
        out[0] = '\0';
    return 0;
}

static inline void vk_kobj_rpc_print_json(const char* req_json) {
    char out[1536];
    vk_kobj_rpc_json(req_json, out, sizeof(out));
    VK_CALL(puts, out);
    VK_CALL(putc, '\n');
}

static inline void vk_kobj_cmd_json(const char* op) {
    char req[96];
    vk_usize pos = 0;
    req[pos++] = '{';
    req[pos++] = '\"'; req[pos++] = 'o'; req[pos++] = 'p'; req[pos++] = '\"';
    req[pos++] = ':'; req[pos++] = '\"';
    pos = vk_json_copy_escaped(req, sizeof(req), pos, op);
    req[pos++] = '\"'; req[pos++] = '}';
    req[pos] = '\0';
    vk_kobj_rpc_print_json(req);
}

static inline void vk_kobj_named_cmd_json(const char* op, const char* name) {
    char req[192];
    vk_usize pos = 0;
    req[pos++] = '{';
    req[pos++] = '\"'; req[pos++] = 'o'; req[pos++] = 'p'; req[pos++] = '\"';
    req[pos++] = ':'; req[pos++] = '\"';
    pos = vk_json_copy_escaped(req, sizeof(req), pos, op);
    req[pos++] = '\"';
    req[pos++] = ','; req[pos++] = '\"'; req[pos++] = 'n'; req[pos++] = 'a'; req[pos++] = 'm'; req[pos++] = 'e'; req[pos++] = '\"';
    req[pos++] = ':'; req[pos++] = '\"';
    pos = vk_json_copy_escaped(req, sizeof(req), pos, name);
    req[pos++] = '\"'; req[pos++] = '}';
    req[pos] = '\0';
    vk_kobj_rpc_print_json(req);
}

static inline void vk_kobj_get_json(const char* path) {
    char req[256];
    vk_usize pos = 0;
    req[pos++] = '{';
    req[pos++] = '\"'; req[pos++] = 'o'; req[pos++] = 'p'; req[pos++] = '\"';
    req[pos++] = ':'; req[pos++] = '\"'; req[pos++] = 'g'; req[pos++] = 'e'; req[pos++] = 't'; req[pos++] = '\"';
    req[pos++] = ','; req[pos++] = '\"'; req[pos++] = 'p'; req[pos++] = 'a'; req[pos++] = 't'; req[pos++] = 'h'; req[pos++] = '\"';
    req[pos++] = ':'; req[pos++] = '\"';
    pos = vk_json_copy_escaped(req, sizeof(req), pos, path);
    req[pos++] = '\"'; req[pos++] = '}';
    req[pos] = '\0';
    vk_kobj_rpc_print_json(req);
}

static inline void vk_kobj_set_json(const char* path, const char* value) {
    char req[512];
    vk_usize pos = 0;
    req[pos++] = '{';
    req[pos++] = '\"'; req[pos++] = 'o'; req[pos++] = 'p'; req[pos++] = '\"';
    req[pos++] = ':'; req[pos++] = '\"'; req[pos++] = 's'; req[pos++] = 'e'; req[pos++] = 't'; req[pos++] = '\"';
    req[pos++] = ','; req[pos++] = '\"'; req[pos++] = 'p'; req[pos++] = 'a'; req[pos++] = 't'; req[pos++] = 'h'; req[pos++] = '\"';
    req[pos++] = ':'; req[pos++] = '\"';
    pos = vk_json_copy_escaped(req, sizeof(req), pos, path);
    req[pos++] = '\"';
    req[pos++] = ','; req[pos++] = '\"'; req[pos++] = 'v'; req[pos++] = 'a'; req[pos++] = 'l'; req[pos++] = 'u'; req[pos++] = 'e'; req[pos++] = '\"';
    req[pos++] = ':'; req[pos++] = '\"';
    pos = vk_json_copy_escaped(req, sizeof(req), pos, value);
    req[pos++] = '\"'; req[pos++] = '}';
    req[pos] = '\0';
    vk_kobj_rpc_print_json(req);
}

static inline void vk_kobj_ls_json(const char* path) {
    char req[256];
    vk_usize pos = 0;
    req[pos++] = '{';
    req[pos++] = '\"'; req[pos++] = 'o'; req[pos++] = 'p'; req[pos++] = '\"';
    req[pos++] = ':'; req[pos++] = '\"'; req[pos++] = 'l'; req[pos++] = 's'; req[pos++] = '\"';
    req[pos++] = ','; req[pos++] = '\"'; req[pos++] = 'p'; req[pos++] = 'a'; req[pos++] = 't'; req[pos++] = 'h'; req[pos++] = '\"';
    req[pos++] = ':'; req[pos++] = '\"';
    pos = vk_json_copy_escaped(req, sizeof(req), pos, path);
    req[pos++] = '\"'; req[pos++] = '}';
    req[pos] = '\0';
    vk_kobj_rpc_print_json(req);
}

static inline void vk_kobj_describe_json(const char* path) {
    char req[256];
    vk_usize pos = 0;
    req[pos++] = '{';
    req[pos++] = '\"'; req[pos++] = 'o'; req[pos++] = 'p'; req[pos++] = '\"';
    req[pos++] = ':'; req[pos++] = '\"'; req[pos++] = 'd'; req[pos++] = 'e'; req[pos++] = 's'; req[pos++] = 'c'; req[pos++] = 'r'; req[pos++] = 'i'; req[pos++] = 'b'; req[pos++] = 'e'; req[pos++] = '\"';
    req[pos++] = ','; req[pos++] = '\"'; req[pos++] = 'p'; req[pos++] = 'a'; req[pos++] = 't'; req[pos++] = 'h'; req[pos++] = '\"';
    req[pos++] = ':'; req[pos++] = '\"';
    pos = vk_json_copy_escaped(req, sizeof(req), pos, path);
    req[pos++] = '\"'; req[pos++] = '}';
    req[pos] = '\0';
    vk_kobj_rpc_print_json(req);
}

static inline void vk_kobj_ls_text(const char* path) {
    char req[256];
    char out[1024];
    vk_usize pos = 0;
    req[pos++] = '{';
    req[pos++] = '\"'; req[pos++] = 'o'; req[pos++] = 'p'; req[pos++] = '\"';
    req[pos++] = ':'; req[pos++] = '\"'; req[pos++] = 'l'; req[pos++] = 's'; req[pos++] = '_'; req[pos++] = 't'; req[pos++] = 'e'; req[pos++] = 'x'; req[pos++] = 't'; req[pos++] = '\"';
    req[pos++] = ','; req[pos++] = '\"'; req[pos++] = 'p'; req[pos++] = 'a'; req[pos++] = 't'; req[pos++] = 'h'; req[pos++] = '\"';
    req[pos++] = ':'; req[pos++] = '\"';
    pos = vk_json_copy_escaped(req, sizeof(req), pos, path);
    req[pos++] = '\"'; req[pos++] = '}';
    req[pos] = '\0';
    vk_kobj_rpc_json(req, out, sizeof(out));
    VK_CALL(puts, out);
}

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* VK_USERSPACE_H */
