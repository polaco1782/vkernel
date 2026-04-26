/*
 * vkernel - UEFI Microkernel
 * Copyright (C) 2026 vkernel authors
 *
 * kernel_api.cpp - Kernel-side API table and user-facing stubs
 */

#include "config.h"
#include "types.h"
#include "console.h"
#include "memory.h"
#include "fs.h"
#include "input.h"
#include "scheduler.h"
#include "arch/x86_64/arch.h"
#include "sound.h"
#include "driver.h"
#include "process.h"
#include "vk.h"
#include "process_internal.h"

namespace vk {
namespace kernel_api {

/* ============================================================
 * Kernel-side API stub functions
 * These are plain C-linkage functions assigned into vk_api_t.
 * Keeping them as named statics (not lambdas) means the compiler
 * can take their address directly without a trampoline.
 * ============================================================ */

/* ---- memory ---- */

static void* stub_malloc(vk_usize size) {
    return g_kernel_heap.allocate(size);
}

static void stub_free(void* ptr) {
    g_kernel_heap.free(ptr);
}

static void* stub_memset(void* dest, int c, vk_usize n) {
    return memory::memory_set(dest, c, n);
}

static void* stub_memcpy(void* dest, const void* src, vk_usize n) {
    return memory::memory_copy(dest, src, n);
}

/* ---- filesystem ---- */

static int stub_file_exists(const char* name) {
    return ramfs::find(name) != null ? 1 : 0;
}

static vk_usize stub_file_size(const char* name) {
    const auto* f = ramfs::find(name);
    return f ? f->size : 0;
}

static vk_usize stub_file_read(const char* name, void* buf, vk_usize buf_size) {
    const auto* f = ramfs::find(name);
    if (!f || buf == null) return 0;
    vk_usize to_copy = f->size < buf_size ? f->size : buf_size;
    memory::memory_copy(buf, f->data, to_copy);
    return to_copy;
}

/* ---- process ---- */

static void stub_exit(int code) {
    auto* ctx = static_cast<process::process_task_context*>(sched::current_task_user_data());
    if (ctx != null) {
        process::cleanup_process_context(ctx, code);
    } else {
        log::printk("Process exited with code %d\n", code);
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

static vk_u64 stub_tick_count() {
    return sched::tick_count();
}

static void stub_dump_memory() {
    log::info("Physical allocator: total=%zu pages, free=%zu pages, used=%zu pages, total RAM=%zu MB",
              g_phys_alloc.total_pages(),
              g_phys_alloc.free_pages(),
              g_phys_alloc.used_pages(),
              (g_phys_alloc.total_pages() * PAGE_SIZE_4K) / (1024 * 1024));

    memory::dump_heap();
}

static void stub_dump_tasks() {
    sched::dump_tasks();
}

static void stub_dump_idt() {
    arch::dump_idt();
}

static void stub_reboot() {
    arch::reboot();
}

static void stub_wait_task(vk_i64 task_id) {
    if (task_id < 0) return;
    sched::wait_for_task(static_cast<u64>(task_id));
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

/* ---- driver management ---- */

static int stub_drv_load(const char* name) {
    if (name == null) return -1;
    return driver::load(name);
}

static int stub_drv_unload(const char* name) {
    if (name == null) return -1;
    return driver::unload(name);
}

/* ---- mouse ---- */

static auto should_use_framebuffer() -> bool;  /* defined below */

static int stub_poll_mouse(vk_mouse_event_t* out) {
    if (out == null || !should_use_framebuffer()) return 0;
    vk_mouse_event_t ev{};
    if (input::poll_mouse(ev)) {
        *out = ev;
        return 1;
    }
    return 0;
}

static vk_u32 stub_ticks_per_sec() {
    return 100;  /* SCHED_HZ = 100 */
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
    vk_key_event_t ev{};
    if (input::poll_key(ev)) {
        *out = ev;
        return 1;
    }
    return 0;
}

static void route_framebuffer_info(vk_framebuffer_info_t* out) {
    if (out == null) return;
    if (!should_use_framebuffer()) {
        *out = {};
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

static void* stub_memchr(const void* ptr, int ch, vk_usize n) {
    const auto* p = static_cast<const unsigned char*>(ptr);
    const unsigned char value = static_cast<unsigned char>(ch);

    for (vk_usize i = 0; i < n; ++i) {
        if (p[i] == value) {
            return const_cast<unsigned char*>(p + i);
        }
    }

    return null;
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

static void* stub_realloc(void* ptr, vk_usize size) {
    if (ptr == null) {
        return g_kernel_heap.allocate(size);
    }
    if (size == 0) {
        g_kernel_heap.free(ptr);
        return null;
    }

    /* Recover the block size from the heap_block header preceding the data. */
    auto* blk = reinterpret_cast<heap_block*>(
        static_cast<u8*>(ptr) - sizeof(heap_block));
    vk_usize old_size = blk->size;

    if (size <= old_size) {
        return ptr;           /* fits in current block */
    }

    void* new_ptr = g_kernel_heap.allocate(size);
    if (new_ptr == null) return null;

    memory::memory_copy(new_ptr, ptr, old_size);
    g_kernel_heap.free(ptr);
    return new_ptr;
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

static auto parse_mode_flags(const char* mode) -> bool {
    if (mode == null) return false;

    bool saw_read = false;
    bool saw_write = false;
    bool saw_append = false;
    bool saw_plus = false;

    for (const char* p = mode; *p != '\0'; ++p) {
        switch (*p) {
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
    memory::memory_copy(buf, stream->entry->data + stream->position, to_copy);
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

static int stub_file_eof(vk_file_handle_t handle) {
    auto* stream = handle_from_id(handle);
    if (stream == null) return 1;
    return stream->eof ? 1 : 0;
}

static int stub_file_error(vk_file_handle_t handle) {
    auto* stream = handle_from_id(handle);
    if (stream == null) return 1;
    return stream->error ? 1 : 0;
}

static int stub_file_flush(vk_file_handle_t handle) {
    auto* stream = handle_from_id(handle);
    if (stream == null) return -1;
    return 0;
}

static int stub_file_remove(const char* path) {
    (void)path;
    return -1;
}

static int stub_file_rename(const char* old_path, const char* new_path) {
    (void)old_path;
    (void)new_path;
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
    s_api.vk_memchr = stub_memchr;
    s_api.vk_memcmp = stub_memcmp;
    s_api.vk_realloc = stub_realloc;
    /* filesystem */
    s_api.vk_file_exists = stub_file_exists;
    s_api.vk_file_size = stub_file_size;
    s_api.vk_file_read = stub_file_read;
    /* process */
    s_api.vk_exit = stub_exit;
    s_api.vk_yield = stub_yield;
    s_api.vk_sleep = stub_sleep;
    s_api.vk_run = stub_run;
    s_api.vk_tick_count = stub_tick_count;
    s_api.vk_dump_memory = stub_dump_memory;
    s_api.vk_dump_tasks = stub_dump_tasks;
    s_api.vk_dump_idt = stub_dump_idt;
    s_api.vk_reboot = stub_reboot;
    /* framebuffer */
    s_api.vk_framebuffer_info = route_framebuffer_info;
    /* file streams */
    s_api.vk_file_open = stub_file_open;
    s_api.vk_file_close = stub_file_close;
    s_api.vk_file_read_handle = stub_file_read_handle;
    s_api.vk_file_write_handle = stub_file_write_handle;
    s_api.vk_file_seek = stub_file_seek;
    s_api.vk_file_tell = stub_file_tell;
    s_api.vk_file_eof = stub_file_eof;
    s_api.vk_file_error = stub_file_error;
    s_api.vk_file_flush = stub_file_flush;
    s_api.vk_file_remove = stub_file_remove;
    s_api.vk_file_rename = stub_file_rename;
    /* raw keyboard */
    s_api.vk_poll_key = route_poll_key;
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
    /* driver management */
    s_api.vk_drv_load = stub_drv_load;
    s_api.vk_drv_unload = stub_drv_unload;
    /* mouse */
    s_api.vk_poll_mouse = stub_poll_mouse;

    s_api_ready = true;
}

auto get_api() -> const vk_api_t* {
    if (!s_api_ready) init();
    return &s_api;
}

} // namespace kernel_api
} // namespace vk
