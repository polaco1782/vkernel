/*
 * vkernel - UEFI Microkernel
 * Copyright (C) 2026 vkernel authors
 *
 * driver.cpp - Driver registry kobj subtree
 */

#include "internal.h"

namespace vk::kobj {

static KNode s_driver;
static KNode s_driver_registered_count;
static KNode s_driver_loaded_count;
static KNode s_driver_entry;
static KNode s_driver_entry_name;
static KNode s_driver_entry_type;
static KNode s_driver_entry_loaded;

static constexpr const char* kDriverTypeLabels[] = { "none", "sound", "block", "network" };
static constexpr static_child_definition kDriverFields[] = {
    { "name", KTag::Str },
    { "type", KTag::Enum },
    { "loaded", KTag::Bool },
};

static auto get_driver_registered_count(KNode&) -> KVal {
    return KVal::from_u64(static_cast<u64>(driver::registered_count()));
}

static auto get_driver_loaded_count(KNode&) -> KVal {
    u64 count = 0;
    for (usize index = 0; index < driver::registered_count(); ++index) {
        const auto* desc = driver::get_registered(index);
        if (desc != null && desc->name != null && driver::is_loaded(desc->name)) {
            ++count;
        }
    }
    return KVal::from_u64(count);
}

void register_driver_nodes() {
    static const node_definition kDefinitions[] = {
        { &s_driver, &s_root, KNodeId::driver, "driver", KTag::Struct },
        { &s_driver_registered_count, &s_driver, KNodeId::driver_registered_count, "registered_count", KTag::U64, false, false, "count", 0x01, get_driver_registered_count },
        { &s_driver_loaded_count, &s_driver, KNodeId::driver_loaded_count, "loaded_count", KTag::U64, false, true, "count", 0x01, get_driver_loaded_count },
        { &s_driver_entry, null, KNodeId::driver_entry, "<driver>", KTag::Struct },
        { &s_driver_entry_name, null, KNodeId::driver_entry_name, "name", KTag::Str },
        { &s_driver_entry_type, null, KNodeId::driver_entry_type, "type", KTag::Enum, false, false, "", 0x01, null, null, kDriverTypeLabels, 4 },
        { &s_driver_entry_loaded, null, KNodeId::driver_entry_loaded, "loaded", KTag::Bool, false, true },
    };

    for (const auto& definition : kDefinitions) {
        register_node(definition);
    }
}

auto resolve_driver_path(const char* path, usize len) -> resolved_node {
    resolved_node resolved {};
    string_view p(path, len);
    if (!p.starts_with("driver/")) {
        return resolved;
    }

    p.remove_prefix(7);
    if (p.empty()) {
        resolved.node = &s_driver;
        return resolved;
    }
    if (p.equals("registered_count")) {
        resolved.node = &s_driver_registered_count;
        return resolved;
    }
    if (p.equals("loaded_count")) {
        resolved.node = &s_driver_loaded_count;
        return resolved;
    }

    usize name_end = 0;
    while (name_end < p.size() && p[name_end] != '/') {
        ++name_end;
    }

    char name_buf[KSTR_MAX];
    if (name_end >= sizeof(name_buf)) {
        return {};
    }
    for (usize index = 0; index < name_end; ++index) {
        name_buf[index] = p[index];
    }
    name_buf[name_end] = '\0';

    const auto* desc = driver::find(name_buf);
    if (desc == null) {
        return {};
    }

    resolved.node = &s_driver_entry;
    resolved.kind = resolved_node::virtual_kind::driver_entry;
    resolved.driver = desc;
    append_path_segment_name(&resolved.name, string_view(p.data(), name_end));
    resolved.has_name = true;

    if (name_end >= p.size()) {
        return resolved;
    }
    if (p[name_end] != '/') {
        return {};
    }

    const string_view tail(p.data() + name_end + 1, p.size() - name_end - 1);
    if (tail.equals("name")) resolved.node = &s_driver_entry_name;
    else if (tail.equals("type")) resolved.node = &s_driver_entry_type;
    else if (tail.equals("loaded")) resolved.node = &s_driver_entry_loaded;
    else return {};
    return resolved;
}

auto driver_query_value(const resolved_node& resolved) -> KVal {
    if (resolved.kind != resolved_node::virtual_kind::driver_entry || resolved.driver == null) {
        return KVal::err("node is not readable");
    }

    switch (resolved.node->id()) {
        case KNodeId::driver_entry_name:
            return KVal::from_str(resolved.driver->name);
        case KNodeId::driver_entry_type:
            return KVal::from_enum(static_cast<u32>(resolved.driver->type == driver_type::none ? 0
                : resolved.driver->type == driver_type::sound ? 1
                : resolved.driver->type == driver_type::block ? 2
                : 3));
        case KNodeId::driver_entry_loaded:
            return KVal::from_bool(driver::is_loaded(resolved.driver->name));
        default:
            return KVal::err("node is not readable");
    }
}

auto driver_list_children(const resolved_node& resolved,
                          KChildInfo* out,
                          usize max_children) -> usize {
    if (resolved.node == &s_driver) {
        const usize driver_count = driver::registered_count();
        const usize total = 2 + driver_count;
        append_child_info(out, max_children, 0, "registered_count", KTag::U64);
        append_child_info(out, max_children, 1, "loaded_count", KTag::U64);
        for (usize index = 0; index < driver_count && (index + 2) < max_children; ++index) {
            const auto* desc = driver::get_registered(index);
            if (desc == null || desc->name == null) {
                continue;
            }
            append_child_info(out, max_children, index + 2, desc->name, KTag::Struct);
        }
        return total;
    }

    return append_static_children(out, max_children, kDriverFields, sizeof(kDriverFields) / sizeof(kDriverFields[0]));
}

} // namespace vk::kobj
