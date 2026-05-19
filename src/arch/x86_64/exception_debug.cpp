/*
 * vkernel - UEFI Microkernel
 * Copyright (C) 2026 vkernel authors
 *
 * exception_debug.cpp - x86_64 exception backtrace helpers.
 *
 * Keeps crash-report formatting and stack walking out of arch_init.cpp so
 * the core interrupt/exception path stays focused on control flow.
 */

#include "config.h"
#include "types.h"
#include "kernel_debug.h"
#include "log.h"
#include "scheduler.h"
#include "process_debug.h"
#include "process_internal.h"
#include "arch/x86_64/exception_debug.h"

namespace vk {
namespace arch {
namespace {

static auto address_in_range(virt_addr addr, virt_addr start, virt_addr end) -> bool {
    return addr >= start && addr < end;
}

static auto frame_slot_in_bounds(virt_addr rbp,
                                 virt_addr stack_low,
                                 virt_addr stack_high) -> bool {
    if ((rbp & 0x7) != 0) {
        return false;
    }

    if (!address_in_range(rbp, stack_low, stack_high)) {
        return false;
    }

    constexpr virt_addr k_frame_size = sizeof(u64) * 2;
    if (rbp > stack_high - k_frame_size) {
        return false;
    }

    return true;
}

static void log_backtrace_address(const char* label,
                                  u64 address,
                                  const process::process_task_context* ctx) {
    auto line = log::crash();
    line << label << log::hex(address, 1, true, false);

    process::resolved_symbol symbol {};
    if (process::lookup_symbol(ctx, address, &symbol) && symbol.name != null) {
        line << " (" << symbol.name;
        if (symbol.offset != 0) {
            line << "+" << log::hex(symbol.offset, 1, true, false);
        }
        line << ")";
    } else {
        kernel_debug::resolved_symbol kernel_symbol {};
        if (kernel_debug::lookup_symbol(address, &kernel_symbol)
            && kernel_symbol.name != null) {
            line << " (" << kernel_symbol.name;
            if (kernel_symbol.offset != 0) {
                line << "+" << log::hex(kernel_symbol.offset, 1, true, false);
            }
            line << ")";
        } else if (ctx != null && ctx->image_base != null && ctx->image_size > 0) {
            const u64 image_base = reinterpret_cast<u64>(ctx->image_base);
            const u64 image_end = image_base + static_cast<u64>(ctx->image_size);
            if (address >= image_base && address < image_end) {
                line << " (image+" << log::hex(address - image_base, 1, true, false) << ")";
            }
        }
    }

    process::resolved_source_location location {};
    if (process::lookup_source_location(ctx, address, &location)
        && location.file_path != null && location.line != 0) {
        line << " @ " << location.file_path << ":" << location.line;
    }
}

} // namespace

void log_exception_backtrace(const register_state* regs,
                             const process::process_task_context* ctx) {
    if (regs == null) {
        return;
    }

    virt_addr stack_low = 0;
    virt_addr stack_high = 0;
    if (!sched::current_task_stack_bounds(&stack_low, &stack_high)) {
        return;
    }

    log::crash() << "\n  Backtrace:";
    log_backtrace_address("    #0  ", regs->frame.rip, ctx);

    virt_addr rbp = static_cast<virt_addr>(regs->rbp);
    constexpr usize k_max_frames = 16;
    for (usize frame = 1; frame < k_max_frames; ++frame) {
        if (!frame_slot_in_bounds(rbp, stack_low, stack_high)) {
            break;
        }

        const auto* fp = reinterpret_cast<const u64*>(static_cast<usize>(rbp));
        const virt_addr next_rbp = static_cast<virt_addr>(fp[0]);
        const u64 return_address = fp[1];
        if (return_address == 0) {
            break;
        }

        auto line = log::crash();
        line << "    #" << static_cast<unsigned long long>(frame) << "  "
             << log::hex(return_address, 1, true, false);

        process::resolved_symbol symbol {};
        if (process::lookup_symbol(ctx, return_address, &symbol) && symbol.name != null) {
            line << " (" << symbol.name;
            if (symbol.offset != 0) {
                line << "+" << log::hex(symbol.offset, 1, true, false);
            }
            line << ")";
        } else {
            kernel_debug::resolved_symbol kernel_symbol {};
            if (kernel_debug::lookup_symbol(return_address, &kernel_symbol)
                && kernel_symbol.name != null) {
                line << " (" << kernel_symbol.name;
                if (kernel_symbol.offset != 0) {
                    line << "+" << log::hex(kernel_symbol.offset, 1, true, false);
                }
                line << ")";
            } else if (ctx != null && ctx->image_base != null && ctx->image_size > 0) {
                const u64 image_base = reinterpret_cast<u64>(ctx->image_base);
                const u64 image_end = image_base + static_cast<u64>(ctx->image_size);
                if (return_address >= image_base && return_address < image_end) {
                    line << " (image+" << log::hex(return_address - image_base, 1, true, false) << ")";
                }
            }
        }

        process::resolved_source_location location {};
        if (process::lookup_source_location(ctx, return_address, &location)
            && location.file_path != null && location.line != 0) {
            line << " @ " << location.file_path << ":" << location.line;
        }

        if (next_rbp <= rbp) {
            break;
        }
        rbp = next_rbp;
    }
}

} // namespace arch
} // namespace vk
