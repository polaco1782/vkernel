/*
 * vkernel - UEFI Microkernel
 * Copyright (C) 2026 vkernel authors
 *
 * kobj_rpc.cpp - JSON compatibility layer over the typed kobj core
 */

#include "arch/x86_64/arch.h"
#include "driver.h"
#include "fs.h"
#include "kobj.h"
#include "detail.h"
#include "log.h"
#include "memory.h"
#include "scheduler.h"

namespace vk::kobj {

using detail::append_ch;
using detail::append_str;
using detail::cstrlen;
using detail::parse_u64_segment;
using detail::render_u64;
using detail::tag_name;

static auto append_json_escaped(char* out, usize out_cap, usize pos, const char* s) -> usize {
    if (s == null) return pos;
    for (usize i = 0; s[i] != '\0'; ++i) {
        const char ch = s[i];
        if (ch == '"') {
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

static auto json_extract_string_buffer(const char* json, const char* key, char* out, usize out_cap) -> bool {
    if (json == null || key == null || out == null || out_cap == 0) return false;

    out[0] = '\0';

    char pattern[32];
    const usize klen = cstrlen(key);
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
                const char esc = json[pos];
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

static auto append_node_info_json(char* out,
                                  usize out_cap,
                                  usize pos,
                                  const KNodeInfo& info) -> usize {
    pos = append_str(out, out_cap, pos, ",\"type\":\"");
    pos = append_str(out, out_cap, pos, tag_name(info.type));
    pos = append_ch(out, out_cap, pos, '"');
    pos = append_str(out, out_cap, pos, ",\"readable\":");
    pos = append_str(out, out_cap, pos, info.readable ? "true" : "false");
    pos = append_str(out, out_cap, pos, ",\"writable\":");
    pos = append_str(out, out_cap, pos, info.writable ? "true" : "false");
    pos = append_str(out, out_cap, pos, ",\"volatile\":");
    pos = append_str(out, out_cap, pos, info.volatile_node ? "true" : "false");
    pos = append_str(out, out_cap, pos, ",\"unit\":\"");
    pos = append_json_escaped(out, out_cap, pos, info.unit.c_str());
    pos = append_ch(out, out_cap, pos, '"');
    if (info.type == KTag::Enum) {
        pos = append_str(out, out_cap, pos, ",\"labels\":[");
        for (u32 i = 0; i < info.enum_count; ++i) {
            if (i != 0) {
                pos = append_ch(out, out_cap, pos, ',');
            }
            pos = append_ch(out, out_cap, pos, '"');
            pos = append_json_escaped(out, out_cap, pos, info.enum_labels[i]);
            pos = append_ch(out, out_cap, pos, '"');
        }
        pos = append_ch(out, out_cap, pos, ']');
    } else {
        pos = append_str(out, out_cap, pos, ",\"range_min\":");
        pos = render_u64(out, out_cap, pos, info.range_min);
        pos = append_str(out, out_cap, pos, ",\"range_max\":");
        pos = render_u64(out, out_cap, pos, info.range_max);
    }
    return pos;
}

void krpc(const char* req_json, char* out, usize out_cap) {
    if (out == null || out_cap == 0) return;
    out[0] = '\0';

    KStr op {};
    KStr path {};
    KStr value {};
    if (!json_extract_string(req_json, "op", &op)) {
        append_str(out, out_cap, 0, "{\"ok\":false,\"error\":\"missing op\"}");
        return;
    }

    usize pos = 0;
    if (op.eq("mem")) {
        log::info() << "Physical allocator: total=" << g_phys_alloc.total_pages()
                    << " pages, free=" << g_phys_alloc.free_pages()
                    << " pages, used=" << g_phys_alloc.used_pages()
                    << " pages, total RAM="
                    << (g_phys_alloc.total_pages() * PAGE_SIZE_4K) / (1024 * 1024)
                    << " MB";
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
        const u64 ticks = sched::tick_count();
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

    if (op.eq("drvload") || op.eq("drvunload")) {
        KStr name {};
        if (!json_extract_string(req_json, "name", &name)) {
            append_str(out, out_cap, 0, "{\"ok\":false,\"error\":\"missing name\"}");
            return;
        }

        const bool load = op.eq("drvload");
        const i32 rc = load ? driver::load(name.c_str()) : driver::unload(name.c_str());
        pos = append_str(out, out_cap, pos, "{\"ok\":");
        pos = append_str(out, out_cap, pos, rc == 0 ? "true" : "false");
        pos = append_str(out, out_cap, pos, ",\"op\":\"");
        pos = append_str(out, out_cap, pos, load ? "drvload" : "drvunload");
        pos = append_str(out, out_cap, pos, "\",\"name\":\"");
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
        KVal rendered_value {};
        KNodeInfo info {};
        if (!kquery(path.c_str(), &rendered_value, &info)) {
            append_str(out, out_cap, 0, "{\"ok\":false,\"error\":\"path not found\"}");
            return;
        }

        char rendered[256];
        kval_render(rendered_value, resolve(path.c_str()), rendered, sizeof(rendered));
        pos = append_str(out, out_cap, pos, "{\"ok\":");
        pos = append_str(out, out_cap, pos, rendered_value.tag == KTag::Err ? "false" : "true");
        pos = append_str(out, out_cap, pos, ",\"op\":\"get\",\"path\":\"");
        pos = append_json_escaped(out, out_cap, pos, path.c_str());
        pos = append_str(out, out_cap, pos, "\",\"value\":\"");
        pos = append_json_escaped(out, out_cap, pos, rendered);
        pos = append_ch(out, out_cap, pos, '"');
        pos = append_node_info_json(out, out_cap, pos, info);
        pos = append_ch(out, out_cap, pos, '}');
        return;
    }

    if (op.eq("set")) {
        if (!json_extract_string(req_json, "value", &value)) {
            append_str(out, out_cap, 0, "{\"ok\":false,\"error\":\"missing value\"}");
            return;
        }

        KNodeInfo info {};
        if (!kinfo(path.c_str(), &info)) {
            append_str(out, out_cap, 0, "{\"ok\":false,\"error\":\"path not found\"}");
            return;
        }
        if (!info.writable) {
            append_str(out, out_cap, 0, "{\"ok\":false,\"error\":\"read-only\"}");
            return;
        }

        KVal set_value {};
        bool set_ok = false;
        if (info.type == KTag::U64) {
            u64 parsed = 0;
            if (parse_u64_segment(value.c_str(), cstrlen(value.c_str()), &parsed)) {
                set_value = KVal::from_u64(parsed);
                set_ok = kset(path.c_str(), set_value);
            }
        } else if (info.type == KTag::Bool) {
            const bool parsed = value.eq("true") || value.eq("1") || value.eq("yes");
            set_value = KVal::from_bool(parsed);
            set_ok = kset(path.c_str(), set_value);
        } else if (info.type == KTag::Str) {
            set_value = KVal::from_str(value.c_str());
            set_ok = kset(path.c_str(), set_value);
        } else if (info.type == KTag::Enum) {
            bool found = false;
            for (u32 i = 0; i < info.enum_count; ++i) {
                if (value.eq(info.enum_labels[i])) {
                    set_value = KVal::from_enum(i);
                    set_ok = kset(path.c_str(), set_value);
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
        KChildInfo items[64] {};
        const usize total = klist(path.c_str(), items, sizeof(items) / sizeof(items[0]));
        if (total == 0 && resolve(path.c_str()) == null) {
            append_str(out, out_cap, 0, "{\"ok\":false,\"error\":\"path not found\"}");
            return;
        }

        pos = append_str(out, out_cap, pos, "{\"ok\":true,\"op\":\"ls\",\"path\":\"");
        pos = append_json_escaped(out, out_cap, pos, path.c_str());
        pos = append_str(out, out_cap, pos, "\",\"items\":[");

        const usize limit = total < (sizeof(items) / sizeof(items[0]))
            ? total
            : (sizeof(items) / sizeof(items[0]));
        bool first = true;
        for (usize i = 0; i < limit; ++i) {
            if (items[i].name.c_str()[0] == '\0') {
                continue;
            }
            if (!first) {
                pos = append_ch(out, out_cap, pos, ',');
            }
            pos = append_str(out, out_cap, pos, "{\"name\":\"");
            pos = append_json_escaped(out, out_cap, pos, items[i].name.c_str());
            pos = append_str(out, out_cap, pos, "\",\"type\":\"");
            pos = append_str(out, out_cap, pos, tag_name(items[i].type));
            pos = append_str(out, out_cap, pos, "\"}");
            first = false;
        }
        pos = append_str(out, out_cap, pos, "]}");
        return;
    }

    if (op.eq("describe")) {
        KNodeInfo info {};
        if (!kinfo(path.c_str(), &info)) {
            append_str(out, out_cap, 0, "{\"ok\":false,\"error\":\"path not found\"}");
            return;
        }

        char desc_buf[512];
        kdescribe(path.c_str(), desc_buf, sizeof(desc_buf));
        pos = append_str(out, out_cap, pos, "{\"ok\":true,\"op\":\"describe\",\"path\":\"");
        pos = append_json_escaped(out, out_cap, pos, path.c_str());
        pos = append_str(out, out_cap, pos, "\",\"text\":\"");
        pos = append_json_escaped(out, out_cap, pos, desc_buf);
        pos = append_ch(out, out_cap, pos, '"');
        pos = append_node_info_json(out, out_cap, pos, info);
        pos = append_ch(out, out_cap, pos, '}');
        return;
    }

    append_str(out, out_cap, 0, "{\"ok\":false,\"error\":\"unknown op\"}");
}

} // namespace vk::kobj
