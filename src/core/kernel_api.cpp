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
        console::puts("Process exited with code ");
        console::put_dec(static_cast<u64>(static_cast<u32>(code)));
        console::puts("\n");
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
    console::puts("Physical allocator:\n");
    console::puts("  Total pages: ");
    console::put_dec(g_phys_alloc.total_pages());
    console::puts("\n  Free pages:  ");
    console::put_dec(g_phys_alloc.free_pages());
    console::puts("\n  Used pages:  ");
    console::put_dec(g_phys_alloc.used_pages());
    console::puts("\n  Total RAM:   ");
    console::put_dec((g_phys_alloc.total_pages() * PAGE_SIZE_4K) / (1024 * 1024));
    console::puts(" MB\n\n");

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

static void stub_framebuffer_info(vk_framebuffer_info_t* out) {
    if (out == null) return;

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

    /* The kernel heap does not expose block sizes yet. */
    return null;
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
    s_api.vk_puts = console::puts;
    s_api.vk_putc = console::putc;
    s_api.vk_put_hex = console::put_hex;
    s_api.vk_put_dec = console::put_dec;
    s_api.vk_clear = console::clear;
    /* console input */
    s_api.vk_getc = input::getc;
    s_api.vk_try_getc = input::try_getc;
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
    s_api.vk_framebuffer_info = stub_framebuffer_info;
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

    s_api_ready = true;
}

auto get_api() -> const vk_api_t* {
    if (!s_api_ready) init();
    return &s_api;
}

} // namespace kernel_api
} // namespace vk
