/*
 * vkernel - UEFI Microkernel
 * Copyright (C) 2026 vkernel authors
 *
 * proc.cpp - Dynamic process subtree
 */

#include "internal.h"

namespace vk::kobj {

static KNode s_proc;
static KNode s_proc_count;
static KNode s_proc_pid;
static KNode s_proc_pid_id;
static KNode s_proc_pid_state;
static KNode s_proc_pid_cpu;
static KNode s_proc_pid_cpu_ticks;
static KNode s_proc_pid_name;

static constexpr const char* kProcStateLabels[] = { "ready", "running", "blocked", "terminated" };
static constexpr static_child_definition kProcFields[] = {
    { "id", KTag::U64 },
    { "state", KTag::Enum },
    { "cpu", KTag::I64 },
    { "cpu_ticks", KTag::U64 },
    { "name", KTag::Str },
};

static auto get_proc_count(KNode&) -> KVal {
    return KVal::from_u64(sched::snapshot_tasks(null, 0));
}

void register_proc_nodes() {
    static const node_definition kDefinitions[] = {
        { &s_proc, &s_root, KNodeId::proc, "proc", KTag::Struct },
        { &s_proc_count, &s_proc, KNodeId::proc_count, "count", KTag::U64, false, true, "", 0x01, get_proc_count },
        { &s_proc_pid, null, KNodeId::proc_pid, "<pid>", KTag::Struct },
        { &s_proc_pid_id, null, KNodeId::proc_pid_id, "id", KTag::U64 },
        { &s_proc_pid_state, null, KNodeId::proc_pid_state, "state", KTag::Enum, false, false, "", 0x01, null, null, kProcStateLabels, 4 },
        { &s_proc_pid_cpu, null, KNodeId::proc_pid_cpu, "cpu", KTag::I64, false, true },
        { &s_proc_pid_cpu_ticks, null, KNodeId::proc_pid_cpu_ticks, "cpu_ticks", KTag::U64, false, true },
        { &s_proc_pid_name, null, KNodeId::proc_pid_name, "name", KTag::Str },
    };

    for (const auto& definition : kDefinitions) {
        register_node(definition);
    }
}

auto resolve_proc_path(const char* path, usize len) -> resolved_node {
    resolved_node resolved {};
    string_view p(path, len);
    if (!p.starts_with("proc")) {
        return resolved;
    }

    p.remove_prefix(4);
    if (p.empty()) {
        resolved.node = &s_proc;
        return resolved;
    }
    if (p[0] != '/') {
        return resolved;
    }

    p.remove_prefix(1);
    if (p.empty()) {
        resolved.node = &s_proc;
        return resolved;
    }
    if (p.compare("count")) {
        resolved.node = &s_proc_count;
        return resolved;
    }

    usize pid_end = 0;
    while (pid_end < p.size() && p[pid_end] != '/') {
        ++pid_end;
    }

    u64 pid = 0;
    if (!detail::parse_u64_segment(p.data(), pid_end, &pid)) {
        return {};
    }

    task_snapshot snapshot {};
    if (!sched::snapshot_task(pid, &snapshot)) {
        return {};
    }

    resolved.node = &s_proc_pid;
    resolved.kind = resolved_node::virtual_kind::proc_task;
    resolved.has_task_snapshot = true;
    resolved.task = snapshot;
    append_path_segment_name(&resolved.name, string_view(p.data(), pid_end));
    resolved.has_name = true;

    if (pid_end >= p.size()) {
        return resolved;
    }
    if (p[pid_end] != '/') {
        return {};
    }

    const string_view tail(p.data() + pid_end + 1, p.size() - pid_end - 1);
    if (tail.compare("id")) resolved.node = &s_proc_pid_id;
    else if (tail.compare("state")) resolved.node = &s_proc_pid_state;
    else if (tail.compare("cpu")) resolved.node = &s_proc_pid_cpu;
    else if (tail.compare("cpu_ticks")) resolved.node = &s_proc_pid_cpu_ticks;
    else if (tail.compare("name")) resolved.node = &s_proc_pid_name;
    else return {};
    return resolved;
}

auto proc_query_value(const resolved_node& resolved) -> KVal {
    if (!resolved.has_task_snapshot || resolved.node == null) {
        return KVal::err("path not found");
    }

    switch (resolved.node->id()) {
        case KNodeId::proc_pid_id:
            return KVal::from_u64(resolved.task.id);
        case KNodeId::proc_pid_state:
            return KVal::from_enum(static_cast<u32>(resolved.task.state));
        case KNodeId::proc_pid_cpu:
            return KVal::from_i64(resolved.task.cpu == SCHED_CPU_NONE
                ? -1
                : static_cast<i64>(resolved.task.cpu));
        case KNodeId::proc_pid_cpu_ticks:
            return KVal::from_u64(resolved.task.cpu_ticks);
        case KNodeId::proc_pid_name: {
            char name[33] {};
            usize index = 0;
            while (index < resolved.task.name.size() && index < sizeof(name) - 1) {
                name[index] = resolved.task.name[index];
                ++index;
            }
            name[index] = '\0';
            return KVal::from_str(name);
        }
        default:
            return KVal::err("node is not readable");
    }
}

auto proc_list_children(const resolved_node& resolved,
                        KChildInfo* out,
                        usize max_children) -> usize {
    if (resolved.node == &s_proc) {
        const usize task_count = sched::snapshot_tasks(null, 0);
        const usize total = 1 + task_count;
        append_child_info(out, max_children, 0, "count", KTag::U64);

        if (out == null || max_children <= 1) {
            return total;
        }

        task_snapshot snapshots[MAX_TASKS];
        usize captured = sched::snapshot_tasks(snapshots, MAX_TASKS);
        if (captured > MAX_TASKS) {
            captured = MAX_TASKS;
        }

        usize written = 1;
        for (usize index = 0; index < captured && written < max_children; ++index, ++written) {
            append_numeric_child_name(out, max_children, written, snapshots[index].id);
        }
        return total;
    }

    return append_static_children(out, max_children, kProcFields, sizeof(kProcFields) / sizeof(kProcFields[0]));
}

} // namespace vk::kobj
