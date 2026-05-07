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

struct process_allocation {
    void* user_ptr;
    void* raw_ptr;
    usize requested_size;
    usize allocated_size;
    bool from_phys;
    process_allocation* next;
};

struct process_task_context {
    static constexpr usize KEY_QUEUE_SIZE = 64;
    static constexpr usize MOUSE_QUEUE_SIZE = 64;
    static constexpr usize COMMAND_LINE_CAP = 256;

    u64   entry;
    u8*   image_base;
    usize image_size;
    bool  image_from_phys; /* true = free via g_phys_alloc, false = g_kernel_heap */
    console_interface interface;
    vk_key_event_t key_queue[KEY_QUEUE_SIZE];
    usize key_q_head;
    usize key_q_tail;
    vk_mouse_event_t mouse_queue[MOUSE_QUEUE_SIZE];
    usize mouse_q_head;
    usize mouse_q_tail;
    char command_line[COMMAND_LINE_CAP];
    usize command_line_len;
    vk_framebuffer_info_t fb_override;
    bool fb_override_valid;
    u64 task_id;
    vk_u32 fb_text_col;
    vk_u32 fb_text_row;
    process_allocation* allocations;
};

void cleanup_process_context(process_task_context* ctx, int exit_code);

} // namespace process
} // namespace vk

#endif /* VKERNEL_PROCESS_INTERNAL_H */
