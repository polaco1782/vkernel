/*
 * vkernel - UEFI Microkernel
 * Copyright (C) 2026 vkernel authors
 *
 * kernel_api.cpp - Kernel-side API table and user-facing stubs
 */

#include "config.h"
#include "types.h"
#include "console.h"
#include "log.h"
#include "memory.h"
#include "fs.h"
#include "framebuffer.h"
#include "input.h"
#include "scheduler.h"
#include "sound.h"
#include "process.h"
#include "driver.h"
#include "kobj.h"
#include "kobj/detail.h"
#include "vk.h"
#include "process_internal.h"
#include "virtual_memory.h"
#include "arch/x86_64/arch.h"

namespace vk {
namespace kernel_api {

static auto current_console_interface() -> process::console_interface;
static auto validate_fb(const vk_framebuffer_info_t* fb) -> bool;
static void enqueue_framebuffer_event(process::process_task_context* target,
                                      vk_u32 type,
                                      const vk_framebuffer_info_t& framebuffer);
static int stub_set_framebuffer_resize_events(vk_u32 enabled);
static int stub_task_accepts_framebuffer_resize(vk_u64 task_id);
static int stub_set_startup_window_size(vk_u32 width, vk_u32 height);
static int stub_get_task_startup_window_size(vk_u64 task_id, vk_u32* out_width, vk_u32* out_height);
static void route_putc(char c);
static void route_puts(const char* str);
static auto route_stdio_write(const char* buf, vk_usize len) -> vk_usize;
static auto dequeue_ascii_key(process::process_task_context* ctx) -> char;
static void copy_cstr(char* out, usize out_cap, const char* src);

/* Named statics keep vk_api_t entries addressable without trampolines. */

/* ---- memory ---- */

static constexpr usize PROCESS_ALLOC_GUARD_SIZE = PAGE_SIZE_4K;

static void trace_process_buffer(const char* label,
                                 process::process_task_context* ctx,
                                 const void* ptr,
                                 usize size) {
    if (label == null || ctx == null || ptr == null || ctx->address_space == null) {
        return;
    }

    const virt_addr start = reinterpret_cast<virt_addr>(ptr);
    const virt_addr end = size > 0 ? start + size - 1 : start;
    phys_addr start_phys = 0;
    phys_addr end_phys = 0;
    u64 start_flags = 0;
    u64 end_flags = 0;
    const bool start_ok = vm::debug_resolve(ctx->address_space, start, &start_phys, &start_flags);
    const bool end_ok = vm::debug_resolve(ctx->address_space, end, &end_phys, &end_flags);

    log::debug() << "vmtrace: task=" << static_cast<unsigned long long>(sched::current_task_id())
                 << " name=" << sched::current_task_name()
                 << " " << label
                 << " range=" << reinterpret_cast<const void*>(static_cast<usize>(start))
                 << ".." << reinterpret_cast<const void*>(static_cast<usize>(end))
                 << " size=" << static_cast<unsigned long long>(size)
                 << " start_ok=" << (start_ok ? "yes" : "no")
                 << " start_phys=" << reinterpret_cast<const void*>(static_cast<usize>(start_phys))
                 << " start_flags=" << log::hex(start_flags, 1, true, false)
                 << " end_ok=" << (end_ok ? "yes" : "no")
                 << " end_phys=" << reinterpret_cast<const void*>(static_cast<usize>(end_phys))
                 << " end_flags=" << log::hex(end_flags, 1, true, false);
}

static void free_process_allocation_pages(void* ptr, usize size) {
    if (ptr == null || size == 0) {
        return;
    }

    u32 page_count = static_cast<u32>((size + PAGE_SIZE_4K - 1) / PAGE_SIZE_4K);
    g_phys_alloc.free_pages(reinterpret_cast<phys_addr>(ptr), page_count);
}

static void* stub_malloc_internal(vk_usize size, bool executable) {
    if (size == 0) {
        return null;
    }

    log::debug() << (executable ? "malloc_exec" : "malloc")
                 << ": req size=" << static_cast<unsigned long long>(size);

    auto* ctx = static_cast<process::process_task_context*>(sched::current_task_user_data());
    if (ctx == null) {
        return g_kernel_heap.allocate_zero(size);
    }

    const usize requested = static_cast<usize>(size);
    const usize rounded = align_up(requested, static_cast<usize>(16));
    if (rounded < requested) {
        return null;
    }

    const bool uses_process_vm = ctx->address_space != null;
    if (!uses_process_vm && rounded > ~static_cast<usize>(0) - PROCESS_ALLOC_GUARD_SIZE) {
        return null;
    }

    /* _sbrk expects contiguous growth in VM-backed processes. */
    const usize allocated = uses_process_vm ? rounded : rounded + PROCESS_ALLOC_GUARD_SIZE;
    bool from_phys = false;
    void* raw = null;
    void* user = null;

    if (uses_process_vm) {
        phys_addr phys = 0;
        virt_addr virt = 0;
        if (!vm::allocate_user_heap(ctx->address_space,
                                    allocated,
                                    vm::MAP_WRITABLE | vm::MAP_USER
                                        | (executable ? vm::MAP_EXECUTABLE : 0),
                                    &phys,
                                    &virt)) {
            return null;
        }
        raw = reinterpret_cast<void*>(static_cast<usize>(phys));
        user = reinterpret_cast<void*>(static_cast<usize>(virt));
        from_phys = true;
    } else {
        raw = g_kernel_heap.allocate_zero(allocated);
        user = raw;
        if (raw == null) {
            const u32 page_count = static_cast<u32>(
                (allocated + PAGE_SIZE_4K - 1) / PAGE_SIZE_4K);
            phys_addr phys = g_phys_alloc.allocate_pages(page_count, PAGE_SIZE_4K, 0);
            if (phys == 0) {
                return null;
            }
            raw = reinterpret_cast<void*>(phys);
            user = raw;
            memory::set(raw, 0, static_cast<size_phys>(page_count) * PAGE_SIZE_4K);
            from_phys = true;
        }
    }

    auto* rec = static_cast<process::process_allocation*>(
        g_kernel_heap.allocate_zero(sizeof(process::process_allocation)));
    if (rec == null) {
        if (from_phys) {
            if (ctx->address_space != null && user != raw) {
                vm::unmap_range(ctx->address_space, reinterpret_cast<virt_addr>(user), allocated);
                free_process_allocation_pages(raw, allocated);
            } else {
                free_process_allocation_pages(raw, allocated);
            }
        } else {
            g_kernel_heap.free(raw);
        }
        return null;
    }

    rec->user_ptr = user;
    rec->raw_ptr = raw;
    rec->requested_size = requested;
    rec->allocated_size = allocated;
    rec->from_phys = from_phys;
    rec->next = ctx->allocations;
    ctx->allocations = rec;

    log::debug() << (executable ? "malloc_exec" : "malloc")
                 << ": allocated " << static_cast<unsigned long long>(allocated)
                 << " bytes at " << user
                 << " (raw: " << raw << ", phys: " << (from_phys ? raw : 0) << ")";

    return rec->user_ptr;
}

static void* stub_malloc(vk_usize size) {
    return stub_malloc_internal(size, false);
}

static void* stub_malloc_executable(vk_usize size) {
    return stub_malloc_internal(size, true);
}

static void stub_free(void* ptr) {
    if (ptr == null) {
        return;
    }

    log::debug() << "free: ptr=" << ptr;

    auto* ctx = static_cast<process::process_task_context*>(sched::current_task_user_data());
    if (ctx == null) {
        g_kernel_heap.free(ptr);
        return;
    }

    process::process_allocation* prev = null;
    auto* rec = ctx->allocations;
    while (rec != null) {
        if (rec->user_ptr == ptr) {
            if (prev != null) {
                prev->next = rec->next;
            } else {
                ctx->allocations = rec->next;
            }
            if (ctx->address_space != null && rec->raw_ptr != rec->user_ptr) {
                vm::unmap_range(ctx->address_space,
                                reinterpret_cast<virt_addr>(rec->user_ptr),
                                rec->allocated_size);
                free_process_allocation_pages(rec->raw_ptr, rec->allocated_size);
            } else if (rec->from_phys) {
                free_process_allocation_pages(rec->raw_ptr, rec->allocated_size);
            } else {
                g_kernel_heap.free(rec->raw_ptr);
            }
            g_kernel_heap.free(rec);
            return;
        }
        prev = rec;
        rec = rec->next;
    }

    log::warn() << "process: ignoring free of unowned pointer " << reinterpret_cast<const void*>(ptr);
}

/* ---- filesystem ---- */

static int stub_file_exists(const char* name) {
    return fs::file_exists(name) ? 1 : 0;
}

static vk_usize stub_file_size(const char* name) {
    return fs::file_size(name);
}

/* ---- process ---- */

static void stub_exit(int code) {
    auto* ctx = static_cast<process::process_task_context*>(sched::current_task_user_data());
    if (ctx != null) {
        process::cleanup_process_context(ctx, code);
    } else {
        log::printk() << "Process exited with code " << code << "\n";
    }

    sched::exit_task();
}

static void stub_yield() {
    sched::yield();
}

static void stub_sleep(vk_u64 ticks) {
    sched::sleep(static_cast<u64>(ticks));
}

static vk_i64 stub_run(const char* path) {
    if (path == null) return -1;
    return process::run(path, current_console_interface());
}

static vk_i64 stub_run_with_fb(const char* path, const vk_framebuffer_info_t* fb) {
    if (path == null || fb == null) return -1;
    return process::run(path, process::console_interface::graphical, fb);
}

static bool s_compositor_active = false;
static vk_framebuffer_info_t s_compositor_default_fb = {};
static bool s_compositor_default_fb_valid = false;

static vk_i64 stub_run_auto(const char* path) {
    if (path == null) return -1;
    const auto interface = current_console_interface();
    if (interface == process::console_interface::graphical &&
        s_compositor_active &&
        s_compositor_default_fb_valid) {
        return process::run(path, interface, &s_compositor_default_fb);
    }
    return process::run(path, interface);
}

static vk_i64 stub_run_cmdline(const char* command_line) {
    if (command_line == null) return -1;
    const auto interface = current_console_interface();
    if (interface == process::console_interface::graphical &&
        s_compositor_active &&
        s_compositor_default_fb_valid) {
        return process::run_command_line(command_line,
                                         interface,
                                         &s_compositor_default_fb);
    }
    return process::run_command_line(command_line, interface);
}

static int stub_exec_cmdline(const char* command_line) {
    if (command_line == null) return -1;

    vk_framebuffer_info_t fb_override = {};
    const vk_framebuffer_info_t* fb_ptr = null;
    auto* ctx = static_cast<process::process_task_context*>(sched::current_task_user_data());
    if (ctx != null && ctx->fb_override_valid) {
        fb_override = ctx->fb_override;
        fb_ptr = &fb_override;
    }

    return process::exec_command_line(command_line, current_console_interface(), fb_ptr);
}

static int stub_set_compositor_active(vk_u32 active) {
    s_compositor_active = (active != 0u);
    return 1;
}

static auto validate_fb(const vk_framebuffer_info_t* fb) -> bool {
    return fb != null
        && fb->valid != 0u
        && fb->base != 0u
        && fb->width > 0u
        && fb->height > 0u
        && fb->stride >= fb->width;
}

static int stub_set_compositor_default_fb(const vk_framebuffer_info_t* fb) {
    if (!validate_fb(fb)) {
        s_compositor_default_fb = {};
        s_compositor_default_fb_valid = false;
        return 0;
    }
    s_compositor_default_fb = *fb;
    s_compositor_default_fb_valid = true;
    return 1;
}

static vk_u64 stub_tick_count() {
    return sched::tick_count();
}

static void stub_wait_task(vk_i64 task_id) {
    if (task_id < 0) return;
    sched::wait_for_task(static_cast<u64>(task_id));
}

static int stub_terminate_task(vk_u64 task_id) {
    return sched::terminate_task(task_id) ? 1 : 0;
}

static vk_usize stub_task_snapshot(vk_task_info_t* out, vk_usize max_tasks) {
    if (out == null || max_tasks == 0) {
        return sched::snapshot_tasks(null, 0);
    }

    task_snapshot snapshots[MAX_TASKS];
    vk_usize total = sched::snapshot_tasks(snapshots, max_tasks < MAX_TASKS ? max_tasks : MAX_TASKS);
    vk_usize count = total < max_tasks ? total : max_tasks;
    if (count > MAX_TASKS) count = MAX_TASKS;

    for (vk_usize i = 0; i < count; ++i) {
        out[i].id = snapshots[i].id;
        out[i].state = static_cast<vk_u32>(snapshots[i].state);
        out[i].cpu = snapshots[i].cpu;
        out[i].cpu_ticks = snapshots[i].cpu_ticks;
        static_string<32> name_buf(snapshots[i].name);
        memory::copy(out[i].name, name_buf.c_str(), sizeof(out[i].name));
        out[i].name[sizeof(out[i].name) - 1] = '\0';
    }

    return total;
}

/* ---- sound ---- */

static int stub_snd_mix_play(int channel, const void* data, vk_u32 num_samples,
                              vk_u32 format, vk_u32 sample_rate,
                              vk_u32 vol_left, vk_u32 vol_right) {
    if (!data || num_samples == 0 || sample_rate == 0) return 0;
    auto fmt = static_cast<sound_format>(format);
    return sound::mix_play(channel, static_cast<const u8*>(data), num_samples,
                           fmt, sample_rate,
                           static_cast<u8>(vol_left  & 0xFF),
                           static_cast<u8>(vol_right & 0xFF)) ? 1 : 0;
}

static int stub_snd_mix_queue_play(int channel, const void* data, vk_u32 num_samples,
                                    vk_u32 format, vk_u32 sample_rate,
                                    vk_u32 vol_left, vk_u32 vol_right) {
    if (!data || num_samples == 0 || sample_rate == 0) return 0;
    auto fmt = static_cast<sound_format>(format);
    return sound::mix_queue_play(channel, static_cast<const u8*>(data), num_samples,
                                 fmt, sample_rate,
                                 static_cast<u8>(vol_left  & 0xFF),
                                 static_cast<u8>(vol_right & 0xFF)) ? 1 : 0;
}

static void stub_snd_mix_stop(int channel) {
    sound::mix_stop(channel);
}

static int stub_snd_mix_is_playing(int channel) {
    return sound::mix_is_playing(channel) ? 1 : 0;
}

/* ---- mouse ---- */

static auto should_use_framebuffer() -> bool;  /* defined below */

static int stub_poll_mouse(vk_mouse_event_t* out) {
    if (out == null || !should_use_framebuffer()) return 0;

    auto* ctx = static_cast<process::process_task_context*>(sched::current_task_user_data());
    if (ctx != null && ctx->mouse_q_head != ctx->mouse_q_tail) {
        *out = ctx->mouse_queue[ctx->mouse_q_tail];
        ctx->mouse_q_tail = (ctx->mouse_q_tail + 1) % process::process_task_context::MOUSE_QUEUE_SIZE;
        return 1;
    }

    if (ctx != null && ctx->fb_override_valid) {
        return 0;
    }

    vk_mouse_event_t ev{};
    if (input::poll_mouse(ev)) {
        *out = ev;
        return 1;
    }
    return 0;
}

static int stub_send_mouse(vk_u64 task_id, const vk_mouse_event_t* ev) {
    if (ev == null) return 0;
    auto* target = static_cast<process::process_task_context*>(sched::task_user_data(task_id));
    if (target == null) return 0;

    usize next = (target->mouse_q_head + 1) % process::process_task_context::MOUSE_QUEUE_SIZE;
    if (next == target->mouse_q_tail) {
        target->mouse_q_tail = (target->mouse_q_tail + 1) % process::process_task_context::MOUSE_QUEUE_SIZE;
    }
    target->mouse_queue[target->mouse_q_head] = *ev;
    target->mouse_q_head = next;
    return 1;
}

static void enqueue_framebuffer_event(process::process_task_context* target,
                                      vk_u32 type,
                                      const vk_framebuffer_info_t& framebuffer) {
    if (target == null || !target->framebuffer_resize_events_enabled) {
        return;
    }

    vk_framebuffer_event_t event = {};
    event.type = type;
    event.framebuffer = framebuffer;

    usize next = (target->framebuffer_event_q_head + 1)
               % process::process_task_context::FRAMEBUFFER_EVENT_QUEUE_SIZE;
    if (next == target->framebuffer_event_q_tail) {
        target->framebuffer_event_q_tail = (target->framebuffer_event_q_tail + 1)
                                        % process::process_task_context::FRAMEBUFFER_EVENT_QUEUE_SIZE;
    }
    target->framebuffer_event_queue[target->framebuffer_event_q_head] = event;
    target->framebuffer_event_q_head = next;
}

static vk_u32 stub_ticks_per_sec() {
    return SCHED_TICK_HZ;
}

static void copy_cstr(char* out, usize out_cap, const char* src) {
    if (out == null || out_cap == 0) {
        return;
    }
    usize index = 0;
    if (src != null) {
        while (src[index] != '\0' && index + 1 < out_cap) {
            out[index] = src[index];
            ++index;
        }
    }
    out[index] = '\0';
}

static vk_usize stub_kobj_rpc(const char* req_json, char* out, vk_usize out_cap) {
    if (out == null || out_cap == 0) return 0;
    static usize s_kobj_trace_budget = 64;
    if (s_kobj_trace_budget > 0) {
        --s_kobj_trace_budget;
        auto* ctx = static_cast<process::process_task_context*>(sched::current_task_user_data());
        trace_process_buffer("kobj-rpc-request", ctx, req_json, req_json != null ? 1 : 0);
    }
    kobj::krpc(req_json, out, out_cap);
    vk_usize len = 0;
    while (len < out_cap && out[len] != '\0') ++len;
    return len;
}

static int stub_kobj_query(const char* path,
                           char* out_value,
                           vk_usize out_value_cap,
                           vk_kobj_node_info_t* out_info) {
    if (out_value != null && out_value_cap > 0) {
        out_value[0] = '\0';
    }
    if (out_info != null) {
        *out_info = {};
    }

    kobj::KVal value {};
    kobj::KNodeInfo info {};
    if (!kobj::kquery(path, &value, &info)) {
        return 0;
    }

    if (out_info != null) {
        copy_cstr(out_info->name, sizeof(out_info->name), info.name.c_str());
        copy_cstr(out_info->unit, sizeof(out_info->unit), info.unit.c_str());
        out_info->type = static_cast<vk_u32>(info.type);
        out_info->readable = info.readable ? 1u : 0u;
        out_info->writable = info.writable ? 1u : 0u;
        out_info->volatile_node = info.volatile_node ? 1u : 0u;
        out_info->range_min = info.range_min;
        out_info->range_max = info.range_max;
        out_info->enum_count = info.enum_count;
        for (vk_u32 i = 0; i < info.enum_count && i < VK_KOBJ_ENUM_MAX; ++i) {
            copy_cstr(out_info->enum_labels[i],
                      sizeof(out_info->enum_labels[i]),
                      info.enum_labels[i]);
        }
    }

    if (out_value != null && out_value_cap > 0 && info.readable) {
        kobj::kval_render(value, kobj::resolve(path), out_value, out_value_cap);
    }
    return 1;
}

static vk_usize stub_kobj_list(const char* path, vk_kobj_child_t* out_items, vk_usize max_items) {
    kobj::KChildInfo items[64] {};
    const usize total = kobj::klist(path, items, sizeof(items) / sizeof(items[0]));
    if (out_items == null || max_items == 0) {
        return total;
    }
    const usize limit = total < static_cast<usize>(max_items) ? total : static_cast<usize>(max_items);

    for (usize i = 0; i < limit; ++i) {
        copy_cstr(out_items[i].name, sizeof(out_items[i].name), items[i].name.c_str());
        out_items[i].type = static_cast<vk_u32>(items[i].type);
    }
    return total;
}

static int stub_kobj_set_value(const char* path, const char* value) {
    if (path == null || value == null) {
        return 0;
    }

    kobj::KNodeInfo info {};
    if (!kobj::kinfo(path, &info) || !info.writable) {
        return 0;
    }

    kobj::KVal set_value {};
    if (info.type == kobj::KTag::U64) {
        u64 parsed = 0;
        if (!kobj::detail::parse_u64_segment(value, kobj::detail::cstrlen(value), &parsed)) {
            return 0;
        }
        set_value = kobj::KVal::from_u64(parsed);
    } else if (info.type == kobj::KTag::Bool) {
        const bool parsed = (value[0] == '1' && value[1] == '\0')
            || (value[0] == 't' && value[1] == 'r' && value[2] == 'u' && value[3] == 'e' && value[4] == '\0')
            || (value[0] == 'y' && value[1] == 'e' && value[2] == 's' && value[3] == '\0');
        set_value = kobj::KVal::from_bool(parsed);
    } else if (info.type == kobj::KTag::Str) {
        set_value = kobj::KVal::from_str(value);
    } else if (info.type == kobj::KTag::Enum) {
        for (u32 i = 0; i < info.enum_count; ++i) {
            kobj::KStr label {};
            label.set(info.enum_labels[i]);
            if (label.eq(value)) {
                set_value = kobj::KVal::from_enum(i);
                return kobj::kset(path, set_value) ? 1 : 0;
            }
        }
        return 0;
    } else {
        return 0;
    }

    return kobj::kset(path, set_value) ? 1 : 0;
}

static int stub_driver_load(const char* name) {
    return name != null ? driver::load(name) : -1;
}

static int stub_driver_unload(const char* name) {
    return name != null ? driver::unload(name) : -1;
}

static void stub_reboot() {
    arch::reboot();
}

static vk_usize stub_get_cmdline(char* out, vk_usize out_cap) {
    if (out == null || out_cap == 0) {
        return 0;
    }

    auto* ctx = static_cast<process::process_task_context*>(sched::current_task_user_data());
    if (ctx == null) {
        out[0] = '\0';
        return 0;
    }

    vk_usize len = static_cast<vk_usize>(ctx->command_line_len);
    if (len >= out_cap) {
        len = out_cap - 1;
    }

    if (len > 0) {
        memory::copy(out, ctx->command_line, len);
    }
    out[len] = '\0';
    return len;
}

static auto current_console_interface() -> process::console_interface {
    auto* ctx = static_cast<process::process_task_context*>(sched::current_task_user_data());
    if (ctx != null) {
        return ctx->interface;
    }
    return process::console_interface::graphical;
}

static auto should_use_framebuffer() -> bool {
    if (current_console_interface() != process::console_interface::graphical) {
        return false;
    }
    return console::framebuffer().valid;
}

static auto dequeue_ascii_key(process::process_task_context* ctx) -> char {
    while (ctx != null && ctx->key_q_head != ctx->key_q_tail) {
        vk_key_event_t ev = ctx->key_queue[ctx->key_q_tail];
        ctx->key_q_tail = (ctx->key_q_tail + 1) % process::process_task_context::KEY_QUEUE_SIZE;

        if (ev.pressed == 0u) {
            continue;
        }
        if (ev.ascii != '\0') {
            return ev.ascii;
        }

        if (ev.scancode == 0x1Cu) return '\r';
        if (ev.scancode == 0x0Eu) return '\b';
        if (ev.scancode == 0x0Fu) return '\t';
    }

    return '\0';
}

static void route_putc(char c) {
    if (should_use_framebuffer()) {
        auto* ctx = static_cast<process::process_task_context*>(sched::current_task_user_data());
        if (ctx != null && ctx->fb_override_valid) {
            console::putc_framebuffer_surface(ctx->fb_override, ctx->fb_text_col, ctx->fb_text_row, c);
            return;
        }
        console::putc_framebuffer(c);
    } else {
        console::putc_serial(c);
    }
}

static void route_puts(const char* str) {
    if (str == null) {
        return;
    }
    while (*str != '\0') {
        route_putc(*str++);
    }
}

static auto route_stdio_write(const char* buf, vk_usize len) -> vk_usize {
    if (buf == null || len == 0) {
        return 0;
    }

    auto* ctx = static_cast<process::process_task_context*>(sched::current_task_user_data());
    if (ctx != null && ctx->stdio_to_serial) {
        for (vk_usize i = 0; i < len; ++i) {
            console::putc_serial(buf[i]);
        }
        return len;
    }

    for (vk_usize i = 0; i < len; ++i) {
        route_putc(buf[i]);
    }
    return len;
}

static void route_put_hex(vk_u64 value) {
    char buf[19];
    log::hex(buf, sizeof(buf), value);
    route_puts(buf);
}

static void route_put_dec(vk_u64 value) {
    if (value == 0) {
        route_putc('0');
        return;
    }

    char buf[21];
    i32 i = 0;
    while (value > 0 && i < 20) {
        buf[i++] = static_cast<char>('0' + (value % 10));
        value /= 10;
    }
    while (i > 0) {
        route_putc(buf[--i]);
    }
}

static void route_clear() {
    if (should_use_framebuffer()) {
        auto* ctx = static_cast<process::process_task_context*>(sched::current_task_user_data());
        if (ctx != null && ctx->fb_override_valid) {
            console::clear_framebuffer_surface(ctx->fb_override, ctx->fb_text_col, ctx->fb_text_row);
            return;
        }
        console::clear_framebuffer();
    } else {
        console::clear_serial();
    }
}

static char route_getc() {
    if (should_use_framebuffer()) {
        auto* ctx = static_cast<process::process_task_context*>(sched::current_task_user_data());
        if (ctx != null && ctx->fb_override_valid) {
            while (true) {
                char c = dequeue_ascii_key(ctx);
                if (c != '\0') {
                    return c;
                }
                sched::sleep(1);
            }
        }
        return input::getc_ps2();
    }
    return input::getc_serial();
}

static char route_try_getc() {
    if (should_use_framebuffer()) {
        auto* ctx = static_cast<process::process_task_context*>(sched::current_task_user_data());
        if (ctx != null && ctx->fb_override_valid) {
            return dequeue_ascii_key(ctx);
        }
        return input::try_getc_ps2();
    }
    return input::try_getc_serial();
}

static int route_poll_key(vk_key_event_t* out) {
    if (out == null || !should_use_framebuffer()) return 0;

    auto* ctx = static_cast<process::process_task_context*>(sched::current_task_user_data());
    if (ctx != null && ctx->key_q_head != ctx->key_q_tail) {
        *out = ctx->key_queue[ctx->key_q_tail];
        ctx->key_q_tail = (ctx->key_q_tail + 1) % process::process_task_context::KEY_QUEUE_SIZE;
        return 1;
    }

    if (ctx != null && ctx->fb_override_valid) {
        return 0;
    }

    vk_key_event_t ev{};
    if (input::poll_key(ev)) {
        *out = ev;
        return 1;
    }
    return 0;
}

static int stub_send_key(vk_u64 task_id, const vk_key_event_t* ev) {
    if (ev == null) return 0;
    auto* target = static_cast<process::process_task_context*>(sched::task_user_data(task_id));
    if (target == null) return 0;

    usize next = (target->key_q_head + 1) % process::process_task_context::KEY_QUEUE_SIZE;
    if (next == target->key_q_tail) {
        target->key_q_tail = (target->key_q_tail + 1) % process::process_task_context::KEY_QUEUE_SIZE;
    }
    target->key_queue[target->key_q_head] = *ev;
    target->key_q_head = next;
    return 1;
}

static auto remap_target_framebuffer(process::process_task_context* target,
                                     const vk_framebuffer_info_t* fb) -> bool {
    if (target == null || !validate_fb(fb)) {
        return false;
    }

    vk_framebuffer_info_t mapped = *fb;
    usize fb_bytes = 0;
    if (!framebuffer::byte_size(*fb, fb_bytes)) {
        return false;
    }

    auto* source_ctx = static_cast<process::process_task_context*>(sched::current_task_user_data());
    const virt_addr source_base = static_cast<virt_addr>(fb->base);
    if (target->address_space != null
        && source_base >= vm::USER_IMAGE_BASE
        && source_base < vm::USER_MAP_LIMIT) {
        if (source_ctx == null || source_ctx->address_space == null) {
            log::warn() << "vmtrace: set_task_framebuffer has user address but no source address space";
            return false;
        }

        const usize source_offset = static_cast<usize>(source_base & (PAGE_SIZE_4K - 1));
        const virt_addr target_start = vm::USER_SHARED_BASE;
        const virt_addr target_base = target_start + source_offset;
        const usize mapped_bytes = align_up(source_offset + fb_bytes, PAGE_SIZE_4K);
        const virt_addr source_start = align_down(source_base, PAGE_SIZE_4K);
        usize old_mapped_bytes = 0;

        if (target->fb_override_valid
            && target->fb_override.base >= vm::USER_SHARED_BASE
            && target->fb_override.base < vm::USER_MAP_LIMIT) {
            usize old_fb_bytes = 0;
            if (framebuffer::byte_size(target->fb_override, old_fb_bytes)) {
                const virt_addr old_target_base = static_cast<virt_addr>(target->fb_override.base);
                const usize old_offset = static_cast<usize>(old_target_base - align_down(old_target_base, PAGE_SIZE_4K));
                old_mapped_bytes = align_up(old_offset + old_fb_bytes, PAGE_SIZE_4K);
            }
        }

        for (usize offset = 0; offset < mapped_bytes; offset += PAGE_SIZE_4K) {
            phys_addr source_phys = 0;
            u64 source_flags = 0;
            const virt_addr source_page = source_start + offset;
            if (!vm::debug_resolve(source_ctx->address_space, source_page, &source_phys, &source_flags)) {
                log::warn() << "vmtrace: set_task_framebuffer source resolve failed src="
                            << reinterpret_cast<const void*>(static_cast<usize>(source_page));
                return false;
            }
        }

        /* Map first so USER_SHARED_BASE never goes transiently unmapped. */
        for (usize offset = 0; offset < mapped_bytes; offset += PAGE_SIZE_4K) {
            phys_addr source_phys = 0;
            u64 source_flags = 0;
            const virt_addr source_page = source_start + offset;
            if (!vm::debug_resolve(source_ctx->address_space, source_page, &source_phys, &source_flags)) {
                if (offset > 0) {
                    vm::unmap_range(target->address_space, target_start, offset);
                }
                target->fb_override = {};
                target->fb_override_valid = false;
                target->fb_text_col = 0;
                target->fb_text_row = 0;
                return false;
            }

            if (!vm::map_page(target->address_space,
                              target_start + offset,
                              align_down(source_phys, PAGE_SIZE_4K),
                              vm::MAP_WRITABLE | vm::MAP_USER)) {
                if (offset > 0) {
                    vm::unmap_range(target->address_space, target_start, offset);
                }
                target->fb_override = {};
                target->fb_override_valid = false;
                target->fb_text_col = 0;
                target->fb_text_row = 0;
                log::warn() << "vmtrace: set_task_framebuffer map failed dst="
                            << reinterpret_cast<const void*>(static_cast<usize>(target_start + offset))
                            << " phys=" << reinterpret_cast<const void*>(static_cast<usize>(source_phys));
                return false;
            }
        }

        /* Drop any stale tail pages from the old shared mapping. */
        if (old_mapped_bytes > mapped_bytes) {
            vm::unmap_range(target->address_space,
                            target_start + mapped_bytes,
                            old_mapped_bytes - mapped_bytes);
        }

        mapped.base = static_cast<vk_u64>(target_base);
    }

    target->fb_override = mapped;
    target->fb_override_valid = true;
    target->fb_text_col = 0;
    target->fb_text_row = 0;
    return true;
}

static int stub_set_task_framebuffer(vk_u64 task_id, const vk_framebuffer_info_t* fb) {
    if (fb == null) return 0;

    if (!sched::task_blocked_off_cpu(task_id)) {
        return 0;
    }

    auto* target = static_cast<process::process_task_context*>(sched::task_user_data(task_id));
    if (target == null) return 0;
    if (!remap_target_framebuffer(target, fb)) {
        return 0;
    }
    if (target->fb_override_valid) {
        enqueue_framebuffer_event(target,
                                  VK_FRAMEBUFFER_EVENT_RESIZED,
                                  target->fb_override);
    }
    return target->fb_override_valid ? 1 : 0;
}

static int stub_poll_framebuffer_event(vk_framebuffer_event_t* out) {
    if (out == null || !should_use_framebuffer()) {
        return 0;
    }

    auto* ctx = static_cast<process::process_task_context*>(sched::current_task_user_data());
    if (ctx == null
        || !ctx->framebuffer_resize_events_enabled
        || ctx->framebuffer_event_q_head == ctx->framebuffer_event_q_tail) {
        return 0;
    }

    *out = ctx->framebuffer_event_queue[ctx->framebuffer_event_q_tail];
    ctx->framebuffer_event_q_tail = (ctx->framebuffer_event_q_tail + 1)
                                  % process::process_task_context::FRAMEBUFFER_EVENT_QUEUE_SIZE;
    return 1;
}

static int stub_set_framebuffer_resize_events(vk_u32 enabled) {
    auto* ctx = static_cast<process::process_task_context*>(sched::current_task_user_data());
    if (ctx == null) {
        return 0;
    }

    ctx->framebuffer_resize_events_enabled = (enabled != 0u);
    if (!ctx->framebuffer_resize_events_enabled) {
        ctx->framebuffer_event_q_head = 0;
        ctx->framebuffer_event_q_tail = 0;
    }
    return 1;
}

static int stub_task_accepts_framebuffer_resize(vk_u64 task_id) {
    auto* target = static_cast<process::process_task_context*>(sched::task_user_data(task_id));
    if (target == null) {
        return 0;
    }
    return target->framebuffer_resize_events_enabled ? 1 : 0;
}

static int stub_set_startup_window_size(vk_u32 width, vk_u32 height) {
    auto* ctx = static_cast<process::process_task_context*>(sched::current_task_user_data());
    if (ctx == null) {
        return 0;
    }

    if (width == 0 || height == 0) {
        ctx->startup_window_size_set = false;
        ctx->startup_window_width = 0;
        ctx->startup_window_height = 0;
        return 1;
    }

    ctx->startup_window_size_set = true;
    ctx->startup_window_width = width;
    ctx->startup_window_height = height;
    return 1;
}

static int stub_get_task_startup_window_size(vk_u64 task_id, vk_u32* out_width, vk_u32* out_height) {
    if (out_width == null || out_height == null) {
        return 0;
    }

    *out_width = 0;
    *out_height = 0;

    auto* target = static_cast<process::process_task_context*>(sched::task_user_data(task_id));
    if (target == null || !target->startup_window_size_set) {
        return 0;
    }

    *out_width = target->startup_window_width;
    *out_height = target->startup_window_height;
    return 1;
}

static void route_framebuffer_info(vk_framebuffer_info_t* out) {
    if (out == null) return;
    auto* ctx = static_cast<process::process_task_context*>(sched::current_task_user_data());
    if (!should_use_framebuffer()) {
        *out = {};
        return;
    }
    if (ctx != null && ctx->fb_override_valid) {
        *out = ctx->fb_override;
        return;
    }
    auto fb = console::framebuffer();
    out->base   = static_cast<vk_u64>(fb.base);
    out->width  = static_cast<vk_u32>(fb.width);
    out->height = static_cast<vk_u32>(fb.height);
    out->stride = static_cast<vk_u32>(fb.stride);
    out->format = static_cast<vk_pixel_format_t>(fb.format);
    out->valid  = fb.valid ? 1u : 0u;
}

static vk_file_handle_t stub_file_open(const char* path, const char* mode) {
    return static_cast<vk_file_handle_t>(fs::file_open(path, mode));
}

static int stub_file_close(vk_file_handle_t handle) {
    return fs::file_close(static_cast<fs::file_handle>(handle));
}

static vk_usize stub_file_read_handle(vk_file_handle_t handle, void* buf, vk_usize buf_size) {
    return fs::file_read(static_cast<fs::file_handle>(handle), buf, buf_size);
}

static vk_usize stub_file_write_handle(vk_file_handle_t handle, const void* buf, vk_usize buf_size) {
    return fs::file_write(static_cast<fs::file_handle>(handle), buf, buf_size);
}

static int stub_file_seek(vk_file_handle_t handle, vk_i64 offset, int whence) {
    return fs::file_seek(static_cast<fs::file_handle>(handle), offset, whence);
}

static vk_i64 stub_file_tell(vk_file_handle_t handle) {
    return fs::file_tell(static_cast<fs::file_handle>(handle));
}

static int stub_file_truncate(vk_file_handle_t handle, vk_i64 length) {
    return fs::file_truncate(static_cast<fs::file_handle>(handle), length);
}

static int stub_file_remove(const char* path) {
    return fs::file_remove(path);
}

/* ============================================================
 * Global kernel API table
 * Populated once by init(); never changes at runtime.
 * ============================================================ */

static vk_api_t s_api;
static bool     s_api_ready = false;

void init() {
    if (s_api_ready) return;

    s_api = {};
    s_api.api_version = VK_API_VERSION;
    /* console output */
    s_api.vk_puts = route_puts;
    s_api.vk_putc = route_putc;
    s_api.vk_put_hex = route_put_hex;
    s_api.vk_put_dec = route_put_dec;
    s_api.vk_clear = route_clear;
    /* console input */
    /* stdio */
    s_api.vk_stdio_write = route_stdio_write;
    s_api.vk_getc = route_getc;
    s_api.vk_try_getc = route_try_getc;
    /* memory */
    s_api.vk_malloc = stub_malloc;
    s_api.vk_malloc_executable = stub_malloc_executable;
    s_api.vk_free = stub_free;
    /* filesystem */
    s_api.vk_file_exists = stub_file_exists;
    s_api.vk_file_size = stub_file_size;
    /* process */
    s_api.vk_exit = stub_exit;
    s_api.vk_yield = stub_yield;
    s_api.vk_sleep = stub_sleep;
    s_api.vk_run_with_fb = stub_run_with_fb;
    s_api.vk_run_auto = stub_run_auto;
    s_api.vk_run_cmdline = stub_run_cmdline;
    s_api.vk_run = stub_run;
    s_api.vk_tick_count = stub_tick_count;
    /* framebuffer */
    s_api.vk_framebuffer_info = route_framebuffer_info;
    /* file streams */
    s_api.vk_file_open = stub_file_open;
    s_api.vk_file_close = stub_file_close;
    s_api.vk_file_read_handle = stub_file_read_handle;
    s_api.vk_file_write_handle = stub_file_write_handle;
    s_api.vk_file_seek = stub_file_seek;
    s_api.vk_file_tell = stub_file_tell;
    s_api.vk_file_remove = stub_file_remove;
    /* raw keyboard */
    s_api.vk_poll_key = route_poll_key;
    s_api.vk_send_key = stub_send_key;
    s_api.vk_set_task_framebuffer = stub_set_task_framebuffer;
    s_api.vk_ticks_per_sec = stub_ticks_per_sec;
    /* task sync */
    s_api.vk_wait_task = stub_wait_task;
    /* sound */
    s_api.vk_snd_mix_play       = stub_snd_mix_play;
    s_api.vk_snd_mix_queue_play = stub_snd_mix_queue_play;
    s_api.vk_snd_mix_stop       = stub_snd_mix_stop;
    s_api.vk_snd_mix_is_playing = stub_snd_mix_is_playing;
    /* mouse */
    s_api.vk_poll_mouse = stub_poll_mouse;
    /* task stats */
    s_api.vk_task_snapshot = stub_task_snapshot;
    /* compositor control */
    s_api.vk_set_compositor_active = stub_set_compositor_active;
    s_api.vk_set_compositor_default_fb = stub_set_compositor_default_fb;
    /* kobj */
    s_api.vk_kobj_rpc = stub_kobj_rpc;
    /* input routing */
    s_api.vk_send_mouse = stub_send_mouse;
    /* process command line */
    s_api.vk_get_cmdline = stub_get_cmdline;
    s_api.vk_terminate_task = stub_terminate_task;
    s_api.vk_exec_cmdline = stub_exec_cmdline;
    /* framebuffer events */
    s_api.vk_poll_framebuffer_event = stub_poll_framebuffer_event;
    s_api.vk_set_framebuffer_resize_events = stub_set_framebuffer_resize_events;
    s_api.vk_task_accepts_framebuffer_resize = stub_task_accepts_framebuffer_resize;
    s_api.vk_set_startup_window_size = stub_set_startup_window_size;
    s_api.vk_get_task_startup_window_size = stub_get_task_startup_window_size;
    s_api.vk_kobj_query = stub_kobj_query;
    s_api.vk_kobj_list = stub_kobj_list;
    s_api.vk_kobj_set_value = stub_kobj_set_value;
    s_api.vk_driver_load = stub_driver_load;
    s_api.vk_driver_unload = stub_driver_unload;
    s_api.vk_reboot = stub_reboot;
    s_api.vk_file_truncate = stub_file_truncate;

    s_api_ready = true;
}

auto get_api() -> const vk_api_t* {
    if (!s_api_ready) init();
    return &s_api;
}

} // namespace kernel_api
} // namespace vk
