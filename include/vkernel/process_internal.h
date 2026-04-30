/*
 * vkernel - UEFI Microkernel
 * Copyright (C) 2026 vkernel authors
 *
 * process_internal.h - Shared process loader internals
 */

#ifndef VKERNEL_PROCESS_INTERNAL_H
#define VKERNEL_PROCESS_INTERNAL_H

#include "process.h"
#include "vk.h"

namespace vk {
namespace process {

struct process_task_context {
    static constexpr usize KEY_QUEUE_SIZE = 64;

    u64   entry;
    u8*   image_base;
    usize image_size;
    bool  image_from_phys; /* true = free via g_phys_alloc, false = g_kernel_heap */
    console_interface interface;
    vk_key_event_t key_queue[KEY_QUEUE_SIZE];
    usize key_q_head;
    usize key_q_tail;
    vk_framebuffer_info_t fb_override;
    bool fb_override_valid;
};

void cleanup_process_context(process_task_context* ctx, int exit_code);

} // namespace process
} // namespace vk

#endif /* VKERNEL_PROCESS_INTERNAL_H */
