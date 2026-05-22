/*
 * vkernel - UEFI Microkernel
 * Copyright (C) 2026 vkernel authors
 *
 * acpi.h - ACPI table structures and discovery API
 */

#ifndef VKERNEL_ACPI_H
#define VKERNEL_ACPI_H

#include "types.h"

namespace vk {
namespace acpi {

#pragma pack(push, 1)

struct rsdp_v1 {
    char signature[8];   /* "RSD PTR " (note trailing space) */
    u8   checksum;       /* sum of all bytes in this structure must be 0 */
    char oem_id[6];
    u8   revision;       /* 0 = ACPI 1.0; 2 = ACPI 2.0+ */
    u32  rsdt_address;   /* physical address of RSDT */
};

struct rsdp_v2 {
    rsdp_v1 v1;
    u32     length;        /* length of the entire RSDP (including extension) */
    u64     xsdt_address;  /* physical address of XSDT */
    u8      ext_checksum;  /* checksum of the entire RSDP */
    u8      reserved[3];
};

struct sdt_header {
    char signature[4];
    u32  length;
    u8   revision;
    u8   checksum;
    char oem_id[6];
    char oem_table_id[8];
    u32  oem_revision;
    u32  creator_id;
    u32  creator_revision;
};

/* Followed by u32/u64 entries in firmware memory. */
struct rsdt { sdt_header header; };
struct xsdt { sdt_header header; };

struct madt {
    sdt_header header;
    u32 local_apic_address;  /* physical address of local APIC */
    u32 flags;               /* bit 0: PCAT_COMPAT (dual 8259 present) */
};

enum class madt_entry_type : u8 {
    lapic              = 0,
    ioapic             = 1,
    iso                = 2,   /* Interrupt Source Override */
    lapic_nmi          = 4,
    lapic_addr_override = 5,
    x2apic             = 9,
};

struct madt_entry_hdr {
    madt_entry_type type;
    u8              length;
};

struct madt_lapic {
    madt_entry_hdr hdr;
    u8 acpi_uid;    /* ACPI Processor UID */
    u8 apic_id;     /* local APIC ID */
    u32 flags;      /* bit 0: enabled; bit 1: online capable */
};

struct madt_ioapic {
    madt_entry_hdr hdr;
    u8  ioapic_id;
    u8  reserved;
    u32 ioapic_address;
    u32 gsi_base;   /* global system interrupt base */
};

struct madt_iso {
    madt_entry_hdr hdr;
    u8  bus_source;
    u8  irq_source;
    u32 global_system_interrupt;
    u16 flags;
};

#pragma pack(pop)

/* Must run before ExitBootServices while UEFI tables are reachable. */
void init(void* system_table_ptr);

/* Returns an identity-mapped firmware pointer; caller must not free it. */
[[nodiscard]] const sdt_header* find_table(const char sig[4]);

[[nodiscard]] const madt* get_madt();

/* Visits each MADT entry of the requested type. */
void foreach_madt_entry(madt_entry_type type,
                        void (*cb)(const madt_entry_hdr*, void* ctx),
                        void* ctx);

[[nodiscard]] bool is_initialized();

} // namespace acpi
} // namespace vk

#endif /* VKERNEL_ACPI_H */
