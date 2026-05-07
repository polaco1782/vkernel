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
#include "log.h"
#include "arch/x86_64/arch.h"

namespace vk {

/* Global memory map */
static memory_map_entry g_memory_map[config::max_memory_map_entries];
static u32 g_memory_map_count = 0;

/* Pool of memory_region nodes for the physical allocator.
 * A slot with size == 0 is unallocated (static storage = zero-init). */
static constexpr u32 REGION_POOL_SIZE = 512;
static memory_region g_region_pool[REGION_POOL_SIZE];

static_assert((config::initial_kernel_heap_size % PAGE_SIZE_4K) == 0,
              "initial kernel heap must be page-aligned");

#if defined(_MSC_VER)
__declspec(align(4096))
static u8 g_initial_kernel_heap[config::initial_kernel_heap_size];
#else
static u8 g_initial_kernel_heap[config::initial_kernel_heap_size]
    __attribute__((aligned(4096), section(".heap"), used));
#endif

static_assert(config::initial_kernel_heap_size > sizeof(heap_block),
              "initial kernel heap must fit heap metadata");

static constexpr size_phys HEAP_MIN_ALIGNMENT = 16;
static constexpr size_phys HEAP_MIN_SPLIT_DATA_SIZE = 16;

struct aligned_heap_candidate {
    heap_block* block = null;
    usize aligned_data = 0;
    usize used_header_addr = 0;
    size_phys prefix_data_size = 0;
    size_phys suffix_total = 0;
};

static auto find_heap_block(heap_block* free_list, size_phys size) -> heap_block* {
    for (auto* block = free_list; block != null; block = block->next) {
        if (!block->used && block->size >= size) {
            return block;
        }
    }
    return null;
}

static auto find_aligned_heap_block(heap_block* free_list,
                                    size_phys size,
                                    size_phys alignment) -> aligned_heap_candidate {
    const size_phys header_size = sizeof(heap_block);

    for (auto* block = free_list; block != null; block = block->next) {
        if (block->used) {
            continue;
        }

        const usize block_addr = reinterpret_cast<usize>(block);
        const usize data_start = block_addr + header_size;
        const usize block_end = data_start + static_cast<usize>(block->size);

        usize aligned_data = align_up(data_start, static_cast<usize>(alignment));

        while (true) {
            const usize used_header_addr = aligned_data - header_size;
            if (used_header_addr == block_addr) {
                break;
            }
            if (used_header_addr >= data_start) {
                const size_phys prefix_data_size =
                    static_cast<size_phys>(used_header_addr - data_start);
                if (prefix_data_size >= HEAP_MIN_SPLIT_DATA_SIZE) {
                    break;
                }
            }
            if (aligned_data + static_cast<usize>(size) > block_end) {
                break;
            }
            aligned_data += static_cast<usize>(alignment);
        }

        if (aligned_data < data_start) {
            continue;
        }

        const usize used_header_addr = aligned_data - header_size;
        if (used_header_addr < block_addr || aligned_data + size > block_end) {
            continue;
        }

        aligned_heap_candidate fit;
        fit.block = block;
        fit.aligned_data = aligned_data;
        fit.used_header_addr = used_header_addr;
        fit.prefix_data_size =
            static_cast<size_phys>(used_header_addr - data_start);
        fit.suffix_total =
            static_cast<size_phys>(block_end - (aligned_data + size));
        return fit;
    }

    return {};
}

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
            log::warn() << "phys_alloc: region pool exhausted";
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

    log::debug() << "phys_alloc: " << static_cast<unsigned long long>(total_pages_) << " pages free (" << static_cast<unsigned long long>((total_pages_ * PAGE_SIZE_4K) / (1024 * 1024)) << " MB)";

    return status_code::success;
}

/* Allocate physical pages - first-fit with alignment and optional upper-bound.
 * Returns the physical address of the first page, or 0 on failure. */
auto phys_allocator::allocate_pages(u32 page_count, u32 alignment, phys_addr max_addr) -> phys_addr {
    if (page_count == 0) return 0;

    lock_.acquire();

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
        lock_.release();
        return aligned_start;
    }

    lock_.release();
    return 0; /* out of memory */
}

/* Free physical pages - marks the region free and coalesces adjacent free regions */
void phys_allocator::free_pages(phys_addr addr, u32 page_count) {
    if (addr == 0 || page_count == 0) return;

    lock_.acquire();

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

            lock_.release();
            return;
        }
        prev = region;
    }

    lock_.release();
}

/* Kernel heap initialization */
auto kernel_heap::init(void* base, size_phys size) -> status_code {
    if (base == null || size <= sizeof(heap_block)) {
        return status_code::invalid_param;
    }

    free_list_ = reinterpret_cast<heap_block*>(base);
    free_list_->size = size - sizeof(heap_block);
    free_list_->used = false;
    free_list_->next = null;
    free_list_->prev = null;
    total_bytes_ = size;
    subheap_count_ = 1;
    expanding_ = false;

    log::debug() << "heap: base=" << reinterpret_cast<const void*>(base) << ", capacity=" << static_cast<unsigned long long>(size / (1024 * 1024)) << " MB";

    return status_code::success;
}

/* Add a new physically-contiguous subheap to the heap's free list */
auto kernel_heap::add_region(void* base, size_phys size) -> status_code {
    if (base == null || size <= sizeof(heap_block)) {
        return status_code::invalid_param;
    }

    auto* block  = reinterpret_cast<heap_block*>(base);
    block->size  = size - sizeof(heap_block);
    block->used  = false;
    block->next  = null;
    block->prev  = null;

    lock_.acquire();

    if (free_list_ == null ||
        reinterpret_cast<usize>(block) < reinterpret_cast<usize>(free_list_)) {
        block->next = free_list_;
        if (free_list_ != null) {
            free_list_->prev = block;
        }
        free_list_ = block;
    } else {
        auto* current = free_list_;
        while (current->next != null &&
               reinterpret_cast<usize>(current->next) < reinterpret_cast<usize>(block)) {
            current = current->next;
        }

        block->next = current->next;
        block->prev = current;
        current->next = block;
        if (block->next != null) {
            block->next->prev = block;
        }
    }

    if (block->prev != null && !block->prev->used && block->prev->end() == block) {
        block->prev->size += block->size + sizeof(heap_block);
        block->prev->next = block->next;
        if (block->next != null) {
            block->next->prev = block->prev;
        }
        block = block->prev;
    }

    if (block->next != null && !block->next->used && block->end() == block->next) {
        auto* next = block->next;
        block->size += next->size + sizeof(heap_block);
        block->next = next->next;
        if (block->next != null) {
            block->next->prev = block;
        }
    }

    total_bytes_ += size;
    ++subheap_count_;

    lock_.release();

    log::debug() << "heap: added subheap base=" << reinterpret_cast<const void*>(base) << ", +" << static_cast<unsigned long long>(size / (1024 * 1024)) << " MB";

    return status_code::success;
}

auto kernel_heap::expand(size_phys size, size_phys alignment) -> bool {
    if (size == 0) {
        return false;
    }

    const size_phys header_size = sizeof(heap_block);
    const size_phys max_size = ~static_cast<size_phys>(0);
    const size_phys effective_alignment =
        max(alignment, static_cast<size_phys>(HEAP_MIN_ALIGNMENT));
    const size_phys alignment_slack =
        max(effective_alignment, static_cast<size_phys>(PAGE_SIZE_4K));

    if (size > max_size - header_size - alignment_slack) {
        return false;
    }

    const size_phys minimum_bytes = size + header_size + alignment_slack;
    if (minimum_bytes > (max_size - PAGE_SIZE_4K) / 2) {
        return false;
    }

    size_phys grow_bytes = align_up(
        (minimum_bytes * 2) + PAGE_SIZE_4K,
        static_cast<usize>(PAGE_SIZE_4K));
    if (grow_bytes < config::kernel_heap_min_subheap_size) {
        grow_bytes = config::kernel_heap_min_subheap_size;
    }

    const u64 page_count64 = grow_bytes / PAGE_SIZE_4K;
    if (page_count64 == 0 || page_count64 > 0xFFFFFFFFULL) {
        return false;
    }

    u32 phys_alignment = PAGE_SIZE_4K;
    const size_phys requested_phys_alignment =
        max(alignment, static_cast<size_phys>(PAGE_SIZE_4K));
    if (requested_phys_alignment <= 0xFFFFFFFFULL) {
        phys_alignment = static_cast<u32>(requested_phys_alignment);
    }

    const u32 page_count = static_cast<u32>(page_count64);
    const phys_addr phys = g_phys_alloc.allocate_pages(page_count, phys_alignment, 0);
    if (phys == 0) {
        log::warn() << "heap: expansion allocation failed ("
                    << static_cast<unsigned long long>(page_count)
                    << " pages)";
        return false;
    }

    void* const base = reinterpret_cast<void*>(static_cast<u64>(phys));
    arch::make_region_writable(phys, grow_bytes);
    memory::set(base, 0, grow_bytes);

    if (add_region(base, grow_bytes) != status_code::success) {
        g_phys_alloc.free_pages(phys, page_count);
        log::warn() << "heap: failed to attach expanded subheap";
        return false;
    }

    lock_.acquire();
    const size_phys total_bytes = total_bytes_;
    const u32 subheap_count = subheap_count_;
    lock_.release();

    log::info() << "heap: expanded by "
                << static_cast<unsigned long long>(grow_bytes / (1024 * 1024))
                << " MB (total "
                << static_cast<unsigned long long>(total_bytes / (1024 * 1024))
                << " MB across "
                << static_cast<unsigned long long>(subheap_count)
                << " subheap(s))";
    return true;
}

/* Kernel heap allocation */
auto kernel_heap::allocate(size_phys size) -> void* {
    if (size == 0) {
        return null;
    }

    /* Align size to 16 bytes so every allocation starts on a 16-byte boundary
     * (required by SSE/XMM constants in PE .rdata that use MOVAPS/XORPS). */
    size = align_up(size, static_cast<usize>(HEAP_MIN_ALIGNMENT));

    while (true) {
        lock_.acquire();

        auto* block = find_heap_block(free_list_, size);
        if (block != null) {
            block->used = true;

            if (block->size > size + sizeof(heap_block) + HEAP_MIN_SPLIT_DATA_SIZE) {
                auto* new_block = reinterpret_cast<heap_block*>(
                    reinterpret_cast<u8*>(block) + sizeof(heap_block) + size);
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

            lock_.release();
            return block->data();
        }

        if (!expanding_) {
            expanding_ = true;
            lock_.release();

            const bool expanded = expand(size, HEAP_MIN_ALIGNMENT);

            lock_.acquire();
            expanding_ = false;
            lock_.release();

            if (!expanded) {
                return null;
            }
            continue;
        }

        lock_.release();
        arch::cpu_pause();
    }
}

/* Zero-initialized allocation */
auto kernel_heap::allocate_zero(size_phys size) -> void* {
    void* ptr = allocate(size);
    if (ptr != null) {
        memory::set(ptr, 0, size);
    }
    return ptr;
}

auto kernel_heap::allocate_zero_aligned(size_phys size, size_phys alignment) -> void* {
    if (size == 0) {
        return null;
    }

    if (alignment <= HEAP_MIN_ALIGNMENT) {
        return allocate_zero(size);
    }

    if ((alignment & (alignment - 1)) != 0) {
        return null;
    }

    size = align_up(size, static_cast<usize>(HEAP_MIN_ALIGNMENT));

    while (true) {
        lock_.acquire();

        const auto fit = find_aligned_heap_block(free_list_, size, alignment);
        if (fit.block != null) {
            auto* block = fit.block;
            auto* old_next = block->next;
            const size_phys header_size = sizeof(heap_block);

            heap_block* used_block = null;
            if (fit.prefix_data_size == 0 && fit.used_header_addr == reinterpret_cast<usize>(block)) {
                used_block = block;
            } else {
                block->size = fit.prefix_data_size;
                used_block = reinterpret_cast<heap_block*>(fit.used_header_addr);
                used_block->prev = block;
                used_block->next = old_next;
                block->next = used_block;
                if (old_next != null) {
                    old_next->prev = used_block;
                }
            }

            used_block->used = true;

            if (fit.suffix_total > header_size + HEAP_MIN_SPLIT_DATA_SIZE) {
                auto* suffix = reinterpret_cast<heap_block*>(fit.aligned_data + size);
                suffix->size = fit.suffix_total - header_size;
                suffix->used = false;
                suffix->prev = used_block;
                suffix->next = old_next;

                used_block->size = size;
                used_block->next = suffix;
                if (old_next != null) {
                    old_next->prev = suffix;
                }
            } else {
                used_block->size = size + fit.suffix_total;
                used_block->next = old_next;
                if (old_next != null) {
                    old_next->prev = used_block;
                }
            }

            lock_.release();

            void* ptr = reinterpret_cast<void*>(fit.aligned_data);
            memory::set(ptr, 0, size);
            return ptr;
        }

        if (!expanding_) {
            expanding_ = true;
            lock_.release();

            const bool expanded = expand(size, alignment);

            lock_.acquire();
            expanding_ = false;
            lock_.release();

            if (!expanded) {
                return null;
            }
            continue;
        }

        lock_.release();
        arch::cpu_pause();
    }
}

/* Free kernel heap memory */
void kernel_heap::free(void* ptr) {
    if (ptr == null) {
        return;
    }

    lock_.acquire();
    
    /* Find the block header */
    auto block = reinterpret_cast<heap_block*>(
        reinterpret_cast<u8*>(ptr) - sizeof(heap_block)
    );
    block->used = false;
    
    /* Coalesce with next block if free AND physically adjacent */
    if (block->next != null && !block->next->used &&
        block->end() == block->next) {
        block->size += block->next->size + sizeof(heap_block);
        block->next = block->next->next;
        if (block->next != null) {
            block->next->prev = block;
        }
    }

    /* Coalesce with previous block if free AND physically adjacent */
    if (block->prev != null && !block->prev->used &&
        block->prev->end() == block) {
        block->prev->size += block->size + sizeof(heap_block);
        block->prev->next = block->next;
        if (block->next != null) {
            block->next->prev = block->prev;
        }
    }

    lock_.release();
}

/* Memory set */
void* memory::set(void* dest, i32 c, size_phys n) {
    auto d = static_cast<u8*>(dest);
    for (size_phys i = 0; i < n; ++i) {
        d[i] = static_cast<u8>(c);
    }
    return dest;
}

/* Memory copy */
void* memory::copy(void* dest, const void* src, size_phys n) {
    auto d = static_cast<u8*>(dest);
    auto s = static_cast<const u8*>(src);
    for (size_phys i = 0; i < n; ++i) {
        d[i] = s[i];
    }
    return dest;
}

/* Memory compare */
i32 memory::compare(const void* s1, const void* s2, size_phys n) {
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
void* memory::move(void* dest, const void* src, size_phys n) {
    auto d = static_cast<u8*>(dest);
    auto s = static_cast<const u8*>(src);
    
    if (d < s) {
        /* Copy forward */
        return copy(dest, src, n);
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

    /* ---------------------------------------------------------------
     * Initialize the kernel heap from the fixed .heap reservation.
     *
     * This matches Serenity's boot-heap model more closely: allocator
     * metadata lives in kernel-owned image memory instead of reclaiming
     * arbitrary physical regions during early boot.
     * --------------------------------------------------------------- */
    void* const heap_base = g_initial_kernel_heap;
    const size_phys heap_size = sizeof(g_initial_kernel_heap);

    arch::make_region_writable(
        reinterpret_cast<phys_addr>(heap_base), heap_size);
    memory::set(heap_base, 0, heap_size);

    if (g_kernel_heap.init(heap_base, heap_size) != status_code::success) {
        log::error() << "memory: failed to initialize fixed kernel heap";
        return status_code::no_memory;
    }

    log::info() << "Memory subsystem initialized ("
                << static_cast<unsigned long long>(g_phys_alloc.total_pages())
                << " pages total, "
                << static_cast<unsigned long long>(
                       (g_phys_alloc.total_pages() * PAGE_SIZE_4K) / (1024 * 1024))
                << " MB total, "
                << static_cast<unsigned long long>(heap_size / (1024 * 1024))
                << " MB fixed heap @ "
                << reinterpret_cast<const void*>(heap_base)
                << ")";
    
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
    log::info() << "Memory Map:";
    log::info() << "========================================";
    for (u32 i = 0; i < g_memory_map_count; ++i) {
        log::info() << "Entry " << i << ": " << g_memory_map[i].type_string();
    }
    log::info() << "========================================\n";
}

/* Dump kernel heap allocations */
void memory::dump_heap() {
    log::info() << "Kernel Heap Allocations:";
    log::info() << "========================================";
    log::info() << "  Address          Size       Status";
    log::info() << "  ---------------  ---------  -------";
    
    auto block = g_kernel_heap.get_free_list();
    u64 used_total = 0;
    u64 free_total = 0;
    u32 used_count = 0;
    u32 free_count = 0;
    
    while (block != null) {
        log::info() << "  0x"
                    << log::hex(static_cast<u64>(reinterpret_cast<u64>(block->data())), 1, false, false)
                    << "  " << static_cast<unsigned long long>(block->size)
                    << "  " << (block->used ? "USED" : "FREE");
        
        if (block->used) {
            used_total += block->size;
            used_count++;
        } else {
            free_total += block->size;
            free_count++;
        }

        block = block->next;
    }
    
    log::info() << "----------------------------------------";
    log::info() << "  Total Used:  " << used_count << " blocks, " << used_total << " bytes";
    log::info() << "  Total Free:  " << free_count << " blocks, " << free_total << " bytes";
    log::info() << "  Capacity:    " << g_kernel_heap.total_bytes() << " bytes across "
                << g_kernel_heap.subheap_count() << " subheap(s)";
    log::info() << "========================================\n\n";
}

} // namespace vk
