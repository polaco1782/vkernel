/*
 * vkernel - UEFI Microkernel
 * Copyright (C) 2026 vkernel authors
 *
 * internal.h - Shared kobj module internals
 */

#pragma once

#include "arp.h"
#include "block.h"
#include "detail.h"
#include "driver.h"
#include "fs.h"
#include "ipv4.h"
#include "kobj.h"
#include "net.h"
#include "pci.h"
#include "scheduler.h"

namespace vk::kobj {

extern KNode s_root;
extern KNode s_dev;

struct node_definition {
    KNode* node = null;
    KNode* parent = null;
    KNodeId id = KNodeId::root;
    const char* name = "";
    KTag type = KTag::Struct;
    bool writable = false;
    bool volatile_node = false;
    const char* unit = "";
    u32 cap_mask = 0x01;
    KNode::get_callback get_fn = null;
    KNode::set_callback set_fn = null;
    const char* const* enum_labels = null;
    usize enum_label_count = 0;
};

struct resolved_node {
    enum class virtual_kind : u8 {
        none,
        proc_task,
        block_device,
        pci_device,
        net_device,
        arp_entry,
        driver_entry,
    };

    KNode* node = null;
    virtual_kind kind = virtual_kind::none;
    bool has_task_snapshot = false;
    task_snapshot task {};
    block_device* block = null;
    const pci_device* pci = null;
    net_device* net = null;
    net::arp::cache_entry_info arp {};
    const driver_descriptor* driver = null;
    KStr name;
    bool has_name = false;
};

struct static_child_definition {
    const char* name;
    KTag type;
};

auto make_resolved(KNode* node) -> resolved_node;
auto append_path_segment_name(KStr* out, string_view segment) -> void;
auto register_node(const node_definition& definition) -> void;
auto append_child_info(KChildInfo* out,
                       usize max_children,
                       usize index,
                       const char* name,
                       KTag type) -> void;
auto append_numeric_child_name(KChildInfo* out,
                               usize max_children,
                               usize index,
                               u64 value) -> void;
auto append_static_children(KChildInfo* out,
                            usize max_children,
                            const static_child_definition* definitions,
                            usize count) -> usize;
auto append_attached_children(const KNode& node,
                              KChildInfo* out,
                              usize max_children) -> usize;
auto fill_node_info(const resolved_node& resolved, KNodeInfo* out) -> bool;

auto resolve_proc_path(const char* path, usize len) -> resolved_node;
auto resolve_block_path(const char* path, usize len) -> resolved_node;
auto resolve_pci_path(const char* path, usize len) -> resolved_node;
auto resolve_net_device_path(const char* path, usize len) -> resolved_node;
auto resolve_net_stack_path(const char* path, usize len) -> resolved_node;
auto resolve_driver_path(const char* path, usize len) -> resolved_node;

auto proc_query_value(const resolved_node& resolved) -> KVal;
auto device_query_value(const resolved_node& resolved) -> KVal;
auto network_query_value(const resolved_node& resolved) -> KVal;
auto driver_query_value(const resolved_node& resolved) -> KVal;

auto proc_list_children(const resolved_node& resolved,
                        KChildInfo* out,
                        usize max_children) -> usize;
auto device_list_children(const resolved_node& resolved,
                          KChildInfo* out,
                          usize max_children) -> usize;
auto network_list_children(const resolved_node& resolved,
                           KChildInfo* out,
                           usize max_children) -> usize;
auto driver_list_children(const resolved_node& resolved,
                          KChildInfo* out,
                          usize max_children) -> usize;

void register_core_nodes();
void register_proc_nodes();
void register_device_nodes();
void register_network_nodes();
void register_driver_nodes();

} // namespace vk::kobj
