/*
 * vkernel - UEFI Microkernel
 * Copyright (C) 2026 vkernel authors
 *
 * acpi.h - ACPI table structures and discovery API
 *
 * Supports ACPI 1.0 (RSDT) and ACPI 2.0+ (XSDT).
 * RSDP is located via the UEFI configuration table before ExitBootServices.
 * All ACPI tables remain valid after EBS (they reside in ACPI-reclaimable memory).
 */

#ifndef VKERNEL_ACPI_H
#define VKERNEL_ACPI_H

#include "types.h"

namespace vk {
namespace acpi {

/* ============================================================
 * Root System Description Pointer (RSDP)
 * ============================================================ */

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

/* ============================================================
 * Generic System Descriptor Table Header
 * All ACPI tables begin with this 36-byte header.
 * ============================================================ */

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

/* ============================================================
 * RSDT / XSDT
 * The RSDT has 32-bit entries; the XSDT has 64-bit entries.
 * Both are immediately after the SDT header.
 * We do NOT use flexible array members (no C99/C++26 FAM here)
 * because we cast raw physical memory directly to these types.
 * Access entries via helper functions instead.
 * ============================================================ */

struct rsdt { sdt_header header; /* followed by u32 entries[] */ };
struct xsdt { sdt_header header; /* followed by u64 entries[] */ };

/* ============================================================
 * Multiple APIC Description Table (MADT / "APIC" table)
 * ============================================================ */

struct madt {
    sdt_header header;
    u32 local_apic_address;  /* physical address of local APIC */
    u32 flags;               /* bit 0: PCAT_COMPAT (dual 8259 present) */
    /* variable-length entry records follow */
};

/* MADT entry types */
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

/* Processor Local APIC (type 0) */
struct madt_lapic {
    madt_entry_hdr hdr;
    u8 acpi_uid;    /* ACPI Processor UID */
    u8 apic_id;     /* local APIC ID */
    u32 flags;      /* bit 0: enabled; bit 1: online capable */
};

/* I/O APIC (type 1) */
struct madt_ioapic {
    madt_entry_hdr hdr;
    u8  ioapic_id;
    u8  reserved;
    u32 ioapic_address;
    u32 gsi_base;   /* global system interrupt base */
};

/* Interrupt Source Override (type 2) */
struct madt_iso {
    madt_entry_hdr hdr;
    u8  bus_source;
    u8  irq_source;
    u32 global_system_interrupt;
    u16 flags;
};

#pragma pack(pop)

/* ============================================================
 * ACPI discovery API
 * ============================================================ */

/*
 * Initialize ACPI.
 * Must be called BEFORE ExitBootServices while the UEFI system table
 * pointer is still valid.  Locates the RSDP via the UEFI configuration
 * table, validates checksums, and walks the RSDT/XSDT to index tables.
 *
 * @param system_table_ptr  Pointer to the UEFI system table (uefi::system_table*).
 */
void init(void* system_table_ptr);

/*
 * Find an ACPI table by its 4-character signature (e.g. "APIC" for MADT).
 * Returns a pointer into the physical (identity-mapped) ACPI memory,
 * or null if the table was not found or ACPI was not initialized.
 *
 * The caller must not free the returned pointer.
 */
[[nodiscard]] const sdt_header* find_table(const char sig[4]);

/*
 * Get the MADT (Multiple APIC Description Table).
 * Equivalent to reinterpret_cast<const madt*>(find_table("APIC")).
 */
[[nodiscard]] const madt* get_madt();

/*
 * Iterate MADT entries of a given type.
 * Calls cb(entry_hdr*) for every MADT entry whose type field matches type.
 */
void foreach_madt_entry(madt_entry_type type,
                        void (*cb)(const madt_entry_hdr*, void* ctx),
                        void* ctx);

/* Returns true if ACPI was successfully initialized */
[[nodiscard]] bool is_initialized();

} // namespace acpi
} // namespace vk

#endif /* VKERNEL_ACPI_H */
