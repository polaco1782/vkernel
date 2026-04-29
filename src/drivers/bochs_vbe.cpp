/*
 * vkernel - UEFI Microkernel
 * Copyright (C) 2026 vkernel authors
 *
 * bochs_vbe.cpp - BochsVBE/QEMU display fallback
 *
 * Probes the QEMU standard VGA (PCI 1234:1111) via the PCI bus
 * subsystem and programs the BochsVBE I/O port interface to set
 * a 1024x768x32 linear framebuffer mode.
 */

#include "config.h"
#include "types.h"
#include "uefi.h"
#include "pci.h"
#include "arch/x86_64/arch.h"

namespace vk {
namespace uefi {

/* BochsVBE DISPI I/O ports */
static constexpr u16 VBE_DISPI_IOPORT_INDEX    = 0x01CE;
static constexpr u16 VBE_DISPI_IOPORT_DATA     = 0x01CF;

/* BochsVBE DISPI register indices */
static constexpr u16 VBE_DISPI_INDEX_XRES       = 0x01;
static constexpr u16 VBE_DISPI_INDEX_YRES       = 0x02;
static constexpr u16 VBE_DISPI_INDEX_BPP        = 0x03;
static constexpr u16 VBE_DISPI_INDEX_ENABLE     = 0x04;
static constexpr u16 VBE_DISPI_INDEX_VIRT_WIDTH = 0x06;

/* BochsVBE DISPI enable flags */
static constexpr u16 VBE_DISPI_ENABLED          = 0x01;
static constexpr u16 VBE_DISPI_LFB_ENABLED      = 0x40;

static void vbe_write(u16 index, u16 value) {
    arch::outw(VBE_DISPI_IOPORT_INDEX, index);
    arch::outw(VBE_DISPI_IOPORT_DATA, value);
}

/* Probe the QEMU BochsVBE device (PCI 1234:1111) and program a
 * 1024x768x32bpp linear framebuffer mode.  Returns a valid
 * framebuffer_info on success, or {.valid=false} if the device is
 * not present in the PCI device table. */
auto probe_bochs_vbe() -> framebuffer_info {
    const pci_device* dev = pci::find_device(0x1234, 0x1111);
    if (dev == null) {
        return { .base = 0, .width = 0, .height = 0, .stride = 0,
                 .format = pixel_format::bgrx_8bpp, .valid = false };
    }

    /* BAR0 is the linear framebuffer (memory-mapped); mask type flags */
    phys_addr fb_base = dev->bar[0] & ~0xFu;

    /* Program 1024x768x32bpp via BochsVBE I/O ports */
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
        .stride = WIDTH, /* BochsVBE: virtual width == stride */
        .format = pixel_format::bgrx_8bpp, /* QEMU VGA is always BGRX */
        .valid  = true,
    };
}

} // namespace uefi
} // namespace vk
