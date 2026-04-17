/*
 * vkernel - UEFI Microkernel
 * Copyright (C) 2026 vkernel authors
 *
 * process.cpp - ELF process loader and kernel API table
 *
 * This is the single owner of:
 *   - the global vk_api_t table (kernel → userspace interface)
 *   - the kernel-side stub functions that back each API call
 *   - the load → relocate → execute sequence for ELF binaries
 *
 * The shell (or any other caller) simply invokes process::run().
 */

#include "config.h"
#include "types.h"
#include "console.h"
#include "memory.h"
#include "fs.h"
#include "input.h"
#include "scheduler.h"
#include "elf.h"
#include "pe.h"
#include "process.h"
#include "userapi.h"

namespace vk {
namespace process {

extern vk_api_t s_api;
extern bool     s_api_ready;

struct process_task_context {
    u64 entry;
    u8* image_base;
    usize image_size;
};

static void cleanup_process_context(process_task_context* ctx, int exit_code) {
    console::puts("Process exited with code ");
    console::put_dec(static_cast<u64>(static_cast<u32>(exit_code)));
    console::puts("\n");

    g_kernel_heap.free(ctx->image_base);
    g_kernel_heap.free(ctx);
}

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
    auto* ctx = static_cast<process_task_context*>(sched::current_task_user_data());
    if (ctx != null) {
        cleanup_process_context(ctx, code);
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

static void process_task_main(void* user_data) {
    auto* ctx = static_cast<process_task_context*>(user_data);
    using entry_fn = int (*)(const vk_api_t*);
    auto entry = reinterpret_cast<entry_fn>(ctx->entry);

    int ret = entry(&s_api);

    cleanup_process_context(ctx, ret);
}

/* ============================================================
 * Global kernel API table
 * Populated once by init(); never changes at runtime.
 * ============================================================ */

vk_api_t s_api;
bool     s_api_ready = false;

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

/* ============================================================
 * run()
 * ============================================================ */

auto run(const char* filename) -> i64 {
    /* Look up the file in ramfs */
    const file_entry* f = ramfs::find(filename);
    if (f == null) {
        console::puts("process: file not found: ");
        console::puts(filename);
        console::puts("\n");
        return -1;
    }

    console::puts("Loading binary: ");
    console::puts(filename);
    console::puts(" (");
    console::put_dec(f->size);
    console::puts(" bytes)\n");

    const u8*  data = f->data;
    const usize sz  = f->size;

    u64   entry_addr = 0;
    u8*   image_base = null;
    u64   image_size = 0;

    /* Detect format by magic bytes:
     *   ELF  →  7F 45 4C 46  (\x7FELF)
     *   PE   →  4D 5A        (MZ)       */
    const bool is_elf = sz >= 4 &&
        data[0] == 0x7Fu && data[1] == 'E' &&
        data[2] == 'L'   && data[3] == 'F';
    const bool is_pe = sz >= 2 &&
        data[0] == 'M' && data[1] == 'Z';

    if (is_elf) {
        auto result = elf::load(data, sz);
        if (result.error != elf::elf_error::ok) {
            console::puts("process: ELF load failed: ");
            console::puts(elf::error_string(result.error));
            console::puts("\n");
            return -1;
        }
        entry_addr = result.entry;
        image_base = result.image_base;
        image_size = result.image_size;
    } else if (is_pe) {
        auto result = pe::load(data, sz);
        if (result.error != pe::pe_error::ok) {
            console::puts("process: PE load failed: ");
            console::puts(pe::error_string(result.error));
            console::puts("\n");
            return -1;
        }
        entry_addr = result.entry;
        image_base = result.image_base;
        image_size = result.image_size;
    } else {
        console::puts("process: unknown binary format (not ELF or PE)\n");
        return -1;
    }

    console::puts("Executing at 0x");
    console::put_hex(entry_addr);
    console::puts("\n");

    /* Ensure the API table is ready */
    if (!s_api_ready) init();

    auto* ctx = static_cast<process_task_context*>(g_kernel_heap.allocate(sizeof(process_task_context)));
    if (ctx == null) {
        console::puts("process: out of memory while creating task context\n");
        g_kernel_heap.free(image_base);
        return -1;
    }

    ctx->entry      = entry_addr;
    ctx->image_base = image_base;
    ctx->image_size = image_size;

    i64 task_id = sched::create_task(filename, process_task_main, ctx);
    if (task_id < 0) {
        console::puts("process: failed to create task\n");
        g_kernel_heap.free(ctx);
        g_kernel_heap.free(image_base);
        return -1;
    }

    console::puts("Spawned task id ");
    console::put_dec(static_cast<u64>(task_id));
    console::puts(" for ");
    console::puts(filename);
    console::puts("\n");

    return task_id;
}

} // namespace process
} // namespace vk
