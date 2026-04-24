/*
 * vkernel - UEFI Microkernel
 * Copyright (C) 2026 vkernel authors
 *
 * memory.cpp - Memory management implementation with C++26
 */

#include "config.h"
#include "types.h"
#include "uefi.h"
#include "memory.h"
#include "console.h"

namespace vk {

/* Global memory map */
static memory_map_entry g_memory_map[config::max_memory_map_entries];
static u32 g_memory_map_count = 0;

/* Pool of memory_region nodes for the physical allocator.
 * A slot with size == 0 is unallocated (static storage = zero-init). */
static constexpr u32 REGION_POOL_SIZE = 512;
static memory_region g_region_pool[REGION_POOL_SIZE];

static auto alloc_region_node() -> memory_region* {
    for (u32 i = 0; i < REGION_POOL_SIZE; ++i) {
        if (g_region_pool[i].size == 0)
            return &g_region_pool[i];
    }
    return null;
}

static void free_region_node(memory_region* node) {
    node->start = 0;
    node->size  = 0;
    node->used  = false;
    node->next  = null;
}

/* Physical allocator state */
phys_allocator g_phys_alloc;

/* Kernel heap - 64 MB initial heap */
/* Must be 16-byte aligned so SSE/XMM constants in loaded PE images stay aligned */
#if defined(_MSC_VER)
__declspec(align(16))
#else
__attribute__((aligned(16)))
#endif
static u8 g_kernel_heap_memory[64 * 1024 * 1024];
kernel_heap g_kernel_heap;

/* Convert UEFI memory type to kernel memory type */
[[nodiscard]] constexpr auto uefi_type_to_kernel(u32 uefi_type) -> memory_type {
    switch (uefi_type) {
        case 0:  return memory_type::reserved;
        case 1:  return memory_type::loader_code;
        case 2:  return memory_type::loader_data;
        case 3:  return memory_type::boot_services_code;
        case 4:  return memory_type::boot_services_data;
        case 5:  return memory_type::runtime_services_code;
        case 6:  return memory_type::runtime_services_data;
        case 7:  return memory_type::conventional;
        case 8:  return memory_type::unusable;
        case 9:  return memory_type::acpi_reclaimable;
        case 10: return memory_type::nvs;
        case 11: return memory_type::memory_mapped_io;
        case 12: return memory_type::memory_mapped_io_port_space;
        case 13: return memory_type::pal_code;
        case 14: return memory_type::persistent;
        default: return memory_type::reserved;
    }
}

/* Memory map entry type string */
auto memory_map_entry::type_string() const -> const char* {
    switch (type) {
        case memory_type::reserved:                return "Reserved";
        case memory_type::loader_code:             return "Loader Code";
        case memory_type::loader_data:             return "Loader Data";
        case memory_type::boot_services_code:      return "Boot Services Code";
        case memory_type::boot_services_data:      return "Boot Services Data";
        case memory_type::runtime_services_code:   return "Runtime Services Code";
        case memory_type::runtime_services_data:   return "Runtime Services Data";
        case memory_type::conventional:            return "Conventional";
        case memory_type::unusable:                return "Unusable";
        case memory_type::acpi_reclaimable:        return "ACPI Reclaimable";
        case memory_type::nvs:                     return "NVS";
        case memory_type::memory_mapped_io:        return "Memory Mapped I/O";
        case memory_type::memory_mapped_io_port_space: return "MMIO Port Space";
        case memory_type::pal_code:                return "PAL Code";
        case memory_type::persistent:              return "Persistent";
        default:                                   return "Unknown";
    }
}

/* Physical allocator initialization */
auto phys_allocator::init(span<const memory_map_entry> map) -> status_code {
    free_list_ = null;
    total_pages_ = 0;
    used_pages_ = 0;
    free_pages_ = 0;
    
    /* Build the free list from all conventional memory regions in the map.
     * Memory map entries from UEFI are already sorted by physical address. */
    memory_region* tail = null;

    for (const auto& entry : map) {
        total_pages_ += entry.number_of_pages;

        /* After ExitBootServices (which runs before memory::init()),
         * boot-services regions are fully reclaimed and available as
         * general-purpose memory.  Treat them as free.
         * (Loader regions contain our own image — not reclaimable.) */
        const bool is_free =
            entry.type == memory_type::conventional ||
            entry.type == memory_type::boot_services_code ||
            entry.type == memory_type::boot_services_data;

        if (!is_free) continue;
        if (entry.number_of_pages == 0) continue;

        auto node = alloc_region_node();
        if (node == null) {
            log::warn("phys_alloc: region pool exhausted");
            break;
        }

        node->start = entry.physical_start;
        node->size  = entry.number_of_pages * PAGE_SIZE_4K;
        node->used  = false;
        node->next  = null;

        /* Never hand out physical page 0 — it holds the real-mode IVT/BDA
         * and 0 is used as the "allocation failed" sentinel.  Trim it. */
        if (node->start == 0) {
            if (node->size <= PAGE_SIZE_4K) {
                free_region_node(node);
                continue;           /* entire region is just page 0 */
            }
            node->start = PAGE_SIZE_4K;
            node->size -= PAGE_SIZE_4K;
        }

        if (tail == null) {
            free_list_ = node;
        } else {
            tail->next = node;
        }
        tail = node;
        free_pages_ += entry.number_of_pages;
    }

    log::debug("phys_alloc: %llu pages free (%llu MB)",
               static_cast<unsigned long long>(total_pages_),
               static_cast<unsigned long long>((total_pages_ * PAGE_SIZE_4K) / (1024 * 1024)));

    return status_code::success;
}

/* Allocate physical pages - first-fit with alignment and optional upper-bound.
 * Returns the physical address of the first page, or 0 on failure. */
auto phys_allocator::allocate_pages(u32 page_count, u32 alignment, phys_addr max_addr) -> phys_addr {
    if (page_count == 0) return 0;

    size_phys req_size = static_cast<size_phys>(page_count) * PAGE_SIZE_4K;

    for (auto region = free_list_; region != null; region = region->next) {
        if (region->used) continue;

        /* Find the lowest aligned start address inside this region */
        phys_addr aligned_start = align_up(region->start, static_cast<usize>(alignment));
        phys_addr region_end    = region->start + region->size;

        if (aligned_start + req_size > region_end) continue;
        if (max_addr != 0 && aligned_start + req_size > max_addr) continue;

        size_phys pre_size  = aligned_start - region->start;
        size_phys post_size = region_end - (aligned_start + req_size);

        if (pre_size == 0) {
            /* Region is already aligned - reuse its node for the allocation */
            if (post_size > 0) {
                auto tail_node = alloc_region_node();
                if (tail_node) {
                    tail_node->start = aligned_start + req_size;
                    tail_node->size  = post_size;
                    tail_node->used  = false;
                    tail_node->next  = region->next;
                    region->next     = tail_node;
                    region->size     = req_size;
                }
                /* If pool is exhausted the tail bytes are folded into the allocation */
            }
            region->used = true;
        } else {
            /* Need a new node for the aligned allocation chunk */
            auto alloc_node = alloc_region_node();
            if (!alloc_node) continue; /* try the next region */

            alloc_node->start = aligned_start;
            alloc_node->size  = req_size;
            alloc_node->used  = true;
            alloc_node->next  = region->next;

            region->size = pre_size; /* keep pre-alignment fragment as free */
            region->next = alloc_node;

            if (post_size > 0) {
                auto tail_node = alloc_region_node();
                if (tail_node) {
                    tail_node->start = aligned_start + req_size;
                    tail_node->size  = post_size;
                    tail_node->used  = false;
                    tail_node->next  = alloc_node->next;
                    alloc_node->next = tail_node;
                }
            }
        }

        used_pages_ += page_count;
        free_pages_ -= page_count;
        return aligned_start;
    }

    return 0; /* out of memory */
}

/* Free physical pages - marks the region free and coalesces adjacent free regions */
void phys_allocator::free_pages(phys_addr addr, u32 page_count) {
    if (addr == 0 || page_count == 0) return;

    memory_region* prev = null;
    for (auto region = free_list_; region != null; region = region->next) {
        if (region->start == addr && region->used) {
            region->used = false;
            used_pages_ -= page_count;
            free_pages_ += page_count;

            /* Coalesce with the next region if it is free and contiguous */
            if (region->next != null && !region->next->used &&
                region->start + region->size == region->next->start) {
                auto next = region->next;
                region->size += next->size;
                region->next  = next->next;
                free_region_node(next);
            }

            /* Coalesce with the previous region if it is free and contiguous */
            if (prev != null && !prev->used &&
                prev->start + prev->size == region->start) {
                prev->size += region->size;
                prev->next  = region->next;
                free_region_node(region);
            }

            return;
        }
        prev = region;
    }
}

/* Kernel heap initialization */
auto kernel_heap::init(void* base, size_phys size) -> status_code {
    free_list_ = reinterpret_cast<heap_block*>(base);
    free_list_->size = size - sizeof(heap_block);
    free_list_->used = false;
    free_list_->next = null;
    free_list_->prev = null;

    log::debug("heap: base=%p, capacity=%llu MB",
               base,
               static_cast<unsigned long long>(size / (1024 * 1024)));

    return status_code::success;
}

/* Kernel heap allocation */
auto kernel_heap::allocate(size_phys size) -> void* {
    if (size == 0) {
        return null;
    }
    
    /* Align size to 16 bytes so every allocation starts on a 16-byte boundary
     * (required by SSE/XMM constants in PE .rdata that use MOVAPS/XORPS). */
    size = align_up(size, static_cast<usize>(16));
    
    /* Find a free block that fits */
    auto block = free_list_;
    while (block != null) {
        if (!block->used && block->size >= size) {
            /* Found a suitable block */
            block->used = true;
            
            /* Split the block if there's enough remaining space */
            if (block->size > size + sizeof(heap_block) + 16) {
                auto new_block = reinterpret_cast<heap_block*>(
                    reinterpret_cast<u8*>(block) + sizeof(heap_block) + size
                );
                new_block->size = block->size - size - sizeof(heap_block);
                new_block->used = false;
                new_block->next = block->next;
                new_block->prev = block;
                
                if (block->next != null) {
                    block->next->prev = new_block;
                }
                
                block->next = new_block;
                block->size = size;
            }
            
            return block->data();
        }
        block = block->next;
    }
    
    /* No suitable block found */
    return null;
}

/* Zero-initialized allocation */
auto kernel_heap::allocate_zero(size_phys size) -> void* {
    void* ptr = allocate(size);
    if (ptr != null) {
        memory::memory_set(ptr, 0, size);
    }
    return ptr;
}

/* Free kernel heap memory */
void kernel_heap::free(void* ptr) {
    if (ptr == null) {
        return;
    }

    log::debug("heap: freeing block at %p", ptr);
    
    /* Find the block header */
    auto block = reinterpret_cast<heap_block*>(
        reinterpret_cast<u8*>(ptr) - sizeof(heap_block)
    );
    block->used = false;
    
    /* Coalesce with next block if free */
    if (block->next != null && !block->next->used) {
        block->size += block->next->size + sizeof(heap_block);
        block->next = block->next->next;
        if (block->next != null) {
            block->next->prev = block;
        }
    }
    
    /* Coalesce with previous block if free */
    if (block->prev != null && !block->prev->used) {
        block->prev->size += block->size + sizeof(heap_block);
        block->prev->next = block->next;
        if (block->next != null) {
            block->next->prev = block->prev;
        }
    }
}

/* Memory set */
void* memory::memory_set(void* dest, i32 c, size_phys n) {
    auto d = static_cast<u8*>(dest);
    for (size_phys i = 0; i < n; ++i) {
        d[i] = static_cast<u8>(c);
    }
    return dest;
}

/* Memory copy */
void* memory::memory_copy(void* dest, const void* src, size_phys n) {
    auto d = static_cast<u8*>(dest);
    auto s = static_cast<const u8*>(src);
    for (size_phys i = 0; i < n; ++i) {
        d[i] = s[i];
    }
    return dest;
}

/* Memory compare */
i32 memory::memory_compare(const void* s1, const void* s2, size_phys n) {
    auto p1 = static_cast<const u8*>(s1);
    auto p2 = static_cast<const u8*>(s2);
    for (size_phys i = 0; i < n; ++i) {
        if (p1[i] != p2[i]) {
            return static_cast<i32>(p1[i]) - static_cast<i32>(p2[i]);
        }
    }
    return 0;
}

/* Memory move (handles overlapping regions) */
void* memory::memory_move(void* dest, const void* src, size_phys n) {
    auto d = static_cast<u8*>(dest);
    auto s = static_cast<const u8*>(src);
    
    if (d < s) {
        /* Copy forward */
        return memory_copy(dest, src, n);
    } else if (d > s) {
        /* Copy backward */
        size_phys i = n;
        while (i-- > 0) {
            d[i] = s[i];
        }
    }
    
    return dest;
}

/* Initialize memory subsystem */
auto memory::init(span<const memory_map_entry> map) -> status_code {
    if (map.empty()) {
        return status_code::invalid_param;
    }
    
    /* Copy the memory map */
    g_memory_map_count = static_cast<u32>(
        map.size() < config::max_memory_map_entries ? 
        map.size() : config::max_memory_map_entries
    );
    
    for (u32 i = 0; i < g_memory_map_count; ++i) {
        g_memory_map[i] = map[i];
    }
    
    /* Initialize the physical allocator */
    if (auto status = g_phys_alloc.init(map); status != status_code::success) {
        return status;
    }
    
    /* Initialize the kernel heap */
    if (auto status = g_kernel_heap.init(g_kernel_heap_memory, sizeof(g_kernel_heap_memory)); 
        status != status_code::success) {
        return status;
    }
    
    log::info("Memory subsystem initialized (%llu pages total, %llu MB)",
              static_cast<unsigned long long>(g_phys_alloc.total_pages()),
              static_cast<unsigned long long>((g_phys_alloc.total_pages() * PAGE_SIZE_4K) / (1024 * 1024)));
    
    return status_code::success;
}

/* Get memory map */
auto memory::get_memory_map() -> span<const memory_map_entry> {
    return span(g_memory_map, g_memory_map_count);
}

/* Find memory map entry for address */
auto memory::find_entry(phys_addr addr) -> const memory_map_entry* {
    for (u32 i = 0; i < g_memory_map_count; ++i) {
        if (g_memory_map[i].contains(addr)) {
            return &g_memory_map[i];
        }
    }
    return null;
}

/* Dump memory map */
void memory::dump_map() {
    log::info("Memory Map:");
    log::info("========================================");
    for (u32 i = 0; i < g_memory_map_count; ++i) {
        log::info("Entry %u: %s", i, g_memory_map[i].type_string());
    }
    log::info("========================================\n");
}

/* Dump kernel heap allocations */
void memory::dump_heap() {
    log::info("Kernel Heap Allocations:");
    log::info("========================================");
    log::info("  Address          Size       Status");
    log::info("  ---------------  ---------  -------");
    
    auto block = g_kernel_heap.get_free_list();
    u64 used_total = 0;
    u64 free_total = 0;
    u32 used_count = 0;
    u32 free_count = 0;
    
    while (block != null) {
        log::info("  0x%016llx  %9llu  %s",
                  reinterpret_cast<u64>(block->data()),
                  static_cast<unsigned long long>(block->size),
                  block->used ? "USED" : "FREE");
        
        if (block->used) {
            used_total += block->size;
            used_count++;
        } else {
            free_total += block->size;
            free_count++;
        }

        block = block->next;
    }
    
    log::info("----------------------------------------");
    log::info("  Total Used:  %u blocks, %llu bytes", used_count, used_total);
    log::info("  Total Free:  %u blocks, %llu bytes", free_count, free_total);
    log::info("========================================\n\n");
}

} // namespace vk
