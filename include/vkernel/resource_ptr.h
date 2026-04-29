#ifndef VKERNEL_RESOURCE_PTR_H
#define VKERNEL_RESOURCE_PTR_H

#include "memory.h"
#include "unique_ptr.h"

namespace vk {

struct kernel_heap_deleter {
    void operator()(void* ptr) const noexcept {
        if (ptr != null) {
            g_kernel_heap.free(ptr);
        }
    }
};

struct physical_pages_deleter {
    u32 page_count = 0;

    void operator()(void* ptr) const noexcept {
        if (ptr != null && page_count != 0) {
            g_phys_alloc.free_pages(reinterpret_cast<phys_addr>(ptr), page_count);
        }
    }
};

struct kernel_allocation_deleter {
    usize size = 0;
    bool from_phys = false;

    void operator()(void* ptr) const noexcept {
        if (ptr == null) {
            return;
        }

        if (from_phys) {
            u32 page_count = static_cast<u32>((size + PAGE_SIZE_4K - 1) / PAGE_SIZE_4K);
            g_phys_alloc.free_pages(reinterpret_cast<phys_addr>(ptr), page_count);
            return;
        }

        g_kernel_heap.free(ptr);
    }
};

template<typename T>
using kernel_heap_ptr = unique_ptr<T, kernel_heap_deleter>;

template<typename T>
using physical_pages_ptr = unique_ptr<T, physical_pages_deleter>;

template<typename T>
using kernel_allocation_ptr = unique_ptr<T, kernel_allocation_deleter>;

} // namespace vk

#endif /* VKERNEL_RESOURCE_PTR_H */
