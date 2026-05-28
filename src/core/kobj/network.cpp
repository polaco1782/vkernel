/*
 * vkernel - UEFI Microkernel
 * Copyright (C) 2026 vkernel authors
 *
 * network.cpp - Network device and stack kobj subtrees
 */

#include "internal.h"

namespace vk::kobj {

static KNode s_dev_net;
static KNode s_dev_net_count;
static KNode s_dev_net_entry;
static KNode s_dev_net_entry_name;
static KNode s_dev_net_entry_mac;
static KNode s_dev_net_entry_mtu;
static KNode s_dev_net_entry_link_up;
static KNode s_dev_net_entry_ipv4_address;
static KNode s_net;
static KNode s_net_device_count;
static KNode s_net_primary_device;
static KNode s_net_background_rx;
static KNode s_net_ipv4;
static KNode s_net_ipv4_configured_count;
static KNode s_net_arp;
static KNode s_net_arp_count;
static KNode s_net_arp_entry;
static KNode s_net_arp_entry_ip;
static KNode s_net_arp_entry_mac;

static constexpr static_child_definition kNetFields[] = {
    { "name", KTag::Str },
    { "mac", KTag::Str },
    { "mtu", KTag::U64 },
    { "link_up", KTag::Bool },
    { "ipv4_address", KTag::Str },
};

static constexpr static_child_definition kArpEntryFields[] = {
    { "ip", KTag::Str },
    { "mac", KTag::Str },
};

static auto get_dev_net_count(KNode&) -> KVal {
    return KVal::from_u64(static_cast<u64>(net::device_count()));
}

static auto get_net_device_count(KNode&) -> KVal {
    return KVal::from_u64(static_cast<u64>(net::device_count()));
}

static auto get_net_primary_device(KNode&) -> KVal {
    auto* device = net::primary_device();
    return KVal::from_str(device != null ? device->name.c_str() : "(none)");
}

static auto get_net_background_rx(KNode&) -> KVal {
    return KVal::from_bool(net::background_rx_running());
}

static auto get_net_ipv4_configured_count(KNode&) -> KVal {
    u64 count = 0;
    for (usize index = 0; index < net::device_count(); ++index) {
        auto* device = net::get_device(index);
        net::ipv4_address ip {};
        if (device != null && net::ipv4::configured_address(device, &ip)) {
            ++count;
        }
    }
    return KVal::from_u64(count);
}

static auto get_net_arp_count(KNode&) -> KVal {
    return KVal::from_u64(static_cast<u64>(net::arp::cache_entry_count()));
}

static auto format_mac_kval(net::mac_address mac) -> KVal {
    char buf[18];
    net::format_mac(mac, buf, sizeof(buf));
    return KVal::from_str(buf);
}

static auto format_ipv4_kval(net::ipv4_address ip) -> KVal {
    char buf[16];
    net::format_ipv4(ip, buf, sizeof(buf));
    return KVal::from_str(buf);
}

void register_network_nodes() {
    static const node_definition kDefinitions[] = {
        { &s_dev_net, &s_dev, KNodeId::dev_net, "net", KTag::Struct },
        { &s_dev_net_count, &s_dev_net, KNodeId::dev_net_count, "count", KTag::U64, false, false, "", 0x01, get_dev_net_count },
        { &s_dev_net_entry, null, KNodeId::dev_net_entry, "<device>", KTag::Struct },
        { &s_dev_net_entry_name, null, KNodeId::dev_net_entry_name, "name", KTag::Str },
        { &s_dev_net_entry_mac, null, KNodeId::dev_net_entry_mac, "mac", KTag::Str },
        { &s_dev_net_entry_mtu, null, KNodeId::dev_net_entry_mtu, "mtu", KTag::U64, false, false, "bytes" },
        { &s_dev_net_entry_link_up, null, KNodeId::dev_net_entry_link_up, "link_up", KTag::Bool, false, true },
        { &s_dev_net_entry_ipv4_address, null, KNodeId::dev_net_entry_ipv4_address, "ipv4_address", KTag::Str, false, true },

        { &s_net, &s_root, KNodeId::net, "net", KTag::Struct },
        { &s_net_device_count, &s_net, KNodeId::net_device_count, "device_count", KTag::U64, false, false, "count", 0x01, get_net_device_count },
        { &s_net_primary_device, &s_net, KNodeId::net_primary_device, "primary_device", KTag::Str, false, true, "", 0x01, get_net_primary_device },
        { &s_net_background_rx, &s_net, KNodeId::net_background_rx, "background_rx", KTag::Bool, false, true, "", 0x01, get_net_background_rx },
        { &s_net_ipv4, &s_net, KNodeId::net_ipv4, "ipv4", KTag::Struct },
        { &s_net_ipv4_configured_count, &s_net_ipv4, KNodeId::net_ipv4_configured_count, "configured_count", KTag::U64, false, true, "count", 0x01, get_net_ipv4_configured_count },
        { &s_net_arp, &s_net, KNodeId::net_arp, "arp", KTag::Struct },
        { &s_net_arp_count, &s_net_arp, KNodeId::net_arp_count, "count", KTag::U64, false, true, "count", 0x01, get_net_arp_count },
        { &s_net_arp_entry, null, KNodeId::net_arp_entry, "<entry>", KTag::Struct },
        { &s_net_arp_entry_ip, null, KNodeId::net_arp_entry_ip, "ip", KTag::Str },
        { &s_net_arp_entry_mac, null, KNodeId::net_arp_entry_mac, "mac", KTag::Str },
    };

    for (const auto& definition : kDefinitions) {
        register_node(definition);
    }
}

auto resolve_net_device_path(const char* path, usize len) -> resolved_node {
    resolved_node resolved {};
    string_view p(path, len);
    if (!p.starts_with("dev/net/")) {
        return resolved;
    }

    p.remove_prefix(8);
    if (p.empty()) {
        resolved.node = &s_dev_net;
        return resolved;
    }
    if (p.compare("count")) {
        resolved.node = &s_dev_net_count;
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

    net_device* device = net::find(name_buf);
    if (device == null) {
        return {};
    }

    resolved.node = &s_dev_net_entry;
    resolved.kind = resolved_node::virtual_kind::net_device;
    resolved.net = device;
    append_path_segment_name(&resolved.name, string_view(p.data(), name_end));
    resolved.has_name = true;

    if (name_end >= p.size()) {
        return resolved;
    }
    if (p[name_end] != '/') {
        return {};
    }

    const string_view tail(p.data() + name_end + 1, p.size() - name_end - 1);
    if (tail.compare("name")) resolved.node = &s_dev_net_entry_name;
    else if (tail.compare("mac")) resolved.node = &s_dev_net_entry_mac;
    else if (tail.compare("mtu")) resolved.node = &s_dev_net_entry_mtu;
    else if (tail.compare("link_up")) resolved.node = &s_dev_net_entry_link_up;
    else if (tail.compare("ipv4_address")) resolved.node = &s_dev_net_entry_ipv4_address;
    else return {};
    return resolved;
}

auto resolve_net_stack_path(const char* path, usize len) -> resolved_node {
    resolved_node resolved {};
    string_view p(path, len);
    if (!p.starts_with("net")) {
        return resolved;
    }

    p.remove_prefix(3);
    if (p.empty()) {
        resolved.node = &s_net;
        return resolved;
    }
    if (p[0] != '/') {
        return resolved;
    }

    p.remove_prefix(1);
    if (p.empty()) {
        resolved.node = &s_net;
        return resolved;
    }
    if (p.compare("device_count")) {
        resolved.node = &s_net_device_count;
        return resolved;
    }
    if (p.compare("primary_device")) {
        resolved.node = &s_net_primary_device;
        return resolved;
    }
    if (p.compare("background_rx")) {
        resolved.node = &s_net_background_rx;
        return resolved;
    }
    if (p.compare("ipv4")) {
        resolved.node = &s_net_ipv4;
        return resolved;
    }
    if (p.compare("ipv4/configured_count")) {
        resolved.node = &s_net_ipv4_configured_count;
        return resolved;
    }
    if (p.compare("arp")) {
        resolved.node = &s_net_arp;
        return resolved;
    }
    if (p.compare("arp/count")) {
        resolved.node = &s_net_arp_count;
        return resolved;
    }

    if (!p.starts_with("arp/")) {
        return resolved;
    }

    p.remove_prefix(4);
    usize index_end = 0;
    while (index_end < p.size() && p[index_end] != '/') {
        ++index_end;
    }

    u64 index = 0;
    if (!detail::parse_u64_segment(p.data(), index_end, &index)) {
        return {};
    }

    net::arp::cache_entry_info entry {};
    if (!net::arp::cache_entry(static_cast<usize>(index), &entry)) {
        return {};
    }

    resolved.node = &s_net_arp_entry;
    resolved.kind = resolved_node::virtual_kind::arp_entry;
    resolved.arp = entry;
    append_path_segment_name(&resolved.name, string_view(p.data(), index_end));
    resolved.has_name = true;

    if (index_end >= p.size()) {
        return resolved;
    }
    if (p[index_end] != '/') {
        return {};
    }

    const string_view tail(p.data() + index_end + 1, p.size() - index_end - 1);
    if (tail.compare("ip")) resolved.node = &s_net_arp_entry_ip;
    else if (tail.compare("mac")) resolved.node = &s_net_arp_entry_mac;
    else return {};
    return resolved;
}

auto network_query_value(const resolved_node& resolved) -> KVal {
    if (resolved.kind == resolved_node::virtual_kind::net_device && resolved.net != null) {
        switch (resolved.node->id()) {
            case KNodeId::dev_net_entry_name:
                return KVal::from_str(resolved.net->name.c_str());
            case KNodeId::dev_net_entry_mac:
                return format_mac_kval(resolved.net->mac);
            case KNodeId::dev_net_entry_mtu:
                return KVal::from_u64(resolved.net->mtu);
            case KNodeId::dev_net_entry_link_up:
                return KVal::from_bool(resolved.net->link_up);
            case KNodeId::dev_net_entry_ipv4_address: {
                net::ipv4_address ip {};
                if (!net::ipv4::configured_address(resolved.net, &ip)) {
                    return KVal::from_str("(unconfigured)");
                }
                return format_ipv4_kval(ip);
            }
            default:
                break;
        }
    }

    if (resolved.kind == resolved_node::virtual_kind::arp_entry) {
        switch (resolved.node->id()) {
            case KNodeId::net_arp_entry_ip:
                return format_ipv4_kval(resolved.arp.ip);
            case KNodeId::net_arp_entry_mac:
                return format_mac_kval(resolved.arp.mac);
            default:
                break;
        }
    }

    return KVal::err("node is not readable");
}

auto network_list_children(const resolved_node& resolved,
                           KChildInfo* out,
                           usize max_children) -> usize {
    if (resolved.node == &s_dev_net) {
        const usize device_count = net::device_count();
        const usize total = 1 + device_count;
        append_child_info(out, max_children, 0, "count", KTag::U64);
        for (usize index = 0; index < device_count && (index + 1) < max_children; ++index) {
            auto* device = net::get_device(index);
            if (device == null) {
                continue;
            }
            append_child_info(out, max_children, index + 1, device->name.c_str(), KTag::Struct);
        }
        return total;
    }

    if (resolved.node == &s_net_arp) {
        const usize entry_count = net::arp::cache_entry_count();
        const usize total = 1 + entry_count;
        append_child_info(out, max_children, 0, "count", KTag::U64);
        for (usize index = 0; index < entry_count && (index + 1) < max_children; ++index) {
            append_numeric_child_name(out, max_children, index + 1, static_cast<u64>(index));
        }
        return total;
    }

    if (resolved.node == &s_net_arp_entry) {
        return append_static_children(out, max_children, kArpEntryFields, sizeof(kArpEntryFields) / sizeof(kArpEntryFields[0]));
    }
    return append_static_children(out, max_children, kNetFields, sizeof(kNetFields) / sizeof(kNetFields[0]));
}

} // namespace vk::kobj
