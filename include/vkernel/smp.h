/*
 * vkernel - UEFI Microkernel
 * Copyright (C) 2026 vkernel authors
 *
 * smp.h - Symmetric Multi-Processing (SMP) API
 *
 * Discovers application processors (APs) via the ACPI MADT,
 * brings them up using the INIT-SIPI-SIPI sequence, and exposes
 * per-CPU identification helpers.
 */

#ifndef VKERNEL_SMP_H
#define VKERNEL_SMP_H

#include "types.h"

namespace vk {
namespace smp {

/* Maximum number of CPUs supported */
inline constexpr u32 MAX_CPUS = 32;

/* ============================================================
 * Per-CPU descriptor
 * ============================================================ */

struct cpu_info {
    u8   apic_id;   /* local APIC ID (used to address the CPU) */
    u8   acpi_uid;  /* ACPI processor UID from MADT */
    bool online;    /* true once the AP has reported itself ready */
};

/* ============================================================
 * SMP initialization
 *
 * Parses the ACPI MADT for Processor Local APIC entries,
 * sets up the AP trampoline at physical address 0x8000, then
 * issues INIT-SIPI-SIPI to each disabled AP.
 *
 * Must be called after arch::activate() and memory::init().
 * ACPI must have been initialized (acpi::init()) before calling.
 * ============================================================ */
void init();

/* ============================================================
 * Runtime queries
 * ============================================================ */

/* Total number of online CPUs (BSP + APs) */
[[nodiscard]] u32 cpu_count();

/* Local APIC ID of the calling CPU (reads LAPIC MMIO register) */
[[nodiscard]] u8 current_cpu_apic_id();

/* Get the cpu_info record for CPU index [0 .. cpu_count()-1].
 * Returns null if idx is out of range. */
[[nodiscard]] const cpu_info* get_cpu_info(u32 idx);

/* Dump all known CPUs to the log (for diagnostics) */
void dump_cpus();

} // namespace smp
} // namespace vk

#endif /* VKERNEL_SMP_H */
