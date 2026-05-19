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

struct process_line {
    u64 address;
    u32 line;
    u32 file_offset;
};

struct resolved_symbol {
    const char* name;
    u64 address;
    u64 offset;
    u32 size;
};

struct resolved_source_location {
    const char* file_path;
    u64 address;
    u64 offset;
    u32 line;
};

void init_debug_metadata(process_task_context* ctx);
[[nodiscard]] auto attach_symbols_from_elf(process_task_context* ctx,
                                           const u8* file_data,
                                           usize file_size) -> bool;
[[nodiscard]] auto attach_lines_from_map(process_task_context* ctx,
                                         string_view program_path,
                                         const u8* file_data,
                                         usize file_size) -> bool;
void cleanup_debug_metadata(process_task_context* ctx);
[[nodiscard]] auto lookup_symbol(const process_task_context* ctx,
                                 u64 address,
                                 resolved_symbol* out) -> bool;
[[nodiscard]] auto lookup_source_location(const process_task_context* ctx,
                                          u64 address,
                                          resolved_source_location* out) -> bool;

} // namespace process
} // namespace vk

#endif /* VKERNEL_PROCESS_DEBUG_H */
