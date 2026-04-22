/*
 * vkernel - UEFI Microkernel
 * Copyright (C) 2026 vkernel authors
 *
 * pci.h - PCI bus enumeration and configuration space access
 *
 * Provides PCI configuration space read/write via the legacy
 * I/O port mechanism (ports 0xCF8 / 0xCFC).  Enumerates all
 * devices on buses 0–255 and maintains a simple flat table.
 */

#ifndef VKERNEL_PCI_H
#define VKERNEL_PCI_H

#include "types.h"

namespace vk {

/* ============================================================
 * PCI configuration space address
 * ============================================================ */

struct pci_address {
    u8 bus;
    u8 device;
    u8 function;
};

/* ============================================================
 * PCI device descriptor (populated during enumeration)
 * ============================================================ */

struct pci_device {
    pci_address addr;
    u16 vendor_id;
    u16 device_id;
    u8  class_code;
    u8  subclass;
    u8  prog_if;
    u8  revision;
    u8  header_type;
    u8  irq_line;       /* Interrupt line (from config reg 0x3C) */
    u32 bar[6];         /* Base Address Registers */
};

/* ============================================================
 * Well-known PCI vendor / device IDs
 * ============================================================ */

namespace pci_ids {
    inline constexpr u16 VENDOR_INTEL   = 0x8086;
    inline constexpr u16 DEVICE_AC97    = 0x2415;   /* 82801AA AC'97 Audio */
    inline constexpr u16 DEVICE_ICH4    = 0x24C5;   /* ICH4 AC'97 Audio   */
    inline constexpr u16 DEVICE_ICH6    = 0x2668;   /* ICH6 HD Audio      */

    /* PCI class codes */
    inline constexpr u8 CLASS_MULTIMEDIA = 0x04;
    inline constexpr u8 SUBCLASS_AUDIO   = 0x01;
}

/* ============================================================
 * PCI subsystem
 * ============================================================ */

namespace pci {

inline constexpr usize MAX_DEVICES = 64;

/* I/O ports for PCI configuration mechanism #1 */
inline constexpr u16 CONFIG_ADDRESS = 0x0CF8;
inline constexpr u16 CONFIG_DATA    = 0x0CFC;

/* Initialise PCI: enumerate all buses and populate device table. */
void init();

/* Read/write PCI configuration space (32-bit aligned). */
auto config_read32(pci_address addr, u8 offset)  -> u32;
auto config_read16(pci_address addr, u8 offset)  -> u16;
auto config_read8(pci_address addr, u8 offset)   -> u8;
void config_write32(pci_address addr, u8 offset, u32 value);
void config_write16(pci_address addr, u8 offset, u16 value);
void config_write8(pci_address addr, u8 offset, u8 value);

/* Look up a device by vendor + device ID.  Returns null if not found. */
auto find_device(u16 vendor_id, u16 device_id) -> const pci_device*;

/* Look up a device by class + subclass.  Returns null if not found. */
auto find_by_class(u8 class_code, u8 subclass) -> const pci_device*;

/* Return the number of discovered devices. */
auto device_count() -> usize;

/* Return pointer to device at index i (i < device_count()). */
auto get_device(usize index) -> const pci_device*;

/* Print all discovered PCI devices to the console. */
void list_devices();

/* Enable PCI bus-mastering for a device. */
void enable_bus_master(pci_address addr);

} // namespace pci
} // namespace vk

#endif /* VKERNEL_PCI_H */
