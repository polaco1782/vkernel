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
#include "input.h"
#include "scheduler.h"
#include "sound.h"
#include "process.h"
#include "kobj.h"
#include "vk.h"
#include "process_internal.h"

namespace vk {
namespace kernel_api {

static auto current_console_interface() -> process::console_interface;
static auto validate_fb(const vk_framebuffer_info_t* fb) -> bool;
static void route_putc(char c);
static void route_puts(const char* str);

/* ============================================================
 * Kernel-side API stub functions
 * These are plain C-linkage functions assigned into vk_api_t.
 * Keeping them as named statics (not lambdas) means the compiler
 * can take their address directly without a trampoline.
 * ============================================================ */

/* ---- memory ---- */

static constexpr usize PROCESS_ALLOC_GUARD_SIZE = PAGE_SIZE_4K;

static void free_process_allocation_pages(void* ptr, usize size) {
    if (ptr == null || size == 0) {
        return;
    }

    u32 page_count = static_cast<u32>((size + PAGE_SIZE_4K - 1) / PAGE_SIZE_4K);
    g_phys_alloc.free_pages(reinterpret_cast<phys_addr>(ptr), page_count);
}

static void* stub_malloc(vk_usize size) {
    if (size == 0) {
        return null;
    }

    auto* ctx = static_cast<process::process_task_context*>(sched::current_task_user_data());
    if (ctx == null) {
        return g_kernel_heap.allocate_zero(size);
    }

    const usize requested = static_cast<usize>(size);
    const usize rounded = align_up(requested, static_cast<usize>(16));
    if (rounded < requested || rounded > ~static_cast<usize>(0) - PROCESS_ALLOC_GUARD_SIZE) {
        return null;
    }

    const usize allocated = rounded + PROCESS_ALLOC_GUARD_SIZE;
    bool from_phys = false;
    void* raw = g_kernel_heap.allocate_zero(allocated);
    if (raw == null) {
        const u32 page_count = static_cast<u32>(
            (allocated + PAGE_SIZE_4K - 1) / PAGE_SIZE_4K);
        phys_addr phys = g_phys_alloc.allocate_pages(page_count, PAGE_SIZE_4K, 0);
        if (phys == 0) {
            return null;
        }
        raw = reinterpret_cast<void*>(phys);
        memory::set(raw, 0, static_cast<size_phys>(page_count) * PAGE_SIZE_4K);
        from_phys = true;
    }

    auto* rec = static_cast<process::process_allocation*>(
        g_kernel_heap.allocate_zero(sizeof(process::process_allocation)));
    if (rec == null) {
        if (from_phys) {
            free_process_allocation_pages(raw, allocated);
        } else {
            g_kernel_heap.free(raw);
        }
        return null;
    }

    rec->user_ptr = raw;
    rec->raw_ptr = raw;
    rec->requested_size = requested;
    rec->allocated_size = allocated;
    rec->from_phys = from_phys;
    rec->next = ctx->allocations;
    ctx->allocations = rec;

    return rec->user_ptr;
}

static void stub_free(void* ptr) {
    if (ptr == null) {
        return;
    }

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
            if (rec->from_phys) {
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

static void* stub_memset(void* dest, int c, vk_usize n) {
    return memory::set(dest, c, n);
}

static void* stub_memcpy(void* dest, const void* src, vk_usize n) {
    return memory::copy(dest, src, n);
}

/* ---- filesystem ---- */

static int stub_file_exists(const char* name) {
    return ramfs::find(name) != null ? 1 : 0;
}

static vk_usize stub_file_size(const char* name) {
    const auto* f = ramfs::find(name);
    return f ? f->size : 0;
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
    return process::run(path);
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
    if (s_compositor_active && s_compositor_default_fb_valid) {
        return process::run(path, process::console_interface::graphical, &s_compositor_default_fb);
    }
    return process::run(path);
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
        out[i]._reserved = 0;
        out[i].cpu_ticks = snapshots[i].cpu_ticks;
        static_string<32> name_buf(snapshots[i].name);
        memory::copy(out[i].name, name_buf.c_str(), sizeof(out[i].name));
        out[i].name[sizeof(out[i].name) - 1] = '\0';
    }

    return total;
}

/* ---- sound ---- */

static int stub_snd_play(const void* samples, vk_u32 length, vk_u32 format) {
    if (samples == null || length == 0) return 0;
    auto fmt = static_cast<sound_format>(format);
    return sound::play(static_cast<const u8*>(samples), length, fmt) ? 1 : 0;
}

static void stub_snd_stop() {
    sound::stop();
}

static int stub_snd_is_playing() {
    return sound::is_playing() ? 1 : 0;
}

static int stub_snd_set_sample_rate(vk_u32 rate_hz) {
    return sound::set_sample_rate(rate_hz) ? 1 : 0;
}

static void stub_snd_set_volume(vk_u32 left, vk_u32 right) {
    sound::set_volume(static_cast<u8>(left & 0xFF), static_cast<u8>(right & 0xFF));
}

/* ---- software mixer ---- */

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

static void stub_snd_mix_stop(int channel) {
    sound::mix_stop(channel);
}

static int stub_snd_mix_is_playing(int channel) {
    return sound::mix_is_playing(channel) ? 1 : 0;
}

static void stub_snd_mix_update() {
    sound::mix_update();
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

static vk_u32 stub_ticks_per_sec() {
    return SCHED_TICK_HZ;
}

static vk_usize stub_kobj_rpc(const char* req_json, char* out, vk_usize out_cap) {
    if (out == null || out_cap == 0) return 0;
    kobj::krpc(req_json, out, out_cap);
    vk_usize len = 0;
    while (len < out_cap && out[len] != '\0') ++len;
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

static void route_putc(char c) {
    if (should_use_framebuffer()) {
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
        console::clear_framebuffer();
    } else {
        console::clear_serial();
    }
}

static char route_getc() {
    if (should_use_framebuffer()) {
        return input::getc_ps2();
    }
    return input::getc_serial();
}

static char route_try_getc() {
    if (should_use_framebuffer()) {
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

static int stub_set_task_framebuffer(vk_u64 task_id, const vk_framebuffer_info_t* fb) {
    if (fb == null) return 0;
    auto* target = static_cast<process::process_task_context*>(sched::task_user_data(task_id));
    if (target == null) return 0;
    target->fb_override = *fb;
    target->fb_override_valid = (fb->valid != 0u && fb->base != 0u && fb->width > 0u && fb->height > 0u);
    return target->fb_override_valid ? 1 : 0;
}

static void route_framebuffer_info(vk_framebuffer_info_t* out) {
    if (out == null) return;
    if (!should_use_framebuffer()) {
        *out = {};
        return;
    }
    auto* ctx = static_cast<process::process_task_context*>(sched::current_task_user_data());
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

static void* stub_memmove(void* dest, const void* src, vk_usize n) {
    auto* dst = static_cast<unsigned char*>(dest);
    const auto* s = static_cast<const unsigned char*>(src);

    if (dst == s || n == 0) {
        return dest;
    }

    if (dst < s) {
        for (vk_usize i = 0; i < n; ++i) {
            dst[i] = s[i];
        }
    } else {
        for (vk_usize i = n; i > 0; --i) {
            dst[i - 1] = s[i - 1];
        }
    }

    return dest;
}

static int stub_memcmp(const void* lhs, const void* rhs, vk_usize n) {
    const auto* a = static_cast<const unsigned char*>(lhs);
    const auto* b = static_cast<const unsigned char*>(rhs);

    for (vk_usize i = 0; i < n; ++i) {
        if (a[i] != b[i]) {
            return static_cast<int>(a[i]) - static_cast<int>(b[i]);
        }
    }

    return 0;
}

struct kernel_file_stream {
    const file_entry* entry = null;
    vk_usize          position = 0;
    bool              readable = false;
    bool              writable = false;
    bool              eof = false;
    bool              error = false;
    bool              in_use = false;
};

static constexpr usize k_max_kernel_file_streams = 16;
static kernel_file_stream s_file_streams[k_max_kernel_file_streams];

static auto parse_mode_flags(string_view mode) -> bool {
    if (mode.data() == null) return false;

    bool saw_read = false;
    bool saw_write = false;
    bool saw_append = false;
    bool saw_plus = false;

    for (usize i = 0; i < mode.size(); ++i) {
        switch (mode[i]) {
            case 'r': saw_read = true; break;
            case 'w': saw_write = true; break;
            case 'a': saw_append = true; break;
            case '+': saw_plus = true; break;
            default: break;
        }
    }

    return saw_write || saw_append || saw_plus ? false : saw_read;
}

static auto handle_from_id(vk_file_handle_t handle) -> kernel_file_stream* {
    if (handle == 0 || handle > k_max_kernel_file_streams) return null;
    auto& stream = s_file_streams[static_cast<usize>(handle - 1)];
    return stream.in_use ? &stream : null;
}

static vk_file_handle_t stub_file_open(const char* path, const char* mode) {
    if (!parse_mode_flags(mode)) {
        return 0;
    }

    const auto* entry = ramfs::find(path);
    if (entry == null) {
        return 0;
    }

    for (usize i = 0; i < k_max_kernel_file_streams; ++i) {
        auto& stream = s_file_streams[i];
        if (stream.in_use) continue;

        stream.entry = entry;
        stream.position = 0;
        stream.readable = true;
        stream.writable = false;
        stream.eof = false;
        stream.error = false;
        stream.in_use = true;
        return static_cast<vk_file_handle_t>(i + 1);
    }

    return 0;
}

static int stub_file_close(vk_file_handle_t handle) {
    auto* stream = handle_from_id(handle);
    if (stream == null) return -1;

    *stream = {};
    return 0;
}

static vk_usize stub_file_read_handle(vk_file_handle_t handle, void* buf, vk_usize buf_size) {
    auto* stream = handle_from_id(handle);
    if (stream == null || buf == null || !stream->readable) return 0;

    if (stream->position >= stream->entry->size) {
        stream->eof = true;
        return 0;
    }

    vk_usize remaining = stream->entry->size - stream->position;
    vk_usize to_copy = remaining < buf_size ? remaining : buf_size;
    memory::copy(buf, stream->entry->data + stream->position, to_copy);
    stream->position += to_copy;
    stream->eof = stream->position >= stream->entry->size;
    return to_copy;
}

static vk_usize stub_file_write_handle(vk_file_handle_t handle, const void* buf, vk_usize buf_size) {
    auto* stream = handle_from_id(handle);
    if (stream == null || buf == null) return 0;

    stream->error = true;
    stream->writable = false;
    return 0;
}

static int stub_file_seek(vk_file_handle_t handle, vk_i64 offset, int whence) {
    auto* stream = handle_from_id(handle);
    if (stream == null) return -1;

    vk_i64 base = 0;
    switch (whence) {
        case 0: base = 0; break;
        case 1: base = static_cast<vk_i64>(stream->position); break;
        case 2: base = static_cast<vk_i64>(stream->entry->size); break;
        default: stream->error = true; return -1;
    }

    vk_i64 next = base + offset;
    if (next < 0) {
        stream->error = true;
        return -1;
    }

    stream->position = static_cast<vk_usize>(next);
    stream->eof = stream->position >= stream->entry->size;
    return 0;
}

static vk_i64 stub_file_tell(vk_file_handle_t handle) {
    auto* stream = handle_from_id(handle);
    if (stream == null) return -1;
    return static_cast<vk_i64>(stream->position);
}

static int stub_file_remove(const char* path) {
    (void)path;
    return -1;
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
    s_api.vk_getc = route_getc;
    s_api.vk_try_getc = route_try_getc;
    /* memory */
    s_api.vk_malloc = stub_malloc;
    s_api.vk_free = stub_free;
    s_api.vk_memset = stub_memset;
    s_api.vk_memcpy = stub_memcpy;
    s_api.vk_memmove = stub_memmove;
    s_api.vk_memcmp = stub_memcmp;
    /* filesystem */
    s_api.vk_file_exists = stub_file_exists;
    s_api.vk_file_size = stub_file_size;
    /* process */
    s_api.vk_exit = stub_exit;
    s_api.vk_yield = stub_yield;
    s_api.vk_sleep = stub_sleep;
    s_api.vk_run_with_fb = stub_run_with_fb;
    s_api.vk_run_auto = stub_run_auto;
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
    s_api.vk_snd_play = stub_snd_play;
    s_api.vk_snd_stop = stub_snd_stop;
    s_api.vk_snd_is_playing = stub_snd_is_playing;
    s_api.vk_snd_set_sample_rate = stub_snd_set_sample_rate;
    s_api.vk_snd_set_volume = stub_snd_set_volume;
    /* software mixer */
    s_api.vk_snd_mix_play       = stub_snd_mix_play;
    s_api.vk_snd_mix_stop       = stub_snd_mix_stop;
    s_api.vk_snd_mix_is_playing = stub_snd_mix_is_playing;
    s_api.vk_snd_mix_update     = stub_snd_mix_update;
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

    s_api_ready = true;
}

auto get_api() -> const vk_api_t* {
    if (!s_api_ready) init();
    return &s_api;
}

} // namespace kernel_api
} // namespace vk
