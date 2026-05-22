/*
 * vkernel - UEFI Microkernel
 * Copyright (C) 2026 vkernel authors
 *
 * virtual_memory.cpp - Minimal x86_64 page-table manager.
 */

#include "virtual_memory.h"

#include "arch/x86_64/arch.h"
#include "log.h"
#include "memory.h"

namespace vk {
namespace vm {

namespace {

constexpr u64 PA_MASK = 0x000FFFFFFFFFF000ULL;
constexpr u32 ENTRIES_PER_TABLE = 512;
constexpr u32 USER_PML4_FIRST = static_cast<u32>((USER_IMAGE_BASE >> 39) & 0x1FF);
constexpr u32 USER_PML4_LIMIT = static_cast<u32>((USER_MAP_LIMIT >> 39) & 0x1FF);
/* Keep paging structures inside the identity-mapped bootstrap window. */
constexpr phys_addr IDENTITY_BRIDGE_ALLOC_MAX = 0x80000000ULL;

phys_addr g_kernel_cr3 = 0;

[[nodiscard]] auto table_from_phys(phys_addr phys) -> u64*
{
    return reinterpret_cast<u64*>(static_cast<usize>(phys & PA_MASK));
}

[[nodiscard]] auto page_flags(u64 flags) -> u64
{
    u64 entry = arch::PTE_PRESENT;
    if ((flags & MAP_WRITABLE) != 0) {
        entry |= arch::PTE_WRITABLE;
    }
    if ((flags & MAP_USER) != 0) {
        entry |= arch::PTE_USER;
    }
    if ((flags & MAP_EXECUTABLE) == 0) {
        entry |= arch::PTE_XD;
    }
    return entry;
}

[[nodiscard]] auto alloc_table_page() -> phys_addr
{
    const phys_addr phys = g_phys_alloc.allocate_pages(1, PAGE_SIZE_4K, IDENTITY_BRIDGE_ALLOC_MAX);
    if (phys == 0) {
        return 0;
    }

    arch::make_region_writable(phys, PAGE_SIZE_4K);
    /* Fresh page tables must start zeroed before we publish parent entries. */
    memory::set(reinterpret_cast<void*>(static_cast<usize>(phys)), 0, PAGE_SIZE_4K);
    return phys;
}

[[nodiscard]] auto ensure_next_table(u64* table, u32 index, u64 flags) -> u64*
{
    if ((table[index] & arch::PTE_PRESENT) == 0) {
        const phys_addr child = alloc_table_page();
        if (child == 0) {
            return null;
        }
        table[index] = child | arch::PTE_PRESENT | arch::PTE_WRITABLE | (flags & arch::PTE_USER);
    } else if ((table[index] & arch::PTE_HUGE) != 0) {
        return null;
    } else {
        table[index] |= arch::PTE_WRITABLE | (flags & arch::PTE_USER);
    }

    return table_from_phys(table[index]);
}

void free_page_table_tree(u64* table, int level)
{
    if (table == null || level <= 0) {
        return;
    }

    for (u32 index = 0; index < ENTRIES_PER_TABLE; ++index) {
        const u64 entry = table[index];
        if ((entry & arch::PTE_PRESENT) == 0) {
            continue;
        }
        if ((entry & arch::PTE_HUGE) != 0) {
            continue;
        }

        auto* child = table_from_phys(entry);
        free_page_table_tree(child, level - 1);
        /* Only free paging structures here; mapped leaf pages are owned elsewhere. */
        if (level > 1) {
            g_phys_alloc.free_pages(entry & PA_MASK, 1);
        }
        table[index] = 0;
    }
}

} // namespace

void init()
{
    g_kernel_cr3 = arch::read_cr3() & PA_MASK;
    log::info() << "vm: kernel CR3=" << reinterpret_cast<const void*>(static_cast<usize>(g_kernel_cr3));
}

auto kernel_cr3() -> phys_addr
{
    return g_kernel_cr3;
}

auto create_address_space() -> address_space*
{
    if (g_kernel_cr3 == 0) {
        init();
    }

    auto* as = static_cast<address_space*>(g_kernel_heap.allocate_zero(sizeof(address_space)));
    if (as == null) {
        return null;
    }

    const phys_addr pml4_phys = alloc_table_page();
    if (pml4_phys == 0) {
        g_kernel_heap.free(as);
        return null;
    }

    auto* dst = table_from_phys(pml4_phys);
    auto* src = table_from_phys(g_kernel_cr3);
    /* Clone the kernel half, then clear the user slot range. */
    memory::copy(dst, src, PAGE_SIZE_4K);

    for (u32 index = USER_PML4_FIRST; index < USER_PML4_LIMIT; ++index) {
        dst[index] = 0;
    }

    as->pml4_phys = pml4_phys;
    as->heap_next = USER_HEAP_BASE;
    log::debug() << "vm: create_address_space as=" << reinterpret_cast<const void*>(as)
                 << " pml4=" << reinterpret_cast<const void*>(static_cast<usize>(as->pml4_phys))
                 << " heap_next=" << reinterpret_cast<const void*>(static_cast<usize>(as->heap_next));
    return as;
}

void destroy_address_space(address_space* as)
{
    if (as == null) {
        return;
    }

    if (as->pml4_phys != 0) {
        auto* pml4 = table_from_phys(as->pml4_phys);
        /* Tear down only user-owned branches; kernel mappings stay shared. */
        for (u32 index = USER_PML4_FIRST; index < USER_PML4_LIMIT; ++index) {
            if ((pml4[index] & arch::PTE_PRESENT) != 0 && (pml4[index] & arch::PTE_HUGE) == 0) {
                auto* pdpt = table_from_phys(pml4[index]);
                free_page_table_tree(pdpt, 3);
                g_phys_alloc.free_pages(pml4[index] & PA_MASK, 1);
                pml4[index] = 0;
            }
        }
        g_phys_alloc.free_pages(as->pml4_phys, 1);
    }

    log::debug() << "vm: destroy_address_space as=" << reinterpret_cast<const void*>(as);
    g_kernel_heap.free(as);
}

void activate_kernel()
{
    if (g_kernel_cr3 != 0 && active_cr3() != g_kernel_cr3) {
        arch::write_cr3(g_kernel_cr3);
    }
}

void activate(address_space* as)
{
    const phys_addr cr3 = (as != null && as->pml4_phys != 0) ? as->pml4_phys : g_kernel_cr3;
    if (cr3 != 0 && active_cr3() != cr3) {
        arch::write_cr3(cr3);
    }
}

auto active_cr3() -> phys_addr
{
    return arch::read_cr3() & PA_MASK;
}

auto debug_resolve(address_space* as,
                   virt_addr virt,
                   phys_addr* out_phys,
                   u64* out_flags) -> bool
{
    if (out_phys != null) {
        *out_phys = 0;
    }
    if (out_flags != null) {
        *out_flags = 0;
    }
    if (as == null || as->pml4_phys == 0) {
        return false;
    }

    const u32 pml4_idx = static_cast<u32>((virt >> 39) & 0x1FF);
    const u32 pdpt_idx = static_cast<u32>((virt >> 30) & 0x1FF);
    const u32 pd_idx   = static_cast<u32>((virt >> 21) & 0x1FF);
    const u32 pt_idx   = static_cast<u32>((virt >> 12) & 0x1FF);

    auto* pml4 = table_from_phys(as->pml4_phys);
    const u64 pml4e = pml4[pml4_idx];
    if ((pml4e & arch::PTE_PRESENT) == 0 || (pml4e & arch::PTE_HUGE) != 0) {
        return false;
    }

    auto* pdpt = table_from_phys(pml4e);
    const u64 pdpte = pdpt[pdpt_idx];
    if ((pdpte & arch::PTE_PRESENT) == 0) {
        return false;
    }
    if ((pdpte & arch::PTE_HUGE) != 0) {
        if (out_phys != null) {
            *out_phys = (pdpte & PA_MASK) + (virt & ((1ULL << 30) - 1));
        }
        if (out_flags != null) {
            *out_flags = pdpte & ~PA_MASK;
        }
        return true;
    }

    auto* pd = table_from_phys(pdpte);
    const u64 pde = pd[pd_idx];
    if ((pde & arch::PTE_PRESENT) == 0) {
        return false;
    }
    if ((pde & arch::PTE_HUGE) != 0) {
        if (out_phys != null) {
            *out_phys = (pde & PA_MASK) + (virt & ((1ULL << 21) - 1));
        }
        if (out_flags != null) {
            *out_flags = pde & ~PA_MASK;
        }
        return true;
    }

    auto* pt = table_from_phys(pde);
    const u64 pte = pt[pt_idx];
    if ((pte & arch::PTE_PRESENT) == 0) {
        return false;
    }

    if (out_phys != null) {
        *out_phys = (pte & PA_MASK) + (virt & (PAGE_SIZE_4K - 1));
    }
    if (out_flags != null) {
        *out_flags = pte & ~PA_MASK;
    }
    return true;
}

auto map_page(address_space* as, virt_addr virt, phys_addr phys, u64 flags) -> bool
{
    if (as == null || as->pml4_phys == 0 || phys == 0) {
        log::debug() << "vm: map_page invalid as=" << reinterpret_cast<const void*>(as)
                     << " virt=" << reinterpret_cast<const void*>(static_cast<usize>(virt))
                     << " phys=" << reinterpret_cast<const void*>(static_cast<usize>(phys));
        return false;
    }
    if (!is_aligned(virt, PAGE_SIZE_4K) || !is_aligned(phys, PAGE_SIZE_4K)) {
        log::debug() << "vm: map_page unaligned virt=" << reinterpret_cast<const void*>(static_cast<usize>(virt))
                     << " phys=" << reinterpret_cast<const void*>(static_cast<usize>(phys));
        return false;
    }

    const u32 pml4_idx = static_cast<u32>((virt >> 39) & 0x1FF);
    const u32 pdpt_idx = static_cast<u32>((virt >> 30) & 0x1FF);
    const u32 pd_idx   = static_cast<u32>((virt >> 21) & 0x1FF);
    const u32 pt_idx   = static_cast<u32>((virt >> 12) & 0x1FF);

    auto* pml4 = table_from_phys(as->pml4_phys);
    auto* pdpt = ensure_next_table(pml4, pml4_idx, flags & MAP_USER ? arch::PTE_USER : 0);
    if (pdpt == null) {
        log::debug() << "vm: map_page failed pml4->pdpt virt="
                     << reinterpret_cast<const void*>(static_cast<usize>(virt));
        return false;
    }
    auto* pd = ensure_next_table(pdpt, pdpt_idx, flags & MAP_USER ? arch::PTE_USER : 0);
    if (pd == null) {
        log::debug() << "vm: map_page failed pdpt->pd virt="
                     << reinterpret_cast<const void*>(static_cast<usize>(virt));
        return false;
    }
    auto* pt = ensure_next_table(pd, pd_idx, flags & MAP_USER ? arch::PTE_USER : 0);
    if (pt == null) {
        log::debug() << "vm: map_page failed pd->pt virt="
                     << reinterpret_cast<const void*>(static_cast<usize>(virt));
        return false;
    }

    /* Overwrite-friendly by design so shared windows can be remapped in place. */
    pt[pt_idx] = (phys & PA_MASK) | page_flags(flags);
    if (active_cr3() == as->pml4_phys) {
        arch::invlpg(virt);
    }
    return true;
}

auto map_contiguous(address_space* as,
                    virt_addr virt,
                    phys_addr phys,
                    usize size,
                    u64 flags) -> bool
{
    const usize bytes = align_up(size, PAGE_SIZE_4K);
    for (usize offset = 0; offset < bytes; offset += PAGE_SIZE_4K) {
        if (!map_page(as, virt + offset, phys + offset, flags)) {
            log::debug() << "vm: map_contiguous failed as=" << reinterpret_cast<const void*>(as)
                         << " virt=" << reinterpret_cast<const void*>(static_cast<usize>(virt))
                         << " phys=" << reinterpret_cast<const void*>(static_cast<usize>(phys))
                         << " offset=" << static_cast<unsigned long long>(offset)
                         << " bytes=" << static_cast<unsigned long long>(bytes);
            return false;
        }
    }
    return true;
}

void unmap_range(address_space* as, virt_addr virt, usize size)
{
    if (as == null || as->pml4_phys == 0 || size == 0) {
        return;
    }

    const virt_addr start = align_down(virt, PAGE_SIZE_4K);
    const virt_addr end = align_up(virt + size, PAGE_SIZE_4K);
    auto* pml4 = table_from_phys(as->pml4_phys);

    /* Empty intermediate tables are left in place; callers churn this range often. */
    for (virt_addr addr = start; addr < end; addr += PAGE_SIZE_4K) {
        const u32 pml4_idx = static_cast<u32>((addr >> 39) & 0x1FF);
        const u32 pdpt_idx = static_cast<u32>((addr >> 30) & 0x1FF);
        const u32 pd_idx   = static_cast<u32>((addr >> 21) & 0x1FF);
        const u32 pt_idx   = static_cast<u32>((addr >> 12) & 0x1FF);

        if ((pml4[pml4_idx] & arch::PTE_PRESENT) == 0) {
            continue;
        }
        auto* pdpt = table_from_phys(pml4[pml4_idx]);
        if ((pdpt[pdpt_idx] & arch::PTE_PRESENT) == 0 || (pdpt[pdpt_idx] & arch::PTE_HUGE) != 0) {
            continue;
        }
        auto* pd = table_from_phys(pdpt[pdpt_idx]);
        if ((pd[pd_idx] & arch::PTE_PRESENT) == 0 || (pd[pd_idx] & arch::PTE_HUGE) != 0) {
            continue;
        }
        auto* pt = table_from_phys(pd[pd_idx]);
        pt[pt_idx] = 0;
        if (active_cr3() == as->pml4_phys) {
            arch::invlpg(addr);
        }
    }
}

auto allocate_user_pages(address_space* as,
                         virt_addr virt,
                         usize size,
                         u64 flags,
                         phys_addr* out_phys) -> bool
{
    if (out_phys != null) {
        *out_phys = 0;
    }
    if (as == null || size == 0 || !is_aligned(virt, PAGE_SIZE_4K)) {
        log::debug() << "vm: allocate_user_pages invalid as=" << reinterpret_cast<const void*>(as)
                     << " virt=" << reinterpret_cast<const void*>(static_cast<usize>(virt))
                     << " size=" << static_cast<unsigned long long>(size);
        return false;
    }

    const usize bytes = align_up(size, PAGE_SIZE_4K);
    const u32 page_count = static_cast<u32>(bytes / PAGE_SIZE_4K);
    log::debug() << "vm: allocate_user_pages request as=" << reinterpret_cast<const void*>(as)
                 << " virt=" << reinterpret_cast<const void*>(static_cast<usize>(virt))
                 << " bytes=" << static_cast<unsigned long long>(bytes)
                 << " pages=" << static_cast<unsigned long long>(page_count)
                 << " flags=" << log::hex(static_cast<u64>(flags), 1, true, false);
    const phys_addr phys = g_phys_alloc.allocate_pages(page_count, PAGE_SIZE_4K, IDENTITY_BRIDGE_ALLOC_MAX);
    if (phys == 0) {
        log::debug() << "vm: allocate_user_pages physical allocation failed pages="
                     << static_cast<unsigned long long>(page_count);
        return false;
    }

    arch::make_region_writable(phys, bytes);
    /* Zero the backing pages before the process can observe the mapping. */
    memory::set(reinterpret_cast<void*>(static_cast<usize>(phys)), 0, bytes);
    if (!map_contiguous(as, virt, phys, bytes, flags)) {
        log::debug() << "vm: allocate_user_pages map failed virt="
                     << reinterpret_cast<const void*>(static_cast<usize>(virt))
                     << " phys=" << reinterpret_cast<const void*>(static_cast<usize>(phys));
        g_phys_alloc.free_pages(phys, page_count);
        return false;
    }

    if (out_phys != null) {
        *out_phys = phys;
    }
    log::debug() << "vm: allocate_user_pages mapped as=" << reinterpret_cast<const void*>(as)
                 << " virt=" << reinterpret_cast<const void*>(static_cast<usize>(virt))
                 << " phys=" << reinterpret_cast<const void*>(static_cast<usize>(phys))
                 << " bytes=" << static_cast<unsigned long long>(bytes);
    return true;
}

auto allocate_user_heap(address_space* as,
                        usize size,
                        u64 flags,
                        phys_addr* out_phys,
                        virt_addr* out_virt) -> bool
{
    if (as == null || size == 0) {
        return false;
    }

    const usize bytes = align_up(size, PAGE_SIZE_4K);
    const virt_addr virt = align_up(as->heap_next, PAGE_SIZE_4K);
    if (virt < USER_HEAP_BASE || virt + bytes > USER_MAP_LIMIT || virt + bytes < virt) {
        log::debug() << "vm: allocate_user_heap range failed as=" << reinterpret_cast<const void*>(as)
                     << " heap_next=" << reinterpret_cast<const void*>(static_cast<usize>(as->heap_next))
                     << " bytes=" << static_cast<unsigned long long>(bytes);
        return false;
    }

    if (!allocate_user_pages(as, virt, bytes, flags, out_phys)) {
        log::debug() << "vm: allocate_user_heap page allocation failed as=" << reinterpret_cast<const void*>(as)
                     << " virt=" << reinterpret_cast<const void*>(static_cast<usize>(virt))
                     << " bytes=" << static_cast<unsigned long long>(bytes);
        return false;
    }

    as->heap_next = virt + bytes;
    if (out_virt != null) {
        *out_virt = virt;
    }
    log::debug() << "vm: allocate_user_heap as=" << reinterpret_cast<const void*>(as)
                 << " virt=" << reinterpret_cast<const void*>(static_cast<usize>(virt))
                 << " bytes=" << static_cast<unsigned long long>(bytes)
                 << " next=" << reinterpret_cast<const void*>(static_cast<usize>(as->heap_next));
    return true;
}

} // namespace vm
} // namespace vk
