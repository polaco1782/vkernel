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
#include "log.h"
#include "memory.h"
#include "fs.h"
#include "input.h"
#include "scheduler.h"
#include "sound.h"
#include "elf.h"
#include "pe.h"
#include "process.h"
#include "vk.h"
#include "process_internal.h"
#include "resource_ptr.h"
#include "gcc_asm.h"

namespace vk {
namespace process {

namespace {
using process_image_ptr = kernel_allocation_ptr<u8>;
using process_context_ptr = kernel_heap_ptr<process_task_context>;

} // namespace

static auto current_console_interface() -> console_interface {
    auto* ctx = static_cast<process_task_context*>(sched::current_task_user_data());
    if (ctx != null) {
        return ctx->interface;
    }
    return console_interface::graphical;
}

void cleanup_process_context(process_task_context* ctx, int exit_code) {
    log::printk() << "Process exited with code " << exit_code << "\n";
    log::debug() << "Cleaning up process context: entry=" << log::hex(static_cast<u64>(static_cast<unsigned long long>(ctx->entry)), 1, true, false) << ", image_base=" << reinterpret_cast<const void*>(ctx->image_base) << ", image_size=" << log::hex(static_cast<u64>(static_cast<unsigned long long>(ctx->image_size)), 1, true, false);

    sound::mix_stop_range(ctx->image_base, ctx->image_size);
    for (auto* alloc = ctx->allocations; alloc != null; alloc = alloc->next) {
        sound::mix_stop_range(alloc->user_ptr, alloc->allocated_size);
    }

    auto* alloc = ctx->allocations;
    while (alloc != null) {
        auto* next = alloc->next;
        if (alloc->from_phys) {
            u32 page_count = static_cast<u32>(
                (alloc->allocated_size + PAGE_SIZE_4K - 1) / PAGE_SIZE_4K);
            g_phys_alloc.free_pages(
                reinterpret_cast<phys_addr>(alloc->raw_ptr), page_count);
        } else {
            g_kernel_heap.free(alloc->raw_ptr);
        }
        g_kernel_heap.free(alloc);
        alloc = next;
    }
    ctx->allocations = null;

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

    int ret = asm_call_process_entry(ctx->entry, kernel_api::get_api());

    cleanup_process_context(ctx, ret);
}

/* ============================================================
 * run()
 * ============================================================ */

auto run(string_view filename) -> i64 {
    return run(filename, current_console_interface());
}

auto run(const char* filename) -> i64 {
    return run(string_view(filename), current_console_interface());
}

auto run(string_view filename, console_interface interface) -> i64 {
    return run(filename, interface, null);
}

auto run(string_view filename, console_interface interface, const vk_framebuffer_info_t* fb_override) -> i64 {
    /* Look up the file in ramfs */
    const file_entry* f = ramfs::find(filename);
    if (f == null) {
        static_string<128> filename_buf(filename);
        log::warn() << "process: file not found: " << filename_buf.c_str();
        return -1;
    }

    log::info() << "Loading binary: " << f->name.c_str() << " (" << f->size << " bytes)";

    const u8*  data = f->data;
    const usize sz  = f->size;

    u64   entry_addr      = 0;
    u8*   image_base_raw  = null;
    u64   image_size      = 0;
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
            log::error() << "process: ELF load failed: " << elf::error_string(result.error);
            return -1;
        }
        entry_addr      = result.entry;
        image_base_raw  = result.image_base;
        image_size      = result.image_size;
        image_from_phys = result.image_from_phys;
    } else if (is_pe) {
        auto result = pe::load(data, sz);
        if (result.error != pe::pe_error::ok) {
            log::error() << "process: PE load failed: " << pe::error_string(result.error);
            return -1;
        }
        entry_addr = result.entry;
        image_base_raw = result.image_base;
        image_size = result.image_size;
    } else {
        log::error() << "process: unknown binary format (not ELF or PE)";
        return -1;
    }

    process_image_ptr image_base(
        image_base_raw,
        kernel_allocation_deleter {
            .size = static_cast<usize>(image_size),
            .from_phys = image_from_phys,
        });

    log::info() << "Executing at " << log::hex(static_cast<u64>(static_cast<unsigned long long>(entry_addr)), 1, true, false);

    /* Ensure the API table is ready */
    kernel_api::init();

    process_context_ptr ctx(
        static_cast<process_task_context*>(g_kernel_heap.allocate(sizeof(process_task_context))));
    if (!ctx) {
        log::error() << "process: out of memory while creating task context";
        return -1;
    }

    ctx->entry           = entry_addr;
    ctx->image_base      = image_base.get();
    ctx->image_size      = image_size;
    ctx->image_from_phys = image_from_phys;
    ctx->interface       = interface;
    ctx->key_q_head      = 0;
    ctx->key_q_tail      = 0;
    ctx->mouse_q_head    = 0;
    ctx->mouse_q_tail    = 0;
    ctx->fb_override     = fb_override ? *fb_override : vk_framebuffer_info_t{};
    ctx->fb_override_valid = fb_override != null
        && fb_override->valid != 0u
        && fb_override->base != 0u
        && fb_override->width > 0u
        && fb_override->height > 0u;
    ctx->allocations = null;

	// create a new task and pass the context as user data
    i64 task_id = sched::create_task(filename, process_task_main, ctx.get());
    if (task_id < 0) {
        log::error() << "process: failed to create task";
        return -1;
    }

    (void)ctx.release();
    (void)image_base.release();

    log::info() << "Spawned task id " << static_cast<unsigned long long>(task_id) << " for " << f->name.c_str();

    return task_id;
}

auto run(const char* filename, console_interface interface) -> i64 {
    return run(string_view(filename), interface);
}

auto run(const char* filename, console_interface interface, const vk_framebuffer_info_t* fb_override) -> i64 {
    return run(string_view(filename), interface, fb_override);
}

} // namespace process
} // namespace vk
