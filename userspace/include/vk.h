/*
 * vkernel userspace - kernel API header
 * Copyright (C) 2026 vkernel authors
 *
 * vk.h - Thin wrapper around the canonical kernel ABI header.
 *
 * With newlib providing the standard C library, this header only
 * exposes vkernel-specific APIs that have no C standard equivalent.
 * Standard functions (printf, malloc, memcpy, strlen, ...) come from
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

static inline int vk_exec_cmdline(const char* command_line) {
    if (vk_get_api()->vk_exec_cmdline)
        return vk_get_api()->vk_exec_cmdline(command_line);
    return -1;
}

static inline vk_usize vk_json_copy_escaped(char* out, vk_usize out_cap, vk_usize pos, const char* s) {
    vk_usize i = 0;
    if (!s) return pos;
    while (s[i] && pos + 1 < out_cap) {
        char ch = s[i++];
        if (ch == '"' || ch == '\\') {
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

static inline vk_usize vk_json_copy_literal(char* out, vk_usize out_cap, vk_usize pos, const char* s) {
    vk_usize i = 0;
    if (!out || out_cap == 0 || !s) return pos;
    while (s[i] && pos + 1 < out_cap) {
        out[pos++] = s[i++];
    }
    out[pos] = '\0';
    return pos;
}

static inline int vk_json_is_space(char ch) {
    return ch == ' ' || ch == '\t' || ch == '\n' || ch == '\r';
}

static inline const char* vk_json_skip_ws(const char* s) {
    if (!s) return 0;
    while (vk_json_is_space(*s))
        ++s;
    return s;
}

static inline int vk_json_key_matches(const char* begin, const char* end, const char* key) {
    while (begin < end && key && *key) {
        if (*begin == '\\' || *begin != *key)
            return 0;
        ++begin;
        ++key;
    }
    return begin == end && key && *key == '\0';
}

static inline const char* vk_json_find_field(const char* json, const char* key) {
    const char* cur = json;

    if (!json || !key)
        return 0;

    while (*cur) {
        const char* token_begin;
        const char* token_end;

        cur = vk_json_skip_ws(cur);
        if (!cur || *cur == '\0')
            break;
        if (*cur != '"') {
            ++cur;
            continue;
        }

        token_begin = cur + 1;
        token_end = token_begin;
        while (*token_end && *token_end != '"') {
            if (*token_end == '\\' && token_end[1] != '\0')
                ++token_end;
            ++token_end;
        }
        if (*token_end == '\0')
            return 0;

        cur = vk_json_skip_ws(token_end + 1);
        if (cur && *cur == ':' && vk_json_key_matches(token_begin, token_end, key))
            return vk_json_skip_ws(cur + 1);

        cur = token_end + 1;
    }

    return 0;
}

static inline int vk_json_copy_string_token(const char* json,
                                            char* out,
                                            vk_usize out_cap,
                                            const char** out_next) {
    vk_usize pos = 0;

    if (out_next)
        *out_next = 0;
    if (!json || *json != '"' || !out || out_cap == 0)
        return 0;

    ++json;
    while (*json) {
        char ch = *json++;
        if (ch == '"') {
            out[pos] = '\0';
            if (out_next)
                *out_next = json;
            return 1;
        }
        if (ch == '\\' && *json != '\0') {
            char esc = *json++;
            if (esc == 'n') ch = '\n';
            else if (esc == 'r') ch = '\r';
            else if (esc == 't') ch = '\t';
            else ch = esc;
        }
        if (pos + 1 < out_cap)
            out[pos++] = ch;
    }

    out[pos] = '\0';
    return 0;
}

static inline int vk_json_extract_string_field(const char* json,
                                               const char* key,
                                               char* out,
                                               vk_usize out_cap) {
    const char* value = vk_json_find_field(json, key);
    return vk_json_copy_string_token(value, out, out_cap, 0);
}

static inline int vk_json_extract_bool_field(const char* json,
                                             const char* key,
                                             int* out_value) {
    const char* value = vk_json_find_field(json, key);

    if (!value || !out_value)
        return 0;

    if (value[0] == 't' && value[1] == 'r' && value[2] == 'u' && value[3] == 'e'
            && (value[4] == '\0' || value[4] == ',' || value[4] == '}' || value[4] == ']' || vk_json_is_space(value[4]))) {
        *out_value = 1;
        return 1;
    }
    if (value[0] == 'f' && value[1] == 'a' && value[2] == 'l' && value[3] == 's' && value[4] == 'e'
            && (value[5] == '\0' || value[5] == ',' || value[5] == '}' || value[5] == ']' || vk_json_is_space(value[5]))) {
        *out_value = 0;
        return 1;
    }

    return 0;
}

static inline int vk_json_extract_string_array_field(const char* json,
                                                     const char* key,
                                                     char* items,
                                                     vk_usize item_stride,
                                                     int max_items) {
    const char* cur = vk_json_find_field(json, key);
    int count = 0;

    if (!cur || !items || item_stride == 0 || max_items <= 0 || *cur != '[')
        return 0;

    ++cur;
    while (count < max_items) {
        cur = vk_json_skip_ws(cur);
        if (!cur || *cur == '\0' || *cur == ']')
            return count;
        if (*cur != '"')
            break;

        if (!vk_json_copy_string_token(cur,
                                       items + ((vk_usize)count * item_stride),
                                       item_stride,
                                       &cur)) {
            break;
        }

        ++count;
        cur = vk_json_skip_ws(cur);
        if (!cur || *cur == '\0' || *cur == ']')
            return count;
        if (*cur != ',')
            break;
        ++cur;
    }

    return count;
}

static inline int vk_kobj_response_ok(const char* json) {
    int ok = 0;
    return vk_json_extract_bool_field(json, "ok", &ok) && ok;
}

static inline vk_usize vk_json_append_string_field(char* out,
                                                   vk_usize out_cap,
                                                   vk_usize pos,
                                                   const char* key,
                                                   const char* value,
                                                   int* has_fields) {
    if (has_fields && *has_fields)
        pos = vk_json_copy_literal(out, out_cap, pos, ",");
    if (has_fields)
        *has_fields = 1;

    pos = vk_json_copy_literal(out, out_cap, pos, "\"");
    pos = vk_json_copy_literal(out, out_cap, pos, key ? key : "");
    pos = vk_json_copy_literal(out, out_cap, pos, "\":\"");
    pos = vk_json_copy_escaped(out, out_cap, pos, value ? value : "");
    pos = vk_json_copy_literal(out, out_cap, pos, "\"");
    return pos;
}

static inline vk_usize vk_kobj_build_op_request(char* out, vk_usize out_cap, const char* op) {
    vk_usize pos = 0;
    int has_fields = 0;

    if (out && out_cap > 0)
        out[0] = '\0';

    pos = vk_json_copy_literal(out, out_cap, pos, "{");
    pos = vk_json_append_string_field(out, out_cap, pos, "op", op, &has_fields);
    pos = vk_json_copy_literal(out, out_cap, pos, "}");
    return pos;
}

static inline vk_usize vk_kobj_build_named_request(char* out,
                                                   vk_usize out_cap,
                                                   const char* op,
                                                   const char* name) {
    vk_usize pos = 0;
    int has_fields = 0;

    if (out && out_cap > 0)
        out[0] = '\0';

    pos = vk_json_copy_literal(out, out_cap, pos, "{");
    pos = vk_json_append_string_field(out, out_cap, pos, "op", op, &has_fields);
    pos = vk_json_append_string_field(out, out_cap, pos, "name", name, &has_fields);
    pos = vk_json_copy_literal(out, out_cap, pos, "}");
    return pos;
}

static inline vk_usize vk_kobj_build_path_request(char* out,
                                                  vk_usize out_cap,
                                                  const char* op,
                                                  const char* path) {
    vk_usize pos = 0;
    int has_fields = 0;

    if (out && out_cap > 0)
        out[0] = '\0';

    pos = vk_json_copy_literal(out, out_cap, pos, "{");
    pos = vk_json_append_string_field(out, out_cap, pos, "op", op, &has_fields);
    pos = vk_json_append_string_field(out, out_cap, pos, "path", path, &has_fields);
    pos = vk_json_copy_literal(out, out_cap, pos, "}");
    return pos;
}

static inline vk_usize vk_kobj_build_path_value_request(char* out,
                                                        vk_usize out_cap,
                                                        const char* op,
                                                        const char* path,
                                                        const char* value) {
    vk_usize pos = 0;
    int has_fields = 0;

    if (out && out_cap > 0)
        out[0] = '\0';

    pos = vk_json_copy_literal(out, out_cap, pos, "{");
    pos = vk_json_append_string_field(out, out_cap, pos, "op", op, &has_fields);
    pos = vk_json_append_string_field(out, out_cap, pos, "path", path, &has_fields);
    pos = vk_json_append_string_field(out, out_cap, pos, "value", value, &has_fields);
    pos = vk_json_copy_literal(out, out_cap, pos, "}");
    return pos;
}

static inline vk_usize vk_kobj_rpc_json(const char* req_json, char* out, vk_usize out_cap) {
    if (vk_get_api()->vk_kobj_rpc)
        return vk_get_api()->vk_kobj_rpc(req_json, out, out_cap);
    if (out && out_cap > 0)
        out[0] = '\0';
    return 0;
}

static inline vk_usize vk_kobj_rpc_op_json(const char* op, char* out, vk_usize out_cap) {
    char req[96];
    vk_kobj_build_op_request(req, sizeof(req), op);
    return vk_kobj_rpc_json(req, out, out_cap);
}

static inline vk_usize vk_kobj_rpc_named_json(const char* op,
                                              const char* name,
                                              char* out,
                                              vk_usize out_cap) {
    char req[192];
    vk_kobj_build_named_request(req, sizeof(req), op, name);
    return vk_kobj_rpc_json(req, out, out_cap);
}

static inline vk_usize vk_kobj_rpc_path_json(const char* op,
                                             const char* path,
                                             char* out,
                                             vk_usize out_cap) {
    char req[256];
    vk_kobj_build_path_request(req, sizeof(req), op, path);
    return vk_kobj_rpc_json(req, out, out_cap);
}

static inline vk_usize vk_kobj_rpc_path_value_json(const char* op,
                                                   const char* path,
                                                   const char* value,
                                                   char* out,
                                                   vk_usize out_cap) {
    char req[512];
    vk_kobj_build_path_value_request(req, sizeof(req), op, path, value);
    return vk_kobj_rpc_json(req, out, out_cap);
}

static inline void vk_kobj_rpc_print_json(const char* req_json) {
    char out[1536];
    vk_kobj_rpc_json(req_json, out, sizeof(out));
    VK_CALL(puts, out);
    VK_CALL(putc, '\n');
}

static inline void vk_kobj_cmd_json(const char* op) {
    char req[96];
    vk_kobj_build_op_request(req, sizeof(req), op);
    vk_kobj_rpc_print_json(req);
}

static inline void vk_kobj_named_cmd_json(const char* op, const char* name) {
    char req[192];
    vk_kobj_build_named_request(req, sizeof(req), op, name);
    vk_kobj_rpc_print_json(req);
}

static inline void vk_kobj_get_json(const char* path) {
    char req[256];
    vk_kobj_build_path_request(req, sizeof(req), "get", path);
    vk_kobj_rpc_print_json(req);
}

static inline void vk_kobj_set_json(const char* path, const char* value) {
    char req[512];
    vk_kobj_build_path_value_request(req, sizeof(req), "set", path, value);
    vk_kobj_rpc_print_json(req);
}

static inline void vk_kobj_ls_json(const char* path) {
    char req[256];
    vk_kobj_build_path_request(req, sizeof(req), "ls", path);
    vk_kobj_rpc_print_json(req);
}

static inline void vk_kobj_describe_json(const char* path) {
    char req[256];
    vk_kobj_build_path_request(req, sizeof(req), "describe", path);
    vk_kobj_rpc_print_json(req);
}

static inline void vk_kobj_ls_text(const char* path) {
    char out[1024];
    vk_kobj_rpc_path_json("ls_text", path, out, sizeof(out));
    VK_CALL(puts, out);
}

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* VK_USERSPACE_H */