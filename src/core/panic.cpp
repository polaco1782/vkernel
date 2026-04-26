/*
 * vkernel - UEFI Microkernel
 * Copyright (C) 2026 vkernel authors
 *
 * panic.cpp - Kernel panic handler
 */

#include "config.h"
#include "types.h"
#include "console.h"
#include "arch/x86_64/arch.h"

namespace vk {

/*
 * Broadcast an NMI to all other CPUs so they stop executing too,
 * preventing a cascade of follow-up faults from corrupted state
 * after one CPU panics.  We talk to the LAPIC directly (no smp.h
 * dependency) and tolerate a missing/uninitialised LAPIC silently.
 *
 * ICR encoding (bits):
 *   [10:8]  delivery mode  (0b100 = NMI)
 *   [11]    destination mode (0 = physical)
 *   [14]    level (1 = assert)
 *   [15]    trigger mode (0 = edge)
 *   [19:18] destination shorthand (0b11 = all-excluding-self)
 */
static void broadcast_nmi_halt() {
    constexpr u32 MSR_IA32_APIC_BASE  = 0x1B;
    constexpr u64 APIC_BASE_ENABLE    = (1ULL << 11);
    constexpr u64 APIC_BASE_PHYS_MASK = 0x0000'0000'FFFF'F000ULL;

    u64 base = arch::rdmsr(MSR_IA32_APIC_BASE);
    if (!(base & APIC_BASE_ENABLE)) return;

    auto* lapic = reinterpret_cast<volatile u32*>(
        static_cast<usize>(base & APIC_BASE_PHYS_MASK));

    /* All-excluding-self | NMI | assert */
    constexpr u32 ICR_NMI_BROADCAST =
        (0b11u << 18) | (0b100u << 8) | (1u << 14);

    /* High part of ICR is don't-care for shorthand mode */
    lapic[0x310 / 4] = 0;
    lapic[0x300 / 4] = ICR_NMI_BROADCAST;
}

/* Panic handler */
[[noreturn]] void vk_panic(const char* file, u32 line, const char* condition) {
    /* Stop the other CPUs first so they don't keep faulting and
     * scribbling more error output on top of the panic message. */
    broadcast_nmi_halt();

    log::crash("\n*** KERNEL PANIC ***\nFile: %s\nLine: %u\nCondition: %s\n\nSystem halted.\n",
               file, line, condition);

    arch::disable_interrupts();
    while (true) {
        arch::cpu_halt();
    }
}

} // namespace vk
