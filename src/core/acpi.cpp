/*
 * vkernel - UEFI Microkernel
 * Copyright (C) 2026 vkernel authors
 *
 * acpi.cpp - ACPI table discovery and parsing
 *
 * Locates the RSDP via the UEFI configuration table, validates all
 * checksums, walks the XSDT (or falls back to the RSDT for ACPI 1.0),
 * and caches pointers to the discovered SDTs for later use.
 */

#include "config.h"
#include "types.h"
#include "acpi.h"
#include "uefi.h"
#include "console.h"
#include "memory.h"

namespace vk {
namespace acpi {

/* ============================================================
 * Internal state
 * ============================================================ */

static const rsdp_v1* s_rsdp          = null;
static bool           s_use_xsdt      = false;
static bool           s_initialized   = false;

/* Cache of discovered SDT pointers (physical = virtual via identity map) */
static constexpr usize k_max_tables = 64;
static const sdt_header* s_tables[k_max_tables];
static u32 s_table_count = 0;

/* ============================================================
 * Checksum helpers
 * ============================================================ */

static bool verify_checksum(const void* ptr, usize length) {
    const auto* bytes = static_cast<const u8*>(ptr);
    u8 sum = 0;
    for (usize i = 0; i < length; ++i) {
        sum += bytes[i];
    }
    return sum == 0;
}

static bool sig4_equal(const char* a, const char* b) {
    return a[0] == b[0] && a[1] == b[1] && a[2] == b[2] && a[3] == b[3];
}

/* ============================================================
 * RSDT / XSDT walker
 * ============================================================ */

static void index_rsdt(const rsdt* table) {
    const u32 entry_count =
        (table->header.length - sizeof(sdt_header)) / sizeof(u32);

    const auto* entries = reinterpret_cast<const u32*>(
        reinterpret_cast<const u8*>(table) + sizeof(sdt_header));

    for (u32 i = 0; i < entry_count && s_table_count < k_max_tables; ++i) {
        const auto* hdr = reinterpret_cast<const sdt_header*>(
            static_cast<usize>(entries[i]));

        if (!verify_checksum(hdr, hdr->length)) {
            log::warn("ACPI: SDT %.4s at %p has bad checksum — skipped",
                      hdr->signature, hdr);
            continue;
        }

        s_tables[s_table_count++] = hdr;
        log::debug("ACPI: indexed SDT %.4s at %p (%u bytes)",
                   hdr->signature, hdr, hdr->length);
    }
}

static void index_xsdt(const xsdt* table) {
    const u32 entry_count =
        (table->header.length - sizeof(sdt_header)) / sizeof(u64);

    const auto* entries = reinterpret_cast<const u64*>(
        reinterpret_cast<const u8*>(table) + sizeof(sdt_header));

    for (u32 i = 0; i < entry_count && s_table_count < k_max_tables; ++i) {
        const auto* hdr = reinterpret_cast<const sdt_header*>(
            static_cast<usize>(entries[i]));

        if (!verify_checksum(hdr, hdr->length)) {
            log::warn("ACPI: SDT %.4s at %p has bad checksum — skipped",
                      hdr->signature, hdr);
            continue;
        }

        s_tables[s_table_count++] = hdr;
        log::debug("ACPI: indexed SDT %.4s at %p (%u bytes)",
                   hdr->signature, hdr, hdr->length);
    }
}

/* ============================================================
 * Public API
 * ============================================================ */

void init(void* system_table_ptr) {
    if (s_initialized) return;

    if (!system_table_ptr) {
        log::error("ACPI: null system table pointer");
        return;
    }

    /* Try ACPI 2.0 first, then fall back to 1.0 */
    void* rsdp_ptr = uefi::find_configuration_table(uefi::ACPI_20_GUID);
    bool is_v2 = (rsdp_ptr != null);

    if (!rsdp_ptr) {
        rsdp_ptr = uefi::find_configuration_table(uefi::ACPI_10_GUID);
    }

    if (!rsdp_ptr) {
        log::error("ACPI: RSDP not found in UEFI configuration tables");
        return;
    }

    log::info("ACPI: RSDP at %p (ACPI %s)", rsdp_ptr, is_v2 ? "2.0+" : "1.0");

    /* Validate RSDP v1 checksum (always covers first 20 bytes) */
    if (!verify_checksum(rsdp_ptr, sizeof(rsdp_v1))) {
        log::error("ACPI: RSDP checksum invalid");
        return;
    }

    const auto* rsdp1 = static_cast<const rsdp_v1*>(rsdp_ptr);

    /* Signature sanity check */
    if (memory::memory_compare(rsdp1->signature, "RSD PTR ", 8) != 0) {
        log::error("ACPI: RSDP signature mismatch");
        return;
    }

    s_rsdp = rsdp1;
    s_use_xsdt = false;

    if (rsdp1->revision >= 2 && is_v2) {
        const auto* rsdp2 = static_cast<const rsdp_v2*>(rsdp_ptr);

        /* Validate extended checksum */
        if (!verify_checksum(rsdp2, rsdp2->length)) {
            log::warn("ACPI: RSDP v2 extended checksum invalid, "
                      "falling back to RSDT");
        } else if (rsdp2->xsdt_address != 0) {
            s_use_xsdt = true;

            const auto* xsdt_ptr = reinterpret_cast<const xsdt*>(
                static_cast<usize>(rsdp2->xsdt_address));

            if (verify_checksum(xsdt_ptr, xsdt_ptr->header.length)) {
                log::info("ACPI: XSDT at %p, %u entries",
                          xsdt_ptr,
                          (xsdt_ptr->header.length - (u32)sizeof(sdt_header))
                          / (u32)sizeof(u64));
                index_xsdt(xsdt_ptr);
            } else {
                log::warn("ACPI: XSDT checksum invalid, falling back to RSDT");
                s_use_xsdt = false;
            }
        }
    }

    if (!s_use_xsdt) {
        if (rsdp1->rsdt_address == 0) {
            log::error("ACPI: no valid RSDT or XSDT found");
            return;
        }

        const auto* rsdt_ptr = reinterpret_cast<const rsdt*>(
            static_cast<usize>(rsdp1->rsdt_address));

        if (!verify_checksum(rsdt_ptr, rsdt_ptr->header.length)) {
            log::error("ACPI: RSDT checksum invalid");
            return;
        }

        log::info("ACPI: RSDT at %p, %u entries",
                  rsdt_ptr,
                  (rsdt_ptr->header.length - (u32)sizeof(sdt_header))
                  / (u32)sizeof(u32));

        index_rsdt(rsdt_ptr);
    }

    log::info("ACPI: initialized, %u SDTs indexed", s_table_count);
    s_initialized = true;
}

bool is_initialized() {
    return s_initialized;
}

const sdt_header* find_table(const char sig[4]) {
    for (u32 i = 0; i < s_table_count; ++i) {
        if (sig4_equal(s_tables[i]->signature, sig)) {
            return s_tables[i];
        }
    }
    return null;
}

const madt* get_madt() {
    return reinterpret_cast<const madt*>(find_table("APIC"));
}

void foreach_madt_entry(madt_entry_type type,
                        void (*cb)(const madt_entry_hdr*, void* ctx),
                        void* ctx) {
    const auto* m = get_madt();
    if (!m || !cb) return;

    const u8* pos = reinterpret_cast<const u8*>(m) + sizeof(madt);
    const u8* end = reinterpret_cast<const u8*>(m) + m->header.length;

    while (pos + sizeof(madt_entry_hdr) <= end) {
        const auto* entry = reinterpret_cast<const madt_entry_hdr*>(pos);

        if (entry->length < sizeof(madt_entry_hdr) || pos + entry->length > end) {
            break;
        }

        if (entry->type == type) {
            cb(entry, ctx);
        }

        pos += entry->length;
    }
}

} // namespace acpi
} // namespace vk
