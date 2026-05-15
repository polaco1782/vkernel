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
#include "framebuffer.h"
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
#include "virtual_memory.h"
#include "arch/x86_64/arch.h"

namespace vk {
namespace process {

namespace {
using process_image_ptr = kernel_allocation_ptr<u8>;
using process_context_ptr = kernel_heap_ptr<process_task_context>;
struct address_space_deleter {
    void operator()(vm::address_space* as) const noexcept {
        vm::destroy_address_space(as);
    }
};
using address_space_ptr = unique_ptr<vm::address_space, address_space_deleter>;

struct loaded_process {
    process_context_ptr ctx {};
    process_image_ptr image_base_owner {};
    address_space_ptr owned_address_space {};
};

static auto is_ascii_space(char ch) -> bool {
    return ch == ' ' || ch == '\t' || ch == '\r' || ch == '\n';
}

static auto parse_program_path(string_view command_line, static_string<128>& out_path) -> bool {
    usize i = 0;
    while (i < command_line.size() && is_ascii_space(command_line[i])) {
        ++i;
    }
    if (i >= command_line.size()) {
        return false;
    }

    char quote = '\0';
    if (command_line[i] == '"' || command_line[i] == '\'') {
        quote = command_line[i++];
    }

    usize start = i;
    while (i < command_line.size()) {
        char ch = command_line[i];
        if (quote != '\0') {
            if (ch == quote) {
                break;
            }
        } else if (is_ascii_space(ch)) {
            break;
        }
        ++i;
    }

    if (i <= start) {
        return false;
    }

    return out_path.assign(string_view(command_line.data() + start, i - start));
}

static auto remap_framebuffer_override(vm::address_space* child_as,
                                       const vk_framebuffer_info_t* input,
                                       vk_framebuffer_info_t& output,
                                       bool& output_valid) -> bool {
    output = input != null ? *input : vk_framebuffer_info_t{};
    output_valid = false;

    usize fb_bytes = 0;
    if (input == null || !framebuffer::byte_size(*input, fb_bytes)) {
        return true;
    }

    const virt_addr source_base = static_cast<virt_addr>(input->base);
    if (source_base < vm::USER_IMAGE_BASE || source_base >= vm::USER_MAP_LIMIT) {
        output_valid = true;
        return true;
    }

    auto* source_ctx = static_cast<process_task_context*>(sched::current_task_user_data());
    if (source_ctx == null || source_ctx->address_space == null || child_as == null) {
        log::warn() << "vmtrace: framebuffer override has user address but no source address space";
        output = {};
        return false;
    }

    const virt_addr source_start = align_down(source_base, PAGE_SIZE_4K);
    const usize source_offset = static_cast<usize>(source_base - source_start);
    const usize mapped_bytes = align_up(source_offset + fb_bytes, PAGE_SIZE_4K);
    const virt_addr target_start = vm::USER_SHARED_BASE;
    const virt_addr target_base = target_start + source_offset;

    for (usize offset = 0; offset < mapped_bytes; offset += PAGE_SIZE_4K) {
        phys_addr source_phys = 0;
        u64 source_flags = 0;
        const virt_addr source_page = source_start + offset;
        if (!vm::debug_resolve(source_ctx->address_space, source_page, &source_phys, &source_flags)) {
            log::warn() << "vmtrace: framebuffer override source resolve failed src="
                        << reinterpret_cast<const void*>(static_cast<usize>(source_page));
            output = {};
            return false;
        }
    }

    for (usize offset = 0; offset < mapped_bytes; offset += PAGE_SIZE_4K) {
        phys_addr source_phys = 0;
        u64 source_flags = 0;
        const virt_addr source_page = source_start + offset;
        if (!vm::debug_resolve(source_ctx->address_space, source_page, &source_phys, &source_flags)) {
            vm::unmap_range(child_as, target_start, mapped_bytes);
            log::warn() << "vmtrace: framebuffer override source resolve failed src="
                        << reinterpret_cast<const void*>(static_cast<usize>(source_page));
            output = {};
            return false;
        }

        if (!vm::map_page(child_as,
                          target_start + offset,
                          align_down(source_phys, PAGE_SIZE_4K),
                          vm::MAP_WRITABLE | vm::MAP_USER)) {
            vm::unmap_range(child_as, target_start, mapped_bytes);
            log::warn() << "vmtrace: framebuffer override map failed dst="
                        << reinterpret_cast<const void*>(static_cast<usize>(target_start + offset))
                        << " phys=" << reinterpret_cast<const void*>(static_cast<usize>(source_phys));
            output = {};
            return false;
        }
    }

    output.base = static_cast<vk_u64>(target_base);
    output_valid = true;
    return true;
}

static void copy_command_line(process_task_context* ctx,
                              string_view command_line,
                              string_view fallback_program) {
    string_view source = command_line.empty() ? fallback_program : command_line;

    usize len = source.size();
    if (len >= process_task_context::COMMAND_LINE_CAP) {
        len = process_task_context::COMMAND_LINE_CAP - 1;
    }

    if (len > 0) {
        memory::copy(ctx->command_line, source.data(), len);
    }
    ctx->command_line[len] = '\0';
    ctx->command_line_len = len;
}

static void discard_loaded_process(loaded_process& loaded) {
    auto* ctx = loaded.ctx.get();
    if (ctx != null && ctx->image_vm_mapped && ctx->image_phys != 0 && ctx->address_space != null) {
        vm::unmap_range(ctx->address_space,
                        reinterpret_cast<virt_addr>(ctx->image_base),
                        ctx->image_size);
        g_phys_alloc.free_pages(ctx->image_phys,
                                static_cast<u32>((ctx->image_size + PAGE_SIZE_4K - 1) / PAGE_SIZE_4K));
    }

    loaded.ctx.reset();
    loaded.image_base_owner.reset();
    loaded.owned_address_space.reset();
}

static auto load_process_context(string_view filename,
                                 string_view command_line,
                                 console_interface interface,
                                 const vk_framebuffer_info_t* fb_override,
                                 loaded_process& loaded) -> bool {
    kernel_heap_ptr<u8> owned_file;
    usize file_size = 0;
    const u8* data = fs::load_file(filename, owned_file, file_size);
    if (data == null) {
        static_string<128> filename_buf(filename);
        log::warn() << "process: file not found: " << filename_buf.c_str();
        return false;
    }

    static_string<128> filename_buf(filename);
    log::info() << "Loading binary: " << filename_buf.c_str() << " (" << file_size << " bytes)";

    const usize sz = file_size;

    u64   entry_addr      = 0;
    u8*   image_base_raw  = null;
    u64   image_size      = 0;
    bool  image_from_phys = false;
    phys_addr image_phys  = 0;
    bool  image_vm_mapped = false;

    auto* address_space = vm::create_address_space();
    if (address_space == null) {
        log::error() << "process: failed to create address space";
        return false;
    }
    address_space_ptr owned_address_space(address_space);

    const bool is_elf = sz >= 4 &&
        data[0] == 0x7Fu && data[1] == 'E' &&
        data[2] == 'L'   && data[3] == 'F';
    const bool is_pe = sz >= 2 &&
        data[0] == 'M' && data[1] == 'Z';

    if (is_elf) {
        auto result = elf::load_into_address_space(data, sz, address_space, vm::USER_IMAGE_BASE);
        if (result.error != elf::elf_error::ok) {
            log::error() << "process: ELF load failed: " << elf::error_string(result.error);
            return false;
        }
        entry_addr      = result.entry;
        image_base_raw  = result.image_base;
        image_size      = result.image_size;
        image_from_phys = result.image_from_phys;
        image_phys      = result.image_phys;
        image_vm_mapped = result.image_vm_mapped;
    } else if (is_pe) {
        auto result = pe::load(data, sz);
        if (result.error != pe::pe_error::ok) {
            log::error() << "process: PE load failed: " << pe::error_string(result.error);
            return false;
        }
        entry_addr = result.entry;
        image_base_raw = result.image_base;
        image_size = result.image_size;
    } else {
        log::error() << "process: unknown binary format (not ELF or PE)";
        return false;
    }

    process_image_ptr image_base_owner;
    if (!image_vm_mapped) {
        image_base_owner = process_image_ptr(
            image_base_raw,
            kernel_allocation_deleter {
                .size = static_cast<usize>(image_size),
                .from_phys = image_from_phys,
            });
    }

    log::info() << "Executing at " << log::hex(static_cast<u64>(static_cast<unsigned long long>(entry_addr)), 1, true, false);
    phys_addr entry_phys = 0;
    u64 entry_flags = 0;
    const bool entry_mapped = vm::debug_resolve(address_space, entry_addr, &entry_phys, &entry_flags);
    log::debug() << "vmtrace: process image filename=" << filename_buf.c_str()
                 << " as=" << reinterpret_cast<const void*>(address_space)
                 << " pml4=" << reinterpret_cast<const void*>(static_cast<usize>(address_space->pml4_phys))
                 << " image_base=" << reinterpret_cast<const void*>(image_base_raw)
                 << " image_phys=" << reinterpret_cast<const void*>(static_cast<usize>(image_phys))
                 << " image_size=" << static_cast<unsigned long long>(image_size)
                 << " entry_mapped=" << (entry_mapped ? "yes" : "no")
                 << " entry_phys=" << reinterpret_cast<const void*>(static_cast<usize>(entry_phys))
                 << " entry_flags=" << log::hex(entry_flags, 1, true, false);

    kernel_api::init();

    process_context_ptr ctx(
        static_cast<process_task_context*>(g_kernel_heap.allocate(sizeof(process_task_context))));
    if (!ctx) {
        if (image_vm_mapped && image_phys != 0) {
            g_phys_alloc.free_pages(image_phys,
                                    static_cast<u32>((image_size + PAGE_SIZE_4K - 1) / PAGE_SIZE_4K));
        }
        log::error() << "process: out of memory while creating task context";
        return false;
    }

    ctx->entry           = entry_addr;
    ctx->image_base      = image_base_raw;
    ctx->image_size      = image_size;
    ctx->image_from_phys = image_from_phys;
    ctx->address_space   = owned_address_space.get();
    ctx->image_phys      = image_phys;
    ctx->image_vm_mapped = image_vm_mapped;
    ctx->interface       = interface;
    ctx->key_q_head      = 0;
    ctx->key_q_tail      = 0;
    ctx->mouse_q_head    = 0;
    ctx->mouse_q_tail    = 0;
    copy_command_line(ctx.get(), command_line, filename);
    if (!remap_framebuffer_override(ctx->address_space, fb_override, ctx->fb_override, ctx->fb_override_valid)) {
        if (image_vm_mapped && image_phys != 0) {
            g_phys_alloc.free_pages(image_phys,
                                    static_cast<u32>((image_size + PAGE_SIZE_4K - 1) / PAGE_SIZE_4K));
        }
        log::error() << "process: failed to map framebuffer override";
        return false;
    }
    ctx->stdio_to_serial = ctx->fb_override_valid;
    ctx->task_id = 0;
    ctx->fb_text_col = 0;
    ctx->fb_text_row = 0;
    ctx->allocations = null;

    loaded.ctx = move(ctx);
    loaded.image_base_owner = move(image_base_owner);
    loaded.owned_address_space = move(owned_address_space);
    return true;
}

} // namespace

static auto run_impl(string_view filename,
                     string_view command_line,
                     console_interface interface,
                     const vk_framebuffer_info_t* fb_override) -> i64;
static auto exec_impl(string_view filename,
                      string_view command_line,
                      console_interface interface,
                      const vk_framebuffer_info_t* fb_override) -> int;

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

    const phys_addr previous_cr3 = vm::active_cr3();
    const phys_addr cleaned_cr3 = ctx->address_space != null ? ctx->address_space->pml4_phys : 0;
    const bool cleaning_current_task = sched::current_task_id() == ctx->task_id;
    const bool restore_caller_cr3 = !cleaning_current_task
        && previous_cr3 != 0
        && previous_cr3 != cleaned_cr3;

    /* The task may be exiting while its private CR3 is active.  Switch back
     * to the kernel template before unmapping the address space that contains
     * the just-returned process image.  If another task is requesting this
     * cleanup, restore its CR3 before returning to that caller. */
    vm::activate_kernel();

    fs::close_all_for_task(ctx->task_id);

    sound::mix_stop_range(ctx->image_base, ctx->image_size);
    for (auto* alloc = ctx->allocations; alloc != null; alloc = alloc->next) {
        sound::mix_stop_range(alloc->user_ptr, alloc->allocated_size);
    }

    auto* alloc = ctx->allocations;
    while (alloc != null) {
        auto* next = alloc->next;
        if (ctx->address_space != null && alloc->raw_ptr != alloc->user_ptr) {
            vm::unmap_range(ctx->address_space,
                            reinterpret_cast<virt_addr>(alloc->user_ptr),
                            alloc->allocated_size);
            u32 page_count = static_cast<u32>(
                (alloc->allocated_size + PAGE_SIZE_4K - 1) / PAGE_SIZE_4K);
            g_phys_alloc.free_pages(
                reinterpret_cast<phys_addr>(alloc->raw_ptr), page_count);
        } else if (alloc->from_phys) {
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

    if (ctx->image_vm_mapped && ctx->address_space != null) {
        vm::unmap_range(ctx->address_space,
                        reinterpret_cast<virt_addr>(ctx->image_base),
                        ctx->image_size);
        u32 page_count = static_cast<u32>(
            (ctx->image_size + PAGE_SIZE_4K - 1) / PAGE_SIZE_4K);
        g_phys_alloc.free_pages(ctx->image_phys, page_count);
    } else if (ctx->image_from_phys) {
        u32 page_count = static_cast<u32>(
            (ctx->image_size + PAGE_SIZE_4K - 1) / PAGE_SIZE_4K);
        g_phys_alloc.free_pages(
            reinterpret_cast<phys_addr>(ctx->image_base), page_count);
    } else {
        g_kernel_heap.free(ctx->image_base);
    }
    vm::destroy_address_space(ctx->address_space);
    g_kernel_heap.free(ctx);

    if (restore_caller_cr3 && previous_cr3 != vm::active_cr3()) {
        arch::write_cr3(previous_cr3);
    }
}

void process_task_main(void* user_data) {
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
    return run_impl(filename, filename, interface, fb_override);
}

auto exec(string_view filename) -> int {
    return exec(filename, current_console_interface());
}

auto exec(const char* filename) -> int {
    return exec(string_view(filename), current_console_interface());
}

auto exec(string_view filename, console_interface interface) -> int {
    return exec(filename, interface, null);
}

auto exec(string_view filename, console_interface interface, const vk_framebuffer_info_t* fb_override) -> int {
    return exec_impl(filename, filename, interface, fb_override);
}

auto exec_command_line(string_view command_line) -> int {
    return exec_command_line(command_line, current_console_interface());
}

auto exec_command_line(const char* command_line) -> int {
    return exec_command_line(string_view(command_line), current_console_interface());
}

auto exec_command_line(string_view command_line, console_interface interface) -> int {
    return exec_command_line(command_line, interface, null);
}

auto exec_command_line(const char* command_line, console_interface interface) -> int {
    return exec_command_line(string_view(command_line), interface, null);
}

auto exec_command_line(string_view command_line,
                       console_interface interface,
                       const vk_framebuffer_info_t* fb_override) -> int {
    static_string<128> program;
    if (!parse_program_path(command_line, program)) {
        log::warn() << "process: empty command line";
        return -1;
    }

    return exec_impl(program.view(), command_line, interface, fb_override);
}

auto exec_command_line(const char* command_line,
                       console_interface interface,
                       const vk_framebuffer_info_t* fb_override) -> int {
    return exec_command_line(string_view(command_line), interface, fb_override);
}

auto run_command_line(string_view command_line) -> i64 {
    return run_command_line(command_line, current_console_interface());
}

auto run_command_line(const char* command_line) -> i64 {
    return run_command_line(string_view(command_line), current_console_interface());
}

auto run_command_line(string_view command_line, console_interface interface) -> i64 {
    return run_command_line(command_line, interface, null);
}

auto run_command_line(const char* command_line, console_interface interface) -> i64 {
    return run_command_line(string_view(command_line), interface, null);
}

auto run_command_line(string_view command_line,
                      console_interface interface,
                      const vk_framebuffer_info_t* fb_override) -> i64 {
    static_string<128> program;
    if (!parse_program_path(command_line, program)) {
        log::warn() << "process: empty command line";
        return -1;
    }

    return run_impl(program.view(), command_line, interface, fb_override);
}

auto run_command_line(const char* command_line,
                      console_interface interface,
                      const vk_framebuffer_info_t* fb_override) -> i64 {
    return run_command_line(string_view(command_line), interface, fb_override);
}

static auto run_impl(string_view filename,
                     string_view command_line,
                     console_interface interface,
                     const vk_framebuffer_info_t* fb_override) -> i64 {
    loaded_process loaded;
    static_string<128> filename_buf(filename);
    if (!load_process_context(filename, command_line, interface, fb_override, loaded)) {
        return -1;
    }

    i64 task_id = sched::create_task(filename, process_task_main, loaded.ctx.get());
    if (task_id < 0) {
        discard_loaded_process(loaded);
        log::error() << "process: failed to create task";
        return -1;
    }

    (void)loaded.ctx.release();
    (void)loaded.image_base_owner.release();
    (void)loaded.owned_address_space.release();

    log::info() << "Spawned task id " << static_cast<unsigned long long>(task_id) << " for " << filename_buf.c_str();

    return task_id;
}

static auto exec_impl(string_view filename,
                      string_view command_line,
                      console_interface interface,
                      const vk_framebuffer_info_t* fb_override) -> int {
    loaded_process loaded;
    static_string<128> filename_buf(filename);
    if (!load_process_context(filename, command_line, interface, fb_override, loaded)) {
        return -1;
    }

    auto* current_ctx = static_cast<process_task_context*>(sched::current_task_user_data());
    const u64 current_task_id = sched::current_task_id();
    if (current_ctx == null || current_task_id == 0) {
        discard_loaded_process(loaded);
        log::error() << "process: exec requires a running process task";
        return -1;
    }

    auto* next_ctx = loaded.ctx.get();
    next_ctx->task_id = current_task_id;
    if (!sched::replace_current_task_context(filename, next_ctx)) {
        discard_loaded_process(loaded);
        log::error() << "process: failed to replace current task context";
        return -1;
    }

    log::info() << "Replacing task id " << static_cast<unsigned long long>(current_task_id)
                << " with " << filename_buf.c_str();
    cleanup_process_context(current_ctx, 0);

    (void)loaded.ctx.release();
    (void)loaded.image_base_owner.release();
    (void)loaded.owned_address_space.release();

    if (next_ctx->address_space != null) {
        vm::activate(next_ctx->address_space);
    } else {
        vm::activate_kernel();
    }

    int ret = asm_call_process_entry(next_ctx->entry, kernel_api::get_api());
    cleanup_process_context(next_ctx, ret);
    sched::exit_task();
}

auto run(const char* filename, console_interface interface) -> i64 {
    return run(string_view(filename), interface);
}

auto run(const char* filename, console_interface interface, const vk_framebuffer_info_t* fb_override) -> i64 {
    return run(string_view(filename), interface, fb_override);
}

auto exec(const char* filename, console_interface interface) -> int {
    return exec(string_view(filename), interface);
}

auto exec(const char* filename, console_interface interface, const vk_framebuffer_info_t* fb_override) -> int {
    return exec(string_view(filename), interface, fb_override);
}

} // namespace process
} // namespace vk
