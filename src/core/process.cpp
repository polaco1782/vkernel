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
#include "process.h"
#include "userapi.h"

namespace vk {
namespace process {

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

static void stub_exit(int) {
    /* Full process termination requires a task-switch mechanism.
     * For now the return value from _start() serves as the exit code;
     * this stub is a no-op placeholder. */
}

static void stub_yield() {
    sched::yield();
}

/* ============================================================
 * Global kernel API table
 * Populated once by init(); never changes at runtime.
 * ============================================================ */

static vk_api_t s_api;
static bool     s_api_ready = false;

void init() {
    if (s_api_ready) return;

    s_api = {
        .api_version  = VK_API_VERSION,
        /* console output */
        .puts         = console::puts,
        .putc         = console::putc,
        .put_hex      = console::put_hex,
        .put_dec      = console::put_dec,
        .clear        = console::clear,
        /* console input */
        .getc         = input::getc,
        .try_getc     = input::try_getc,
        /* memory */
        .malloc       = stub_malloc,
        .free         = stub_free,
        .memset       = stub_memset,
        .memcpy       = stub_memcpy,
        /* filesystem */
        .file_exists  = stub_file_exists,
        .file_size    = stub_file_size,
        .file_read    = stub_file_read,
        /* process */
        .exit         = stub_exit,
        .yield        = stub_yield,
    };

    s_api_ready = true;
}

auto get_api() -> const vk_api_t* {
    if (!s_api_ready) init();
    return &s_api;
}

/* ============================================================
 * run()
 * ============================================================ */

auto run(const char* filename) -> int {
    /* Look up the file in ramfs */
    const file_entry* f = ramfs::find(filename);
    if (f == null) {
        console::puts("process: file not found: ");
        console::puts(filename);
        console::puts("\n");
        return -1;
    }

    console::puts("Loading ELF: ");
    console::puts(filename);
    console::puts(" (");
    console::put_dec(f->size);
    console::puts(" bytes)\n");

    /* Load and relocate the ELF binary */
    auto result = elf::load(f->data, f->size);
    if (result.error != elf::elf_error::ok) {
        console::puts("process: ELF load failed: ");
        console::puts(elf::error_string(result.error));
        console::puts("\n");
        return -1;
    }

    console::puts("Executing at 0x");
    console::put_hex(result.entry);
    console::puts("\n");

    /* Ensure the API table is ready */
    if (!s_api_ready) init();

    /* Call the entry point: int _start(const vk_api_t*) */
    using entry_fn = int (*)(const vk_api_t*);
    auto entry = reinterpret_cast<entry_fn>(result.entry);
    int ret = entry(&s_api);

    console::puts("Process exited with code ");
    console::put_dec(static_cast<u64>(static_cast<u32>(ret)));
    console::puts("\n");

    /* Release the loaded image back to the heap */
    g_kernel_heap.free(result.image_base);

    return ret;
}

} // namespace process
} // namespace vk
