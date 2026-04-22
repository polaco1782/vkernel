/*
 * vkernel - UEFI Microkernel
 * Copyright (C) 2026 vkernel authors
 *
 * pci.cpp - PCI bus enumeration via I/O ports 0xCF8/0xCFC
 */

#include "config.h"
#include "types.h"
#include "console.h"
#include "pci.h"
#include "arch/x86_64/arch.h"

namespace vk {
namespace pci {

/* ============================================================
 * Internal state
 * ============================================================ */

static pci_device s_devices[MAX_DEVICES];
static usize      s_count = 0;

/* ============================================================
 * Configuration space helpers
 * ============================================================ */

static u32 make_address(pci_address addr, u8 offset) {
    return (1u << 31)                          /* enable bit */
         | (static_cast<u32>(addr.bus) << 16)
         | (static_cast<u32>(addr.device) << 11)
         | (static_cast<u32>(addr.function) << 8)
         | (offset & 0xFC);
}

auto config_read32(pci_address addr, u8 offset) -> u32 {
    arch::outl(CONFIG_ADDRESS, make_address(addr, offset));
    return arch::inl(CONFIG_DATA);
}

auto config_read16(pci_address addr, u8 offset) -> u16 {
    u32 val = config_read32(addr, static_cast<u8>(offset & 0xFC));
    return static_cast<u16>((val >> ((offset & 2) * 8)) & 0xFFFF);
}

auto config_read8(pci_address addr, u8 offset) -> u8 {
    u32 val = config_read32(addr, static_cast<u8>(offset & 0xFC));
    return static_cast<u8>((val >> ((offset & 3) * 8)) & 0xFF);
}

void config_write32(pci_address addr, u8 offset, u32 value) {
    arch::outl(CONFIG_ADDRESS, make_address(addr, offset));
    arch::outl(CONFIG_DATA, value);
}

void config_write16(pci_address addr, u8 offset, u16 value) {
    arch::outl(CONFIG_ADDRESS, make_address(addr, offset));
    u32 old = arch::inl(CONFIG_DATA);
    u32 shift = (offset & 2) * 8;
    old &= ~(0xFFFFu << shift);
    old |= (static_cast<u32>(value) << shift);
    arch::outl(CONFIG_DATA, old);
}

void config_write8(pci_address addr, u8 offset, u8 value) {
    arch::outl(CONFIG_ADDRESS, make_address(addr, offset));
    u32 old = arch::inl(CONFIG_DATA);
    u32 shift = (offset & 3) * 8;
    old &= ~(0xFFu << shift);
    old |= (static_cast<u32>(value) << shift);
    arch::outl(CONFIG_DATA, old);
}

/* ============================================================
 * Bus mastering
 * ============================================================ */

void enable_bus_master(pci_address addr) {
    u16 cmd = config_read16(addr, 0x04);
    cmd |= (1u << 2);   /* Bus Master Enable */
    cmd |= (1u << 0);   /* I/O Space Enable  */
    config_write16(addr, 0x04, cmd);
}

/* ============================================================
 * Enumeration
 * ============================================================ */

static void probe_function(u8 bus, u8 device, u8 function) {
    pci_address addr = {bus, device, function};

    u32 id = config_read32(addr, 0x00);
    u16 vendor = static_cast<u16>(id & 0xFFFF);
    u16 dev_id = static_cast<u16>(id >> 16);

    if (vendor == 0xFFFF || vendor == 0x0000) return;
    if (s_count >= MAX_DEVICES) return;

    auto& d = s_devices[s_count];
    d.addr      = addr;
    d.vendor_id = vendor;
    d.device_id = dev_id;

    u32 class_reg   = config_read32(addr, 0x08);
    d.revision      = static_cast<u8>(class_reg & 0xFF);
    d.prog_if       = static_cast<u8>((class_reg >> 8) & 0xFF);
    d.subclass      = static_cast<u8>((class_reg >> 16) & 0xFF);
    d.class_code    = static_cast<u8>((class_reg >> 24) & 0xFF);

    d.header_type = config_read8(addr, 0x0E);
    d.irq_line    = config_read8(addr, 0x3C);

    /* Read BARs (only for header type 0) */
    if ((d.header_type & 0x7F) == 0x00) {
        for (int i = 0; i < 6; ++i) {
            d.bar[i] = config_read32(addr, static_cast<u8>(0x10 + i * 4));
        }
    } else {
        for (int i = 0; i < 6; ++i) d.bar[i] = 0;
    }

    ++s_count;
}

static void probe_device(u8 bus, u8 device) {
    pci_address addr = {bus, device, 0};
    u32 id = config_read32(addr, 0x00);
    if ((id & 0xFFFF) == 0xFFFF) return;

    probe_function(bus, device, 0);

    /* Check for multi-function device */
    u8 header = config_read8(addr, 0x0E);
    if (header & 0x80) {
        for (u8 func = 1; func < 8; ++func) {
            probe_function(bus, device, func);
        }
    }
}

void init() {
    s_count = 0;

    /* Scan all buses */
    for (u32 bus = 0; bus < 256; ++bus) {
        for (u8 dev = 0; dev < 32; ++dev) {
            probe_device(static_cast<u8>(bus), dev);
        }
    }

    console::puts("pci: enumerated ");
    console::put_dec(s_count);
    console::puts(" device(s)\n");
}

/* ============================================================
 * Lookup
 * ============================================================ */

auto find_device(u16 vendor_id, u16 device_id) -> const pci_device* {
    for (usize i = 0; i < s_count; ++i) {
        if (s_devices[i].vendor_id == vendor_id &&
            s_devices[i].device_id == device_id) {
            return &s_devices[i];
        }
    }
    return null;
}

auto find_by_class(u8 class_code, u8 subclass) -> const pci_device* {
    for (usize i = 0; i < s_count; ++i) {
        if (s_devices[i].class_code == class_code &&
            s_devices[i].subclass == subclass) {
            return &s_devices[i];
        }
    }
    return null;
}

auto device_count() -> usize {
    return s_count;
}

auto get_device(usize index) -> const pci_device* {
    if (index >= s_count) return null;
    return &s_devices[index];
}

/* ============================================================
 * Debug listing
 * ============================================================ */

void list_devices() {
    console::puts("PCI devices:\n");
    for (usize i = 0; i < s_count; ++i) {
        auto& d = s_devices[i];
        console::puts("  ");
        console::put_hex(d.addr.bus);
        console::puts(":");
        console::put_hex(d.addr.device);
        console::puts(".");
        console::put_hex(d.addr.function);
        console::puts(" vendor=");
        console::put_hex(d.vendor_id);
        console::puts(" device=");
        console::put_hex(d.device_id);
        console::puts(" class=");
        console::put_hex(d.class_code);
        console::puts(":");
        console::put_hex(d.subclass);
        console::puts(" irq=");
        console::put_dec(d.irq_line);
        console::puts("\n");
    }
}

} // namespace pci
} // namespace vk
