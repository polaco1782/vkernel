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

enum class KNodeId : u16 {
    root,
    sys,
    sys_cpu,
    sys_cpu_count,
    sys_cpu_ticks,
    sys_mem,
    sys_mem_total_kb,
    sys_mem_free_kb,
    sys_mem_heap_used,
    sys_mem_heap_capacity,
    sys_mem_heap_subheaps,
    sys_power,
    sys_power_state,
    sys_log,
    sys_log_route,
    proc,
    proc_count,
    proc_pid,
    proc_pid_id,
    proc_pid_state,
    proc_pid_cpu,
    proc_pid_cpu_ticks,
    proc_pid_name,
    fs,
    fs_active_backend,
    fs_fallback_ready,
    fs_fat32_mounted,
    fs_writable,
    fs_block_device,
    fs_root_path,
    fs_cluster_size,
    fs_root_cluster,
    dev,
    dev_block,
    dev_block_count,
    dev_block_entry,
    dev_block_entry_name,
    dev_block_entry_size_kb,
    dev_block_entry_block_size,
    dev_block_entry_removable,
    dev_pci,
    dev_pci_count,
    dev_pci_entry,
    dev_pci_entry_bus,
    dev_pci_entry_device,
    dev_pci_entry_function,
    dev_pci_entry_vendor_id,
    dev_pci_entry_device_id,
    dev_pci_entry_class_code,
    dev_pci_entry_subclass,
    dev_pci_entry_prog_if,
    dev_pci_entry_revision,
    dev_pci_entry_irq_line,
    dev_net,
    dev_net_count,
    dev_net_entry,
    dev_net_entry_name,
    dev_net_entry_mac,
    dev_net_entry_mtu,
    dev_net_entry_link_up,
    dev_net_entry_ipv4_address,
    dev_sound,
    dev_sound_active_driver,
    dev_sound_initialized,
    dev_sound_playing,
    dev_sound_sample_rate,
    dev_sound_volume_left,
    dev_sound_volume_right,
    dev_sound_mix_channels_active,
    net,
    net_device_count,
    net_primary_device,
    net_background_rx,
    net_ipv4,
    net_ipv4_configured_count,
    net_arp,
    net_arp_count,
    net_arp_entry,
    net_arp_entry_ip,
    net_arp_entry_mac,
    driver,
    driver_registered_count,
    driver_loaded_count,
    driver_entry,
    driver_entry_name,
    driver_entry_type,
    driver_entry_loaded,
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

struct KChildInfo {
    KStr name;
    KTag type;
};

struct KNodeInfo {
    KStr name;
    KTag type;
    bool readable;
    bool writable;
    bool volatile_node;
    KStr unit;
    u64 range_min;
    u64 range_max;
    const char* enum_labels[KENUM_MAX];
    u32 enum_count;
    u32 cap_mask;
};

struct KNodeSchema {
    const char* name = "";
    KTag type = KTag::Struct;
    bool writable = false;
    bool volatile_node = false;
    const char* unit = "";
    u64 range_min = 0;
    u64 range_max = 0;
    const char* enum_labels[KENUM_MAX] {};
    u32 cap_mask = 0x01;

    void reset(const char* name,
               KTag type,
               bool writable = false,
               bool volatile_node = false,
               const char* unit = "",
               u32 cap_mask = 0x01);
    void set_enum_labels(const char* const* labels, usize count);
};

struct KNode {
    using get_callback = KVal (*)(KNode&);
    using set_callback = bool (*)(KNode&, const KVal&);

    KNodeId node_id = KNodeId::root;
    KNodeSchema schema {};
    get_callback get_fn = null;
    set_callback set_fn = null;
    KNode* children[KNODE_CHILDREN_MAX] {};
    u32 child_count = 0;
    KNode* parent = null;

    void reset(KNodeId id,
               const char* name,
               KTag type,
               bool writable = false,
               bool volatile_node = false,
               const char* unit = "",
               u32 cap_mask = 0x01,
               get_callback get_fn = null,
               set_callback set_fn = null);
    void attach(KNode& child);
    auto find_child(string_view name) const -> KNode*;
    auto child(usize index) const -> KNode*;
    auto id() const -> KNodeId { return node_id; }
    auto readable() const -> bool { return get_fn != null; }
    auto writable() const -> bool { return schema.writable && set_fn != null; }
    auto read() -> KVal;
    auto write(const KVal& value) -> bool;
};

void init();
bool is_initialized();

KNode* resolve(const char* path);
KNode* resolve(const char* path, usize len);

bool kinfo(const char* path, KNodeInfo* out);
bool kquery(const char* path, KVal* out_value, KNodeInfo* out_info = null);
usize klist(const char* path, KChildInfo* out, usize max_children);
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
