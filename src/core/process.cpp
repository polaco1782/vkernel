/*
 * vkernel - UEFI Microkernel
 * Copyright (C) 2026 vkernel authors
 *
 * process.cpp - ELF process loader
 *
 * This is the single owner of the load → relocate → execute
 * sequence for process binaries.
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
#include "vk.h"
#include "process_internal.h"

namespace vk {
namespace process {

void cleanup_process_context(process_task_context* ctx, int exit_code) {
    console::puts("Process exited with code ");
    console::put_dec(static_cast<u64>(static_cast<u32>(exit_code)));
    console::puts("\n");

    #if VK_DEBUG_LEVEL >= 4
    console::puts("[DEBUG] Cleaning up process context: entry=0x");
    console::put_hex(ctx->entry);
    console::puts(", image_base=0x");
    console::put_hex(reinterpret_cast<u64>(ctx->image_base));
    console::puts(", image_size=0x");
    console::put_hex(ctx->image_size);
    console::puts("\n");
    #endif

    g_kernel_heap.free(ctx->image_base);
    g_kernel_heap.free(ctx);
}

static void process_task_main(void* user_data) {
    auto* ctx = static_cast<process_task_context*>(user_data);
    using entry_fn = int (*)(const vk_api_t*);
    auto entry = reinterpret_cast<entry_fn>(ctx->entry);

    int ret = entry(kernel_api::get_api());

    cleanup_process_context(ctx, ret);
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
    kernel_api::init();

    auto* ctx = static_cast<process_task_context*>(g_kernel_heap.allocate(sizeof(process_task_context)));
    if (ctx == null) {
        console::puts("process: out of memory while creating task context\n");
        g_kernel_heap.free(image_base);
        return -1;
    }

    ctx->entry      = entry_addr;
    ctx->image_base = image_base;
    ctx->image_size = image_size;

	// create a new task and pass the context as user data
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
