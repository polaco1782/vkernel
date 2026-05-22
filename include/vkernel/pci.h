/*
 * vkernel - UEFI Microkernel
 * Copyright (C) 2026 vkernel authors
 *
 * pci.h - PCI bus enumeration and configuration space access
 */

#ifndef VKERNEL_PCI_H
#define VKERNEL_PCI_H

#include "types.h"

namespace vk {

struct pci_address {
    u8 bus;
    u8 device;
    u8 function;
};

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

namespace pci_ids {
    inline constexpr u16 VENDOR_INTEL   = 0x8086;
    inline constexpr u16 VENDOR_VIRTIO  = 0x1AF4;
    inline constexpr u16 DEVICE_AC97    = 0x2415;   /* 82801AA AC'97 Audio */
    inline constexpr u16 DEVICE_ICH4    = 0x24C5;   /* ICH4 AC'97 Audio   */
    inline constexpr u16 DEVICE_ICH6    = 0x2668;   /* ICH6 HD Audio      */
    inline constexpr u16 DEVICE_VIRTIO_NET_LEGACY = 0x1000;
    inline constexpr u16 DEVICE_VIRTIO_BLK_LEGACY = 0x1001;

    /* PCI class codes */
    inline constexpr u8 CLASS_NETWORK    = 0x02;
    inline constexpr u8 SUBCLASS_ETHERNET = 0x00;
    inline constexpr u8 CLASS_MULTIMEDIA = 0x04;
    inline constexpr u8 SUBCLASS_AUDIO   = 0x01;
    inline constexpr u8 CLASS_STORAGE    = 0x01;
}

namespace pci {

inline constexpr usize MAX_DEVICES = 64;

/* Legacy PCI config-mechanism-1 ports. */
inline constexpr u16 CONFIG_ADDRESS = 0x0CF8;
inline constexpr u16 CONFIG_DATA    = 0x0CFC;

void init();

auto config_read32(pci_address addr, u8 offset)  -> u32;
auto config_read16(pci_address addr, u8 offset)  -> u16;
auto config_read8(pci_address addr, u8 offset)   -> u8;
void config_write32(pci_address addr, u8 offset, u32 value);
void config_write16(pci_address addr, u8 offset, u16 value);
void config_write8(pci_address addr, u8 offset, u8 value);

auto find_device(u16 vendor_id, u16 device_id) -> const pci_device*;

auto find_by_class(u8 class_code, u8 subclass) -> const pci_device*;

auto device_count() -> usize;

auto get_device(usize index) -> const pci_device*;

void list_devices();

void enable_bus_master(pci_address addr);

} // namespace pci
} // namespace vk

#endif /* VKERNEL_PCI_H */
