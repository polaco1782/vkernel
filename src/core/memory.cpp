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

/* Physical allocator state */
phys_allocator g_phys_alloc;

/* Kernel heap - 64 MB initial heap */
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
    
    /* Add conventional memory to the free pool */
    for (const auto& entry : map) {
        if (entry.type == memory_type::conventional) {
            /* TODO: Add to free list */
            total_pages_ += entry.number_of_pages;
            free_pages_ += entry.number_of_pages;
        }
    }

#if VK_DEBUG_LEVEL >= 4
    console::puts("[DEBUG] phys_alloc: ");
    console::put_dec(total_pages_);
    console::puts(" pages free (");
    console::put_dec((total_pages_ * PAGE_SIZE_4K) / (1024 * 1024));
    console::puts(" MB)\n");
#endif

    return status_code::success;
}

/* Allocate physical pages */
auto phys_allocator::allocate_pages(u32 page_count, u32 alignment) -> phys_addr {
    /* TODO: Implement proper page allocation */
    (void)page_count;
    (void)alignment;
    
    return 0;
}

/* Free physical pages */
void phys_allocator::free_pages(phys_addr addr, u32 page_count) {
    /* TODO: Implement page deallocation */
    (void)addr;
    (void)page_count;
}

/* Kernel heap initialization */
auto kernel_heap::init(void* base, size_phys size) -> status_code {
    free_list_ = reinterpret_cast<heap_block*>(base);
    free_list_->size = size - sizeof(heap_block);
    free_list_->used = false;
    free_list_->next = null;
    free_list_->prev = null;

#if VK_DEBUG_LEVEL >= 4
    console::puts("[DEBUG] heap: base=0x");
    console::put_hex(reinterpret_cast<u64>(base));
    console::puts(", capacity=");
    console::put_dec(size / (1024 * 1024));
    console::puts(" MB\n");
#endif

    return status_code::success;
}

/* Kernel heap allocation */
auto kernel_heap::allocate(size_phys size) -> void* {
    if (size == 0) {
        return null;
    }
    
    /* Align size to 8 bytes */
    size = align_up(size, 8uz);
    
    /* Find a free block that fits */
    auto block = free_list_;
    while (block != null) {
        if (!block->used && block->size >= size) {
            /* Found a suitable block */
            block->used = true;
            
            /* Split the block if there's enough remaining space */
            if (block->size > size + sizeof(heap_block) + 8) {
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
    
    log::info("Memory subsystem initialized");
    /* TODO: Print page count with variadic logging */
    (void)g_phys_alloc.total_pages();
    
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
    console::puts("\nMemory Map:\n");
    console::puts("========================================\n");
    
    for (u32 i = 0; i < g_memory_map_count; ++i) {
        console::puts("Entry ");
        char buf[8];
        buf[0] = static_cast<char>('0' + (i / 100));
        buf[1] = static_cast<char>('0' + ((i / 10) % 10));
        buf[2] = static_cast<char>('0' + (i % 10));
        buf[3] = '\0';
        console::puts(buf);
        console::puts(": ");
        console::puts(g_memory_map[i].type_string());
        console::puts("\n");
    }
    
    console::puts("========================================\n\n");
}

/* Dump kernel heap allocations */
void memory::dump_heap() {
    console::puts("\nKernel Heap Allocations:\n");
    console::puts("========================================\n");
    console::puts("  Address          Size       Status\n");
    console::puts("  ---------------  ---------  -------\n");
    
    auto block = g_kernel_heap.get_free_list();
    u64 used_total = 0;
    u64 free_total = 0;
    u32 used_count = 0;
    u32 free_count = 0;
    
    while (block != null) {
        console::puts("  0x");
        console::put_hex(reinterpret_cast<u64>(block->data()));
        console::puts("  ");
        
        if (block->size < 1000) {
            console::putc(' ');
        }
        if (block->size < 100) {
            console::putc(' ');
        }
        if (block->size < 10) {
            console::putc(' ');
        }
        console::put_dec(block->size);
        
        console::puts("  ");
        if (block->used) {
            console::puts("USED\n");
            used_total += block->size;
            used_count++;
        } else {
            console::puts("FREE\n");
            free_total += block->size;
            free_count++;
        }
        
        block = block->next;
    }
    
    console::puts("----------------------------------------\n");
    console::puts("  Total Used:  ");
    console::put_dec(used_count);
    console::puts(" blocks, ");
    console::put_dec(used_total);
    console::puts(" bytes\n");
    console::puts("  Total Free:  ");
    console::put_dec(free_count);
    console::puts(" blocks, ");
    console::put_dec(free_total);
    console::puts(" bytes\n");
    console::puts("========================================\n\n");
}

} // namespace vk
