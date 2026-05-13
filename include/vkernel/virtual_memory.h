/*
 * vkernel - UEFI Microkernel
 * Copyright (C) 2026 vkernel authors
 *
 * virtual_memory.h - Minimal x86_64 address-space manager.
 */

#ifndef VKERNEL_VIRTUAL_MEMORY_H
#define VKERNEL_VIRTUAL_MEMORY_H

#include "types.h"

namespace vk {
namespace vm {

inline constexpr virt_addr USER_IMAGE_BASE = 0x0000400000000000ULL;
inline constexpr virt_addr USER_HEAP_BASE  = 0x0000410000000000ULL;
inline constexpr virt_addr USER_SHARED_BASE = 0x00007E0000000000ULL;
inline constexpr virt_addr USER_MAP_LIMIT  = 0x0000800000000000ULL;

inline constexpr u64 MAP_WRITABLE   = (1ULL << 0);
inline constexpr u64 MAP_USER       = (1ULL << 1);
inline constexpr u64 MAP_EXECUTABLE = (1ULL << 2);

struct address_space {
    phys_addr pml4_phys = 0;
    virt_addr heap_next = USER_HEAP_BASE;
};

void init();

[[nodiscard]] auto kernel_cr3() -> phys_addr;
[[nodiscard]] auto create_address_space() -> address_space*;
void destroy_address_space(address_space* as);

void activate_kernel();
void activate(address_space* as);
[[nodiscard]] auto active_cr3() -> phys_addr;

[[nodiscard]] auto map_page(address_space* as, virt_addr virt, phys_addr phys, u64 flags) -> bool;
[[nodiscard]] auto map_contiguous(address_space* as,
                                  virt_addr virt,
                                  phys_addr phys,
                                  usize size,
                                  u64 flags) -> bool;
void unmap_range(address_space* as, virt_addr virt, usize size);

[[nodiscard]] auto debug_resolve(address_space* as,
                                 virt_addr virt,
                                 phys_addr* out_phys,
                                 u64* out_flags) -> bool;

[[nodiscard]] auto allocate_user_pages(address_space* as,
                                       virt_addr virt,
                                       usize size,
                                       u64 flags,
                                       phys_addr* out_phys) -> bool;

[[nodiscard]] auto allocate_user_heap(address_space* as,
                                      usize size,
                                      u64 flags,
                                      phys_addr* out_phys,
                                      virt_addr* out_virt) -> bool;

} // namespace vm
} // namespace vk

#endif /* VKERNEL_VIRTUAL_MEMORY_H */
