/*
 * vkernel - UEFI Microkernel
 * Copyright (C) 2026 vkernel authors
 *
 * process_internal.h - Shared process loader internals
 */

#ifndef VKERNEL_PROCESS_INTERNAL_H
#define VKERNEL_PROCESS_INTERNAL_H

#include "process.h"

namespace vk {
namespace process {

struct process_task_context {
    u64   entry;
    u8*   image_base;
    usize image_size;
    bool  image_from_phys; /* true = free via g_phys_alloc, false = g_kernel_heap */
    console_interface interface;
};

void cleanup_process_context(process_task_context* ctx, int exit_code);

} // namespace process
} // namespace vk

#endif /* VKERNEL_PROCESS_INTERNAL_H */
