/*
 * vkernel - UEFI Microkernel
 * Copyright (C) 2026 vkernel authors
 *
 * uefi.cpp - UEFI interface implementation
 */

#include "config.h"
#include "types.h"
#include "uefi.h"
#include "console.h"
#include "arch/x86_64/arch.h"

namespace vk {
namespace uefi {

/* Global system table */
system_table* g_system_table = null;

/* Initialize UEFI subsystem */
auto init(handle image_handle, system_table* system_table) -> status {
    if (system_table == null) {
        return status::invalid_parameter;
    }
    
    /* Store the system table */
    g_system_table = system_table;
    
    /* Verify the system table signature */
    if (system_table->hdr.signature != SYSTEM_TABLE_SIGNATURE) {
        return status::device_error;
    }
    
    return status::success;
}

/* Set the global system table pointer */
void set_system_table(system_table* system_table) {
    g_system_table = system_table;
}

/* Get the console protocol */
auto get_console() -> text_output_protocol* {
    if (g_system_table == null) {
        return null;
    }
    
    return g_system_table->con_out;
}

/* Calculate UCS-2 string length */
usize strlen(const char16_t* str) {
    usize len = 0;
    
    if (str == null) {
        return 0;
    }
    
    while (str[len] != 0) {
        ++len;
    }
    
    return len;
}

/* Copy UCS-2 string */
void strcpy(char16_t* dest, const char16_t* src) {
    if (dest == null || src == null) {
        return;
    }
    
    while (*src != 0) {
        *dest++ = *src++;
    }
    *dest = 0;
}

/* Compare UCS-2 strings */
i32 strcmp(const char16_t* s1, const char16_t* s2) {
    if (s1 == null || s2 == null) {
        return -1;
    }
    
    while (*s1 && (*s1 == *s2)) {
        ++s1;
        ++s2;
    }
    
    return static_cast<i32>(*s1 - *s2);
}

/* Backing storage for raw UEFI memory map descriptors.
 * Start with an inline buffer to avoid pool allocations on common firmware,
 * then grow it via AllocatePool when real hardware needs a larger map. */
static constexpr usize k_initial_raw_buf_size = 256 * 64;
static u8    s_raw_map_inline_buf[k_initial_raw_buf_size];
static void* s_raw_map_storage  = s_raw_map_inline_buf;
static usize s_raw_map_capacity = k_initial_raw_buf_size;

static auto ensure_raw_map_capacity(usize required_size, usize descriptor_size) -> status {
    if (required_size <= s_raw_map_capacity) {
        return status::success;
    }
    if (g_system_table == null || g_system_table->boot_services == null) {
        return status::not_ready;
    }

    constexpr u32 EFI_LOADER_DATA = 2;
    const usize slack = (descriptor_size != 0 ? descriptor_size : sizeof(memory_descriptor)) * 8;
    const usize target_size = required_size + slack;

    usize new_capacity = s_raw_map_capacity;
    while (new_capacity < target_size) {
        if (new_capacity > static_cast<usize>(-1) / 2) {
            new_capacity = target_size;
            break;
        }
        new_capacity *= 2;
    }

    void* new_storage = null;
    auto* bs = g_system_table->boot_services;
    auto st = bs->allocate_pool(EFI_LOADER_DATA, new_capacity, &new_storage);
    if (st != status::success || new_storage == null) {
        return st != status::success ? st : status::out_of_resources;
    }

    if (s_raw_map_storage != s_raw_map_inline_buf) {
        bs->free_pool(s_raw_map_storage);
    }

    s_raw_map_storage = new_storage;
    s_raw_map_capacity = new_capacity;
    return status::success;
}

/* Query the firmware memory map via GetMemoryMap boot service */
auto query_memory_map() -> memory_map_result {
    memory_map_result result{};
    if (g_system_table == null || g_system_table->boot_services == null) {
        return result;
    }

    auto* bs = g_system_table->boot_services;
    for (u32 attempt = 0; attempt < 4; ++attempt) {
        usize map_size        = s_raw_map_capacity;
        usize map_key         = 0;
        usize descriptor_size = 0;
        u32   descriptor_ver  = 0;

        auto st = bs->get_memory_map(
            &map_size,
            reinterpret_cast<memory_descriptor*>(s_raw_map_storage),
            &map_key,
            &descriptor_size,
            &descriptor_ver
        );

        if (st == status::success) {
            if (descriptor_size == 0 || map_size < descriptor_size) {
                result.query_status = status::device_error;
                result.required_size = map_size;
                return result;
            }

            result.entries = reinterpret_cast<const memory_descriptor*>(s_raw_map_storage);
            result.count = map_size / descriptor_size;
            result.descriptor_size = descriptor_size;
            result.map_key = map_key;
            result.query_status = status::success;
            result.required_size = map_size;
            return result;
        }

        result.descriptor_size = descriptor_size;
        result.required_size = map_size;
        if (st != status::buffer_too_small) {
            result.query_status = st;
            return result;
        }

        auto grow_status = ensure_raw_map_capacity(map_size, descriptor_size);
        if (grow_status != status::success) {
            result.query_status = grow_status;
            return result;
        }
    }

    result.query_status = status::buffer_too_small;
    return result;
}

/* Call ExitBootServices with the map key obtained from query_memory_map() */
auto do_exit_boot_services(handle image_handle, usize map_key) -> status {
    if (g_system_table == null || g_system_table->boot_services == null) {
        return status::not_ready;
    }
    return g_system_table->boot_services->exit_boot_services(image_handle, map_key);
}

/* Try to locate a GOP protocol instance.
 * Strategy:
 *   1. LocateHandleBuffer(ByProtocol, GOP_GUID) — finds handles that
 *      already have GOP installed.
 *   2. If that fails, try ConnectController(NULL, ..., recursive=true)
 *      to make OVMF bind its GOP driver, then retry.
 *   3. As a last resort try LocateProtocol (simpler but less reliable). */
/* Probe BochsVBE display directly via PCI + VBE I/O ports.
 * This bypass is needed when OVMF doesn't include a GOP driver
 * for the QEMU standard VGA (PCI 1234:1111). */
static inline void outl_pci(u16 port, u32 val) {
    arch::outl(port, val);
}
static inline auto inl_pci(u16 port) -> u32 {
    return arch::inl(port);
}
static inline void outw_vbe(u16 port, u16 val) {
    arch::outw(port, val);
}
static inline auto inw_vbe(u16 port) -> u16 {
    return arch::inw(port);
}

static constexpr u16 VBE_DISPI_IOPORT_INDEX = 0x01CE;
static constexpr u16 VBE_DISPI_IOPORT_DATA  = 0x01CF;
static constexpr u16 VBE_DISPI_INDEX_XRES   = 0x01;
static constexpr u16 VBE_DISPI_INDEX_YRES   = 0x02;
static constexpr u16 VBE_DISPI_INDEX_BPP    = 0x03;
static constexpr u16 VBE_DISPI_INDEX_ENABLE  = 0x04;
static constexpr u16 VBE_DISPI_INDEX_VIRT_WIDTH = 0x06;
static constexpr u16 VBE_DISPI_ENABLED       = 0x01;
static constexpr u16 VBE_DISPI_LFB_ENABLED   = 0x40;

static void vbe_write(u16 index, u16 value) {
    outw_vbe(VBE_DISPI_IOPORT_INDEX, index);
    outw_vbe(VBE_DISPI_IOPORT_DATA, value);
}
[[maybe_unused]] static auto vbe_read(u16 index) -> u16 {
    outw_vbe(VBE_DISPI_IOPORT_INDEX, index);
    return inw_vbe(VBE_DISPI_IOPORT_DATA);
}

static auto probe_bochs_vbe() -> framebuffer_info {
    /* Scan PCI bus 0 for device 1234:1111 (QEMU BochsVBE) */
    for (u8 dev = 0; dev < 32; ++dev) {
        u32 addr = 0x80000000u | (static_cast<u32>(dev) << 11);
        outl_pci(0xCF8, addr);
        u32 id = inl_pci(0xCFC);
        if ((id & 0xFFFF) != 0x1234 || ((id >> 16) & 0xFFFF) != 0x1111) continue;

        /* Read BAR0 — framebuffer physical address */
        outl_pci(0xCF8, addr | 0x10);
        u32 bar0 = inl_pci(0xCFC);
        phys_addr fb_base = bar0 & ~0xFu; /* mask lower 4 bits */

        /* Set video mode: 1024x768 x 32bpp via BochsVBE I/O ports */
        constexpr u16 WIDTH  = 1024;
        constexpr u16 HEIGHT = 768;
        vbe_write(VBE_DISPI_INDEX_ENABLE, 0);  /* disable first */
        vbe_write(VBE_DISPI_INDEX_XRES, WIDTH);
        vbe_write(VBE_DISPI_INDEX_YRES, HEIGHT);
        vbe_write(VBE_DISPI_INDEX_BPP, 32);
        vbe_write(VBE_DISPI_INDEX_VIRT_WIDTH, WIDTH);
        vbe_write(VBE_DISPI_INDEX_ENABLE, VBE_DISPI_ENABLED | VBE_DISPI_LFB_ENABLED);

        return {
            .base   = fb_base,
            .width  = WIDTH,
            .height = HEIGHT,
            .stride = WIDTH, /* BochsVBE: virtual width = stride */
            .format = pixel_format::bgrx_8bpp, /* QEMU VGA is always BGRX */
            .valid  = true,
        };
    }
    return { .base = 0, .width = 0, .height = 0, .stride = 0,
             .format = pixel_format::bgrx_8bpp, .valid = false };
}

auto query_gop() -> framebuffer_info {
    if (g_system_table == null || g_system_table->boot_services == null) {
        return {};
    }

    auto* bs = g_system_table->boot_services;
    constexpr u32 BY_PROTOCOL = 2;

    /* First try proper UEFI GOP */
    usize   count   = 0;
    handle* handles = null;
    gop_protocol* gop = null;

    auto st = bs->locate_handle_buffer(BY_PROTOCOL, &GOP_GUID, null, &count, &handles);
    if (st == status::success && count > 0 && handles != null) {
        for (usize i = 0; i < count; ++i) {
            void* iface = null;
            if (bs->handle_protocol(handles[i], &GOP_GUID, &iface) == status::success && iface != null) {
                auto* g = static_cast<gop_protocol*>(iface);
                if (g->mode != null && g->mode->frame_buffer_base != 0) {
                    gop = g;
                    break;
                }
            }
        }
        bs->free_pool(handles);
    }

    /* Try LocateProtocol as fallback */
    if (gop == null) {
        void* iface = null;
        if (bs->locate_protocol(&GOP_GUID, null, &iface) == status::success && iface != null) {
            auto* g = static_cast<gop_protocol*>(iface);
            if (g->mode != null && g->mode->frame_buffer_base != 0)
                gop = g;
        }
    }

    if (gop != null) {
        auto* mode = gop->mode;
        auto* info = mode->info;
        if (info) {
            return {
                .base   = mode->frame_buffer_base,
                .width  = info->horizontal_resolution,
                .height = info->vertical_resolution,
                .stride = info->pixels_per_scan_line,
                .format = info->fmt,
                .valid  = true,
            };
        }
    }

    /* GOP not available — fall back to direct BochsVBE programming */
    log::warn("GOP not found, trying direct BochsVBE probe...");
    return probe_bochs_vbe();
}

} // namespace uefi
} // namespace vk
