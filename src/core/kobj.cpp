/*
 * vkernel - UEFI Microkernel
 * Copyright (C) 2026 vkernel authors
 *
 * kobj.cpp - Typed kernel object tree subsystem
 */

#include "kobj.h"

#include "block.h"
#include "console.h"
#include "driver.h"
#include "fs.h"
#include "log.h"
#include "memory.h"
#include "panic.h"
#include "pci.h"
#include "scheduler.h"
#include "smp.h"
#include "arch/x86_64/arch.h"

namespace vk {
namespace kobj {

static KNode s_root;
static KNode s_sys;
static KNode s_sys_cpu;
static KNode s_sys_cpu_count;
static KNode s_sys_cpu_ticks;
static KNode s_sys_mem;
static KNode s_sys_mem_total_kb;
static KNode s_sys_mem_free_kb;
static KNode s_sys_mem_heap_used;
static KNode s_sys_mem_heap_capacity;
static KNode s_sys_mem_heap_subheaps;
static KNode s_sys_power;
static KNode s_sys_power_state;

static KNode s_sys_log;
static KNode s_sys_log_route;

static KNode s_proc;
static KNode s_proc_count;
static KNode s_proc_pid;
static KNode s_proc_pid_id;
static KNode s_proc_pid_state;
static KNode s_proc_pid_cpu;
static KNode s_proc_pid_cpu_ticks;
static KNode s_proc_pid_name;

static KNode s_fs;
static KNode s_fs_active_backend;
static KNode s_fs_fallback_ready;
static KNode s_fs_fat32_mounted;
static KNode s_fs_writable;
static KNode s_fs_block_device;
static KNode s_fs_root_path;
static KNode s_fs_cluster_size;
static KNode s_fs_root_cluster;

static KNode s_dev;
static KNode s_dev_block;
static KNode s_dev_block_count;
static KNode s_dev_block_devs[block::MAX_BLOCK_DEVICES];
static KNode s_dev_block_dev_name[block::MAX_BLOCK_DEVICES];
static KNode s_dev_block_dev_size_kb[block::MAX_BLOCK_DEVICES];
static KNode s_dev_pci;
static KNode s_dev_pci_count;

static u32 s_power_state = 0;
static u64 s_proc_virtual_pid = 0;
static u64 s_proc_virtual_ticks = 0;
static u32 s_proc_virtual_state = 0;
static i64 s_proc_virtual_cpu = -1;
static static_string<32> s_proc_virtual_name;
static bool s_initialized = false;

static auto cstrlen(const char* s) -> usize {
    if (s == null) return 0;
    usize n = 0;
    while (s[n] != '\0') ++n;
    return n;
}

static auto append_ch(char* out, usize out_cap, usize pos, char ch) -> usize {
    if (out_cap == 0) return 0;
    if (pos + 1 < out_cap) {
        out[pos++] = ch;
        out[pos] = '\0';
    }
    return pos;
}

static auto append_str(char* out, usize out_cap, usize pos, const char* s) -> usize {
    if (s == null) return pos;
    for (usize i = 0; s[i] != '\0'; ++i) {
        pos = append_ch(out, out_cap, pos, s[i]);
    }
    return pos;
}

static auto append_json_escaped(char* out, usize out_cap, usize pos, const char* s) -> usize {
    if (s == null) return pos;
    for (usize i = 0; s[i] != '\0'; ++i) {
        const char ch = s[i];
        if (ch == '\"') {
            pos = append_str(out, out_cap, pos, "\\\"");
        } else if (ch == '\\') {
            pos = append_str(out, out_cap, pos, "\\\\");
        } else if (ch == '\n') {
            pos = append_str(out, out_cap, pos, "\\n");
        } else if (ch == '\r') {
            pos = append_str(out, out_cap, pos, "\\r");
        } else if (ch == '\t') {
            pos = append_str(out, out_cap, pos, "\\t");
        } else {
            pos = append_ch(out, out_cap, pos, ch);
        }
    }
    return pos;
}

static auto append_json_ok(char* out, usize out_cap, const char* op) -> void {
    usize pos = 0;
    pos = append_str(out, out_cap, pos, "{\"ok\":true,\"op\":\"");
    pos = append_json_escaped(out, out_cap, pos, op);
    pos = append_str(out, out_cap, pos, "\"}");
}

static auto render_u64(char* out, usize cap, usize pos, u64 v) -> usize {
    char tmp[21];
    usize n = 0;
    if (v == 0) {
        return append_ch(out, cap, pos, '0');
    }
    while (v > 0 && n < sizeof(tmp)) {
        tmp[n++] = static_cast<char>('0' + (v % 10));
        v /= 10;
    }
    while (n > 0) {
        pos = append_ch(out, cap, pos, tmp[--n]);
    }
    return pos;
}

static auto render_i64(char* out, usize cap, usize pos, i64 v) -> usize {
    if (v < 0) {
        pos = append_ch(out, cap, pos, '-');
        u64 uv = static_cast<u64>(-(v + 1)) + 1;
        return render_u64(out, cap, pos, uv);
    }
    return render_u64(out, cap, pos, static_cast<u64>(v));
}

static auto tag_name(KTag tag) -> const char* {
    switch (tag) {
        case KTag::U64: return "u64";
        case KTag::I64: return "i64";
        case KTag::Bool: return "bool";
        case KTag::Str: return "str";
        case KTag::Enum: return "enum";
        case KTag::Struct: return "struct";
        case KTag::Stream: return "stream";
        case KTag::Err: return "err";
        default: return "unknown";
    }
}

static auto parse_u64_segment(const char* s, usize n, u64* out) -> bool {
    if (s == null || out == null || n == 0) return false;
    u64 value = 0;
    for (usize i = 0; i < n; ++i) {
        if (s[i] < '0' || s[i] > '9') return false;
        value = (value * 10ULL) + static_cast<u64>(s[i] - '0');
    }
    *out = value;
    return true;
}

static auto json_extract_string_buffer(const char* json, const char* key, char* out, usize out_cap) -> bool {
    if (json == null || key == null || out == null || out_cap == 0) return false;

    out[0] = '\0';

    char pattern[32];
    usize klen = cstrlen(key);
    if (klen + 3 >= sizeof(pattern)) return false;

    usize p = 0;
    pattern[p++] = '"';
    for (usize i = 0; i < klen; ++i) pattern[p++] = key[i];
    pattern[p++] = '"';
    pattern[p] = '\0';

    for (usize i = 0; json[i] != '\0'; ++i) {
        bool match = true;
        for (usize j = 0; pattern[j] != '\0'; ++j) {
            if (json[i + j] == '\0' || json[i + j] != pattern[j]) {
                match = false;
                break;
            }
        }
        if (!match) continue;

        usize pos = i + p;
        while (json[pos] == ' ' || json[pos] == '\t' || json[pos] == '\n' || json[pos] == '\r') ++pos;
        if (json[pos] != ':') continue;
        ++pos;
        while (json[pos] == ' ' || json[pos] == '\t' || json[pos] == '\n' || json[pos] == '\r') ++pos;
        if (json[pos] != '"') continue;
        ++pos;

        usize written = 0;
        while (json[pos] != '\0' && json[pos] != '"' && written + 1 < out_cap) {
            if (json[pos] == '\\' && json[pos + 1] != '\0') {
                ++pos;
                char esc = json[pos];
                if (esc == 'n') out[written++] = '\n';
                else if (esc == 'r') out[written++] = '\r';
                else if (esc == 't') out[written++] = '\t';
                else out[written++] = esc;
                ++pos;
                continue;
            }
            out[written++] = json[pos++];
        }
        out[written] = '\0';
        return true;
    }

    return false;
}

static auto json_extract_string(const char* json, const char* key, KStr* out) -> bool {
    if (json == null || key == null || out == null) return false;

    char tmp[KSTR_MAX];
    if (!json_extract_string_buffer(json, key, tmp, sizeof(tmp))) {
        return false;
    }
    out->set(tmp);
    return true;
}

struct fs_list_json_context {
    char* out = null;
    usize out_cap = 0;
    usize pos = 0;
    bool first = true;
};

static auto append_fs_list_item(const fs::directory_entry_info& entry, void* raw_context) -> bool {
    auto* context = static_cast<fs_list_json_context*>(raw_context);
    if (context == null) {
        return true;
    }

    if (!context->first) {
        context->pos = append_ch(context->out, context->out_cap, context->pos, ',');
    }
    context->pos = append_ch(context->out, context->out_cap, context->pos, '"');
    context->pos = append_ch(context->out, context->out_cap, context->pos, entry.is_directory ? 'D' : 'F');
    context->pos = append_str(context->out, context->out_cap, context->pos, "\\t");
    context->pos = append_json_escaped(context->out, context->out_cap, context->pos, entry.name.c_str());
    context->pos = append_str(context->out, context->out_cap, context->pos, "\\t");
    context->pos = render_u64(context->out, context->out_cap, context->pos, static_cast<u64>(entry.size));
    context->pos = append_ch(context->out, context->out_cap, context->pos, '"');
    context->first = false;
    return false;
}

void KStr::set(const char* s) {
    if (!s) { buf[0] = '\0'; len = 0; return; }
    usize i = 0;
    while (s[i] && i < KSTR_MAX - 1) { buf[i] = s[i]; ++i; }
    buf[i] = '\0';
    len = static_cast<u32>(i);
}

void KStr::set(const char* s, usize n) {
    if (s == null) {
        buf[0] = '\0';
        len = 0;
        return;
    }
    usize i = 0;
    while (i < n && i < KSTR_MAX - 1) { buf[i] = s[i]; ++i; }
    buf[i] = '\0';
    len = static_cast<u32>(i);
}

bool KStr::eq(const char* s) const {
    if (s == null) return false;
    for (usize i = 0; ; ++i) {
        if (buf[i] != s[i]) return false;
        if (buf[i] == '\0') return true;
    }
}

KVal KVal::from_u64(u64 v) {
    KVal out{};
    out.tag = KTag::U64;
    out.as_u64 = v;
    return out;
}

KVal KVal::from_i64(i64 v) {
    KVal out{};
    out.tag = KTag::I64;
    out.as_i64 = v;
    return out;
}

KVal KVal::from_bool(bool v) {
    KVal out{};
    out.tag = KTag::Bool;
    out.as_bool = v;
    return out;
}

KVal KVal::from_str(const char* s) {
    KVal out{};
    out.tag = KTag::Str;
    out.as_str.set(s);
    return out;
}

KVal KVal::from_enum(u32 idx) {
    KVal out{};
    out.tag = KTag::Enum;
    out.as_enum_idx = idx;
    return out;
}

KVal KVal::stream() {
    KVal out{};
    out.tag = KTag::Stream;
    return out;
}

KVal KVal::err(const char* msg) {
    KVal out{};
    out.tag = KTag::Err;
    out.as_err.set(msg);
    return out;
}

static auto get_sys_cpu_count(KNode*) -> KVal {
    return KVal::from_u64(static_cast<u64>(smp::cpu_count()));
}

static auto get_sys_cpu_ticks(KNode*) -> KVal {
    return KVal::from_u64(sched::tick_count());
}

static auto get_sys_mem_total_kb(KNode*) -> KVal {
    return KVal::from_u64((g_phys_alloc.total_pages() * PAGE_SIZE_4K) / 1024ULL);
}

static auto get_sys_mem_free_kb(KNode*) -> KVal {
    return KVal::from_u64((g_phys_alloc.free_pages() * PAGE_SIZE_4K) / 1024ULL);
}

static auto get_sys_mem_heap_used(KNode*) -> KVal {
    u64 used = 0;
    for (auto* block = g_kernel_heap.get_free_list(); block != null; block = block->next) {
        if (block->used) {
            used += block->size;
        }
    }
    return KVal::from_u64(used);
}

static auto get_sys_mem_heap_capacity(KNode*) -> KVal {
    return KVal::from_u64(g_kernel_heap.total_bytes());
}

static auto get_sys_mem_heap_subheaps(KNode*) -> KVal {
    return KVal::from_u64(static_cast<u64>(g_kernel_heap.subheap_count()));
}

static auto get_sys_power_state(KNode*) -> KVal {
    return KVal::from_enum(s_power_state);
}

static auto set_sys_power_state(KNode*, KVal v) -> bool {
    if (v.tag != KTag::Enum) return false;
    if (v.as_enum_idx > 2) return false;
    s_power_state = v.as_enum_idx;
    return true;
}

static auto get_sys_log_route(KNode*) -> KVal {
    return KVal::from_enum(static_cast<u32>(log::get_route()));
}

static auto set_sys_log_route(KNode*, KVal v) -> bool {
    if (v.tag != KTag::Enum) return false;
    if (v.as_enum_idx > 2) return false;
    log::set_route(static_cast<log::route>(v.as_enum_idx));
    return true;
}

static auto get_proc_count(KNode*) -> KVal {
    return KVal::from_u64(sched::snapshot_tasks(null, 0));
}

static auto get_proc_pid_id(KNode*) -> KVal {
    return KVal::from_u64(s_proc_virtual_pid);
}

static auto get_proc_pid_state(KNode*) -> KVal {
    return KVal::from_enum(s_proc_virtual_state);
}

static auto get_proc_pid_cpu(KNode*) -> KVal {
    return KVal::from_i64(s_proc_virtual_cpu);
}

static auto get_proc_pid_cpu_ticks(KNode*) -> KVal {
    return KVal::from_u64(s_proc_virtual_ticks);
}

static auto get_proc_pid_name(KNode*) -> KVal {
    return KVal::from_str(s_proc_virtual_name.c_str());
}

static auto get_fs_active_backend(KNode*) -> KVal {
    auto info = fs::query_info();
    return KVal::from_str(info.active_backend.c_str());
}

static auto get_fs_fallback_ready(KNode*) -> KVal {
    return KVal::from_bool(fs::query_info().fallback_ready);
}

static auto get_fs_fat32_mounted(KNode*) -> KVal {
    return KVal::from_bool(fs::query_info().fat32_mounted);
}

static auto get_fs_writable(KNode*) -> KVal {
    return KVal::from_bool(fs::query_info().writable);
}

static auto get_fs_block_device(KNode*) -> KVal {
    auto info = fs::query_info();
    return KVal::from_str(info.block_device.c_str());
}

static auto get_fs_root_path(KNode*) -> KVal {
    auto info = fs::query_info();
    return KVal::from_str(info.logical_root_path.c_str());
}

static auto get_fs_cluster_size(KNode*) -> KVal {
    return KVal::from_u64(fs::query_info().cluster_size);
}

static auto get_fs_root_cluster(KNode*) -> KVal {
    return KVal::from_u64(fs::query_info().root_cluster);
}

static auto get_dev_block_count(KNode*) -> KVal {
    return KVal::from_u64(static_cast<u64>(block::device_count()));
}

static auto get_dev_block_name(KNode* node) -> KVal {
    if (node == null || node->parent == null) return KVal::err("invalid node");
    const char* dev_name = node->parent->schema.name;
    if (dev_name == null) return KVal::err("device missing");
    return KVal::from_str(dev_name);
}

static auto get_dev_block_size_kb(KNode* node) -> KVal {
    if (node == null || node->parent == null) return KVal::err("invalid node");
    auto* dev = block::find(node->parent->schema.name);
    if (dev == null) return KVal::err("device missing");
    u64 bytes = dev->block_count * static_cast<u64>(dev->block_size);
    return KVal::from_u64(bytes / 1024ULL);
}

static auto get_dev_pci_count(KNode*) -> KVal {
    return KVal::from_u64(static_cast<u64>(pci::device_count()));
}

static void add_child(KNode* parent, KNode* child) {
    if (parent == null || child == null) return;
    if (parent->child_count >= KNODE_CHILDREN_MAX) {
        vk_panic(__FILE__, __LINE__, "kobj: too many children");
    }
    child->parent = parent;
    parent->children[parent->child_count++] = child;
}

void init() {
    if (s_initialized) return;

    s_root = {};
    s_root.schema.name = "";
    s_root.schema.type = KTag::Struct;
    s_root.schema.writable = false;
    s_root.schema.volatile_node = false;
    s_root.schema.unit = "";
    s_root.schema.cap_mask = 0x01;

    s_sys = {};
    s_sys.schema.name = "sys";
    s_sys.schema.type = KTag::Struct;
    s_sys.schema.unit = "";
    s_sys.schema.cap_mask = 0x01;
    add_child(&s_root, &s_sys);

    s_sys_cpu = {};
    s_sys_cpu.schema.name = "cpu";
    s_sys_cpu.schema.type = KTag::Struct;
    s_sys_cpu.schema.unit = "";
    s_sys_cpu.schema.cap_mask = 0x01;
    add_child(&s_sys, &s_sys_cpu);

    s_sys_cpu_count = {};
    s_sys_cpu_count.schema.name = "count";
    s_sys_cpu_count.schema.type = KTag::U64;
    s_sys_cpu_count.schema.writable = false;
    s_sys_cpu_count.schema.volatile_node = false;
    s_sys_cpu_count.schema.unit = "";
    s_sys_cpu_count.schema.cap_mask = 0x01;
    s_sys_cpu_count.get_fn = get_sys_cpu_count;
    add_child(&s_sys_cpu, &s_sys_cpu_count);

    s_sys_cpu_ticks = {};
    s_sys_cpu_ticks.schema.name = "ticks";
    s_sys_cpu_ticks.schema.type = KTag::U64;
    s_sys_cpu_ticks.schema.writable = false;
    s_sys_cpu_ticks.schema.volatile_node = true;
    s_sys_cpu_ticks.schema.unit = "";
    s_sys_cpu_ticks.schema.cap_mask = 0x01;
    s_sys_cpu_ticks.get_fn = get_sys_cpu_ticks;
    add_child(&s_sys_cpu, &s_sys_cpu_ticks);

    s_sys_mem = {};
    s_sys_mem.schema.name = "mem";
    s_sys_mem.schema.type = KTag::Struct;
    s_sys_mem.schema.unit = "";
    s_sys_mem.schema.cap_mask = 0x01;
    add_child(&s_sys, &s_sys_mem);

    s_sys_mem_total_kb = {};
    s_sys_mem_total_kb.schema.name = "total_kb";
    s_sys_mem_total_kb.schema.type = KTag::U64;
    s_sys_mem_total_kb.schema.writable = false;
    s_sys_mem_total_kb.schema.unit = "kb";
    s_sys_mem_total_kb.schema.cap_mask = 0x01;
    s_sys_mem_total_kb.get_fn = get_sys_mem_total_kb;
    add_child(&s_sys_mem, &s_sys_mem_total_kb);

    s_sys_mem_free_kb = {};
    s_sys_mem_free_kb.schema.name = "free_kb";
    s_sys_mem_free_kb.schema.type = KTag::U64;
    s_sys_mem_free_kb.schema.writable = false;
    s_sys_mem_free_kb.schema.volatile_node = true;
    s_sys_mem_free_kb.schema.unit = "kb";
    s_sys_mem_free_kb.schema.cap_mask = 0x01;
    s_sys_mem_free_kb.get_fn = get_sys_mem_free_kb;
    add_child(&s_sys_mem, &s_sys_mem_free_kb);

    s_sys_mem_heap_used = {};
    s_sys_mem_heap_used.schema.name = "heap_used";
    s_sys_mem_heap_used.schema.type = KTag::U64;
    s_sys_mem_heap_used.schema.writable = false;
    s_sys_mem_heap_used.schema.volatile_node = true;
    s_sys_mem_heap_used.schema.unit = "bytes";
    s_sys_mem_heap_used.schema.cap_mask = 0x01;
    s_sys_mem_heap_used.get_fn = get_sys_mem_heap_used;
    add_child(&s_sys_mem, &s_sys_mem_heap_used);

    s_sys_mem_heap_capacity = {};
    s_sys_mem_heap_capacity.schema.name = "heap_capacity";
    s_sys_mem_heap_capacity.schema.type = KTag::U64;
    s_sys_mem_heap_capacity.schema.writable = false;
    s_sys_mem_heap_capacity.schema.volatile_node = true;
    s_sys_mem_heap_capacity.schema.unit = "bytes";
    s_sys_mem_heap_capacity.schema.cap_mask = 0x01;
    s_sys_mem_heap_capacity.get_fn = get_sys_mem_heap_capacity;
    add_child(&s_sys_mem, &s_sys_mem_heap_capacity);

    s_sys_mem_heap_subheaps = {};
    s_sys_mem_heap_subheaps.schema.name = "heap_subheaps";
    s_sys_mem_heap_subheaps.schema.type = KTag::U64;
    s_sys_mem_heap_subheaps.schema.writable = false;
    s_sys_mem_heap_subheaps.schema.volatile_node = true;
    s_sys_mem_heap_subheaps.schema.unit = "count";
    s_sys_mem_heap_subheaps.schema.cap_mask = 0x01;
    s_sys_mem_heap_subheaps.get_fn = get_sys_mem_heap_subheaps;
    add_child(&s_sys_mem, &s_sys_mem_heap_subheaps);

    s_sys_power = {};
    s_sys_power.schema.name = "power";
    s_sys_power.schema.type = KTag::Struct;
    s_sys_power.schema.cap_mask = 0x01;
    add_child(&s_sys, &s_sys_power);

    s_sys_power_state = {};
    s_sys_power_state.schema.name = "state";
    s_sys_power_state.schema.type = KTag::Enum;
    s_sys_power_state.schema.writable = true;
    s_sys_power_state.schema.unit = "";
    s_sys_power_state.schema.enum_labels[0] = "running";
    s_sys_power_state.schema.enum_labels[1] = "suspend";
    s_sys_power_state.schema.enum_labels[2] = "hibernate";
    s_sys_power_state.schema.cap_mask = 0x03;
    s_sys_power_state.get_fn = get_sys_power_state;
    s_sys_power_state.set_fn = set_sys_power_state;
    add_child(&s_sys_power, &s_sys_power_state);

    s_sys_log = {};
    s_sys_log.schema.name = "log";
    s_sys_log.schema.type = KTag::Struct;
    s_sys_log.schema.cap_mask = 0x01;
    add_child(&s_sys, &s_sys_log);

    s_sys_log_route = {};
    s_sys_log_route.schema.name = "route";
    s_sys_log_route.schema.type = KTag::Enum;
    s_sys_log_route.schema.writable = true;
    s_sys_log_route.schema.volatile_node = false;
    s_sys_log_route.schema.unit = "";
    s_sys_log_route.schema.enum_labels[0] = "default";
    s_sys_log_route.schema.enum_labels[1] = "serial";
    s_sys_log_route.schema.enum_labels[2] = "disabled";
    s_sys_log_route.schema.cap_mask = 0x03;
    s_sys_log_route.get_fn = get_sys_log_route;
    s_sys_log_route.set_fn = set_sys_log_route;
    add_child(&s_sys_log, &s_sys_log_route);

    s_proc = {};
    s_proc.schema.name = "proc";
    s_proc.schema.type = KTag::Struct;
    s_proc.schema.cap_mask = 0x01;
    add_child(&s_root, &s_proc);

    s_proc_count = {};
    s_proc_count.schema.name = "count";
    s_proc_count.schema.type = KTag::U64;
    s_proc_count.schema.writable = false;
    s_proc_count.schema.volatile_node = true;
    s_proc_count.schema.cap_mask = 0x01;
    s_proc_count.get_fn = get_proc_count;
    add_child(&s_proc, &s_proc_count);

    s_proc_pid = {};
    s_proc_pid.schema.name = "<pid>";
    s_proc_pid.schema.type = KTag::Struct;
    s_proc_pid.schema.cap_mask = 0x01;

    s_proc_pid_id = {};
    s_proc_pid_id.schema.name = "id";
    s_proc_pid_id.schema.type = KTag::U64;
    s_proc_pid_id.schema.writable = false;
    s_proc_pid_id.schema.cap_mask = 0x01;
    s_proc_pid_id.get_fn = get_proc_pid_id;

    s_proc_pid_state = {};
    s_proc_pid_state.schema.name = "state";
    s_proc_pid_state.schema.type = KTag::Enum;
    s_proc_pid_state.schema.writable = false;
    s_proc_pid_state.schema.enum_labels[0] = "ready";
    s_proc_pid_state.schema.enum_labels[1] = "running";
    s_proc_pid_state.schema.enum_labels[2] = "blocked";
    s_proc_pid_state.schema.enum_labels[3] = "terminated";
    s_proc_pid_state.schema.cap_mask = 0x01;
    s_proc_pid_state.get_fn = get_proc_pid_state;

    s_proc_pid_cpu = {};
    s_proc_pid_cpu.schema.name = "cpu";
    s_proc_pid_cpu.schema.type = KTag::I64;
    s_proc_pid_cpu.schema.writable = false;
    s_proc_pid_cpu.schema.volatile_node = true;
    s_proc_pid_cpu.schema.cap_mask = 0x01;
    s_proc_pid_cpu.get_fn = get_proc_pid_cpu;

    s_proc_pid_cpu_ticks = {};
    s_proc_pid_cpu_ticks.schema.name = "cpu_ticks";
    s_proc_pid_cpu_ticks.schema.type = KTag::U64;
    s_proc_pid_cpu_ticks.schema.writable = false;
    s_proc_pid_cpu_ticks.schema.volatile_node = true;
    s_proc_pid_cpu_ticks.schema.cap_mask = 0x01;
    s_proc_pid_cpu_ticks.get_fn = get_proc_pid_cpu_ticks;

    s_proc_pid_name = {};
    s_proc_pid_name.schema.name = "name";
    s_proc_pid_name.schema.type = KTag::Str;
    s_proc_pid_name.schema.writable = false;
    s_proc_pid_name.schema.cap_mask = 0x01;
    s_proc_pid_name.get_fn = get_proc_pid_name;

    s_fs = {};
    s_fs.schema.name = "fs";
    s_fs.schema.type = KTag::Struct;
    s_fs.schema.cap_mask = 0x01;
    add_child(&s_root, &s_fs);

    s_fs_active_backend = {};
    s_fs_active_backend.schema.name = "active_backend";
    s_fs_active_backend.schema.type = KTag::Str;
    s_fs_active_backend.schema.cap_mask = 0x01;
    s_fs_active_backend.get_fn = get_fs_active_backend;
    add_child(&s_fs, &s_fs_active_backend);

    s_fs_fallback_ready = {};
    s_fs_fallback_ready.schema.name = "fallback_ready";
    s_fs_fallback_ready.schema.type = KTag::Bool;
    s_fs_fallback_ready.schema.cap_mask = 0x01;
    s_fs_fallback_ready.get_fn = get_fs_fallback_ready;
    add_child(&s_fs, &s_fs_fallback_ready);

    s_fs_fat32_mounted = {};
    s_fs_fat32_mounted.schema.name = "fat32_mounted";
    s_fs_fat32_mounted.schema.type = KTag::Bool;
    s_fs_fat32_mounted.schema.cap_mask = 0x01;
    s_fs_fat32_mounted.get_fn = get_fs_fat32_mounted;
    add_child(&s_fs, &s_fs_fat32_mounted);

    s_fs_writable = {};
    s_fs_writable.schema.name = "writable";
    s_fs_writable.schema.type = KTag::Bool;
    s_fs_writable.schema.cap_mask = 0x01;
    s_fs_writable.get_fn = get_fs_writable;
    add_child(&s_fs, &s_fs_writable);

    s_fs_block_device = {};
    s_fs_block_device.schema.name = "block_device";
    s_fs_block_device.schema.type = KTag::Str;
    s_fs_block_device.schema.cap_mask = 0x01;
    s_fs_block_device.get_fn = get_fs_block_device;
    add_child(&s_fs, &s_fs_block_device);

    s_fs_root_path = {};
    s_fs_root_path.schema.name = "root_path";
    s_fs_root_path.schema.type = KTag::Str;
    s_fs_root_path.schema.cap_mask = 0x01;
    s_fs_root_path.get_fn = get_fs_root_path;
    add_child(&s_fs, &s_fs_root_path);

    s_fs_cluster_size = {};
    s_fs_cluster_size.schema.name = "cluster_size";
    s_fs_cluster_size.schema.type = KTag::U64;
    s_fs_cluster_size.schema.unit = "bytes";
    s_fs_cluster_size.schema.cap_mask = 0x01;
    s_fs_cluster_size.get_fn = get_fs_cluster_size;
    add_child(&s_fs, &s_fs_cluster_size);

    s_fs_root_cluster = {};
    s_fs_root_cluster.schema.name = "root_cluster";
    s_fs_root_cluster.schema.type = KTag::U64;
    s_fs_root_cluster.schema.cap_mask = 0x01;
    s_fs_root_cluster.get_fn = get_fs_root_cluster;
    add_child(&s_fs, &s_fs_root_cluster);

    s_dev = {};
    s_dev.schema.name = "dev";
    s_dev.schema.type = KTag::Struct;
    s_dev.schema.cap_mask = 0x01;
    add_child(&s_root, &s_dev);

    s_dev_block = {};
    s_dev_block.schema.name = "block";
    s_dev_block.schema.type = KTag::Struct;
    s_dev_block.schema.cap_mask = 0x01;
    add_child(&s_dev, &s_dev_block);

    s_dev_block_count = {};
    s_dev_block_count.schema.name = "count";
    s_dev_block_count.schema.type = KTag::U64;
    s_dev_block_count.schema.writable = false;
    s_dev_block_count.schema.cap_mask = 0x01;
    s_dev_block_count.get_fn = get_dev_block_count;
    add_child(&s_dev_block, &s_dev_block_count);

    usize dev_count = block::device_count();
    if (dev_count > block::MAX_BLOCK_DEVICES) dev_count = block::MAX_BLOCK_DEVICES;
    for (usize i = 0; i < dev_count; ++i) {
        auto* dev = block::get_device(i);
        if (dev == null) continue;

        s_dev_block_devs[i] = {};
        s_dev_block_devs[i].schema.name = dev->name.c_str();
        s_dev_block_devs[i].schema.type = KTag::Struct;
        s_dev_block_devs[i].schema.cap_mask = 0x01;
        add_child(&s_dev_block, &s_dev_block_devs[i]);

        s_dev_block_dev_name[i] = {};
        s_dev_block_dev_name[i].schema.name = "name";
        s_dev_block_dev_name[i].schema.type = KTag::Str;
        s_dev_block_dev_name[i].schema.cap_mask = 0x01;
        s_dev_block_dev_name[i].get_fn = get_dev_block_name;
        add_child(&s_dev_block_devs[i], &s_dev_block_dev_name[i]);

        s_dev_block_dev_size_kb[i] = {};
        s_dev_block_dev_size_kb[i].schema.name = "size_kb";
        s_dev_block_dev_size_kb[i].schema.type = KTag::U64;
        s_dev_block_dev_size_kb[i].schema.unit = "kb";
        s_dev_block_dev_size_kb[i].schema.cap_mask = 0x01;
        s_dev_block_dev_size_kb[i].get_fn = get_dev_block_size_kb;
        add_child(&s_dev_block_devs[i], &s_dev_block_dev_size_kb[i]);
    }

    s_dev_pci = {};
    s_dev_pci.schema.name = "pci";
    s_dev_pci.schema.type = KTag::Struct;
    s_dev_pci.schema.cap_mask = 0x01;
    add_child(&s_dev, &s_dev_pci);

    s_dev_pci_count = {};
    s_dev_pci_count.schema.name = "count";
    s_dev_pci_count.schema.type = KTag::U64;
    s_dev_pci_count.schema.writable = false;
    s_dev_pci_count.schema.cap_mask = 0x01;
    s_dev_pci_count.get_fn = get_dev_pci_count;
    add_child(&s_dev_pci, &s_dev_pci_count);

    s_initialized = true;
    log::info() << "kobj: initialized";
}

bool is_initialized() {
    return s_initialized;
}

static auto fill_proc_virtual(u64 pid) -> bool {
    task_snapshot snapshots[MAX_TASKS];
    usize count = sched::snapshot_tasks(snapshots, MAX_TASKS);
    if (count > MAX_TASKS) count = MAX_TASKS;

    for (usize i = 0; i < count; ++i) {
        if (snapshots[i].id == pid) {
            s_proc_virtual_pid = snapshots[i].id;
            s_proc_virtual_ticks = snapshots[i].cpu_ticks;
            s_proc_virtual_state = static_cast<u32>(snapshots[i].state);
            s_proc_virtual_cpu = (snapshots[i].cpu == SCHED_CPU_NONE)
                ? -1
                : static_cast<i64>(snapshots[i].cpu);
            (void)s_proc_virtual_name.assign(snapshots[i].name);
            return true;
        }
    }
    return false;
}

static auto resolve_proc_virtual(const char* path, usize len) -> KNode* {
    string_view p(path, len);
    if (!p.starts_with("proc/")) return null;
    p.remove_prefix(5);

    if (p.empty()) return &s_proc;
    if (p.equals("count")) return &s_proc_count;

    usize pid_end = 0;
    while (pid_end < p.size() && p[pid_end] != '/') ++pid_end;
    u64 pid = 0;
    if (!parse_u64_segment(p.data(), pid_end, &pid)) return null;
    if (!fill_proc_virtual(pid)) return null;

    if (pid_end >= p.size()) return &s_proc_pid;
    if (p[pid_end] != '/') return null;

    string_view tail(p.data() + pid_end + 1, p.size() - pid_end - 1);
    if (tail.equals("id")) return &s_proc_pid_id;
    if (tail.equals("state")) return &s_proc_pid_state;
    if (tail.equals("cpu")) return &s_proc_pid_cpu;
    if (tail.equals("cpu_ticks")) return &s_proc_pid_cpu_ticks;
    if (tail.equals("name")) return &s_proc_pid_name;
    return null;
}

KNode* resolve(const char* path, usize len) {
    init();

    if (path == null) return null;
    if (len == 0) return &s_root;

    usize start = 0;
    while (start < len && path[start] == '/') ++start;
    if (start >= len || path[start] == '\0') return &s_root;

    KNode* proc_virtual = resolve_proc_virtual(path + start, len - start);
    if (proc_virtual != null) return proc_virtual;

    KNode* node = &s_root;
    while (start < len && path[start] != '\0') {
        usize end = start;
        while (end < len && path[end] != '/' && path[end] != '\0') ++end;

        if (end == start) {
            start = end + 1;
            continue;
        }

        KNode* found = null;
        for (u32 i = 0; i < node->child_count; ++i) {
            auto* child = node->children[i];
            if (child == null || child->schema.name == null) continue;
            string_view child_name(child->schema.name);
            string_view seg(path + start, end - start);
            if (child_name.equals(seg)) {
                found = child;
                break;
            }
        }

        if (found == null) return null;
        node = found;

        start = end;
        while (start < len && path[start] == '/') ++start;
    }

    return node;
}

KNode* resolve(const char* path) {
    return resolve(path, cstrlen(path));
}

KVal kget(const char* path) {
    auto* node = resolve(path);
    if (node == null) return KVal::err("path not found");
    if (node->get_fn == null) return KVal::err("node is not readable");
    return node->get_fn(node);
}

bool kset(const char* path, KVal val) {
    auto* node = resolve(path);
    if (node == null) return false;
    if (!node->schema.writable || node->set_fn == null) return false;
    if (node->schema.type != val.tag) return false;
    return node->set_fn(node, val);
}

void kls(const char* path, char* out, usize out_cap) {
    if (out == null || out_cap == 0) return;
    out[0] = '\0';

    auto* node = resolve(path);
    if (node == null) {
        append_str(out, out_cap, 0, "error: path not found\n");
        return;
    }

    usize pos = 0;
    if (node == &s_proc) {
        pos = append_str(out, out_cap, pos, "count\n");
        task_snapshot snapshots[MAX_TASKS];
        usize count = sched::snapshot_tasks(snapshots, MAX_TASKS);
        if (count > MAX_TASKS) count = MAX_TASKS;
        for (usize i = 0; i < count; ++i) {
            pos = render_u64(out, out_cap, pos, snapshots[i].id);
            pos = append_ch(out, out_cap, pos, '\n');
        }
        return;
    }
    if (node == &s_proc_pid) {
        pos = append_str(out, out_cap, pos, "id\n");
        pos = append_str(out, out_cap, pos, "state\n");
        pos = append_str(out, out_cap, pos, "cpu\n");
        pos = append_str(out, out_cap, pos, "cpu_ticks\n");
        pos = append_str(out, out_cap, pos, "name\n");
        return;
    }

    for (u32 i = 0; i < node->child_count; ++i) {
        auto* child = node->children[i];
        if (child == null || child->schema.name == null) continue;
        pos = append_str(out, out_cap, pos, child->schema.name);
        pos = append_ch(out, out_cap, pos, '\n');
    }
}

void kdescribe(const char* path, char* out, usize out_cap) {
    if (out == null || out_cap == 0) return;
    out[0] = '\0';

    auto* node = resolve(path);
    if (node == null) {
        append_str(out, out_cap, 0, "error: path not found\n");
        return;
    }

    usize pos = 0;
    pos = append_str(out, out_cap, pos, "path:     ");
    pos = append_str(out, out_cap, pos, path);
    pos = append_ch(out, out_cap, pos, '\n');

    pos = append_str(out, out_cap, pos, "type:     ");
    pos = append_str(out, out_cap, pos, tag_name(node->schema.type));
    pos = append_ch(out, out_cap, pos, '\n');

    if (node->schema.type == KTag::Enum) {
        pos = append_str(out, out_cap, pos, "labels:   ");
        bool first = true;
        for (usize i = 0; i < KENUM_MAX && node->schema.enum_labels[i] != null; ++i) {
            if (!first) pos = append_str(out, out_cap, pos, ", ");
            pos = append_str(out, out_cap, pos, node->schema.enum_labels[i]);
            first = false;
        }
        pos = append_ch(out, out_cap, pos, '\n');
    } else {
        pos = append_str(out, out_cap, pos, "unit:     ");
        if (node->schema.unit != null && node->schema.unit[0] != '\0') pos = append_str(out, out_cap, pos, node->schema.unit);
        else pos = append_str(out, out_cap, pos, "(none)");
        pos = append_ch(out, out_cap, pos, '\n');

        pos = append_str(out, out_cap, pos, "writable: ");
        pos = append_str(out, out_cap, pos, node->schema.writable ? "yes" : "no");
        pos = append_ch(out, out_cap, pos, '\n');

        pos = append_str(out, out_cap, pos, "volatile: ");
        pos = append_str(out, out_cap, pos, node->schema.volatile_node ? "yes" : "no");
        pos = append_ch(out, out_cap, pos, '\n');

        if ((node->schema.type == KTag::U64 || node->schema.type == KTag::I64) && node->schema.range_max > 0) {
            pos = append_str(out, out_cap, pos, "range:    ");
            pos = render_u64(out, out_cap, pos, node->schema.range_min);
            pos = append_str(out, out_cap, pos, "..");
            pos = render_u64(out, out_cap, pos, node->schema.range_max);
            pos = append_ch(out, out_cap, pos, '\n');
        } else {
            pos = append_str(out, out_cap, pos, "range:    (unbounded)\n");
        }
        return;
    }

    pos = append_str(out, out_cap, pos, "writable: ");
    pos = append_str(out, out_cap, pos, node->schema.writable ? "yes" : "no");
    pos = append_ch(out, out_cap, pos, '\n');

    pos = append_str(out, out_cap, pos, "volatile: ");
    pos = append_str(out, out_cap, pos, node->schema.volatile_node ? "yes" : "no");
    pos = append_ch(out, out_cap, pos, '\n');
}

usize kval_render(const KVal& v, const KNode* node, char* out, usize out_cap) {
    if (out == null || out_cap == 0) return 0;
    out[0] = '\0';

    usize pos = 0;
    switch (v.tag) {
        case KTag::U64:
            pos = render_u64(out, out_cap, pos, v.as_u64);
            if (node != null && node->schema.unit != null && node->schema.unit[0] != '\0') {
                pos = append_ch(out, out_cap, pos, ' ');
                pos = append_str(out, out_cap, pos, node->schema.unit);
            }
            break;
        case KTag::I64:
            pos = render_i64(out, out_cap, pos, v.as_i64);
            if (node != null && node->schema.unit != null && node->schema.unit[0] != '\0') {
                pos = append_ch(out, out_cap, pos, ' ');
                pos = append_str(out, out_cap, pos, node->schema.unit);
            }
            break;
        case KTag::Bool:
            pos = append_str(out, out_cap, pos, v.as_bool ? "yes" : "no");
            break;
        case KTag::Str:
            pos = append_str(out, out_cap, pos, v.as_str.c_str());
            break;
        case KTag::Enum:
            if (node != null && v.as_enum_idx < KENUM_MAX && node->schema.enum_labels[v.as_enum_idx] != null) {
                pos = append_str(out, out_cap, pos, node->schema.enum_labels[v.as_enum_idx]);
            } else {
                pos = append_str(out, out_cap, pos, "<invalid-enum>");
            }
            break;
        case KTag::Struct:
            for (u32 i = 0; i < v.s.count && i < KFIELD_MAX; ++i) {
                pos = append_str(out, out_cap, pos, v.s.fields[i].key.c_str());
                pos = append_str(out, out_cap, pos, ": ");
                switch (v.s.fields[i].tag) {
                    case KTag::U64: pos = render_u64(out, out_cap, pos, v.s.fields[i].as_u64); break;
                    case KTag::I64: pos = render_i64(out, out_cap, pos, v.s.fields[i].as_i64); break;
                    case KTag::Bool: pos = append_str(out, out_cap, pos, v.s.fields[i].as_bool ? "yes" : "no"); break;
                    case KTag::Str: pos = append_str(out, out_cap, pos, v.s.fields[i].as_str.c_str()); break;
                    case KTag::Enum: pos = render_u64(out, out_cap, pos, v.s.fields[i].as_enum_idx); break;
                    case KTag::Err: pos = append_str(out, out_cap, pos, v.s.fields[i].as_err.c_str()); break;
                    default: pos = append_str(out, out_cap, pos, "<unsupported>"); break;
                }
                pos = append_ch(out, out_cap, pos, '\n');
            }
            break;
        case KTag::Stream:
            pos = append_str(out, out_cap, pos, "<stream>");
            break;
        case KTag::Err:
            pos = append_str(out, out_cap, pos, "error: ");
            pos = append_str(out, out_cap, pos, v.as_err.c_str());
            break;
        default:
            pos = append_str(out, out_cap, pos, "error: unknown value");
            break;
    }
    return pos;
}

KVal kobj_get_by_path(const char* path) {
    return kget(path);
}

bool kobj_set_by_path(const char* path, KTag tag, u64 raw_u64, const char* raw_str) {
    KVal v{};
    switch (tag) {
        case KTag::U64: v = KVal::from_u64(raw_u64); break;
        case KTag::I64: v = KVal::from_i64(static_cast<i64>(raw_u64)); break;
        case KTag::Bool: v = KVal::from_bool(raw_u64 != 0); break;
        case KTag::Str: v = KVal::from_str(raw_str); break;
        case KTag::Enum: v = KVal::from_enum(static_cast<u32>(raw_u64)); break;
        default: return false;
    }
    return kset(path, v);
}

void krpc(const char* req_json, char* out, usize out_cap) {
    if (out == null || out_cap == 0) return;
    out[0] = '\0';

    KStr op{};
    KStr path{};
    KStr value{};
    if (!json_extract_string(req_json, "op", &op)) {
        append_str(out, out_cap, 0, "{\"ok\":false,\"error\":\"missing op\"}");
        return;
    }

    usize pos = 0;
    if (op.eq("mem")) {
        log::info() << "Physical allocator: total=" << g_phys_alloc.total_pages() << " pages, free=" << g_phys_alloc.free_pages() << " pages, used=" << g_phys_alloc.used_pages() << " pages, total RAM=" << (g_phys_alloc.total_pages() * PAGE_SIZE_4K) / (1024 * 1024) << " MB";
        memory::dump_heap();
        append_json_ok(out, out_cap, "mem");
        return;
    }

    if (op.eq("tasks")) {
        sched::dump_tasks();
        append_json_ok(out, out_cap, "tasks");
        return;
    }

    if (op.eq("idt")) {
        arch::dump_idt();
        append_json_ok(out, out_cap, "idt");
        return;
    }

    if (op.eq("uptime")) {
        u64 ticks = sched::tick_count();
        pos = append_str(out, out_cap, pos, "{\"ok\":true,\"op\":\"uptime\",\"ticks\":");
        pos = render_u64(out, out_cap, pos, ticks);
        pos = append_str(out, out_cap, pos, ",\"seconds\":");
        pos = render_u64(out, out_cap, pos, ticks / SCHED_TICK_HZ);
        pos = append_ch(out, out_cap, pos, '}');
        return;
    }

    if (op.eq("reboot")) {
        append_json_ok(out, out_cap, "reboot");
        arch::reboot();
        return;
    }

    if (op.eq("drvload")) {
        KStr name{};
        if (!json_extract_string(req_json, "name", &name)) {
            append_str(out, out_cap, 0, "{\"ok\":false,\"error\":\"missing name\"}");
            return;
        }
        i32 rc = driver::load(name.c_str());
        pos = append_str(out, out_cap, pos, "{\"ok\":");
        pos = append_str(out, out_cap, pos, rc == 0 ? "true" : "false");
        pos = append_str(out, out_cap, pos, ",\"op\":\"drvload\",\"name\":\"");
        pos = append_json_escaped(out, out_cap, pos, name.c_str());
        pos = append_str(out, out_cap, pos, "\"}");
        return;
    }

    if (op.eq("drvunload")) {
        KStr name{};
        if (!json_extract_string(req_json, "name", &name)) {
            append_str(out, out_cap, 0, "{\"ok\":false,\"error\":\"missing name\"}");
            return;
        }
        i32 rc = driver::unload(name.c_str());
        pos = append_str(out, out_cap, pos, "{\"ok\":");
        pos = append_str(out, out_cap, pos, rc == 0 ? "true" : "false");
        pos = append_str(out, out_cap, pos, ",\"op\":\"drvunload\",\"name\":\"");
        pos = append_json_escaped(out, out_cap, pos, name.c_str());
        pos = append_str(out, out_cap, pos, "\"}");
        return;
    }

    if (op.eq("fs_list")) {
        char fs_path[256];
        if (!json_extract_string_buffer(req_json, "path", fs_path, sizeof(fs_path))) {
            append_str(out, out_cap, 0, "{\"ok\":false,\"error\":\"missing path\"}");
            return;
        }

        pos = append_str(out, out_cap, pos, "{\"ok\":true,\"op\":\"fs_list\",\"path\":\"");
        pos = append_json_escaped(out, out_cap, pos, fs_path);
        pos = append_str(out, out_cap, pos, "\",\"items\":[");

        fs_list_json_context context { out, out_cap, pos, true };
        if (!fs::list_directory(fs_path, append_fs_list_item, &context)) {
            append_str(out, out_cap, 0, "{\"ok\":false,\"error\":\"directory not found\"}");
            return;
        }

        pos = append_str(out, out_cap, context.pos, "]}");
        return;
    }

    if (!json_extract_string(req_json, "path", &path)) {
        append_str(out, out_cap, 0, "{\"ok\":false,\"error\":\"missing path\"}");
        return;
    }

    if (op.eq("get")) {
        auto val = kget(path.c_str());
        auto* node = resolve(path.c_str());
        char rendered[256];
        kval_render(val, node, rendered, sizeof(rendered));

        pos = append_str(out, out_cap, pos, "{\"ok\":");
        pos = append_str(out, out_cap, pos, val.tag == KTag::Err ? "false" : "true");
        pos = append_str(out, out_cap, pos, ",\"op\":\"get\",\"path\":\"");
        pos = append_json_escaped(out, out_cap, pos, path.c_str());
        pos = append_str(out, out_cap, pos, "\",\"value\":\"");
        pos = append_json_escaped(out, out_cap, pos, rendered);
        pos = append_ch(out, out_cap, pos, '\"');
        if (node != null) {
            pos = append_str(out, out_cap, pos, ",\"type\":\"");
            pos = append_str(out, out_cap, pos, tag_name(node->schema.type));
            pos = append_ch(out, out_cap, pos, '\"');
        }
        pos = append_ch(out, out_cap, pos, '}');
        return;
    }

    if (op.eq("set")) {
        if (!json_extract_string(req_json, "value", &value)) {
            append_str(out, out_cap, 0, "{\"ok\":false,\"error\":\"missing value\"}");
            return;
        }
        auto* node = resolve(path.c_str());
        if (node == null) {
            append_str(out, out_cap, 0, "{\"ok\":false,\"error\":\"path not found\"}");
            return;
        }
        if (!node->schema.writable) {
            append_str(out, out_cap, 0, "{\"ok\":false,\"error\":\"read-only\"}");
            return;
        }

        KVal set_val{};
        bool set_ok = false;
        if (node->schema.type == KTag::U64) {
            u64 parsed = 0;
            usize n = cstrlen(value.c_str());
            if (parse_u64_segment(value.c_str(), n, &parsed)) {
                set_val = KVal::from_u64(parsed);
                set_ok = kset(path.c_str(), set_val);
            }
        } else if (node->schema.type == KTag::Bool) {
            bool b = value.eq("true") || value.eq("1") || value.eq("yes");
            set_val = KVal::from_bool(b);
            set_ok = kset(path.c_str(), set_val);
        } else if (node->schema.type == KTag::Str) {
            set_val = KVal::from_str(value.c_str());
            set_ok = kset(path.c_str(), set_val);
        } else if (node->schema.type == KTag::Enum) {
            bool found = false;
            for (usize i = 0; i < KENUM_MAX && node->schema.enum_labels[i] != null; ++i) {
                if (value.eq(node->schema.enum_labels[i])) {
                    set_val = KVal::from_enum(static_cast<u32>(i));
                    set_ok = kset(path.c_str(), set_val);
                    found = true;
                    break;
                }
            }
            if (!found) {
                append_str(out, out_cap, 0, "{\"ok\":false,\"error\":\"unknown enum value\"}");
                return;
            }
        } else {
            append_str(out, out_cap, 0, "{\"ok\":false,\"error\":\"type not settable\"}");
            return;
        }

        if (!set_ok) {
            append_str(out, out_cap, 0, "{\"ok\":false,\"error\":\"set failed\"}");
            return;
        }
        append_str(out, out_cap, 0, "{\"ok\":true,\"op\":\"set\"}");
        return;
    }

    if (op.eq("ls_text")) {
        kls(path.c_str(), out, out_cap);
        return;
    }

    if (op.eq("ls")) {
        char list_buf[1024];
        kls(path.c_str(), list_buf, sizeof(list_buf));
        pos = append_str(out, out_cap, pos, "{\"ok\":true,\"op\":\"ls\",\"path\":\"");
        pos = append_json_escaped(out, out_cap, pos, path.c_str());
        pos = append_str(out, out_cap, pos, "\",\"items\":[");

        usize i = 0;
        bool first = true;
        while (list_buf[i] != '\0') {
            char item[KSTR_MAX];
            usize n = 0;
            while (list_buf[i] != '\0' && list_buf[i] != '\n' && n < KSTR_MAX - 1) item[n++] = list_buf[i++];
            item[n] = '\0';
            if (list_buf[i] == '\n') ++i;
            if (n == 0) continue;
            if (!first) pos = append_ch(out, out_cap, pos, ',');
            pos = append_ch(out, out_cap, pos, '\"');
            pos = append_json_escaped(out, out_cap, pos, item);
            pos = append_ch(out, out_cap, pos, '\"');
            first = false;
        }
        pos = append_str(out, out_cap, pos, "]}");
        return;
    }

    if (op.eq("describe")) {
        char desc_buf[512];
        kdescribe(path.c_str(), desc_buf, sizeof(desc_buf));
        pos = append_str(out, out_cap, pos, "{\"ok\":true,\"op\":\"describe\",\"path\":\"");
        pos = append_json_escaped(out, out_cap, pos, path.c_str());
        pos = append_str(out, out_cap, pos, "\",\"text\":\"");
        pos = append_json_escaped(out, out_cap, pos, desc_buf);
        pos = append_str(out, out_cap, pos, "\"}");
        return;
    }

    append_str(out, out_cap, 0, "{\"ok\":false,\"error\":\"unknown op\"}");
}

} // namespace kobj
} // namespace vk
