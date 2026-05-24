/*
 * vkernel - UEFI Microkernel
 * Copyright (C) 2026 vkernel authors
 *
 * core.cpp - Shared kobj tree mechanics and core subtrees
 */

#include "internal.h"

#include "log.h"
#include "memory.h"
#include "panic.h"
#include "smp.h"

namespace vk::kobj {

using detail::append_ch;
using detail::append_str;
using detail::cstrlen;
using detail::render_i64;
using detail::render_u64;
using detail::tag_name;

KNode s_root;

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
static KNode s_fs;
static KNode s_fs_active_backend;
static KNode s_fs_fallback_ready;
static KNode s_fs_fat32_mounted;
static KNode s_fs_writable;
static KNode s_fs_block_device;
static KNode s_fs_root_path;
static KNode s_fs_cluster_size;
static KNode s_fs_root_cluster;

static constexpr const char* kPowerStateLabels[] = { "running", "suspend", "hibernate" };
static constexpr const char* kLogRouteLabels[] = { "default", "serial", "disabled" };

static u32 s_power_state = 0;
static bool s_initialized = false;

static auto resolved_is_proc_leaf(const resolved_node& resolved) -> bool;
static auto resolved_is_dynamic_leaf(const resolved_node& resolved) -> bool;

auto make_resolved(KNode* node) -> resolved_node {
    resolved_node resolved {};
    resolved.node = node;
    return resolved;
}

auto append_path_segment_name(KStr* out, string_view segment) -> void {
    if (out == null) {
        return;
    }
    out->set(segment.data(), segment.size());
}

void KNodeSchema::reset(const char* node_name,
                        KTag node_type,
                        bool node_writable,
                        bool node_volatile,
                        const char* node_unit,
                        u32 node_cap_mask) {
    *this = {};
    name = node_name != null ? node_name : "";
    type = node_type;
    writable = node_writable;
    volatile_node = node_volatile;
    unit = node_unit != null ? node_unit : "";
    cap_mask = node_cap_mask;
}

void KNodeSchema::set_enum_labels(const char* const* labels, usize count) {
    for (usize index = 0; index < count && index < KENUM_MAX; ++index) {
        enum_labels[index] = labels[index];
    }
}

void KNode::reset(KNodeId id,
                  const char* name,
                  KTag type,
                  bool writable,
                  bool volatile_node,
                  const char* unit,
                  u32 cap_mask,
                  get_callback read_fn,
                  set_callback write_fn) {
    *this = {};
    node_id = id;
    schema.reset(name, type, writable, volatile_node, unit, cap_mask);
    get_fn = read_fn;
    set_fn = write_fn;
}

void KNode::attach(KNode& child) {
    if (child_count >= KNODE_CHILDREN_MAX) {
        vk_panic(__FILE__, __LINE__, "kobj: too many children");
    }
    child.parent = this;
    children[child_count++] = &child;
}

auto KNode::find_child(string_view name) const -> KNode* {
    for (u32 index = 0; index < child_count; ++index) {
        auto* candidate = children[index];
        if (candidate == null || candidate->schema.name == null) {
            continue;
        }
        if (string_view(candidate->schema.name).equals(name)) {
            return candidate;
        }
    }
    return null;
}

auto KNode::child(usize index) const -> KNode* {
    if (index >= child_count) {
        return null;
    }
    return children[index];
}

auto KNode::read() -> KVal {
    return get_fn != null ? get_fn(*this) : KVal::err("node is not readable");
}

auto KNode::write(const KVal& value) -> bool {
    return set_fn != null ? set_fn(*this, value) : false;
}

auto register_node(const node_definition& definition) -> void {
    if (definition.node == null) {
        return;
    }

    definition.node->reset(definition.id,
                           definition.name,
                           definition.type,
                           definition.writable,
                           definition.volatile_node,
                           definition.unit,
                           definition.cap_mask,
                           definition.get_fn,
                           definition.set_fn);
    if (definition.enum_labels != null && definition.enum_label_count != 0) {
        definition.node->schema.set_enum_labels(definition.enum_labels, definition.enum_label_count);
    }
    if (definition.parent != null) {
        definition.parent->attach(*definition.node);
    }
}

void KStr::set(const char* s) {
    if (s == null) {
        buf[0] = '\0';
        len = 0;
        return;
    }
    usize index = 0;
    while (s[index] != '\0' && index < KSTR_MAX - 1) {
        buf[index] = s[index];
        ++index;
    }
    buf[index] = '\0';
    len = static_cast<u32>(index);
}

void KStr::set(const char* s, usize n) {
    if (s == null) {
        buf[0] = '\0';
        len = 0;
        return;
    }
    usize index = 0;
    while (index < n && index < KSTR_MAX - 1) {
        buf[index] = s[index];
        ++index;
    }
    buf[index] = '\0';
    len = static_cast<u32>(index);
}

bool KStr::eq(const char* s) const {
    if (s == null) {
        return false;
    }
    for (usize index = 0;; ++index) {
        if (buf[index] != s[index]) {
            return false;
        }
        if (buf[index] == '\0') {
            return true;
        }
    }
}

KVal KVal::from_u64(u64 v) {
    KVal out {};
    out.tag = KTag::U64;
    out.as_u64 = v;
    return out;
}

KVal KVal::from_i64(i64 v) {
    KVal out {};
    out.tag = KTag::I64;
    out.as_i64 = v;
    return out;
}

KVal KVal::from_bool(bool v) {
    KVal out {};
    out.tag = KTag::Bool;
    out.as_bool = v;
    return out;
}

KVal KVal::from_str(const char* s) {
    KVal out {};
    out.tag = KTag::Str;
    out.as_str.set(s);
    return out;
}

KVal KVal::from_enum(u32 idx) {
    KVal out {};
    out.tag = KTag::Enum;
    out.as_enum_idx = idx;
    return out;
}

KVal KVal::stream() {
    KVal out {};
    out.tag = KTag::Stream;
    return out;
}

KVal KVal::err(const char* msg) {
    KVal out {};
    out.tag = KTag::Err;
    out.as_err.set(msg);
    return out;
}

static auto get_sys_cpu_count(KNode&) -> KVal {
    return KVal::from_u64(static_cast<u64>(smp::cpu_count()));
}

static auto get_sys_cpu_ticks(KNode&) -> KVal {
    return KVal::from_u64(sched::tick_count());
}

static auto get_sys_mem_total_kb(KNode&) -> KVal {
    return KVal::from_u64((g_phys_alloc.total_pages() * PAGE_SIZE_4K) / 1024ULL);
}

static auto get_sys_mem_free_kb(KNode&) -> KVal {
    return KVal::from_u64((g_phys_alloc.free_pages() * PAGE_SIZE_4K) / 1024ULL);
}

static auto get_sys_mem_heap_used(KNode&) -> KVal {
    u64 used = 0;
    for (auto* block = g_kernel_heap.get_free_list(); block != null; block = block->next) {
        if (block->used) {
            used += block->size;
        }
    }
    return KVal::from_u64(used);
}

static auto get_sys_mem_heap_capacity(KNode&) -> KVal {
    return KVal::from_u64(g_kernel_heap.total_bytes());
}

static auto get_sys_mem_heap_subheaps(KNode&) -> KVal {
    return KVal::from_u64(static_cast<u64>(g_kernel_heap.subheap_count()));
}

static auto get_sys_power_state(KNode&) -> KVal {
    return KVal::from_enum(s_power_state);
}

static auto set_sys_power_state(KNode&, const KVal& value) -> bool {
    if (value.tag != KTag::Enum || value.as_enum_idx > 2) {
        return false;
    }
    s_power_state = value.as_enum_idx;
    return true;
}

static auto get_sys_log_route(KNode&) -> KVal {
    return KVal::from_enum(static_cast<u32>(log::get_route()));
}

static auto set_sys_log_route(KNode&, const KVal& value) -> bool {
    if (value.tag != KTag::Enum || value.as_enum_idx > 2) {
        return false;
    }
    log::set_route(static_cast<log::route>(value.as_enum_idx));
    return true;
}

static auto get_fs_active_backend(KNode&) -> KVal {
    const auto info = fs::query_info();
    return KVal::from_str(info.active_backend.c_str());
}

static auto get_fs_fallback_ready(KNode&) -> KVal {
    return KVal::from_bool(fs::query_info().fallback_ready);
}

static auto get_fs_fat32_mounted(KNode&) -> KVal {
    return KVal::from_bool(fs::query_info().fat32_mounted);
}

static auto get_fs_writable(KNode&) -> KVal {
    return KVal::from_bool(fs::query_info().writable);
}

static auto get_fs_block_device(KNode&) -> KVal {
    const auto info = fs::query_info();
    return KVal::from_str(info.block_device.c_str());
}

static auto get_fs_root_path(KNode&) -> KVal {
    const auto info = fs::query_info();
    return KVal::from_str(info.logical_root_path.c_str());
}

static auto get_fs_cluster_size(KNode&) -> KVal {
    return KVal::from_u64(fs::query_info().cluster_size);
}

static auto get_fs_root_cluster(KNode&) -> KVal {
    return KVal::from_u64(fs::query_info().root_cluster);
}

void register_core_nodes() {
    static const node_definition kDefinitions[] = {
        { &s_root, null, KNodeId::root, "", KTag::Struct },

        { &s_sys, &s_root, KNodeId::sys, "sys", KTag::Struct },
        { &s_sys_cpu, &s_sys, KNodeId::sys_cpu, "cpu", KTag::Struct },
        { &s_sys_cpu_count, &s_sys_cpu, KNodeId::sys_cpu_count, "count", KTag::U64, false, false, "", 0x01, get_sys_cpu_count },
        { &s_sys_cpu_ticks, &s_sys_cpu, KNodeId::sys_cpu_ticks, "ticks", KTag::U64, false, true, "", 0x01, get_sys_cpu_ticks },

        { &s_sys_mem, &s_sys, KNodeId::sys_mem, "mem", KTag::Struct },
        { &s_sys_mem_total_kb, &s_sys_mem, KNodeId::sys_mem_total_kb, "total_kb", KTag::U64, false, false, "kb", 0x01, get_sys_mem_total_kb },
        { &s_sys_mem_free_kb, &s_sys_mem, KNodeId::sys_mem_free_kb, "free_kb", KTag::U64, false, true, "kb", 0x01, get_sys_mem_free_kb },
        { &s_sys_mem_heap_used, &s_sys_mem, KNodeId::sys_mem_heap_used, "heap_used", KTag::U64, false, true, "bytes", 0x01, get_sys_mem_heap_used },
        { &s_sys_mem_heap_capacity, &s_sys_mem, KNodeId::sys_mem_heap_capacity, "heap_capacity", KTag::U64, false, true, "bytes", 0x01, get_sys_mem_heap_capacity },
        { &s_sys_mem_heap_subheaps, &s_sys_mem, KNodeId::sys_mem_heap_subheaps, "heap_subheaps", KTag::U64, false, true, "count", 0x01, get_sys_mem_heap_subheaps },

        { &s_sys_power, &s_sys, KNodeId::sys_power, "power", KTag::Struct },
        { &s_sys_power_state, &s_sys_power, KNodeId::sys_power_state, "state", KTag::Enum, true, false, "", 0x03, get_sys_power_state, set_sys_power_state, kPowerStateLabels, 3 },
        { &s_sys_log, &s_sys, KNodeId::sys_log, "log", KTag::Struct },
        { &s_sys_log_route, &s_sys_log, KNodeId::sys_log_route, "route", KTag::Enum, true, false, "", 0x03, get_sys_log_route, set_sys_log_route, kLogRouteLabels, 3 },

        { &s_fs, &s_root, KNodeId::fs, "fs", KTag::Struct },
        { &s_fs_active_backend, &s_fs, KNodeId::fs_active_backend, "active_backend", KTag::Str, false, false, "", 0x01, get_fs_active_backend },
        { &s_fs_fallback_ready, &s_fs, KNodeId::fs_fallback_ready, "fallback_ready", KTag::Bool, false, false, "", 0x01, get_fs_fallback_ready },
        { &s_fs_fat32_mounted, &s_fs, KNodeId::fs_fat32_mounted, "fat32_mounted", KTag::Bool, false, false, "", 0x01, get_fs_fat32_mounted },
        { &s_fs_writable, &s_fs, KNodeId::fs_writable, "writable", KTag::Bool, false, false, "", 0x01, get_fs_writable },
        { &s_fs_block_device, &s_fs, KNodeId::fs_block_device, "block_device", KTag::Str, false, false, "", 0x01, get_fs_block_device },
        { &s_fs_root_path, &s_fs, KNodeId::fs_root_path, "root_path", KTag::Str, false, false, "", 0x01, get_fs_root_path },
        { &s_fs_cluster_size, &s_fs, KNodeId::fs_cluster_size, "cluster_size", KTag::U64, false, false, "bytes", 0x01, get_fs_cluster_size },
        { &s_fs_root_cluster, &s_fs, KNodeId::fs_root_cluster, "root_cluster", KTag::U64, false, false, "", 0x01, get_fs_root_cluster },
    };

    for (const auto& definition : kDefinitions) {
        register_node(definition);
    }
}

auto fill_node_info(const resolved_node& resolved, KNodeInfo* out) -> bool {
    if (out == null || resolved.node == null) {
        return false;
    }

    *out = {};
    if (resolved.has_name) {
        out->name = resolved.name;
    } else {
        out->name.set(resolved.node->schema.name);
    }
    out->type = resolved.node->schema.type;
    out->readable = resolved_is_dynamic_leaf(resolved) || resolved.node->readable();
    out->writable = resolved.node->writable();
    out->volatile_node = resolved.node->schema.volatile_node;
    out->unit.set(resolved.node->schema.unit);
    out->range_min = resolved.node->schema.range_min;
    out->range_max = resolved.node->schema.range_max;
    out->cap_mask = resolved.node->schema.cap_mask;

    for (usize index = 0; index < KENUM_MAX && resolved.node->schema.enum_labels[index] != null; ++index) {
        out->enum_labels[out->enum_count++] = resolved.node->schema.enum_labels[index];
    }
    return true;
}

auto append_child_info(KChildInfo* out,
                       usize max_children,
                       usize index,
                       const char* name,
                       KTag type) -> void {
    if (out == null || index >= max_children) {
        return;
    }
    out[index].name.set(name);
    out[index].type = type;
}

auto append_numeric_child_name(KChildInfo* out,
                               usize max_children,
                               usize index,
                               u64 value) -> void {
    if (out == null || index >= max_children) {
        return;
    }
    out[index].name.set("");
    render_u64(out[index].name.buf, KSTR_MAX, 0, value);
    out[index].name.len = static_cast<u32>(cstrlen(out[index].name.buf));
    out[index].type = KTag::Struct;
}

auto append_static_children(KChildInfo* out,
                            usize max_children,
                            const static_child_definition* definitions,
                            usize count) -> usize {
    for (usize index = 0; index < count; ++index) {
        append_child_info(out, max_children, index, definitions[index].name, definitions[index].type);
    }
    return count;
}

auto append_attached_children(const KNode& node,
                              KChildInfo* out,
                              usize max_children) -> usize {
    const usize total = node.child_count;
    if (out == null || max_children == 0) {
        return total;
    }
    for (usize index = 0; index < total && index < max_children; ++index) {
        auto* child = node.child(index);
        if (child == null || child->schema.name == null) {
            continue;
        }
        out[index].name.set(child->schema.name);
        out[index].type = child->schema.type;
    }
    return total;
}

static auto resolve_internal(const char* path, usize len) -> resolved_node {
    init();

    if (path == null) {
        return {};
    }
    if (len == 0) {
        return make_resolved(&s_root);
    }

    usize start = 0;
    while (start < len && path[start] == '/') {
        ++start;
    }
    if (start >= len || path[start] == '\0') {
        return make_resolved(&s_root);
    }

    const resolved_node proc = resolve_proc_path(path + start, len - start);
    if (proc.node != null) {
        return proc;
    }
    const resolved_node block = resolve_block_path(path + start, len - start);
    if (block.node != null) {
        return block;
    }
    const resolved_node pci = resolve_pci_path(path + start, len - start);
    if (pci.node != null) {
        return pci;
    }
    const resolved_node dev_net = resolve_net_device_path(path + start, len - start);
    if (dev_net.node != null) {
        return dev_net;
    }
    const resolved_node net_stack = resolve_net_stack_path(path + start, len - start);
    if (net_stack.node != null) {
        return net_stack;
    }
    const resolved_node driver = resolve_driver_path(path + start, len - start);
    if (driver.node != null) {
        return driver;
    }

    KNode* node = &s_root;
    while (start < len && path[start] != '\0') {
        usize end = start;
        while (end < len && path[end] != '/' && path[end] != '\0') {
            ++end;
        }

        if (end == start) {
            start = end + 1;
            continue;
        }

        const string_view seg(path + start, end - start);
        KNode* found = node->find_child(seg);
        if (found == null) {
            return {};
        }
        node = found;

        start = end;
        while (start < len && path[start] == '/') {
            ++start;
        }
    }

    return make_resolved(node);
}

static auto resolved_is_proc_leaf(const resolved_node& resolved) -> bool {
    if (!resolved.has_task_snapshot || resolved.node == null) {
        return false;
    }

    switch (resolved.node->id()) {
        case KNodeId::proc:
        case KNodeId::proc_count:
        case KNodeId::proc_pid:
            return false;
        default:
            return true;
    }
}

static auto resolved_is_dynamic_leaf(const resolved_node& resolved) -> bool {
    if (resolved_is_proc_leaf(resolved)) {
        return true;
    }

    switch (resolved.kind) {
        case resolved_node::virtual_kind::block_device:
            return resolved.node != null && resolved.node->id() != KNodeId::dev_block;
        case resolved_node::virtual_kind::pci_device:
            return resolved.node != null && resolved.node->id() != KNodeId::dev_pci;
        case resolved_node::virtual_kind::net_device:
            return resolved.node != null && resolved.node->id() != KNodeId::dev_net;
        case resolved_node::virtual_kind::arp_entry:
            return resolved.node != null && resolved.node->id() != KNodeId::net_arp;
        case resolved_node::virtual_kind::driver_entry:
            return resolved.node != null && resolved.node->id() != KNodeId::driver;
        default:
            return false;
    }
}

static auto query_dynamic_value(const resolved_node& resolved) -> KVal {
    if (resolved_is_proc_leaf(resolved)) {
        return proc_query_value(resolved);
    }

    switch (resolved.kind) {
        case resolved_node::virtual_kind::block_device:
        case resolved_node::virtual_kind::pci_device:
            return device_query_value(resolved);
        case resolved_node::virtual_kind::net_device:
        case resolved_node::virtual_kind::arp_entry:
            return network_query_value(resolved);
        case resolved_node::virtual_kind::driver_entry:
            return driver_query_value(resolved);
        default:
            return KVal::err("node is not readable");
    }
}

void init() {
    if (s_initialized) {
        return;
    }

    register_core_nodes();
    register_proc_nodes();
    register_device_nodes();
    register_network_nodes();
    register_driver_nodes();

    s_initialized = true;
    log::info() << "kobj: initialized";
}

bool is_initialized() {
    return s_initialized;
}

KNode* resolve(const char* path, usize len) {
    return resolve_internal(path, len).node;
}

KNode* resolve(const char* path) {
    return resolve(path, cstrlen(path));
}

bool kinfo(const char* path, KNodeInfo* out) {
    if (out == null) {
        return false;
    }
    return fill_node_info(resolve_internal(path, cstrlen(path)), out);
}

bool kquery(const char* path, KVal* out_value, KNodeInfo* out_info) {
    const resolved_node resolved = resolve_internal(path, cstrlen(path));
    if (resolved.node == null) {
        return false;
    }

    if (out_info != null) {
        fill_node_info(resolved, out_info);
    }

    if (out_value == null) {
        return true;
    }

    if (resolved_is_dynamic_leaf(resolved)) {
        *out_value = query_dynamic_value(resolved);
        return true;
    }

    if (!resolved.node->readable()) {
        *out_value = KVal::err("node is not readable");
        return true;
    }

    *out_value = resolved.node->read();
    return true;
}

usize klist(const char* path, KChildInfo* out, usize max_children) {
    const resolved_node resolved = resolve_internal(path, cstrlen(path));
    if (resolved.node == null) {
        return 0;
    }

    switch (resolved.node->id()) {
        case KNodeId::proc:
        case KNodeId::proc_pid:
            return proc_list_children(resolved, out, max_children);
        case KNodeId::dev_block:
        case KNodeId::dev_block_entry:
        case KNodeId::dev_pci:
        case KNodeId::dev_pci_entry:
            return device_list_children(resolved, out, max_children);
        case KNodeId::dev_net:
        case KNodeId::dev_net_entry:
        case KNodeId::net_arp:
        case KNodeId::net_arp_entry:
            return network_list_children(resolved, out, max_children);
        case KNodeId::driver:
        case KNodeId::driver_entry:
            return driver_list_children(resolved, out, max_children);
        default:
            return append_attached_children(*resolved.node, out, max_children);
    }
}

KVal kget(const char* path) {
    KVal value {};
    if (!kquery(path, &value, null)) {
        return KVal::err("path not found");
    }
    return value;
}

bool kset(const char* path, KVal val) {
    const resolved_node resolved = resolve_internal(path, cstrlen(path));
    if (resolved.node == null) {
        return false;
    }
    if (resolved_is_dynamic_leaf(resolved)) {
        return false;
    }
    if (!resolved.node->writable()) {
        return false;
    }
    if (resolved.node->schema.type != val.tag) {
        return false;
    }
    return resolved.node->write(val);
}

void kls(const char* path, char* out, usize out_cap) {
    if (out == null || out_cap == 0) {
        return;
    }
    out[0] = '\0';

    KChildInfo children[64] {};
    const usize total = klist(path, children, sizeof(children) / sizeof(children[0]));
    if (total == 0 && resolve(path) == null) {
        append_str(out, out_cap, 0, "error: path not found\n");
        return;
    }

    usize pos = 0;
    const usize limit = total < (sizeof(children) / sizeof(children[0]))
        ? total
        : (sizeof(children) / sizeof(children[0]));
    for (usize index = 0; index < limit; ++index) {
        if (children[index].name.c_str()[0] == '\0') {
            continue;
        }
        pos = append_str(out, out_cap, pos, children[index].name.c_str());
        pos = append_ch(out, out_cap, pos, '\n');
    }
}

void kdescribe(const char* path, char* out, usize out_cap) {
    if (out == null || out_cap == 0) {
        return;
    }
    out[0] = '\0';

    KNodeInfo info {};
    if (!kinfo(path, &info)) {
        append_str(out, out_cap, 0, "error: path not found\n");
        return;
    }

    usize pos = 0;
    pos = append_str(out, out_cap, pos, "path:     ");
    pos = append_str(out, out_cap, pos, path != null ? path : "");
    pos = append_ch(out, out_cap, pos, '\n');

    pos = append_str(out, out_cap, pos, "type:     ");
    pos = append_str(out, out_cap, pos, tag_name(info.type));
    pos = append_ch(out, out_cap, pos, '\n');

    if (info.type == KTag::Enum) {
        pos = append_str(out, out_cap, pos, "labels:   ");
        for (u32 index = 0; index < info.enum_count; ++index) {
            if (index != 0) {
                pos = append_str(out, out_cap, pos, ", ");
            }
            pos = append_str(out, out_cap, pos, info.enum_labels[index]);
        }
        pos = append_ch(out, out_cap, pos, '\n');
    } else {
        pos = append_str(out, out_cap, pos, "unit:     ");
        pos = append_str(out, out_cap, pos, info.unit.c_str()[0] != '\0' ? info.unit.c_str() : "(none)");
        pos = append_ch(out, out_cap, pos, '\n');
    }

    pos = append_str(out, out_cap, pos, "readable: ");
    pos = append_str(out, out_cap, pos, info.readable ? "yes" : "no");
    pos = append_ch(out, out_cap, pos, '\n');

    pos = append_str(out, out_cap, pos, "writable: ");
    pos = append_str(out, out_cap, pos, info.writable ? "yes" : "no");
    pos = append_ch(out, out_cap, pos, '\n');

    pos = append_str(out, out_cap, pos, "volatile: ");
    pos = append_str(out, out_cap, pos, info.volatile_node ? "yes" : "no");
    pos = append_ch(out, out_cap, pos, '\n');

    if ((info.type == KTag::U64 || info.type == KTag::I64) && info.range_max > 0) {
        pos = append_str(out, out_cap, pos, "range:    ");
        pos = render_u64(out, out_cap, pos, info.range_min);
        pos = append_str(out, out_cap, pos, "..");
        pos = render_u64(out, out_cap, pos, info.range_max);
        pos = append_ch(out, out_cap, pos, '\n');
    } else if (info.type != KTag::Enum) {
        pos = append_str(out, out_cap, pos, "range:    (unbounded)\n");
    }
}

usize kval_render(const KVal& v, const KNode* node, char* out, usize out_cap) {
    if (out == null || out_cap == 0) {
        return 0;
    }
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
            for (u32 index = 0; index < v.s.count && index < KFIELD_MAX; ++index) {
                pos = append_str(out, out_cap, pos, v.s.fields[index].key.c_str());
                pos = append_str(out, out_cap, pos, ": ");
                switch (v.s.fields[index].tag) {
                    case KTag::U64: pos = render_u64(out, out_cap, pos, v.s.fields[index].as_u64); break;
                    case KTag::I64: pos = render_i64(out, out_cap, pos, v.s.fields[index].as_i64); break;
                    case KTag::Bool: pos = append_str(out, out_cap, pos, v.s.fields[index].as_bool ? "yes" : "no"); break;
                    case KTag::Str: pos = append_str(out, out_cap, pos, v.s.fields[index].as_str.c_str()); break;
                    case KTag::Enum: pos = render_u64(out, out_cap, pos, v.s.fields[index].as_enum_idx); break;
                    case KTag::Err: pos = append_str(out, out_cap, pos, v.s.fields[index].as_err.c_str()); break;
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
    KVal value {};
    switch (tag) {
        case KTag::U64: value = KVal::from_u64(raw_u64); break;
        case KTag::I64: value = KVal::from_i64(static_cast<i64>(raw_u64)); break;
        case KTag::Bool: value = KVal::from_bool(raw_u64 != 0); break;
        case KTag::Str: value = KVal::from_str(raw_str); break;
        case KTag::Enum: value = KVal::from_enum(static_cast<u32>(raw_u64)); break;
        default: return false;
    }
    return kset(path, value);
}

} // namespace vk::kobj
