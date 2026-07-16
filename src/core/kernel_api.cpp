/*
 * vkernel - UEFI Microkernel
 * Copyright (C) 2026 vkernel authors
 *
 * kernel_api.cpp - Kernel-side API table and user-facing stubs
 */

#include "config.h"
#include "types.h"
#include "console.h"
#include "log.h"
#include "memory.h"
#include "fs.h"
#include "framebuffer.h"
#include "input.h"
#include "scheduler.h"
#include "sound.h"
#include "process.h"
#include "driver.h"
#include "kobj.h"
#include "kobj/detail.h"
#include "vk.h"
#include "process_internal.h"
#include "virtual_memory.h"
#include "arch/x86_64/arch.h"

namespace vk {
namespace kernel_api {

static auto current_console_interface() -> process::console_interface;
static auto validate_fb(const vk_framebuffer_info_t* fb) -> bool;
static void enqueue_framebuffer_event(process::process_task_context* target,
                                      vk_u32 type,
                                      const vk_framebuffer_info_t& framebuffer);
static int stub_set_framebuffer_resize_events(vk_u32 enabled);
static int stub_task_accepts_framebuffer_resize(vk_u64 task_id);
static int stub_set_startup_window_size(vk_u32 width, vk_u32 height);
static int stub_get_task_startup_window_size(vk_u64 task_id, vk_u32* out_width, vk_u32* out_height);
static auto route_getc() -> char;
static void route_putc(char c);
static void route_puts(const char* str);
static auto route_stdio_write(const char* buf, vk_usize len) -> vk_usize;
static auto dequeue_ascii_key(process::process_task_context* ctx) -> char;
static void copy_cstr(char* out, usize out_cap, const char* src);
static auto track_process_allocation(process::process_task_context* ctx,
                                     void* user_ptr,
                                     void* raw_ptr,
                                     usize requested_size,
                                     usize allocated_size,
                                     bool from_phys) -> bool;
static auto fill_stat_for_handle(vk_i32 fd, vk_stat_t* out) -> vk_i64;
static auto fill_stat_for_path(const char* path, vk_stat_t* out) -> vk_i64;
static auto allocate_process_mapping(process::process_task_context* ctx,
                                     usize size,
                                     u64 flags,
                                     virt_addr min_addr,
                                     virt_addr* out_virt,
                                     phys_addr* out_phys) -> bool;
static auto track_process_mapping(process::process_task_context* ctx,
                                  virt_addr virt,
                                  phys_addr phys,
                                  usize size) -> bool;
static auto untrack_process_mapping(process::process_task_context* ctx,
                                    virt_addr virt,
                                    usize size,
                                    process::process_allocation** out_alloc) -> bool;
static auto protect_process_mapping(process::process_task_context* ctx,
                                    virt_addr virt,
                                    usize size,
                                    vk_u32 prot) -> bool;
static auto adjust_process_brk(process::process_task_context* ctx,
                               virt_addr requested_break) -> vk_i64;
static auto syscall_read_fd(vk_i32 fd, void* buf, usize len) -> vk_i64;
static auto syscall_write_fd(vk_i32 fd, const void* buf, usize len) -> vk_i64;
static auto sleep_from_timespec(vk_u64 clock_id,
                                vk_u64 flags,
                                const vk_timespec_t* req,
                                vk_timespec_t* rem) -> vk_i64;
static auto clock_id_uses_scheduler_ticks(vk_u64 clock_id) -> bool;
static auto stub_syscall(vk_u64 nr,
                         vk_u64 a1,
                         vk_u64 a2,
                         vk_u64 a3,
                         vk_u64 a4,
                         vk_u64 a5,
                         vk_u64 a6) -> vk_i64;

/* Named statics keep vk_api_t entries addressable without trampolines. */

static constexpr vk_u32 VK_STAT_MODE_IFCHR = 0020000u;
static constexpr vk_u32 VK_STAT_MODE_IFREG = 0100000u;
static constexpr vk_u32 VK_STAT_MODE_RW_RW_RW = 0666u;
static constexpr vk_i32 VK_AT_FDCWD = -100;
static constexpr vk_i32 VK_AT_EMPTY_PATH = 0x1000;
static constexpr vk_i32 VK_AT_SYMLINK_NOFOLLOW = 0x100;
static constexpr vk_u32 VK_TIMER_ABSTIME = 1u;
static constexpr vk_u64 VK_CLOCK_PROCESS_CPUTIME_ID = 2u;

struct linux_iovec {
    void* iov_base;
    usize iov_len;
};

/* ---- memory ---- */

static constexpr usize PROCESS_ALLOC_GUARD_SIZE = PAGE_SIZE_4K;

static void trace_process_buffer(const char* label,
                                 process::process_task_context* ctx,
                                 const void* ptr,
                                 usize size) {
    if (label == null || ctx == null || ptr == null || ctx->address_space == null) {
        return;
    }

    const virt_addr start = reinterpret_cast<virt_addr>(ptr);
    const virt_addr end = size > 0 ? start + size - 1 : start;
    phys_addr start_phys = 0;
    phys_addr end_phys = 0;
    u64 start_flags = 0;
    u64 end_flags = 0;
    const bool start_ok = vm::debug_resolve(ctx->address_space, start, &start_phys, &start_flags);
    const bool end_ok = vm::debug_resolve(ctx->address_space, end, &end_phys, &end_flags);

    log::debug() << "vmtrace: task=" << static_cast<unsigned long long>(sched::current_task_id())
                 << " name=" << sched::current_task_name()
                 << " " << label
                 << " range=" << reinterpret_cast<const void*>(static_cast<usize>(start))
                 << ".." << reinterpret_cast<const void*>(static_cast<usize>(end))
                 << " size=" << static_cast<unsigned long long>(size)
                 << " start_ok=" << (start_ok ? "yes" : "no")
                 << " start_phys=" << reinterpret_cast<const void*>(static_cast<usize>(start_phys))
                 << " start_flags=" << log::hex(start_flags, 1, true, false)
                 << " end_ok=" << (end_ok ? "yes" : "no")
                 << " end_phys=" << reinterpret_cast<const void*>(static_cast<usize>(end_phys))
                 << " end_flags=" << log::hex(end_flags, 1, true, false);
}

static void free_process_allocation_pages(void* ptr, usize size) {
    if (ptr == null || size == 0) {
        return;
    }

    u32 page_count = static_cast<u32>((size + PAGE_SIZE_4K - 1) / PAGE_SIZE_4K);
    g_phys_alloc.free_pages(reinterpret_cast<phys_addr>(ptr), page_count);
}

static auto track_process_allocation(process::process_task_context* ctx,
                                     void* user_ptr,
                                     void* raw_ptr,
                                     usize requested_size,
                                     usize allocated_size,
                                     bool from_phys) -> bool {
    if (ctx == null || user_ptr == null || raw_ptr == null || allocated_size == 0) {
        return false;
    }

    auto* rec = static_cast<process::process_allocation*>(
        g_kernel_heap.allocate_zero(sizeof(process::process_allocation)));
    if (rec == null) {
        return false;
    }

    rec->user_ptr = user_ptr;
    rec->raw_ptr = raw_ptr;
    rec->requested_size = requested_size;
    rec->allocated_size = allocated_size;
    rec->from_phys = from_phys;
    rec->next = ctx->allocations;
    ctx->allocations = rec;
    return true;
}

static auto path_inode(const char* path) -> u64 {
    if (path == null) {
        return 1;
    }

    const auto* cur = reinterpret_cast<const unsigned char*>(path);
    u64 hash = 1469598103934665603ULL;
    while (*cur != '\0') {
        hash ^= static_cast<u64>(*cur++);
        hash *= 1099511628211ULL;
    }
    return hash != 0 ? hash : 1;
}

static auto fd_to_handle(vk_i32 fd, fs::file_handle* out_handle) -> bool {
    if (out_handle != null) {
        *out_handle = 0;
    }
    if (fd <= 2) {
        return false;
    }

    const auto handle = static_cast<fs::file_handle>(fd - 2);
    if (handle == 0) {
        return false;
    }
    if (out_handle != null) {
        *out_handle = handle;
    }
    return true;
}

static auto file_size_for_handle(fs::file_handle handle, u64* out_size) -> bool {
    if (out_size != null) {
        *out_size = 0;
    }

    const i64 original = fs::file_tell(handle);
    if (original < 0) {
        return false;
    }
    if (fs::file_seek(handle, 0, VK_SEEK_END) != 0) {
        return false;
    }

    const i64 end = fs::file_tell(handle);
    const bool restored = fs::file_seek(handle, original, VK_SEEK_SET) == 0;
    if (end < 0 || !restored) {
        return false;
    }

    if (out_size != null) {
        *out_size = static_cast<u64>(end);
    }
    return true;
}

static auto fill_stat_common(vk_stat_t* out, u32 mode, u64 size, u64 inode) -> vk_i64 {
    if (out == null) {
        return -VK_ERR_FAULT;
    }

    *out = {};
    out->st_dev = 1;
    out->st_ino = inode;
    out->st_size = size;
    out->st_mode = mode;
    out->st_nlink = 1;
    out->st_blksize = 512;
    return 0;
}

static auto fill_stat_for_handle(vk_i32 fd, vk_stat_t* out) -> vk_i64 {
    if (fd >= 0 && fd <= 2) {
        return fill_stat_common(out,
                                VK_STAT_MODE_IFCHR | VK_STAT_MODE_RW_RW_RW,
                                0,
                                static_cast<u64>(fd + 1));
    }

    fs::file_handle handle = 0;
    if (!fd_to_handle(fd, &handle)) {
        return -VK_ERR_BADF;
    }

    u64 size = 0;
    if (!file_size_for_handle(handle, &size)) {
        return -VK_ERR_IO;
    }

    return fill_stat_common(out,
                            VK_STAT_MODE_IFREG | VK_STAT_MODE_RW_RW_RW,
                            size,
                            static_cast<u64>(handle));
}

static auto fill_stat_for_path(const char* path, vk_stat_t* out) -> vk_i64 {
    if (path == null) {
        return -VK_ERR_FAULT;
    }
    if (!fs::file_exists(path)) {
        return -VK_ERR_NOENT;
    }

    return fill_stat_common(out,
                            VK_STAT_MODE_IFREG | VK_STAT_MODE_RW_RW_RW,
                            fs::file_size(path),
                            path_inode(path));
}

static auto syscall_read_fd(vk_i32 fd, void* buf, usize len) -> vk_i64 {
    auto* out = static_cast<char*>(buf);
    if (out == null && len > 0) {
        return -VK_ERR_FAULT;
    }
    if (len == 0) {
        return 0;
    }

    if (fd == 0) {
        for (usize i = 0; i < len; ++i) {
            const char c = route_getc();
            out[i] = c;
            if (c == '\n' || c == '\r') {
                return static_cast<vk_i64>(i + 1);
            }
        }
        return static_cast<vk_i64>(len);
    }
    if (fd == 1 || fd == 2) {
        return -VK_ERR_BADF;
    }

    fs::file_handle handle = 0;
    if (!fd_to_handle(fd, &handle)) {
        return -VK_ERR_BADF;
    }
    return static_cast<vk_i64>(fs::file_read(handle, out, len));
}

static auto syscall_write_fd(vk_i32 fd, const void* buf, usize len) -> vk_i64 {
    auto* in = static_cast<const char*>(buf);
    if (in == null && len > 0) {
        return -VK_ERR_FAULT;
    }
    if (len == 0) {
        return 0;
    }

    if (fd == 1 || fd == 2) {
        return static_cast<vk_i64>(route_stdio_write(in, len));
    }
    if (fd == 0) {
        return -VK_ERR_BADF;
    }

    fs::file_handle handle = 0;
    if (!fd_to_handle(fd, &handle)) {
        return -VK_ERR_BADF;
    }
    return static_cast<vk_i64>(fs::file_write(handle, in, len));
}

static auto sleep_from_timespec(vk_u64 clock_id,
                                vk_u64 flags,
                                const vk_timespec_t* req,
                                vk_timespec_t* rem) -> vk_i64 {
    if (req == null) {
        return -VK_ERR_FAULT;
    }
    if (clock_id != VK_CLOCK_REALTIME && clock_id != VK_CLOCK_MONOTONIC) {
        return -VK_ERR_INVAL;
    }
    if ((flags & ~VK_TIMER_ABSTIME) != 0 || (flags & VK_TIMER_ABSTIME) != 0) {
        return -VK_ERR_NOSYS;
    }
    if (req->tv_sec < 0 || req->tv_nsec < 0 || req->tv_nsec >= 1000000000LL) {
        return -VK_ERR_INVAL;
    }

    if (rem != null) {
        rem->tv_sec = 0;
        rem->tv_nsec = 0;
    }

    const u64 seconds = static_cast<u64>(req->tv_sec);
    const u64 nanos = static_cast<u64>(req->tv_nsec);
    u64 ticks = seconds * static_cast<u64>(SCHED_TICK_HZ);
    ticks += (nanos * static_cast<u64>(SCHED_TICK_HZ) + 999999999ULL) / 1000000000ULL;

    if (ticks == 0 && (seconds != 0 || nanos != 0)) {
        ticks = 1;
    }
    if (ticks != 0) {
        sched::sleep(ticks);
    }
    return 0;
}

static auto clock_id_uses_scheduler_ticks(vk_u64 clock_id) -> bool {
    return clock_id == VK_CLOCK_REALTIME
        || clock_id == VK_CLOCK_MONOTONIC
        || clock_id == VK_CLOCK_PROCESS_CPUTIME_ID;
}

static auto track_process_mapping(process::process_task_context* ctx,
                                  virt_addr virt,
                                  phys_addr phys,
                                  usize size) -> bool {
    return track_process_allocation(ctx,
                                    reinterpret_cast<void*>(static_cast<usize>(virt)),
                                    reinterpret_cast<void*>(static_cast<usize>(phys)),
                                    size,
                                    size,
                                    true);
}

static auto untrack_process_mapping(process::process_task_context* ctx,
                                    virt_addr virt,
                                    usize size,
                                    process::process_allocation** out_alloc) -> bool {
    if (out_alloc != null) {
        *out_alloc = null;
    }
    if (ctx == null || size == 0) {
        return false;
    }

    const usize bytes = align_up(size, PAGE_SIZE_4K);
    process::process_allocation* prev = null;
    auto* rec = ctx->allocations;
    while (rec != null) {
        const virt_addr rec_virt = reinterpret_cast<virt_addr>(rec->user_ptr);
        if (rec->raw_ptr != rec->user_ptr
            && rec->from_phys
            && rec_virt == virt
            && rec->allocated_size == bytes) {
            if (prev != null) {
                prev->next = rec->next;
            } else {
                ctx->allocations = rec->next;
            }
            rec->next = null;
            if (out_alloc != null) {
                *out_alloc = rec;
            }
            return true;
        }
        prev = rec;
        rec = rec->next;
    }
    return false;
}

static auto mapping_flags_from_prot(vk_u32 prot) -> u64 {
    u64 flags = 0;
    if (prot != VK_PROT_NONE) {
        flags |= vm::MAP_USER;
    }
    if ((prot & VK_PROT_WRITE) != 0) {
        flags |= vm::MAP_WRITABLE;
    }
    if ((prot & VK_PROT_EXEC) != 0) {
        flags |= vm::MAP_EXECUTABLE;
    }
    return flags;
}

static auto protect_process_mapping(process::process_task_context* ctx,
                                    virt_addr virt,
                                    usize size,
                                    vk_u32 prot) -> bool {
    if (ctx == null || ctx->address_space == null || size == 0) {
        return false;
    }

    const virt_addr start = align_down(virt, PAGE_SIZE_4K);
    const usize bytes = align_up((virt - start) + size, PAGE_SIZE_4K);
    const u64 flags = mapping_flags_from_prot(prot);

    for (usize offset = 0; offset < bytes; offset += PAGE_SIZE_4K) {
        phys_addr phys = 0;
        u64 current_flags = 0;
        if (!vm::debug_resolve(ctx->address_space, start + offset, &phys, &current_flags)) {
            return false;
        }
        if (!vm::map_page(ctx->address_space,
                          start + offset,
                          align_down(phys, PAGE_SIZE_4K),
                          flags)) {
            return false;
        }
    }

    return true;
}

static auto adjust_process_brk(process::process_task_context* ctx,
                               virt_addr requested_break) -> vk_i64 {
    if (ctx == null || ctx->address_space == null) {
        return -VK_ERR_NOSYS;
    }
    if (requested_break == 0) {
        return static_cast<vk_i64>(ctx->brk_current);
    }

    if (requested_break < ctx->brk_base || requested_break >= vm::USER_MMAP_BASE) {
        return -VK_ERR_INVAL;
    }

    const virt_addr desired_mapped_end = align_up(requested_break, PAGE_SIZE_4K);
    if (desired_mapped_end > ctx->brk_mapped_end) {
        const usize grow_bytes = desired_mapped_end - ctx->brk_mapped_end;
        phys_addr phys = 0;
        if (!vm::allocate_user_pages(ctx->address_space,
                                     ctx->brk_mapped_end,
                                     grow_bytes,
                                     vm::MAP_USER | vm::MAP_WRITABLE,
                                     &phys)) {
            return -VK_ERR_NOMEM;
        }
        if (!track_process_allocation(ctx,
                                      reinterpret_cast<void*>(static_cast<usize>(ctx->brk_mapped_end)),
                                      reinterpret_cast<void*>(static_cast<usize>(phys)),
                                      grow_bytes,
                                      grow_bytes,
                                      true)) {
            vm::unmap_range(ctx->address_space, ctx->brk_mapped_end, grow_bytes);
            free_process_allocation_pages(reinterpret_cast<void*>(static_cast<usize>(phys)), grow_bytes);
            return -VK_ERR_NOMEM;
        }
        ctx->brk_mapped_end = desired_mapped_end;
    } else if (desired_mapped_end < ctx->brk_mapped_end) {
        while (ctx->brk_mapped_end > desired_mapped_end) {
            process::process_allocation* prev = null;
            auto* rec = ctx->allocations;
            auto* top_prev = static_cast<process::process_allocation*>(null);
            auto* top_rec = static_cast<process::process_allocation*>(null);

            while (rec != null) {
                const virt_addr rec_start = reinterpret_cast<virt_addr>(rec->user_ptr);
                const virt_addr rec_end = rec_start + rec->allocated_size;
                if (rec->raw_ptr != rec->user_ptr
                    && rec->from_phys
                    && rec_start >= ctx->brk_base
                    && rec_end == ctx->brk_mapped_end) {
                    top_prev = prev;
                    top_rec = rec;
                    break;
                }
                prev = rec;
                rec = rec->next;
            }

            if (top_rec == null) {
                return -VK_ERR_INVAL;
            }

            const virt_addr rec_start = reinterpret_cast<virt_addr>(top_rec->user_ptr);
            if (desired_mapped_end <= rec_start) {
                if (top_prev != null) {
                    top_prev->next = top_rec->next;
                } else {
                    ctx->allocations = top_rec->next;
                }
                vm::unmap_range(ctx->address_space, rec_start, top_rec->allocated_size);
                free_process_allocation_pages(top_rec->raw_ptr, top_rec->allocated_size);
                ctx->brk_mapped_end = rec_start;
                g_kernel_heap.free(top_rec);
                continue;
            }

            const usize shrink_bytes = ctx->brk_mapped_end - desired_mapped_end;
            const usize keep_bytes = top_rec->allocated_size - shrink_bytes;
            const virt_addr shrink_start = rec_start + keep_bytes;
            const phys_addr shrink_phys = reinterpret_cast<phys_addr>(top_rec->raw_ptr) + keep_bytes;

            vm::unmap_range(ctx->address_space, shrink_start, shrink_bytes);
            free_process_allocation_pages(reinterpret_cast<void*>(static_cast<usize>(shrink_phys)), shrink_bytes);
            top_rec->allocated_size = keep_bytes;
            top_rec->requested_size = keep_bytes;
            ctx->brk_mapped_end = desired_mapped_end;
        }
    }

    ctx->brk_current = requested_break;
    return static_cast<vk_i64>(ctx->brk_current);
}

static auto user_range_is_unmapped(vm::address_space* as,
                                   virt_addr start,
                                   usize size) -> bool {
    if (as == null || size == 0) {
        return false;
    }

    const virt_addr aligned_start = align_down(start, PAGE_SIZE_4K);
    const virt_addr aligned_end = align_up(start + size, PAGE_SIZE_4K);
    for (virt_addr addr = aligned_start; addr < aligned_end; addr += PAGE_SIZE_4K) {
        phys_addr phys = 0;
        if (vm::debug_resolve(as, addr, &phys, null)) {
            return false;
        }
    }
    return true;
}

static auto allocate_process_mapping(process::process_task_context* ctx,
                                     usize size,
                                     u64 flags,
                                     virt_addr min_addr,
                                     virt_addr* out_virt,
                                     phys_addr* out_phys) -> bool {
    if (out_virt != null) {
        *out_virt = 0;
    }
    if (out_phys != null) {
        *out_phys = 0;
    }
    if (ctx == null || ctx->address_space == null || size == 0) {
        return false;
    }

    const usize bytes = align_up(size, PAGE_SIZE_4K);
    virt_addr base = align_up(ctx->address_space->map_next, PAGE_SIZE_4K);
    if (base < min_addr) {
        base = align_up(min_addr, PAGE_SIZE_4K);
    }

    while (base + bytes > base && base + bytes <= vm::USER_MAP_LIMIT) {
        if (user_range_is_unmapped(ctx->address_space, base, bytes)) {
            phys_addr phys = 0;
            if (!vm::allocate_user_pages(ctx->address_space, base, bytes, flags, &phys)) {
                return false;
            }
            if (out_virt != null) {
                *out_virt = base;
            }
            if (out_phys != null) {
                *out_phys = phys;
            }
            ctx->address_space->map_next = base + bytes;
            return true;
        }
        base += PAGE_SIZE_4K;
    }

    return false;
}

static void* stub_malloc_internal(vk_usize size, bool executable) {
    if (size == 0) {
        return null;
    }

    log::debug() << (executable ? "malloc_exec" : "malloc")
                 << ": req size=" << static_cast<unsigned long long>(size);

    auto* ctx = static_cast<process::process_task_context*>(sched::current_task_user_data());
    if (ctx == null) {
        return g_kernel_heap.allocate_zero(size);
    }

    const usize requested = static_cast<usize>(size);
    const usize rounded = align_up(requested, static_cast<usize>(16));
    if (rounded < requested) {
        return null;
    }

    const bool uses_process_vm = ctx->address_space != null;
    if (!uses_process_vm && rounded > ~static_cast<usize>(0) - PROCESS_ALLOC_GUARD_SIZE) {
        return null;
    }

    /* _sbrk expects contiguous growth in VM-backed processes. */
    const usize allocated = uses_process_vm ? rounded : rounded + PROCESS_ALLOC_GUARD_SIZE;
    bool from_phys = false;
    void* raw = null;
    void* user = null;

    if (uses_process_vm) {
        phys_addr phys = 0;
        virt_addr virt = 0;
        if (!allocate_process_mapping(ctx,
                                      allocated,
                                      vm::MAP_WRITABLE | vm::MAP_USER
                                          | (executable ? vm::MAP_EXECUTABLE : 0),
                                      vm::USER_MMAP_BASE,
                                      &virt,
                                      &phys)) {
            return null;
        }
        raw = reinterpret_cast<void*>(static_cast<usize>(phys));
        user = reinterpret_cast<void*>(static_cast<usize>(virt));
        from_phys = true;
    } else {
        raw = g_kernel_heap.allocate_zero(allocated);
        user = raw;
        if (raw == null) {
            const u32 page_count = static_cast<u32>(
                (allocated + PAGE_SIZE_4K - 1) / PAGE_SIZE_4K);
            phys_addr phys = g_phys_alloc.allocate_pages(page_count, PAGE_SIZE_4K, 0);
            if (phys == 0) {
                return null;
            }
            raw = reinterpret_cast<void*>(phys);
            user = raw;
            memory::set(raw, 0, static_cast<size_phys>(page_count) * PAGE_SIZE_4K);
            from_phys = true;
        }
    }

    if (!track_process_allocation(ctx, user, raw, requested, allocated, from_phys)) {
        if (from_phys) {
            if (ctx->address_space != null && user != raw) {
                vm::unmap_range(ctx->address_space, reinterpret_cast<virt_addr>(user), allocated);
                free_process_allocation_pages(raw, allocated);
            } else {
                free_process_allocation_pages(raw, allocated);
            }
        } else {
            g_kernel_heap.free(raw);
        }
        return null;
    }

    log::debug() << (executable ? "malloc_exec" : "malloc")
                 << ": allocated " << static_cast<unsigned long long>(allocated)
                 << " bytes at " << user
                 << " (raw: " << raw << ", phys: " << (from_phys ? raw : 0) << ")";

    return user;
}

static void* stub_malloc(vk_usize size) {
    return stub_malloc_internal(size, false);
}

static void* stub_malloc_executable(vk_usize size) {
    return stub_malloc_internal(size, true);
}

static void stub_free(void* ptr) {
    if (ptr == null) {
        return;
    }

    log::debug() << "free: ptr=" << ptr;

    auto* ctx = static_cast<process::process_task_context*>(sched::current_task_user_data());
    if (ctx == null) {
        g_kernel_heap.free(ptr);
        return;
    }

    process::process_allocation* prev = null;
    auto* rec = ctx->allocations;
    while (rec != null) {
        if (rec->user_ptr == ptr) {
            if (prev != null) {
                prev->next = rec->next;
            } else {
                ctx->allocations = rec->next;
            }
            if (ctx->address_space != null && rec->raw_ptr != rec->user_ptr) {
                vm::unmap_range(ctx->address_space,
                                reinterpret_cast<virt_addr>(rec->user_ptr),
                                rec->allocated_size);
                free_process_allocation_pages(rec->raw_ptr, rec->allocated_size);
            } else if (rec->from_phys) {
                free_process_allocation_pages(rec->raw_ptr, rec->allocated_size);
            } else {
                g_kernel_heap.free(rec->raw_ptr);
            }
            g_kernel_heap.free(rec);
            return;
        }
        prev = rec;
        rec = rec->next;
    }

    log::warn() << "process: ignoring free of unowned pointer " << reinterpret_cast<const void*>(ptr);
}

/* ---- filesystem ---- */

static int stub_file_exists(const char* name) {
    return fs::file_exists(name) ? 1 : 0;
}

static vk_usize stub_file_size(const char* name) {
    return fs::file_size(name);
}

/* ---- process ---- */

static void stub_exit(int code) {
    auto* ctx = static_cast<process::process_task_context*>(sched::current_task_user_data());
    if (ctx != null) {
        process::cleanup_process_context(ctx, code);
    } else {
        log::printk() << "Process exited with code " << code << "\n";
    }

    sched::exit_task();
}

static void stub_yield() {
    sched::yield();
}

static void stub_sleep(vk_u64 ticks) {
    sched::sleep(static_cast<u64>(ticks));
}

static vk_i64 stub_run(const char* path) {
    if (path == null) return -1;
    return process::run(path, current_console_interface());
}

static vk_i64 stub_run_with_fb(const char* path, const vk_framebuffer_info_t* fb) {
    if (path == null || fb == null) return -1;
    return process::run(path, process::console_interface::graphical, fb);
}

static bool s_compositor_active = false;
static vk_framebuffer_info_t s_compositor_default_fb = {};
static bool s_compositor_default_fb_valid = false;

static vk_i64 stub_run_auto(const char* path) {
    if (path == null) return -1;
    const auto interface = current_console_interface();
    if (interface == process::console_interface::graphical &&
        s_compositor_active &&
        s_compositor_default_fb_valid) {
        return process::run(path, interface, &s_compositor_default_fb);
    }
    return process::run(path, interface);
}

static vk_i64 stub_run_cmdline(const char* command_line) {
    if (command_line == null) return -1;
    const auto interface = current_console_interface();
    if (interface == process::console_interface::graphical &&
        s_compositor_active &&
        s_compositor_default_fb_valid) {
        return process::run_command_line(command_line,
                                         interface,
                                         &s_compositor_default_fb);
    }
    return process::run_command_line(command_line, interface);
}

static int stub_exec_cmdline(const char* command_line) {
    if (command_line == null) return -1;

    vk_framebuffer_info_t fb_override = {};
    const vk_framebuffer_info_t* fb_ptr = null;
    auto* ctx = static_cast<process::process_task_context*>(sched::current_task_user_data());
    if (ctx != null && ctx->fb_override_valid) {
        fb_override = ctx->fb_override;
        fb_ptr = &fb_override;
    }

    return process::exec_command_line(command_line, current_console_interface(), fb_ptr);
}

static int stub_set_compositor_active(vk_u32 active) {
    s_compositor_active = (active != 0u);
    return 1;
}

static auto validate_fb(const vk_framebuffer_info_t* fb) -> bool {
    return fb != null
        && fb->valid != 0u
        && fb->base != 0u
        && fb->width > 0u
        && fb->height > 0u
        && fb->stride >= fb->width;
}

static int stub_set_compositor_default_fb(const vk_framebuffer_info_t* fb) {
    if (!validate_fb(fb)) {
        s_compositor_default_fb = {};
        s_compositor_default_fb_valid = false;
        return 0;
    }
    s_compositor_default_fb = *fb;
    s_compositor_default_fb_valid = true;
    return 1;
}

static vk_u64 stub_tick_count() {
    return sched::tick_count();
}

static void stub_wait_task(vk_i64 task_id) {
    if (task_id < 0) return;
    sched::wait_for_task(static_cast<u64>(task_id));
}

static int stub_terminate_task(vk_u64 task_id) {
    return sched::terminate_task(task_id) ? 1 : 0;
}

static vk_usize stub_task_snapshot(vk_task_info_t* out, vk_usize max_tasks) {
    if (out == null || max_tasks == 0) {
        return sched::snapshot_tasks(null, 0);
    }

    task_snapshot snapshots[MAX_TASKS];
    vk_usize total = sched::snapshot_tasks(snapshots, max_tasks < MAX_TASKS ? max_tasks : MAX_TASKS);
    vk_usize count = total < max_tasks ? total : max_tasks;
    if (count > MAX_TASKS) count = MAX_TASKS;

    for (vk_usize i = 0; i < count; ++i) {
        out[i].id = snapshots[i].id;
        out[i].state = static_cast<vk_u32>(snapshots[i].state);
        out[i].cpu = snapshots[i].cpu;
        out[i].cpu_ticks = snapshots[i].cpu_ticks;
        static_string<32> name_buf(snapshots[i].name);
        memory::copy(out[i].name, name_buf.c_str(), sizeof(out[i].name));
        out[i].name[sizeof(out[i].name) - 1] = '\0';
    }

    return total;
}

/* ---- sound ---- */

static int stub_snd_mix_play(int channel, const void* data, vk_u32 num_samples,
                              vk_u32 format, vk_u32 sample_rate,
                              vk_u32 vol_left, vk_u32 vol_right) {
    if (!data || num_samples == 0 || sample_rate == 0) return 0;
    auto fmt = static_cast<sound_format>(format);
    return sound::mix_play(channel, static_cast<const u8*>(data), num_samples,
                           fmt, sample_rate,
                           static_cast<u8>(vol_left  & 0xFF),
                           static_cast<u8>(vol_right & 0xFF)) ? 1 : 0;
}

static int stub_snd_mix_queue_play(int channel, const void* data, vk_u32 num_samples,
                                    vk_u32 format, vk_u32 sample_rate,
                                    vk_u32 vol_left, vk_u32 vol_right) {
    if (!data || num_samples == 0 || sample_rate == 0) return 0;
    auto fmt = static_cast<sound_format>(format);
    return sound::mix_queue_play(channel, static_cast<const u8*>(data), num_samples,
                                 fmt, sample_rate,
                                 static_cast<u8>(vol_left  & 0xFF),
                                 static_cast<u8>(vol_right & 0xFF)) ? 1 : 0;
}

static void stub_snd_mix_stop(int channel) {
    sound::mix_stop(channel);
}

static int stub_snd_mix_is_playing(int channel) {
    return sound::mix_is_playing(channel) ? 1 : 0;
}

/* ---- mouse ---- */

static auto should_use_framebuffer() -> bool;  /* defined below */

static int stub_poll_mouse(vk_mouse_event_t* out) {
    if (out == null || !should_use_framebuffer()) return 0;

    auto* ctx = static_cast<process::process_task_context*>(sched::current_task_user_data());
    if (ctx != null && ctx->mouse_q_head != ctx->mouse_q_tail) {
        *out = ctx->mouse_queue[ctx->mouse_q_tail];
        ctx->mouse_q_tail = (ctx->mouse_q_tail + 1) % process::process_task_context::MOUSE_QUEUE_SIZE;
        return 1;
    }

    if (ctx != null && ctx->fb_override_valid) {
        return 0;
    }

    vk_mouse_event_t ev{};
    if (input::poll_mouse(ev)) {
        *out = ev;
        return 1;
    }
    return 0;
}

static int stub_send_mouse(vk_u64 task_id, const vk_mouse_event_t* ev) {
    if (ev == null) return 0;
    auto* target = static_cast<process::process_task_context*>(sched::task_user_data(task_id));
    if (target == null) return 0;

    usize next = (target->mouse_q_head + 1) % process::process_task_context::MOUSE_QUEUE_SIZE;
    if (next == target->mouse_q_tail) {
        target->mouse_q_tail = (target->mouse_q_tail + 1) % process::process_task_context::MOUSE_QUEUE_SIZE;
    }
    target->mouse_queue[target->mouse_q_head] = *ev;
    target->mouse_q_head = next;
    return 1;
}

static void enqueue_framebuffer_event(process::process_task_context* target,
                                      vk_u32 type,
                                      const vk_framebuffer_info_t& framebuffer) {
    if (target == null || !target->framebuffer_resize_events_enabled) {
        return;
    }

    vk_framebuffer_event_t event = {};
    event.type = type;
    event.framebuffer = framebuffer;

    usize next = (target->framebuffer_event_q_head + 1)
               % process::process_task_context::FRAMEBUFFER_EVENT_QUEUE_SIZE;
    if (next == target->framebuffer_event_q_tail) {
        target->framebuffer_event_q_tail = (target->framebuffer_event_q_tail + 1)
                                        % process::process_task_context::FRAMEBUFFER_EVENT_QUEUE_SIZE;
    }
    target->framebuffer_event_queue[target->framebuffer_event_q_head] = event;
    target->framebuffer_event_q_head = next;
}

static vk_u32 stub_ticks_per_sec() {
    return SCHED_TICK_HZ;
}

static void copy_cstr(char* out, usize out_cap, const char* src) {
    if (out == null || out_cap == 0) {
        return;
    }
    usize index = 0;
    if (src != null) {
        while (src[index] != '\0' && index + 1 < out_cap) {
            out[index] = src[index];
            ++index;
        }
    }
    out[index] = '\0';
}

static vk_usize stub_kobj_rpc(const char* req_json, char* out, vk_usize out_cap) {
    if (out == null || out_cap == 0) return 0;
    static usize s_kobj_trace_budget = 64;
    if (s_kobj_trace_budget > 0) {
        --s_kobj_trace_budget;
        auto* ctx = static_cast<process::process_task_context*>(sched::current_task_user_data());
        trace_process_buffer("kobj-rpc-request", ctx, req_json, req_json != null ? 1 : 0);
    }
    kobj::krpc(req_json, out, out_cap);
    vk_usize len = 0;
    while (len < out_cap && out[len] != '\0') ++len;
    return len;
}

static int stub_kobj_query(const char* path,
                           char* out_value,
                           vk_usize out_value_cap,
                           vk_kobj_node_info_t* out_info) {
    if (out_value != null && out_value_cap > 0) {
        out_value[0] = '\0';
    }
    if (out_info != null) {
        *out_info = {};
    }

    kobj::KVal value {};
    kobj::KNodeInfo info {};
    if (!kobj::kquery(path, &value, &info)) {
        return 0;
    }

    if (out_info != null) {
        copy_cstr(out_info->name, sizeof(out_info->name), info.name.c_str());
        copy_cstr(out_info->unit, sizeof(out_info->unit), info.unit.c_str());
        out_info->type = static_cast<vk_u32>(info.type);
        out_info->readable = info.readable ? 1u : 0u;
        out_info->writable = info.writable ? 1u : 0u;
        out_info->volatile_node = info.volatile_node ? 1u : 0u;
        out_info->range_min = info.range_min;
        out_info->range_max = info.range_max;
        out_info->enum_count = info.enum_count;
        for (vk_u32 i = 0; i < info.enum_count && i < VK_KOBJ_ENUM_MAX; ++i) {
            copy_cstr(out_info->enum_labels[i],
                      sizeof(out_info->enum_labels[i]),
                      info.enum_labels[i]);
        }
    }

    if (out_value != null && out_value_cap > 0 && info.readable) {
        kobj::kval_render(value, kobj::resolve(path), out_value, out_value_cap);
    }
    return 1;
}

static vk_usize stub_kobj_list(const char* path, vk_kobj_child_t* out_items, vk_usize max_items) {
    kobj::KChildInfo items[64] {};
    const usize total = kobj::klist(path, items, sizeof(items) / sizeof(items[0]));
    if (out_items == null || max_items == 0) {
        return total;
    }
    const usize limit = total < static_cast<usize>(max_items) ? total : static_cast<usize>(max_items);

    for (usize i = 0; i < limit; ++i) {
        copy_cstr(out_items[i].name, sizeof(out_items[i].name), items[i].name.c_str());
        out_items[i].type = static_cast<vk_u32>(items[i].type);
    }
    return total;
}

static int stub_kobj_set_value(const char* path, const char* value) {
    if (path == null || value == null) {
        return 0;
    }

    kobj::KNodeInfo info {};
    if (!kobj::kinfo(path, &info) || !info.writable) {
        return 0;
    }

    kobj::KVal set_value {};
    if (info.type == kobj::KTag::U64) {
        u64 parsed = 0;
        if (!kobj::detail::parse_u64_segment(value, kobj::detail::cstrlen(value), &parsed)) {
            return 0;
        }
        set_value = kobj::KVal::from_u64(parsed);
    } else if (info.type == kobj::KTag::Bool) {
        const bool parsed = (value[0] == '1' && value[1] == '\0')
            || (value[0] == 't' && value[1] == 'r' && value[2] == 'u' && value[3] == 'e' && value[4] == '\0')
            || (value[0] == 'y' && value[1] == 'e' && value[2] == 's' && value[3] == '\0');
        set_value = kobj::KVal::from_bool(parsed);
    } else if (info.type == kobj::KTag::Str) {
        set_value = kobj::KVal::from_str(value);
    } else if (info.type == kobj::KTag::Enum) {
        for (u32 i = 0; i < info.enum_count; ++i) {
            kobj::KStr label {};
            label.set(info.enum_labels[i]);
            if (label.eq(value)) {
                set_value = kobj::KVal::from_enum(i);
                return kobj::kset(path, set_value) ? 1 : 0;
            }
        }
        return 0;
    } else {
        return 0;
    }

    return kobj::kset(path, set_value) ? 1 : 0;
}

static int stub_driver_load(const char* name) {
    return name != null ? driver::load(name) : -1;
}

static int stub_driver_unload(const char* name) {
    return name != null ? driver::unload(name) : -1;
}

static void stub_reboot() {
    arch::reboot();
}

static vk_usize stub_get_cmdline(char* out, vk_usize out_cap) {
    if (out == null || out_cap == 0) {
        return 0;
    }

    auto* ctx = static_cast<process::process_task_context*>(sched::current_task_user_data());
    if (ctx == null) {
        out[0] = '\0';
        return 0;
    }

    vk_usize len = static_cast<vk_usize>(ctx->command_line_len);
    if (len >= out_cap) {
        len = out_cap - 1;
    }

    if (len > 0) {
        memory::copy(out, ctx->command_line, len);
    }
    out[len] = '\0';
    return len;
}

static auto current_console_interface() -> process::console_interface {
    auto* ctx = static_cast<process::process_task_context*>(sched::current_task_user_data());
    if (ctx != null) {
        return ctx->interface;
    }
    return process::console_interface::graphical;
}

static auto should_use_framebuffer() -> bool {
    if (current_console_interface() != process::console_interface::graphical) {
        return false;
    }
    return console::framebuffer().valid;
}

static auto dequeue_ascii_key(process::process_task_context* ctx) -> char {
    while (ctx != null && ctx->key_q_head != ctx->key_q_tail) {
        vk_key_event_t ev = ctx->key_queue[ctx->key_q_tail];
        ctx->key_q_tail = (ctx->key_q_tail + 1) % process::process_task_context::KEY_QUEUE_SIZE;

        if (ev.pressed == 0u) {
            continue;
        }
        if (ev.ascii != '\0') {
            return ev.ascii;
        }

        if (ev.scancode == 0x1Cu) return '\r';
        if (ev.scancode == 0x0Eu) return '\b';
        if (ev.scancode == 0x0Fu) return '\t';
    }

    return '\0';
}

static void route_putc(char c) {
    if (should_use_framebuffer()) {
        auto* ctx = static_cast<process::process_task_context*>(sched::current_task_user_data());
        if (ctx != null && ctx->fb_override_valid) {
            console::putc_framebuffer_surface(ctx->fb_override, ctx->fb_text_col, ctx->fb_text_row, c);
            return;
        }
        console::putc_framebuffer(c);
    } else {
        console::putc_serial(c);
    }
}

static void route_puts(const char* str) {
    if (str == null) {
        return;
    }
    while (*str != '\0') {
        route_putc(*str++);
    }
}

static auto route_stdio_write(const char* buf, vk_usize len) -> vk_usize {
    if (buf == null || len == 0) {
        return 0;
    }

    /* stdio should follow the task's active console route so windowed apps
     * render into their assigned framebuffer instead of disappearing to COM1. */
    for (vk_usize i = 0; i < len; ++i) {
        route_putc(buf[i]);
    }
    return len;
}

static void route_put_hex(vk_u64 value) {
    char buf[19];
    log::hex(buf, sizeof(buf), value);
    route_puts(buf);
}

static void route_put_dec(vk_u64 value) {
    if (value == 0) {
        route_putc('0');
        return;
    }

    char buf[21];
    i32 i = 0;
    while (value > 0 && i < 20) {
        buf[i++] = static_cast<char>('0' + (value % 10));
        value /= 10;
    }
    while (i > 0) {
        route_putc(buf[--i]);
    }
}

static void route_clear() {
    if (should_use_framebuffer()) {
        auto* ctx = static_cast<process::process_task_context*>(sched::current_task_user_data());
        if (ctx != null && ctx->fb_override_valid) {
            console::clear_framebuffer_surface(ctx->fb_override, ctx->fb_text_col, ctx->fb_text_row);
            return;
        }
        console::clear_framebuffer();
    } else {
        console::clear_serial();
    }
}

static char route_getc() {
    if (should_use_framebuffer()) {
        auto* ctx = static_cast<process::process_task_context*>(sched::current_task_user_data());
        if (ctx != null && ctx->fb_override_valid) {
            while (true) {
                char c = dequeue_ascii_key(ctx);
                if (c != '\0') {
                    return c;
                }
                sched::sleep(1);
            }
        }
        return input::getc_ps2();
    }
    return input::getc_serial();
}

static char route_try_getc() {
    if (should_use_framebuffer()) {
        auto* ctx = static_cast<process::process_task_context*>(sched::current_task_user_data());
        if (ctx != null && ctx->fb_override_valid) {
            return dequeue_ascii_key(ctx);
        }
        return input::try_getc_ps2();
    }
    return input::try_getc_serial();
}

static int route_poll_key(vk_key_event_t* out) {
    if (out == null || !should_use_framebuffer()) return 0;

    auto* ctx = static_cast<process::process_task_context*>(sched::current_task_user_data());
    if (ctx != null && ctx->key_q_head != ctx->key_q_tail) {
        *out = ctx->key_queue[ctx->key_q_tail];
        ctx->key_q_tail = (ctx->key_q_tail + 1) % process::process_task_context::KEY_QUEUE_SIZE;
        return 1;
    }

    if (ctx != null && ctx->fb_override_valid) {
        return 0;
    }

    vk_key_event_t ev{};
    if (input::poll_key(ev)) {
        *out = ev;
        return 1;
    }
    return 0;
}

static int stub_send_key(vk_u64 task_id, const vk_key_event_t* ev) {
    if (ev == null) return 0;
    auto* target = static_cast<process::process_task_context*>(sched::task_user_data(task_id));
    if (target == null) return 0;

    usize next = (target->key_q_head + 1) % process::process_task_context::KEY_QUEUE_SIZE;
    if (next == target->key_q_tail) {
        target->key_q_tail = (target->key_q_tail + 1) % process::process_task_context::KEY_QUEUE_SIZE;
    }
    target->key_queue[target->key_q_head] = *ev;
    target->key_q_head = next;
    return 1;
}

static auto remap_target_framebuffer(process::process_task_context* target,
                                     const vk_framebuffer_info_t* fb) -> bool {
    if (target == null || !validate_fb(fb)) {
        return false;
    }

    vk_framebuffer_info_t mapped = *fb;
    usize fb_bytes = 0;
    if (!framebuffer::byte_size(*fb, fb_bytes)) {
        return false;
    }

    auto* source_ctx = static_cast<process::process_task_context*>(sched::current_task_user_data());
    const virt_addr source_base = static_cast<virt_addr>(fb->base);
    if (target->address_space != null
        && source_base >= vm::USER_IMAGE_BASE
        && source_base < vm::USER_MAP_LIMIT) {
        if (source_ctx == null || source_ctx->address_space == null) {
            log::warn() << "vmtrace: set_task_framebuffer has user address but no source address space";
            return false;
        }

        const usize source_offset = static_cast<usize>(source_base & (PAGE_SIZE_4K - 1));
        const virt_addr target_start = vm::USER_SHARED_BASE;
        const virt_addr target_base = target_start + source_offset;
        const usize mapped_bytes = align_up(source_offset + fb_bytes, PAGE_SIZE_4K);
        const virt_addr source_start = align_down(source_base, PAGE_SIZE_4K);
        usize old_mapped_bytes = 0;

        if (target->fb_override_valid
            && target->fb_override.base >= vm::USER_SHARED_BASE
            && target->fb_override.base < vm::USER_MAP_LIMIT) {
            usize old_fb_bytes = 0;
            if (framebuffer::byte_size(target->fb_override, old_fb_bytes)) {
                const virt_addr old_target_base = static_cast<virt_addr>(target->fb_override.base);
                const usize old_offset = static_cast<usize>(old_target_base - align_down(old_target_base, PAGE_SIZE_4K));
                old_mapped_bytes = align_up(old_offset + old_fb_bytes, PAGE_SIZE_4K);
            }
        }

        for (usize offset = 0; offset < mapped_bytes; offset += PAGE_SIZE_4K) {
            phys_addr source_phys = 0;
            u64 source_flags = 0;
            const virt_addr source_page = source_start + offset;
            if (!vm::debug_resolve(source_ctx->address_space, source_page, &source_phys, &source_flags)) {
                log::warn() << "vmtrace: set_task_framebuffer source resolve failed src="
                            << reinterpret_cast<const void*>(static_cast<usize>(source_page));
                return false;
            }
        }

        /* Map first so USER_SHARED_BASE never goes transiently unmapped. */
        for (usize offset = 0; offset < mapped_bytes; offset += PAGE_SIZE_4K) {
            phys_addr source_phys = 0;
            u64 source_flags = 0;
            const virt_addr source_page = source_start + offset;
            if (!vm::debug_resolve(source_ctx->address_space, source_page, &source_phys, &source_flags)) {
                if (offset > 0) {
                    vm::unmap_range(target->address_space, target_start, offset);
                }
                target->fb_override = {};
                target->fb_override_valid = false;
                target->fb_text_col = 0;
                target->fb_text_row = 0;
                return false;
            }

            if (!vm::map_page(target->address_space,
                              target_start + offset,
                              align_down(source_phys, PAGE_SIZE_4K),
                              vm::MAP_WRITABLE | vm::MAP_USER)) {
                if (offset > 0) {
                    vm::unmap_range(target->address_space, target_start, offset);
                }
                target->fb_override = {};
                target->fb_override_valid = false;
                target->fb_text_col = 0;
                target->fb_text_row = 0;
                log::warn() << "vmtrace: set_task_framebuffer map failed dst="
                            << reinterpret_cast<const void*>(static_cast<usize>(target_start + offset))
                            << " phys=" << reinterpret_cast<const void*>(static_cast<usize>(source_phys));
                return false;
            }
        }

        /* Drop any stale tail pages from the old shared mapping. */
        if (old_mapped_bytes > mapped_bytes) {
            vm::unmap_range(target->address_space,
                            target_start + mapped_bytes,
                            old_mapped_bytes - mapped_bytes);
        }

        mapped.base = static_cast<vk_u64>(target_base);
    }

    target->fb_override = mapped;
    target->fb_override_valid = true;
    target->fb_text_col = 0;
    target->fb_text_row = 0;
    return true;
}

static int stub_set_task_framebuffer(vk_u64 task_id, const vk_framebuffer_info_t* fb) {
    if (fb == null) return 0;

    if (!sched::task_blocked_off_cpu(task_id)) {
        return 0;
    }

    auto* target = static_cast<process::process_task_context*>(sched::task_user_data(task_id));
    if (target == null) return 0;
    if (!remap_target_framebuffer(target, fb)) {
        return 0;
    }
    if (target->fb_override_valid) {
        enqueue_framebuffer_event(target,
                                  VK_FRAMEBUFFER_EVENT_RESIZED,
                                  target->fb_override);
    }
    return target->fb_override_valid ? 1 : 0;
}

static int stub_poll_framebuffer_event(vk_framebuffer_event_t* out) {
    if (out == null || !should_use_framebuffer()) {
        return 0;
    }

    auto* ctx = static_cast<process::process_task_context*>(sched::current_task_user_data());
    if (ctx == null
        || !ctx->framebuffer_resize_events_enabled
        || ctx->framebuffer_event_q_head == ctx->framebuffer_event_q_tail) {
        return 0;
    }

    *out = ctx->framebuffer_event_queue[ctx->framebuffer_event_q_tail];
    ctx->framebuffer_event_q_tail = (ctx->framebuffer_event_q_tail + 1)
                                  % process::process_task_context::FRAMEBUFFER_EVENT_QUEUE_SIZE;
    return 1;
}

static int stub_set_framebuffer_resize_events(vk_u32 enabled) {
    auto* ctx = static_cast<process::process_task_context*>(sched::current_task_user_data());
    if (ctx == null) {
        return 0;
    }

    ctx->framebuffer_resize_events_enabled = (enabled != 0u);
    if (!ctx->framebuffer_resize_events_enabled) {
        ctx->framebuffer_event_q_head = 0;
        ctx->framebuffer_event_q_tail = 0;
    }
    return 1;
}

static int stub_task_accepts_framebuffer_resize(vk_u64 task_id) {
    auto* target = static_cast<process::process_task_context*>(sched::task_user_data(task_id));
    if (target == null) {
        return 0;
    }
    return target->framebuffer_resize_events_enabled ? 1 : 0;
}

static int stub_set_startup_window_size(vk_u32 width, vk_u32 height) {
    auto* ctx = static_cast<process::process_task_context*>(sched::current_task_user_data());
    if (ctx == null) {
        return 0;
    }

    if (width == 0 || height == 0) {
        ctx->startup_window_size_set = false;
        ctx->startup_window_width = 0;
        ctx->startup_window_height = 0;
        return 1;
    }

    ctx->startup_window_size_set = true;
    ctx->startup_window_width = width;
    ctx->startup_window_height = height;
    return 1;
}

static int stub_get_task_startup_window_size(vk_u64 task_id, vk_u32* out_width, vk_u32* out_height) {
    if (out_width == null || out_height == null) {
        return 0;
    }

    *out_width = 0;
    *out_height = 0;

    auto* target = static_cast<process::process_task_context*>(sched::task_user_data(task_id));
    if (target == null || !target->startup_window_size_set) {
        return 0;
    }

    *out_width = target->startup_window_width;
    *out_height = target->startup_window_height;
    return 1;
}

static void route_framebuffer_info(vk_framebuffer_info_t* out) {
    if (out == null) return;
    auto* ctx = static_cast<process::process_task_context*>(sched::current_task_user_data());
    if (!should_use_framebuffer()) {
        *out = {};
        return;
    }
    if (ctx != null && ctx->fb_override_valid) {
        *out = ctx->fb_override;
        return;
    }
    auto fb = console::framebuffer();
    out->base   = static_cast<vk_u64>(fb.base);
    out->width  = static_cast<vk_u32>(fb.width);
    out->height = static_cast<vk_u32>(fb.height);
    out->stride = static_cast<vk_u32>(fb.stride);
    out->format = static_cast<vk_pixel_format_t>(fb.format);
    out->valid  = fb.valid ? 1u : 0u;
}

static vk_file_handle_t stub_file_open(const char* path, const char* mode) {
    return static_cast<vk_file_handle_t>(fs::file_open(path, mode));
}

static int stub_file_close(vk_file_handle_t handle) {
    return fs::file_close(static_cast<fs::file_handle>(handle));
}

static vk_usize stub_file_read_handle(vk_file_handle_t handle, void* buf, vk_usize buf_size) {
    return fs::file_read(static_cast<fs::file_handle>(handle), buf, buf_size);
}

static vk_usize stub_file_write_handle(vk_file_handle_t handle, const void* buf, vk_usize buf_size) {
    return fs::file_write(static_cast<fs::file_handle>(handle), buf, buf_size);
}

static int stub_file_seek(vk_file_handle_t handle, vk_i64 offset, int whence) {
    return fs::file_seek(static_cast<fs::file_handle>(handle), offset, whence);
}

static vk_i64 stub_file_tell(vk_file_handle_t handle) {
    return fs::file_tell(static_cast<fs::file_handle>(handle));
}

static int stub_file_truncate(vk_file_handle_t handle, vk_i64 length) {
    return fs::file_truncate(static_cast<fs::file_handle>(handle), length);
}

static int stub_file_remove(const char* path) {
    return fs::file_remove(path);
}

static auto stub_syscall(vk_u64 nr,
                         vk_u64 a1,
                         vk_u64 a2,
                         vk_u64 a3,
                         vk_u64 a4,
                         vk_u64 a5,
                         vk_u64 a6) -> vk_i64 {
    auto* ctx = static_cast<process::process_task_context*>(sched::current_task_user_data());

    switch (nr) {
        case VK_SYS_EXIT:
            stub_exit(static_cast<int>(a1));
            return 0;

        case VK_SYS_OPEN: {
            const char* path = reinterpret_cast<const char*>(static_cast<usize>(a1));
            const vk_u32 flags = static_cast<vk_u32>(a2);
            (void)a3;
            if (path == null) {
                return -VK_ERR_FAULT;
            }

            const vk_u32 accmode = flags & VK_O_ACCMODE;
            const bool wants_create = (flags & VK_O_CREAT) != 0;
            const bool wants_truncate = (flags & VK_O_TRUNC) != 0;
            const bool wants_append = (flags & VK_O_APPEND) != 0;
            const char* mode = "r";

            if (wants_append) {
                mode = accmode == VK_O_RDWR ? "a+" : "a";
            } else if (wants_truncate) {
                mode = accmode == VK_O_RDWR ? "w+" : "w";
            } else if (wants_create && accmode == VK_O_RDWR) {
                mode = fs::file_exists(path) ? "r+" : "w+";
            } else if (accmode == VK_O_WRONLY) {
                mode = wants_create ? "w" : "w";
            } else if (accmode == VK_O_RDWR) {
                mode = wants_create && !fs::file_exists(path) ? "w+" : "r+";
            }

            const auto handle = fs::file_open(path, mode);
            if (handle == 0) {
                return -VK_ERR_NOENT;
            }
            return static_cast<vk_i64>(handle + 2);
        }

        case VK_SYS_CLOSE: {
            const vk_i32 fd = static_cast<vk_i32>(a1);
            if (fd >= 0 && fd <= 2) {
                return 0;
            }

            fs::file_handle handle = 0;
            if (!fd_to_handle(fd, &handle)) {
                return -VK_ERR_BADF;
            }
            return fs::file_close(handle) == 0 ? 0 : -VK_ERR_BADF;
        }

        case VK_SYS_READ: {
            const vk_i32 fd = static_cast<vk_i32>(a1);
            auto* buf = reinterpret_cast<void*>(static_cast<usize>(a2));
            const usize len = static_cast<usize>(a3);
            return syscall_read_fd(fd, buf, len);
        }

        case VK_SYS_WRITE: {
            const vk_i32 fd = static_cast<vk_i32>(a1);
            const void* buf = reinterpret_cast<const void*>(static_cast<usize>(a2));
            const usize len = static_cast<usize>(a3);
            return syscall_write_fd(fd, buf, len);
        }

        case VK_SYS_LSEEK: {
            const vk_i32 fd = static_cast<vk_i32>(a1);
            const vk_i64 offset = static_cast<vk_i64>(a2);
            const int whence = static_cast<int>(a3);
            if (fd >= 0 && fd <= 2) {
                return -VK_ERR_SPIPE;
            }

            fs::file_handle handle = 0;
            if (!fd_to_handle(fd, &handle)) {
                return -VK_ERR_BADF;
            }
            if (fs::file_seek(handle, offset, whence) != 0) {
                return -VK_ERR_INVAL;
            }
            return fs::file_tell(handle);
        }

        case VK_SYS_FSTAT:
            return fill_stat_for_handle(static_cast<vk_i32>(a1),
                                        reinterpret_cast<vk_stat_t*>(static_cast<usize>(a2)));

        case VK_SYS_STAT:
            return fill_stat_for_path(reinterpret_cast<const char*>(static_cast<usize>(a1)),
                                      reinterpret_cast<vk_stat_t*>(static_cast<usize>(a2)));

        case VK_SYS_UNLINK: {
            const char* path = reinterpret_cast<const char*>(static_cast<usize>(a1));
            if (path == null) {
                return -VK_ERR_FAULT;
            }
            if (!fs::file_exists(path)) {
                return -VK_ERR_NOENT;
            }
            return fs::file_remove(path) == 0 ? 0 : -VK_ERR_IO;
        }

        case VK_SYS_GETPID:
            return static_cast<vk_i64>(sched::current_task_id());

        case VK_SYS_GETTIMEOFDAY: {
            auto* tv = reinterpret_cast<vk_timeval_t*>(static_cast<usize>(a1));
            (void)a2;
            if (tv == null) {
                return -VK_ERR_FAULT;
            }
            const u64 ticks = sched::tick_count();
            const u64 tps = SCHED_TICK_HZ;
            tv->tv_sec = static_cast<vk_i64>(ticks / tps);
            tv->tv_usec = static_cast<vk_i64>(((ticks % tps) * 1000000ULL) / tps);
            return 0;
        }

        case VK_SYS_CLOCK_GETTIME: {
            const vk_u64 clock_id = a1;
            auto* ts = reinterpret_cast<vk_timespec_t*>(static_cast<usize>(a2));
            if (ts == null) {
                return -VK_ERR_FAULT;
            }
            if (!clock_id_uses_scheduler_ticks(clock_id)) {
                return -VK_ERR_INVAL;
            }
            const u64 ticks = sched::tick_count();
            const u64 tps = SCHED_TICK_HZ;
            ts->tv_sec = static_cast<vk_i64>(ticks / tps);
            ts->tv_nsec = static_cast<vk_i64>(((ticks % tps) * 1000000000ULL) / tps);
            return 0;
        }

        case VK_SYS_MMAP: {
            auto requested_addr = static_cast<virt_addr>(a1);
            const usize length = static_cast<usize>(a2);
            const vk_u32 prot = static_cast<vk_u32>(a3);
            const vk_u32 flags = static_cast<vk_u32>(a4);
            const vk_i64 fd = static_cast<vk_i64>(a5);
            (void)a6;
            if (ctx == null || ctx->address_space == null || length == 0) {
                return -VK_ERR_INVAL;
            }
            if ((flags & VK_MAP_FIXED) != 0) {
                return -VK_ERR_NOSYS;
            }
            if ((flags & VK_MAP_ANONYMOUS) == 0 || fd != -1) {
                return -VK_ERR_NOSYS;
            }

            const usize bytes = align_up(length, PAGE_SIZE_4K);
            virt_addr base = 0;
            phys_addr phys = 0;
            if (!allocate_process_mapping(ctx,
                                          bytes,
                                          mapping_flags_from_prot(prot),
                                          vm::USER_MMAP_BASE,
                                          &base,
                                          &phys)) {
                return -VK_ERR_NOMEM;
            }
            if (!track_process_mapping(ctx, base, phys, bytes)) {
                vm::unmap_range(ctx->address_space, base, bytes);
                free_process_allocation_pages(reinterpret_cast<void*>(static_cast<usize>(phys)), bytes);
                return -VK_ERR_NOMEM;
            }
            requested_addr = base;
            return static_cast<vk_i64>(requested_addr);
        }

        case VK_SYS_MUNMAP: {
            const virt_addr addr = static_cast<virt_addr>(a1);
            const usize length = static_cast<usize>(a2);
            process::process_allocation* alloc = null;
            if (!untrack_process_mapping(ctx, addr, length, &alloc) || alloc == null) {
                return -VK_ERR_INVAL;
            }

            vm::unmap_range(ctx->address_space, addr, alloc->allocated_size);
            free_process_allocation_pages(alloc->raw_ptr, alloc->allocated_size);
            g_kernel_heap.free(alloc);
            return 0;
        }

        case VK_SYS_MPROTECT: {
            const virt_addr addr = static_cast<virt_addr>(a1);
            const usize length = static_cast<usize>(a2);
            const vk_u32 prot = static_cast<vk_u32>(a3);
            return protect_process_mapping(ctx, addr, length, prot) ? 0 : -VK_ERR_INVAL;
        }

        case VK_SYS_NANOSLEEP:
            return sleep_from_timespec(VK_CLOCK_REALTIME,
                                       0,
                                       reinterpret_cast<const vk_timespec_t*>(static_cast<usize>(a1)),
                                       reinterpret_cast<vk_timespec_t*>(static_cast<usize>(a2)));

        case VK_SYS_CLOCK_NANOSLEEP:
            return sleep_from_timespec(a1,
                                       a2,
                                       reinterpret_cast<const vk_timespec_t*>(static_cast<usize>(a3)),
                                       reinterpret_cast<vk_timespec_t*>(static_cast<usize>(a4)));

        case VK_SYS_READV:
        case VK_SYS_WRITEV: {
            const vk_i32 fd = static_cast<vk_i32>(a1);
            const auto* iov = reinterpret_cast<const linux_iovec*>(static_cast<usize>(a2));
            const vk_i32 count = static_cast<vk_i32>(a3);
            if (iov == null && count > 0) {
                return -VK_ERR_FAULT;
            }
            if (count < 0) {
                return -VK_ERR_INVAL;
            }

            usize total = 0;
            for (vk_i32 i = 0; i < count; ++i) {
                const auto& entry = iov[i];
                const vk_i64 part = nr == VK_SYS_READV
                    ? syscall_read_fd(fd, entry.iov_base, entry.iov_len)
                    : syscall_write_fd(fd, entry.iov_base, entry.iov_len);
                if (part < 0) {
                    return total > 0 ? static_cast<vk_i64>(total) : part;
                }

                total += static_cast<usize>(part);
                if (static_cast<usize>(part) < entry.iov_len) {
                    break;
                }
            }
            return static_cast<vk_i64>(total);
        }

        case VK_SYS_FSTATAT: {
            const vk_i32 fd = static_cast<vk_i32>(a1);
            const char* path = reinterpret_cast<const char*>(static_cast<usize>(a2));
            auto* out = reinterpret_cast<vk_stat_t*>(static_cast<usize>(a3));
            const vk_i32 flags = static_cast<vk_i32>(a4);
            const vk_i32 unsupported = flags & ~(VK_AT_EMPTY_PATH | VK_AT_SYMLINK_NOFOLLOW);

            if (path == null || out == null) {
                return -VK_ERR_FAULT;
            }
            if (unsupported != 0) {
                return -VK_ERR_INVAL;
            }
            if (path[0] == '\0') {
                if ((flags & VK_AT_EMPTY_PATH) == 0) {
                    return -VK_ERR_NOENT;
                }
                return fill_stat_for_handle(fd, out);
            }
            if (fd != VK_AT_FDCWD && path[0] != '/') {
                return -VK_ERR_NOSYS;
            }
            return fill_stat_for_path(path, out);
        }

        case VK_SYS_IOCTL:
            (void)a1;
            (void)a2;
            (void)a3;
            return -VK_ERR_NOTTY;

        case VK_SYS_SCHED_YIELD:
            sched::yield();
            return 0;

        case VK_SYS_SET_THREAD_POINTER:
            if (ctx == null) {
                return -VK_ERR_NOSYS;
            }
            ctx->user_thread_pointer = a1;
            arch::write_fs_base(a1);
            return 0;

        case VK_SYS_GET_THREAD_POINTER:
            return ctx != null ? static_cast<vk_i64>(ctx->user_thread_pointer) : 0;

        case VK_SYS_SET_TID_ADDRESS:
            (void)a1;
            return static_cast<vk_i64>(sched::current_task_id());

        case VK_SYS_PROCESS_IMAGE_INFO: {
            auto* out = reinterpret_cast<vk_process_image_info_t*>(static_cast<usize>(a1));
            if (ctx == null || out == null) {
                return -VK_ERR_FAULT;
            }
            *out = {};
            out->entry = ctx->entry;
            out->phdr_addr = ctx->phdr_addr;
            out->page_size = PAGE_SIZE_4K;
            out->tls_vaddr = ctx->tls_vaddr;
            out->tls_filesz = ctx->tls_filesz;
            out->tls_memsz = ctx->tls_memsz;
            out->tls_align = ctx->tls_align;
            out->phent = ctx->phentsize;
            out->phnum = ctx->phnum;
            out->has_tls = ctx->has_tls ? 1u : 0u;
            return 0;
        }

        case VK_SYS_FTRUNCATE: {
            const vk_i32 fd = static_cast<vk_i32>(a1);
            const vk_i64 length = static_cast<vk_i64>(a2);
            if (length < 0) {
                return -VK_ERR_INVAL;
            }
            fs::file_handle handle = 0;
            if (!fd_to_handle(fd, &handle)) {
                return -VK_ERR_BADF;
            }
            return fs::file_truncate(handle, length) == 0 ? 0 : -VK_ERR_IO;
        }

        case VK_SYS_ACCESS: {
            const char* path = reinterpret_cast<const char*>(static_cast<usize>(a1));
            (void)a2;
            if (path == null) {
                return -VK_ERR_FAULT;
            }
            return fs::file_exists(path) ? 0 : -VK_ERR_NOENT;
        }

        case VK_SYS_GETCWD: {
            auto* buf = reinterpret_cast<char*>(static_cast<usize>(a1));
            const usize size = static_cast<usize>(a2);
            if (buf == null) {
                return -VK_ERR_FAULT;
            }
            if (size < 2) {
                return -VK_ERR_RANGE;
            }
            buf[0] = '/';
            buf[1] = '\0';
            return static_cast<vk_i64>(1);
        }

        case VK_SYS_PREAD:
        case VK_SYS_PWRITE: {
            const vk_i32 fd = static_cast<vk_i32>(a1);
            void* buf = reinterpret_cast<void*>(static_cast<usize>(a2));
            const usize count = static_cast<usize>(a3);
            const vk_i64 offset = static_cast<vk_i64>(a4);
            if (buf == null && count > 0) {
                return -VK_ERR_FAULT;
            }

            fs::file_handle handle = 0;
            if (!fd_to_handle(fd, &handle)) {
                return -VK_ERR_BADF;
            }

            const i64 original = fs::file_tell(handle);
            if (original < 0 || fs::file_seek(handle, offset, VK_SEEK_SET) != 0) {
                return -VK_ERR_INVAL;
            }

            const vk_i64 result = nr == VK_SYS_PREAD
                ? static_cast<vk_i64>(fs::file_read(handle, buf, count))
                : static_cast<vk_i64>(fs::file_write(handle,
                                                     reinterpret_cast<const void*>(buf),
                                                     count));
            (void)fs::file_seek(handle, original, VK_SEEK_SET);
            return result;
        }

        case VK_SYS_FCNTL: {
            const vk_i32 fd = static_cast<vk_i32>(a1);
            const vk_i32 cmd = static_cast<vk_i32>(a2);
            const vk_u64 arg = a3;
            switch (cmd) {
                case VK_F_GETFD:
                case VK_F_SETFD:
                case VK_F_SETFL:
                    (void)fd;
                    (void)arg;
                    return 0;
                case VK_F_GETFL:
                    return fd >= 0 && fd <= 2 ? VK_O_RDWR : VK_O_RDWR;
                case VK_F_GETLK: {
                    auto* lock = reinterpret_cast<unsigned short*>(static_cast<usize>(arg));
                    if (lock != null) {
                        *lock = static_cast<unsigned short>(VK_F_UNLCK);
                    }
                    return 0;
                }
                case VK_F_SETLK:
                case VK_F_SETLKW:
                    return 0;
                default:
                    return -VK_ERR_INVAL;
            }
        }

        case VK_SYS_BRK:
            return adjust_process_brk(ctx, static_cast<virt_addr>(a1));

        default:
            return -VK_ERR_NOSYS;
    }
}

/* ============================================================
 * Global kernel API table
 * Populated once by init(); never changes at runtime.
 * ============================================================ */

static vk_api_t s_api;
static bool     s_api_ready = false;

void init() {
    if (s_api_ready) return;

    s_api = {};
    s_api.api_version = VK_API_VERSION;
    /* console output */
    s_api.vk_puts = route_puts;
    s_api.vk_putc = route_putc;
    s_api.vk_put_hex = route_put_hex;
    s_api.vk_put_dec = route_put_dec;
    s_api.vk_clear = route_clear;
    /* console input */
    /* stdio */
    s_api.vk_stdio_write = route_stdio_write;
    s_api.vk_getc = route_getc;
    s_api.vk_try_getc = route_try_getc;
    /* memory */
    s_api.vk_malloc = stub_malloc;
    s_api.vk_malloc_executable = stub_malloc_executable;
    s_api.vk_free = stub_free;
    /* filesystem */
    s_api.vk_file_exists = stub_file_exists;
    s_api.vk_file_size = stub_file_size;
    /* process */
    s_api.vk_exit = stub_exit;
    s_api.vk_yield = stub_yield;
    s_api.vk_sleep = stub_sleep;
    s_api.vk_run_with_fb = stub_run_with_fb;
    s_api.vk_run_auto = stub_run_auto;
    s_api.vk_run_cmdline = stub_run_cmdline;
    s_api.vk_run = stub_run;
    s_api.vk_tick_count = stub_tick_count;
    /* framebuffer */
    s_api.vk_framebuffer_info = route_framebuffer_info;
    /* file streams */
    s_api.vk_file_open = stub_file_open;
    s_api.vk_file_close = stub_file_close;
    s_api.vk_file_read_handle = stub_file_read_handle;
    s_api.vk_file_write_handle = stub_file_write_handle;
    s_api.vk_file_seek = stub_file_seek;
    s_api.vk_file_tell = stub_file_tell;
    s_api.vk_file_remove = stub_file_remove;
    /* raw keyboard */
    s_api.vk_poll_key = route_poll_key;
    s_api.vk_send_key = stub_send_key;
    s_api.vk_set_task_framebuffer = stub_set_task_framebuffer;
    s_api.vk_ticks_per_sec = stub_ticks_per_sec;
    /* task sync */
    s_api.vk_wait_task = stub_wait_task;
    /* sound */
    s_api.vk_snd_mix_play       = stub_snd_mix_play;
    s_api.vk_snd_mix_queue_play = stub_snd_mix_queue_play;
    s_api.vk_snd_mix_stop       = stub_snd_mix_stop;
    s_api.vk_snd_mix_is_playing = stub_snd_mix_is_playing;
    /* mouse */
    s_api.vk_poll_mouse = stub_poll_mouse;
    /* task stats */
    s_api.vk_task_snapshot = stub_task_snapshot;
    /* compositor control */
    s_api.vk_set_compositor_active = stub_set_compositor_active;
    s_api.vk_set_compositor_default_fb = stub_set_compositor_default_fb;
    /* kobj */
    s_api.vk_kobj_rpc = stub_kobj_rpc;
    /* input routing */
    s_api.vk_send_mouse = stub_send_mouse;
    /* process command line */
    s_api.vk_get_cmdline = stub_get_cmdline;
    s_api.vk_terminate_task = stub_terminate_task;
    s_api.vk_exec_cmdline = stub_exec_cmdline;
    /* framebuffer events */
    s_api.vk_poll_framebuffer_event = stub_poll_framebuffer_event;
    s_api.vk_set_framebuffer_resize_events = stub_set_framebuffer_resize_events;
    s_api.vk_task_accepts_framebuffer_resize = stub_task_accepts_framebuffer_resize;
    s_api.vk_set_startup_window_size = stub_set_startup_window_size;
    s_api.vk_get_task_startup_window_size = stub_get_task_startup_window_size;
    s_api.vk_kobj_query = stub_kobj_query;
    s_api.vk_kobj_list = stub_kobj_list;
    s_api.vk_kobj_set_value = stub_kobj_set_value;
    s_api.vk_driver_load = stub_driver_load;
    s_api.vk_driver_unload = stub_driver_unload;
    s_api.vk_reboot = stub_reboot;
    s_api.vk_file_truncate = stub_file_truncate;
    s_api.vk_syscall = stub_syscall;

    s_api_ready = true;
}

auto get_api() -> const vk_api_t* {
    if (!s_api_ready) init();
    return &s_api;
}

} // namespace kernel_api
} // namespace vk
