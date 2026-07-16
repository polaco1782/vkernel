/*
 * vkernel - UEFI Microkernel
 * Copyright (C) 2026 vkernel authors
 *
 * process_internal.h - Shared process loader internals
 */

#ifndef VKERNEL_PROCESS_INTERNAL_H
#define VKERNEL_PROCESS_INTERNAL_H

#include "process.h"
#include "process_debug.h"
#include "vk.h"

namespace vk {
namespace vm {
struct address_space;
}
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
    static constexpr usize FRAMEBUFFER_EVENT_QUEUE_SIZE = 8;
    static constexpr usize COMMAND_LINE_CAP = 256;

    u64   entry;
    u8*   image_base;
    usize image_size;
    bool  image_from_phys; /* true = free via g_phys_alloc, false = g_kernel_heap */
    vm::address_space* address_space;
    phys_addr image_phys;
    bool image_vm_mapped;
    u64 phdr_addr;
    u16 phnum;
    u16 phentsize;
    bool has_tls;
    u64 tls_vaddr;
    u64 tls_filesz;
    u64 tls_memsz;
    u64 tls_align;
    u64 brk_base;
    u64 brk_current;
    u64 brk_mapped_end;
    u64 user_thread_pointer;
    console_interface interface;
    vk_key_event_t key_queue[KEY_QUEUE_SIZE];
    usize key_q_head;
    usize key_q_tail;
    vk_mouse_event_t mouse_queue[MOUSE_QUEUE_SIZE];
    usize mouse_q_head;
    usize mouse_q_tail;
    vk_framebuffer_event_t framebuffer_event_queue[FRAMEBUFFER_EVENT_QUEUE_SIZE];
    usize framebuffer_event_q_head;
    usize framebuffer_event_q_tail;
    char command_line[COMMAND_LINE_CAP];
    usize command_line_len;
    vk_framebuffer_info_t fb_override;
    bool fb_override_valid;
    bool framebuffer_resize_events_enabled;
    bool startup_window_size_set;
    vk_u32 startup_window_width;
    vk_u32 startup_window_height;
    u64 task_id;
    vk_u32 fb_text_col;
    vk_u32 fb_text_row;
    process_allocation* allocations;
    process_symbol* symbols;
    usize symbol_count;
    const char* symbol_strings;
    void* symbol_storage;
    process_line* lines;
    usize line_count;
    const char* line_files;
    void* line_storage;
};

void cleanup_process_context(process_task_context* ctx, int exit_code);
void process_task_main(void* user_data);

} // namespace process
} // namespace vk

#endif /* VKERNEL_PROCESS_INTERNAL_H */
