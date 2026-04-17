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
    s_api.puts = console::puts;
    s_api.putc = console::putc;
    s_api.put_hex = console::put_hex;
    s_api.put_dec = console::put_dec;
    s_api.clear = console::clear;
    /* console input */
    s_api.getc = input::getc;
    s_api.try_getc = input::try_getc;
    /* memory */
    s_api.malloc = stub_malloc;
    s_api.free = stub_free;
    s_api.memset = stub_memset;
    s_api.memcpy = stub_memcpy;
    /* filesystem */
    s_api.file_exists = stub_file_exists;
    s_api.file_size = stub_file_size;
    s_api.file_read = stub_file_read;
    /* process */
    s_api.exit = stub_exit;
    s_api.yield = stub_yield;
    s_api.sleep = stub_sleep;
    /* framebuffer */
    s_api.framebuffer_info = stub_framebuffer_info;

    s_api_ready = true;
}

auto get_api() -> const vk_api_t* {
    if (!s_api_ready) init();
    return &s_api;
}

} // namespace kernel_api
} // namespace vk
