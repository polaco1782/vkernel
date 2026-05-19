#ifndef VKERNEL_KERNEL_DEBUG_H
#define VKERNEL_KERNEL_DEBUG_H

#include "types.h"

namespace vk {
namespace kernel_debug {

struct resolved_symbol {
    const char* name;
    u64 address;
    u64 offset;
};

struct resolved_source_location {
    const char* file_path;
    u64 address;
    u64 offset;
    u32 line;
};

[[nodiscard]] auto lookup_symbol(u64 address, resolved_symbol* out) -> bool;
[[nodiscard]] auto lookup_source_location(u64 address,
                                          resolved_source_location* out) -> bool;

} // namespace kernel_debug
} // namespace vk

#endif /* VKERNEL_KERNEL_DEBUG_H */
