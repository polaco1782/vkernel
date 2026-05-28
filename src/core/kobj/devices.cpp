/*
 * vkernel - UEFI Microkernel
 * Copyright (C) 2026 vkernel authors
 *
 * devices.cpp - Block, PCI, and sound kobj subtrees
 */

#include "internal.h"

#include "sound.h"

namespace vk::kobj {

KNode s_dev;

static KNode s_dev_block;
static KNode s_dev_block_count;
static KNode s_dev_block_entry;
static KNode s_dev_block_entry_name;
static KNode s_dev_block_entry_size_kb;
static KNode s_dev_block_entry_block_size;
static KNode s_dev_block_entry_removable;
static KNode s_dev_pci;
static KNode s_dev_pci_count;
static KNode s_dev_pci_entry;
static KNode s_dev_pci_entry_bus;
static KNode s_dev_pci_entry_device;
static KNode s_dev_pci_entry_function;
static KNode s_dev_pci_entry_vendor_id;
static KNode s_dev_pci_entry_device_id;
static KNode s_dev_pci_entry_class_code;
static KNode s_dev_pci_entry_subclass;
static KNode s_dev_pci_entry_prog_if;
static KNode s_dev_pci_entry_revision;
static KNode s_dev_pci_entry_irq_line;
static KNode s_dev_sound;
static KNode s_dev_sound_active_driver;
static KNode s_dev_sound_initialized;
static KNode s_dev_sound_playing;
static KNode s_dev_sound_sample_rate;
static KNode s_dev_sound_volume_left;
static KNode s_dev_sound_volume_right;
static KNode s_dev_sound_mix_channels_active;

static constexpr static_child_definition kBlockFields[] = {
    { "name", KTag::Str },
    { "size_kb", KTag::U64 },
    { "block_size", KTag::U64 },
    { "removable", KTag::Bool },
};

static constexpr static_child_definition kPciFields[] = {
    { "bus", KTag::U64 },
    { "device", KTag::U64 },
    { "function", KTag::U64 },
    { "vendor_id", KTag::U64 },
    { "device_id", KTag::U64 },
    { "class_code", KTag::U64 },
    { "subclass", KTag::U64 },
    { "prog_if", KTag::U64 },
    { "revision", KTag::U64 },
    { "irq_line", KTag::U64 },
};

static auto get_dev_block_count(KNode&) -> KVal {
    return KVal::from_u64(static_cast<u64>(block::device_count()));
}

static auto get_dev_pci_count(KNode&) -> KVal {
    return KVal::from_u64(static_cast<u64>(pci::device_count()));
}

static auto get_sound_active_driver(KNode&) -> KVal {
    const auto* active = sound::active_driver();
    return KVal::from_str(active != null && active->name != null ? active->name : "(none)");
}

static auto get_sound_initialized(KNode&) -> KVal {
    return KVal::from_bool(sound::initialized());
}

static auto get_sound_playing(KNode&) -> KVal {
    return KVal::from_bool(sound::is_playing());
}

static auto get_sound_sample_rate(KNode&) -> KVal {
    return KVal::from_u64(sound::sample_rate());
}

static auto get_sound_volume_left(KNode&) -> KVal {
    u8 left = 0;
    u8 right = 0;
    sound::volume(&left, &right);
    (void)right;
    return KVal::from_u64(left);
}

static auto get_sound_volume_right(KNode&) -> KVal {
    u8 left = 0;
    u8 right = 0;
    sound::volume(&left, &right);
    (void)left;
    return KVal::from_u64(right);
}

static auto get_sound_mix_channels_active(KNode&) -> KVal {
    return KVal::from_u64(sound::active_mix_channels());
}

void register_device_nodes() {
    static const node_definition kDefinitions[] = {
        { &s_dev, &s_root, KNodeId::dev, "dev", KTag::Struct },

        { &s_dev_block, &s_dev, KNodeId::dev_block, "block", KTag::Struct },
        { &s_dev_block_count, &s_dev_block, KNodeId::dev_block_count, "count", KTag::U64, false, false, "", 0x01, get_dev_block_count },
        { &s_dev_block_entry, null, KNodeId::dev_block_entry, "<device>", KTag::Struct },
        { &s_dev_block_entry_name, null, KNodeId::dev_block_entry_name, "name", KTag::Str },
        { &s_dev_block_entry_size_kb, null, KNodeId::dev_block_entry_size_kb, "size_kb", KTag::U64, false, false, "kb" },
        { &s_dev_block_entry_block_size, null, KNodeId::dev_block_entry_block_size, "block_size", KTag::U64, false, false, "bytes" },
        { &s_dev_block_entry_removable, null, KNodeId::dev_block_entry_removable, "removable", KTag::Bool },

        { &s_dev_pci, &s_dev, KNodeId::dev_pci, "pci", KTag::Struct },
        { &s_dev_pci_count, &s_dev_pci, KNodeId::dev_pci_count, "count", KTag::U64, false, false, "", 0x01, get_dev_pci_count },
        { &s_dev_pci_entry, null, KNodeId::dev_pci_entry, "<device>", KTag::Struct },
        { &s_dev_pci_entry_bus, null, KNodeId::dev_pci_entry_bus, "bus", KTag::U64 },
        { &s_dev_pci_entry_device, null, KNodeId::dev_pci_entry_device, "device", KTag::U64 },
        { &s_dev_pci_entry_function, null, KNodeId::dev_pci_entry_function, "function", KTag::U64 },
        { &s_dev_pci_entry_vendor_id, null, KNodeId::dev_pci_entry_vendor_id, "vendor_id", KTag::U64 },
        { &s_dev_pci_entry_device_id, null, KNodeId::dev_pci_entry_device_id, "device_id", KTag::U64 },
        { &s_dev_pci_entry_class_code, null, KNodeId::dev_pci_entry_class_code, "class_code", KTag::U64 },
        { &s_dev_pci_entry_subclass, null, KNodeId::dev_pci_entry_subclass, "subclass", KTag::U64 },
        { &s_dev_pci_entry_prog_if, null, KNodeId::dev_pci_entry_prog_if, "prog_if", KTag::U64 },
        { &s_dev_pci_entry_revision, null, KNodeId::dev_pci_entry_revision, "revision", KTag::U64 },
        { &s_dev_pci_entry_irq_line, null, KNodeId::dev_pci_entry_irq_line, "irq_line", KTag::U64 },

        { &s_dev_sound, &s_dev, KNodeId::dev_sound, "sound", KTag::Struct },
        { &s_dev_sound_active_driver, &s_dev_sound, KNodeId::dev_sound_active_driver, "active_driver", KTag::Str, false, true, "", 0x01, get_sound_active_driver },
        { &s_dev_sound_initialized, &s_dev_sound, KNodeId::dev_sound_initialized, "initialized", KTag::Bool, false, true, "", 0x01, get_sound_initialized },
        { &s_dev_sound_playing, &s_dev_sound, KNodeId::dev_sound_playing, "playing", KTag::Bool, false, true, "", 0x01, get_sound_playing },
        { &s_dev_sound_sample_rate, &s_dev_sound, KNodeId::dev_sound_sample_rate, "sample_rate", KTag::U64, false, true, "hz", 0x01, get_sound_sample_rate },
        { &s_dev_sound_volume_left, &s_dev_sound, KNodeId::dev_sound_volume_left, "volume_left", KTag::U64, false, true, "level", 0x01, get_sound_volume_left },
        { &s_dev_sound_volume_right, &s_dev_sound, KNodeId::dev_sound_volume_right, "volume_right", KTag::U64, false, true, "level", 0x01, get_sound_volume_right },
        { &s_dev_sound_mix_channels_active, &s_dev_sound, KNodeId::dev_sound_mix_channels_active, "mix_channels_active", KTag::U64, false, true, "count", 0x01, get_sound_mix_channels_active },
    };

    for (const auto& definition : kDefinitions) {
        register_node(definition);
    }
}

auto resolve_block_path(const char* path, usize len) -> resolved_node {
    resolved_node resolved {};
    string_view p(path, len);
    if (!p.starts_with("dev/block/")) {
        return resolved;
    }

    p.remove_prefix(10);
    if (p.empty()) {
        resolved.node = &s_dev_block;
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

    block_device* device = block::find(name_buf);
    if (device == null) {
        return {};
    }

    resolved.node = &s_dev_block_entry;
    resolved.kind = resolved_node::virtual_kind::block_device;
    resolved.block = device;
    append_path_segment_name(&resolved.name, string_view(p.data(), name_end));
    resolved.has_name = true;

    if (name_end >= p.size()) {
        return resolved;
    }
    if (p[name_end] != '/') {
        return {};
    }

    const string_view tail(p.data() + name_end + 1, p.size() - name_end - 1);
    if (tail.compare("name")) resolved.node = &s_dev_block_entry_name;
    else if (tail.compare("size_kb")) resolved.node = &s_dev_block_entry_size_kb;
    else if (tail.compare("block_size")) resolved.node = &s_dev_block_entry_block_size;
    else if (tail.compare("removable")) resolved.node = &s_dev_block_entry_removable;
    else return {};
    return resolved;
}

auto resolve_pci_path(const char* path, usize len) -> resolved_node {
    resolved_node resolved {};
    string_view p(path, len);
    if (!p.starts_with("dev/pci/")) {
        return resolved;
    }

    p.remove_prefix(8);
    if (p.empty()) {
        resolved.node = &s_dev_pci;
        return resolved;
    }
    if (p.compare("count")) {
        resolved.node = &s_dev_pci_count;
        return resolved;
    }

    usize index_end = 0;
    while (index_end < p.size() && p[index_end] != '/') {
        ++index_end;
    }

    u64 index = 0;
    if (!detail::parse_u64_segment(p.data(), index_end, &index)) {
        return {};
    }

    const auto* device = pci::get_device(static_cast<usize>(index));
    if (device == null) {
        return {};
    }

    resolved.node = &s_dev_pci_entry;
    resolved.kind = resolved_node::virtual_kind::pci_device;
    resolved.pci = device;
    append_path_segment_name(&resolved.name, string_view(p.data(), index_end));
    resolved.has_name = true;

    if (index_end >= p.size()) {
        return resolved;
    }
    if (p[index_end] != '/') {
        return {};
    }

    const string_view tail(p.data() + index_end + 1, p.size() - index_end - 1);
    if (tail.compare("bus")) resolved.node = &s_dev_pci_entry_bus;
    else if (tail.compare("device")) resolved.node = &s_dev_pci_entry_device;
    else if (tail.compare("function")) resolved.node = &s_dev_pci_entry_function;
    else if (tail.compare("vendor_id")) resolved.node = &s_dev_pci_entry_vendor_id;
    else if (tail.compare("device_id")) resolved.node = &s_dev_pci_entry_device_id;
    else if (tail.compare("class_code")) resolved.node = &s_dev_pci_entry_class_code;
    else if (tail.compare("subclass")) resolved.node = &s_dev_pci_entry_subclass;
    else if (tail.compare("prog_if")) resolved.node = &s_dev_pci_entry_prog_if;
    else if (tail.compare("revision")) resolved.node = &s_dev_pci_entry_revision;
    else if (tail.compare("irq_line")) resolved.node = &s_dev_pci_entry_irq_line;
    else return {};
    return resolved;
}

auto device_query_value(const resolved_node& resolved) -> KVal {
    if (resolved.kind == resolved_node::virtual_kind::block_device && resolved.block != null) {
        switch (resolved.node->id()) {
            case KNodeId::dev_block_entry_name:
                return KVal::from_str(resolved.block->name.c_str());
            case KNodeId::dev_block_entry_size_kb: {
                const u64 bytes = resolved.block->block_count * static_cast<u64>(resolved.block->block_size);
                return KVal::from_u64(bytes / 1024ULL);
            }
            case KNodeId::dev_block_entry_block_size:
                return KVal::from_u64(resolved.block->block_size);
            case KNodeId::dev_block_entry_removable:
                return KVal::from_bool(resolved.block->removable);
            default:
                break;
        }
    }

    if (resolved.kind == resolved_node::virtual_kind::pci_device && resolved.pci != null) {
        switch (resolved.node->id()) {
            case KNodeId::dev_pci_entry_bus: return KVal::from_u64(resolved.pci->addr.bus);
            case KNodeId::dev_pci_entry_device: return KVal::from_u64(resolved.pci->addr.device);
            case KNodeId::dev_pci_entry_function: return KVal::from_u64(resolved.pci->addr.function);
            case KNodeId::dev_pci_entry_vendor_id: return KVal::from_u64(resolved.pci->vendor_id);
            case KNodeId::dev_pci_entry_device_id: return KVal::from_u64(resolved.pci->device_id);
            case KNodeId::dev_pci_entry_class_code: return KVal::from_u64(resolved.pci->class_code);
            case KNodeId::dev_pci_entry_subclass: return KVal::from_u64(resolved.pci->subclass);
            case KNodeId::dev_pci_entry_prog_if: return KVal::from_u64(resolved.pci->prog_if);
            case KNodeId::dev_pci_entry_revision: return KVal::from_u64(resolved.pci->revision);
            case KNodeId::dev_pci_entry_irq_line: return KVal::from_u64(resolved.pci->irq_line);
            default: break;
        }
    }

    return KVal::err("node is not readable");
}

auto device_list_children(const resolved_node& resolved,
                          KChildInfo* out,
                          usize max_children) -> usize {
    if (resolved.node == &s_dev_block) {
        const usize device_count = block::device_count();
        const usize total = 1 + device_count;
        append_child_info(out, max_children, 0, "count", KTag::U64);
        for (usize index = 0; index < device_count && (index + 1) < max_children; ++index) {
            auto* device = block::get_device(index);
            if (device == null) {
                continue;
            }
            append_child_info(out, max_children, index + 1, device->name.c_str(), KTag::Struct);
        }
        return total;
    }

    if (resolved.node == &s_dev_pci) {
        const usize device_count = pci::device_count();
        const usize total = 1 + device_count;
        append_child_info(out, max_children, 0, "count", KTag::U64);
        for (usize index = 0; index < device_count && (index + 1) < max_children; ++index) {
            append_numeric_child_name(out, max_children, index + 1, static_cast<u64>(index));
        }
        return total;
    }

    if (resolved.node == &s_dev_block_entry) {
        return append_static_children(out, max_children, kBlockFields, sizeof(kBlockFields) / sizeof(kBlockFields[0]));
    }
    return append_static_children(out, max_children, kPciFields, sizeof(kPciFields) / sizeof(kPciFields[0]));
}

} // namespace vk::kobj
