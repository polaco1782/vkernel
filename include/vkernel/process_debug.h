#ifndef VKERNEL_PROCESS_DEBUG_H
#define VKERNEL_PROCESS_DEBUG_H

#include "types.h"

namespace vk {
namespace process {

struct process_task_context;

struct process_symbol {
    u64 address;
    u32 size;
    u32 name_offset;
};

struct resolved_symbol {
    const char* name;
    u64 address;
    u64 offset;
    u32 size;
};

void init_debug_metadata(process_task_context* ctx);
[[nodiscard]] auto attach_symbols_from_elf(process_task_context* ctx,
                                           const u8* file_data,
                                           usize file_size) -> bool;
void cleanup_debug_metadata(process_task_context* ctx);
[[nodiscard]] auto lookup_symbol(const process_task_context* ctx,
                                 u64 address,
                                 resolved_symbol* out) -> bool;

} // namespace process
} // namespace vk

#endif /* VKERNEL_PROCESS_DEBUG_H */
