/*
 * vkernel - UEFI Microkernel
 * Copyright (C) 2026 vkernel authors
 *
 * kobj.h - Typed kernel object tree subsystem
 */

#pragma once

#include "types.h"

namespace vk {
namespace kobj {

static constexpr usize KSTR_MAX = 64;
static constexpr usize KFIELD_MAX = 8;
static constexpr usize KENUM_MAX = 8;
static constexpr usize KNODE_CHILDREN_MAX = 16;
static constexpr usize KNODE_PATH_MAX = 128;
static constexpr usize KNODE_MAX = 128;

enum class KTag : u8 {
    U64,
    I64,
    Bool,
    Str,
    Enum,
    Struct,
    Stream,
    Err,
};

struct KStr {
    char buf[KSTR_MAX];
    u32 len;

    void set(const char* s);
    void set(const char* s, usize n);
    bool eq(const char* s) const;
    const char* c_str() const { return buf; }
};

struct KVal;

struct KField {
    KStr key;
    KTag tag;
    union {
        u64 as_u64;
        i64 as_i64;
        bool as_bool;
        KStr as_str;
        u32 as_enum_idx;
        KStr as_err;
    };
};

struct KVal {
    KTag tag;
    union {
        u64 as_u64;
        i64 as_i64;
        bool as_bool;
        KStr as_str;
        u32 as_enum_idx;
        struct {
            KField fields[KFIELD_MAX];
            u32 count;
        } s;
        KStr as_err;
    };

    static KVal from_u64(u64 v);
    static KVal from_i64(i64 v);
    static KVal from_bool(bool v);
    static KVal from_str(const char* s);
    static KVal from_enum(u32 idx);
    static KVal stream();
    static KVal err(const char* msg);
};

struct KNodeSchema {
    const char* name;
    KTag type;
    bool writable;
    bool volatile_node;
    const char* unit;
    u64 range_min;
    u64 range_max;
    const char* enum_labels[KENUM_MAX];
    u32 cap_mask;
};

struct KNode {
    KNodeSchema schema;
    KVal (*get_fn)(KNode*);
    bool (*set_fn)(KNode*, KVal);
    KNode* children[KNODE_CHILDREN_MAX];
    u32 child_count;
    KNode* parent;
};

void init();
bool is_initialized();

KNode* resolve(const char* path);
KNode* resolve(const char* path, usize len);

KVal kget(const char* path);
bool kset(const char* path, KVal val);
void kls(const char* path, char* out, usize out_cap);
void kdescribe(const char* path, char* out, usize out_cap);

usize kval_render(const KVal& v, const KNode* node, char* out, usize out_cap);

KVal kobj_get_by_path(const char* path);
bool kobj_set_by_path(const char* path, KTag tag, u64 raw_u64, const char* raw_str);
void krpc(const char* req_json, char* out, usize out_cap);

} // namespace kobj
} // namespace vk
