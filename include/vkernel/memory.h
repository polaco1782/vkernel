/*
 * vkernel - UEFI Microkernel
 * Copyright (C) 2026 vkernel authors
 *
 * memory.h - Memory management (freestanding C++26)
 */

#ifndef VKERNEL_MEMORY_H
#define VKERNEL_MEMORY_H

#include "types.h"

namespace vk {

/* ============================================================
 * Freestanding span (no <span>)
 * ============================================================ */

template<typename T>
class span {
public:
    constexpr span() noexcept : data_(null), size_(0) {}
    constexpr span(T* data, usize count) noexcept
        : data_(data), size_(count) {}

    template<usize N>
    constexpr span(T (&arr)[N]) noexcept : data_(arr), size_(N) {}

    /* Allow non-const to const conversion */
    template<typename U, typename = enable_if_t<is_same_v<T, const U>>>
    constexpr span(span<U> other) noexcept
        : data_(other.data()), size_(other.size()) {}

    [[nodiscard]] constexpr auto data() const noexcept -> T*       { return data_; }
    [[nodiscard]] constexpr auto size() const noexcept -> usize { return size_; }
    [[nodiscard]] constexpr auto empty() const noexcept -> bool   { return size_ == 0; }

    [[nodiscard]] constexpr auto operator[](usize i) const noexcept -> T& {
        return data_[i];
    }

    [[nodiscard]] constexpr auto begin() const noexcept -> T*       { return data_; }
    [[nodiscard]] constexpr auto end() const noexcept -> T*         { return data_ + size_; }

private:
    T* data_;
    usize size_;
};

/* ============================================================
 * Memory map entry types
 * ============================================================ */

enum class memory_type : u32 {
    reserved = 0,
    loader_code,
    loader_data,
    boot_services_code,
    boot_services_data,
    runtime_services_code,
    runtime_services_data,
    conventional,
    unusable,
    acpi_reclaimable,
    nvs,
    memory_mapped_io,
    memory_mapped_io_port_space,
    pal_code,
    persistent,
    count
};

/* Memory attributes */
enum class memory_attr : u64 {
    uncached       = 0x0000000000000001ULL,
    write_combined = 0x0000000000000002ULL,
    write_through  = 0x0000000000000004ULL,
    write_back     = 0x0000000000000008ULL,
    more_reliable  = 0x0000000000000010ULL,
    readonly       = 0x0000000000000020ULL,
    runtime        = 0x8000000000000000ULL,
};

[[nodiscard]] constexpr auto operator&(memory_attr a, memory_attr b) -> bool {
    return (static_cast<u64>(a) & static_cast<u64>(b)) != 0;
}

/* ============================================================
 * Memory map entry
 * ============================================================ */

struct memory_map_entry {
    phys_addr physical_start;
    virt_addr virtual_start;
    size_phys number_of_pages;
    memory_type type;
    u64 attribute;

    [[nodiscard]] constexpr auto end_address() const -> phys_addr {
        return physical_start + (number_of_pages * PAGE_SIZE_4K);
    }

    [[nodiscard]] constexpr auto contains(phys_addr addr) const -> bool {
        return addr >= physical_start && addr < end_address();
    }

    [[nodiscard]] auto type_string() const -> const char*;
};

/* ============================================================
 * Memory region (intrusive linked list)
 * ============================================================ */

struct memory_region {
    phys_addr start;
    size_phys size;
    bool used;
    memory_region* next;

    [[nodiscard]] constexpr auto end() const -> phys_addr {
        return start + size;
    }

    [[nodiscard]] constexpr auto can_allocate(size_phys req_size) const -> bool {
        return !used && size >= req_size;
    }
};

/* ============================================================
 * Physical memory allocator
 * ============================================================ */

class phys_allocator {
public:
    phys_allocator() = default;

    auto init(span<const memory_map_entry> map) -> status_code;

    /* Allocate page_count pages with the given byte alignment.
     * If max_addr != 0 the returned address + size must be <= max_addr. */
    auto allocate_pages(u32 page_count,
                        u32       alignment = PAGE_SIZE_4K,
                        phys_addr max_addr  = 0) -> phys_addr;
    void free_pages(phys_addr addr, u32 page_count);

    [[nodiscard]] auto total_pages() const -> size_phys { return total_pages_; }
    [[nodiscard]] auto used_pages() const -> size_phys { return used_pages_; }
    [[nodiscard]] auto free_pages() const -> size_phys { return free_pages_; }

private:
    memory_region* free_list_ = null;
    size_phys total_pages_ = 0;
    size_phys used_pages_ = 0;
    size_phys free_pages_ = 0;
};

/* ============================================================
 * Heap block (intrusive doubly-linked list)
 * ============================================================ */

struct heap_block {
    size_phys size;
    bool used;
    heap_block* next;
    heap_block* prev;

    [[nodiscard]] auto data() -> void* {
        return reinterpret_cast<void*>(
            reinterpret_cast<u8*>(this) + sizeof(heap_block));
    }

    [[nodiscard]] auto end() -> heap_block* {
        return reinterpret_cast<heap_block*>(
            reinterpret_cast<u8*>(this) + sizeof(heap_block) + size);
    }
};

/* ============================================================
 * Kernel heap allocator
 * ============================================================ */

class kernel_heap {
public:
    kernel_heap() = default;

    auto init(void* base, size_phys size) -> status_code;

    auto allocate(size_phys size) -> void*;

    auto allocate_zero(size_phys size) -> void*;

    void free(void* ptr);

    /* For diagnostic purposes only (not thread-safe) */
    [[nodiscard]] auto get_free_list() const -> heap_block* { return free_list_; }

private:
    heap_block* free_list_ = null;
};

/* Global allocator instances */
extern phys_allocator g_phys_alloc;
extern kernel_heap g_kernel_heap;

/* ============================================================
 * Memory utilities
 * ============================================================ */

namespace memory {

auto init(span<const memory_map_entry> map) -> status_code;

void* memory_set(void* dest, i32 c, size_phys n);
void* memory_copy(void* dest, const void* src, size_phys n);
i32 memory_compare(const void* s1, const void* s2, size_phys n);
void* memory_move(void* dest, const void* src, size_phys n);

auto get_memory_map() -> span<const memory_map_entry>;
auto find_entry(phys_addr addr) -> const memory_map_entry*;

void dump_map();
void dump_heap();

} // namespace memory

} // namespace vk

#endif /* VKERNEL_MEMORY_H */
