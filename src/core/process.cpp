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
    log::printk("Process exited with code %d\n", exit_code);
    log::debug("Cleaning up process context: entry=%#llx, image_base=%p, image_size=%#llx",
               static_cast<unsigned long long>(ctx->entry),
               ctx->image_base,
               static_cast<unsigned long long>(ctx->image_size));

    if (ctx->image_from_phys) {
        u32 page_count = static_cast<u32>(
            (ctx->image_size + PAGE_SIZE_4K - 1) / PAGE_SIZE_4K);
        g_phys_alloc.free_pages(
            reinterpret_cast<phys_addr>(ctx->image_base), page_count);
    } else {
        g_kernel_heap.free(ctx->image_base);
    }
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
        log::warn("process: file not found: %s", filename);
        return -1;
    }

    log::info("Loading binary: %s (%zu bytes)", filename, f->size);

    const u8*  data = f->data;
    const usize sz  = f->size;

    u64   entry_addr    = 0;
    u8*   image_base    = null;
    u64   image_size    = 0;
    bool  image_from_phys = false;

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
            log::error("process: ELF load failed: %s", elf::error_string(result.error));
            return -1;
        }
        entry_addr      = result.entry;
        image_base      = result.image_base;
        image_size      = result.image_size;
        image_from_phys = result.image_from_phys;
    } else if (is_pe) {
        auto result = pe::load(data, sz);
        if (result.error != pe::pe_error::ok) {
            log::error("process: PE load failed: %s", pe::error_string(result.error));
            return -1;
        }
        entry_addr = result.entry;
        image_base = result.image_base;
        image_size = result.image_size;
    } else {
        log::error("process: unknown binary format (not ELF or PE)");
        return -1;
    }

    log::info("Executing at %#llx", static_cast<unsigned long long>(entry_addr));

    /* Ensure the API table is ready */
    kernel_api::init();

    auto* ctx = static_cast<process_task_context*>(g_kernel_heap.allocate(sizeof(process_task_context)));
    if (ctx == null) {
        log::error("process: out of memory while creating task context");
        if (image_from_phys) {
            u32 pc = static_cast<u32>(
                (image_size + PAGE_SIZE_4K - 1) / PAGE_SIZE_4K);
            g_phys_alloc.free_pages(
                reinterpret_cast<phys_addr>(image_base), pc);
        } else {
            g_kernel_heap.free(image_base);
        }
        return -1;
    }

    ctx->entry           = entry_addr;
    ctx->image_base      = image_base;
    ctx->image_size      = image_size;
    ctx->image_from_phys = image_from_phys;

	// create a new task and pass the context as user data
    i64 task_id = sched::create_task(filename, process_task_main, ctx);
    if (task_id < 0) {
        log::error("process: failed to create task");
        g_kernel_heap.free(ctx);
        if (image_from_phys) {
            u32 pc = static_cast<u32>(
                (image_size + PAGE_SIZE_4K - 1) / PAGE_SIZE_4K);
            g_phys_alloc.free_pages(
                reinterpret_cast<phys_addr>(image_base), pc);
        } else {
            g_kernel_heap.free(image_base);
        }
        return -1;
    }

    log::info("Spawned task id %llu for %s",
              static_cast<unsigned long long>(task_id), filename);

    return task_id;
}

} // namespace process
} // namespace vk
