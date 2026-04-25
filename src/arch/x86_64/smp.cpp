/*
 * vkernel - UEFI Microkernel
 * Copyright (C) 2026 vkernel authors
 *
 * smp.cpp - Symmetric Multi-Processing initialization (x86_64)
 *
 * Discovers all logical processors via the ACPI MADT (type-0 Local APIC
 * entries), sets up the AP startup trampoline at physical address 0x8000,
 * and brings each AP up with the Intel INIT-SIPI-SIPI protocol.
 *
 * APs share the BSP's GDT/IDT and page tables (identity-mapped UEFI
 * layout).  Each AP gets its own private kernel stack.
 *
 * Reference: Intel SDM Vol. 3A §8.4 — MP Initialization Protocol
 */

#include "config.h"
#include "types.h"
#include "smp.h"
#include "acpi.h"
#include "memory.h"
#include "console.h"
#include "arch/x86_64/arch.h"
#include "scheduler.h"

namespace vk {
namespace smp {

/* ============================================================
 * Local APIC (LAPIC) MMIO access
 * ============================================================ */

/* IA32_APIC_BASE MSR */
static constexpr u32 MSR_IA32_APIC_BASE = 0x1B;
static constexpr u64 APIC_BASE_ENABLE   = (1ULL << 11);
static constexpr u64 APIC_BASE_PHYS_MASK = 0x0000'0000'FFFF'F000ULL;

/* LAPIC register offsets (byte offsets from LAPIC MMIO base) */
static constexpr u32 LAPIC_ID          = 0x020;  /* Identification */
static constexpr u32 LAPIC_VER         = 0x030;  /* Version */
static constexpr u32 LAPIC_SVR         = 0x0F0;  /* Spurious Interrupt Vector */
static constexpr u32 LAPIC_ESR         = 0x280;  /* Error Status */
static constexpr u32 LAPIC_ICR_LOW     = 0x300;  /* Interrupt Command (low)  */
static constexpr u32 LAPIC_ICR_HIGH    = 0x310;  /* Interrupt Command (high) */

/* SVR bits */
static constexpr u32 LAPIC_SVR_ENABLE  = (1u << 8);
static constexpr u32 LAPIC_SVR_VECTOR  = 0xFF;   /* spurious vector */

/* ICR delivery modes */
static constexpr u32 LAPIC_ICR_INIT   = 0x00000500u; /* INIT IPI */
static constexpr u32 LAPIC_ICR_SIPI   = 0x00000600u; /* Startup IPI */

/* ICR level/trigger flags */
static constexpr u32 LAPIC_ICR_ASSERT  = (1u << 14);
static constexpr u32 LAPIC_ICR_LEVEL   = (1u << 15);

/* ICR_LOW encoding for INIT assert */
static constexpr u32 ICR_INIT_ASSERT =
    LAPIC_ICR_INIT | LAPIC_ICR_ASSERT | LAPIC_ICR_LEVEL;

/* ICR_LOW encoding for SIPI (OR in trampoline page vector) */
static constexpr u32 ICR_SIPI_BASE =
    LAPIC_ICR_SIPI | LAPIC_ICR_ASSERT;

/* Trampoline SIPI vector: page 0x08 → physical base 0x8000 */
static constexpr u8  SIPI_VECTOR      = 0x08;

static volatile u32* s_lapic_base = null;

static inline auto lapic_read(u32 offset) -> u32 {
    return s_lapic_base[offset / 4];
}

static inline void lapic_write(u32 offset, u32 value) {
    s_lapic_base[offset / 4] = value;
    (void)lapic_read(LAPIC_ID);   /* serialising read-back */
}

static void lapic_init_local() {
    /* Enable LAPIC via SVR register; set spurious vector to 0xFF */
    u32 svr = lapic_read(LAPIC_SVR);
    svr &= ~LAPIC_SVR_VECTOR;
    svr |= LAPIC_SVR_VECTOR | LAPIC_SVR_ENABLE;
    lapic_write(LAPIC_SVR, svr);

    /* Clear any pending errors */
    lapic_write(LAPIC_ESR, 0);
    (void)lapic_read(LAPIC_ESR);
}

/* ============================================================
 * Trampoline page physical layout
 *
 * The AP trampoline blob (from ap_trampoline.S) is copied to the
 * physical page at 0x8000.  The BSP then writes per-AP data into
 * the second half of that page (offsets 0x100–0x154).
 * ============================================================ */

static constexpr u64  TRAM_PHYS_BASE  = 0x8000;

/* Byte offsets within the trampoline page */
static constexpr u64 TRAM_GDT_DESC  = TRAM_PHYS_BASE + 0x100;
static constexpr u64 TRAM_GDT_DATA  = TRAM_PHYS_BASE + 0x110; /* 5 × 8 = 40 bytes */
static constexpr u64 TRAM_CR3       = TRAM_PHYS_BASE + 0x138;
static constexpr u64 TRAM_STACK     = TRAM_PHYS_BASE + 0x140;
static constexpr u64 TRAM_JUMP_FP   = TRAM_PHYS_BASE + 0x148; /* { u32 addr, u16 sel } */
static constexpr u64 TRAM_READY     = TRAM_PHYS_BASE + 0x150;

/* Pointer helper to a physical address (identity mapped) */
template<typename T>
static inline T* phys_ptr(u64 addr) {
    return reinterpret_cast<T*>(static_cast<usize>(addr));
}

/* ============================================================
 * Temporary GDT written into the trampoline page
 *
 * We build a minimal 5-entry GDT in the trampoline data area so the
 * AP can switch from real mode → 32-bit PM → 64-bit LM without
 * depending on the kernel's GDT location before page tables are stable.
 *
 * Entry layout (standard x86 8-byte descriptor):
 *   [15: 0]  limit[15:0]
 *   [31:16]  base[15:0]
 *   [39:32]  base[23:16]
 *   [47:40]  access byte
 *   [51:48]  limit[19:16]
 *   [55:52]  flags (G, D/B, L, AVL)
 *   [63:56]  base[31:24]
 * ============================================================ */

static void build_gdt_entry(u64* entry, u32 base, u32 limit,
                              u8 access, u8 flags) {
    /* limit[15:0] | base[15:0] | base[23:16] | access | flags|limit[19:16] | base[31:24] */
    *entry =
        ( static_cast<u64>(limit  & 0xFFFF)      )       |
        ( static_cast<u64>(base   & 0xFFFF) << 16 )       |
        ( static_cast<u64>((base  >> 16) & 0xFF) << 32 )  |
        ( static_cast<u64>(access)               << 40 )  |
        ( static_cast<u64>(((limit >> 16) & 0xF) | (flags & 0xF0)) << 48 ) |
        ( static_cast<u64>((base  >> 24) & 0xFF) << 56 );
}

static void write_trampoline_gdt() {
    /*
     * GDT descriptor at TRAM_GDT_DESC:
     *   limit = 5 * 8 - 1 = 39
     *   base  = TRAM_GDT_DATA (physical)
     */
    auto* gdt_desc_limit = phys_ptr<u16>(TRAM_GDT_DESC);
    auto* gdt_desc_base  = phys_ptr<u32>(TRAM_GDT_DESC + 2);
    *gdt_desc_limit = 5 * 8 - 1;
    *gdt_desc_base  = static_cast<u32>(TRAM_GDT_DATA);

    /* GDT entries */
    auto* gdt = phys_ptr<u64>(TRAM_GDT_DATA);

    /* [0] Null descriptor */
    gdt[0] = 0;

    /* [1] 32-bit kernel code: selector 0x08
     *     access=0x9A (present|ring0|code|readable), flags=0xCF (32-bit, 4KB) */
    build_gdt_entry(&gdt[1], 0, 0xFFFFF, 0x9A, 0xCF);

    /* [2] 32-bit kernel data: selector 0x10
     *     access=0x92 (present|ring0|data|writable), flags=0xCF */
    build_gdt_entry(&gdt[2], 0, 0xFFFFF, 0x92, 0xCF);

    /* [3] 64-bit kernel code: selector 0x18
     *     access=0x9A, flags=0xA0 (L=1, D=0 = 64-bit code, 4KB gran.) */
    build_gdt_entry(&gdt[3], 0, 0xFFFFF, 0x9A, 0xA0);

    /* [4] 64-bit kernel data: selector 0x20
     *     access=0x92, flags=0x00 (base/limit ignored in 64-bit) */
    build_gdt_entry(&gdt[4], 0, 0xFFFFF, 0x92, 0x00);
}

/* ============================================================
 * AP trampoline blob symbols (from ap_trampoline.S)
 * ============================================================ */

extern "C" u8 ap_trampoline_start[];
extern "C" u8 ap_trampoline_end[];

/* ap_entry_64 is defined in gcc_asm.S; it sets up RSP then calls
 * ap_init_secondary().                                              */
extern "C" void ap_entry_64();

/* ============================================================
 * Per-CPU state
 * ============================================================ */

static cpu_info s_cpus[MAX_CPUS];
static u32      s_cpu_count = 0;
static u8       s_bsp_apic_id = 0;

/* Per-AP kernel stacks */
static constexpr usize AP_STACK_SIZE = 65536;  /* 64 KB per AP */
static u8 s_ap_stacks[MAX_CPUS][AP_STACK_SIZE]
    __attribute__((aligned(16)));

/* ============================================================
 * I/O delay used for INIT/SIPI timing
 *
 * Each write to port 0x80 (BIOS POST code port) takes ~1–2 µs on a PC.
 * For the coarse delays needed here (10 ms, 200 µs) this is adequate.
 * ============================================================ */

static void io_delay_us(u32 us) {
    /* Conservative: assume each outb takes 0.5 µs → 2 writes per µs */
    for (u32 i = 0; i < us * 2; ++i) {
        arch::outb(0x80, 0x00);
    }
}

/* ============================================================
 * INIT-SIPI-SIPI sequence
 * ============================================================ */

static void send_init_ipi(u8 target_apic_id) {
    /* Write destination APIC ID into ICR_HIGH */
    lapic_write(LAPIC_ICR_HIGH, static_cast<u32>(target_apic_id) << 24);
    /* Send INIT assert */
    lapic_write(LAPIC_ICR_LOW, ICR_INIT_ASSERT);
}

static void send_sipi(u8 target_apic_id, u8 vector) {
    lapic_write(LAPIC_ICR_HIGH, static_cast<u32>(target_apic_id) << 24);
    lapic_write(LAPIC_ICR_LOW, ICR_SIPI_BASE | static_cast<u32>(vector));
}

/* Wait up to timeout_ms for the AP at ap_idx to set its ready flag */
static bool wait_ap_ready(u32 ap_idx, u32 timeout_ms) {
    auto* flag = phys_ptr<volatile u32>(TRAM_READY);
    for (u32 i = 0; i < timeout_ms * 1000; ++i) {
        arch::memory_barrier();
        if (*flag != 0) {
            s_cpus[ap_idx].online = true;
            return true;
        }
        io_delay_us(1);
    }
    return false;
}

/* ============================================================
 * AP secondary init — called from ap_entry_64 (gcc_asm.S)
 * ============================================================ */

extern "C" void ap_init_secondary() {
    /*
     * We are now in 64-bit long mode, running on an AP.
     * RSP was loaded from TRAM_STACK by ap_entry_64.
     *
     * Step 1: load the kernel's real GDT/IDT and reload segment selectors.
     */
    arch::ap_activate();

    /*
     * Step 2: enable the LAPIC on this AP.
     */
    lapic_init_local();

    /*
     * Step 3: enable interrupts (the IDT is now live).
     */
    arch::enable_interrupts();

    /*
     * Step 4: signal the BSP that this AP is ready.
     */
    arch::memory_barrier();
    *phys_ptr<volatile u32>(TRAM_READY) = 1;

    log::info("AP APIC %u: online", current_cpu_apic_id());

    /*
     * Step 5: idle loop.
     * Future work: integrate with scheduler (per-AP task queue).
     */
    while (true) {
        arch::cpu_halt();
    }
}

/* ============================================================
 * smp::init() — BSP-side SMP bringup
 * ============================================================ */

void init() {
    log::info("SMP: initializing...");

    if (!acpi::is_initialized()) {
        log::warn("SMP: ACPI not initialized — skipping AP bringup");
        return;
    }

    /* Locate the LAPIC base address */
    u64 apic_base_msr = arch::rdmsr(MSR_IA32_APIC_BASE);
    if (!(apic_base_msr & APIC_BASE_ENABLE)) {
        log::warn("SMP: LAPIC globally disabled — skipping AP bringup");
        return;
    }

    u64 lapic_phys = apic_base_msr & APIC_BASE_PHYS_MASK;
    s_lapic_base = reinterpret_cast<volatile u32*>(static_cast<usize>(lapic_phys));
    log::debug("SMP: LAPIC MMIO at %p", s_lapic_base);

    /* Enable the BSP's LAPIC */
    lapic_init_local();

    /* Record the BSP's APIC ID */
    s_bsp_apic_id = static_cast<u8>((lapic_read(LAPIC_ID) >> 24) & 0xFF);
    log::debug("SMP: BSP APIC ID = %u", s_bsp_apic_id);

    /* Enumerate CPUs from MADT */
    struct lapic_enum_ctx {
        u32  count;
        u8   bsp_id;
    };

    lapic_enum_ctx enum_ctx{ 0, s_bsp_apic_id };

    acpi::foreach_madt_entry(acpi::madt_entry_type::lapic,
        [](const acpi::madt_entry_hdr* raw, void* ctx_ptr) {
            const auto* entry = reinterpret_cast<const acpi::madt_lapic*>(raw);
            auto* ctx = static_cast<lapic_enum_ctx*>(ctx_ptr);

            /* Skip entries that are not enabled and not online-capable */
            const bool enabled        = (entry->flags & 0x1) != 0;
            const bool online_capable = (entry->flags & 0x2) != 0;
            if (!enabled && !online_capable) return;

            if (ctx->count >= MAX_CPUS) return;

            auto& cpu = s_cpus[ctx->count];
            cpu.apic_id  = entry->apic_id;
            cpu.acpi_uid = entry->acpi_uid;
            cpu.online   = (entry->apic_id == ctx->bsp_id);
            ++ctx->count;
        },
        &enum_ctx);

    s_cpu_count = enum_ctx.count;
    log::info("SMP: found %u CPU(s) in MADT", s_cpu_count);

    if (s_cpu_count == 0) {
        /* No LAPIC entries — treat BSP as CPU 0 */
        s_cpus[0] = { s_bsp_apic_id, 0, true };
        s_cpu_count = 1;
        log::warn("SMP: no MADT LAPIC entries found; single-CPU mode");
        return;
    }

    /* ── Prepare the AP trampoline page ── */

    /* Copy the blob to physical 0x8000 */
    const usize blob_size =
        static_cast<usize>(ap_trampoline_end - ap_trampoline_start);
    memory::memory_copy(phys_ptr<void>(TRAM_PHYS_BASE),
                        ap_trampoline_start, blob_size);
    log::debug("SMP: trampoline blob (%zu bytes) copied to %#llx",
               blob_size, static_cast<unsigned long long>(TRAM_PHYS_BASE));

    /* Write the temporary GDT into the data area */
    write_trampoline_gdt();

    /* Write BSP's CR3 */
    *phys_ptr<u64>(TRAM_CR3) = arch::read_cr3();

    /* Write the 64-bit far-jump descriptor:
     *   { u32 physical_address_of_ap_entry_64, u16 SEL_CODE64=0x18 }
     * Since UEFI loads images below 4 GB, ap_entry_64's address fits
     * in 32 bits and can be used as a 32-bit far-jump target.         */
    const u64 entry_addr = reinterpret_cast<u64>(&ap_entry_64);
    *phys_ptr<u32>(TRAM_JUMP_FP)     = static_cast<u32>(entry_addr);
    *phys_ptr<u16>(TRAM_JUMP_FP + 4) = 0x18; /* SEL_CODE64 */

    log::debug("SMP: ap_entry_64 @ %#llx",
               static_cast<unsigned long long>(entry_addr));

    u32 ap_count = 0;
    for (u32 i = 0; i < s_cpu_count; ++i) {
        if (s_cpus[i].apic_id == s_bsp_apic_id) continue; /* skip BSP */

        log::info("SMP - Booting processor #%u: APIC ID %u, ACPI UID %u",
                  i, s_cpus[i].apic_id, s_cpus[i].acpi_uid);

        const u8 apic_id = s_cpus[i].apic_id;
        const u32 ap_idx = i;

        /* Allocate and wire this AP's stack (top of stack) */
        const u64 stack_top =
            reinterpret_cast<u64>(&s_ap_stacks[ap_idx][AP_STACK_SIZE]);
        *phys_ptr<u64>(TRAM_STACK) = stack_top;

        /* Clear the ready flag */
        *phys_ptr<volatile u32>(TRAM_READY) = 0;
        arch::memory_barrier();

        log::debug("SMP: starting AP APIC ID %u (stack top %#llx)...",
                   apic_id,
                   static_cast<unsigned long long>(stack_top));

        /* INIT IPI */
        send_init_ipi(apic_id);
        io_delay_us(10000);   /* 10 ms */

        /* First SIPI */
        send_sipi(apic_id, SIPI_VECTOR);
        io_delay_us(200);     /* 200 µs */

        /* Second SIPI (in case the first was missed) */
        send_sipi(apic_id, SIPI_VECTOR);

        /* Wait up to 1 s for the AP to set its ready flag */
        if (wait_ap_ready(ap_idx, 1000)) {
            ++ap_count;
            log::info("SMP: AP APIC %u up", apic_id);
        } else {
            log::warn("SMP: AP APIC %u did not respond within 1 s", apic_id);
            s_cpus[ap_idx].online = false;
        }
    }

    log::info("SMP: %u AP(s) started; total %u CPU(s) online",
              ap_count, ap_count + 1 /* BSP */);
}

/* ============================================================
 * Public query API
 * ============================================================ */

u32 cpu_count() {
    u32 online = 0;
    for (u32 i = 0; i < s_cpu_count; ++i) {
        if (s_cpus[i].online) ++online;
    }
    return online == 0 ? 1 : online;
}

u8 current_cpu_apic_id() {
    if (!s_lapic_base) return s_bsp_apic_id;
    return static_cast<u8>((lapic_read(LAPIC_ID) >> 24) & 0xFF);
}

const cpu_info* get_cpu_info(u32 idx) {
    if (idx >= s_cpu_count) return null;
    return &s_cpus[idx];
}

void dump_cpus() {
    log::info("SMP: %u CPU(s) detected:", s_cpu_count);
    for (u32 i = 0; i < s_cpu_count; ++i) {
        log::info("  CPU %u: APIC ID=%u, ACPI UID=%u, %s",
                  i, s_cpus[i].apic_id, s_cpus[i].acpi_uid,
                  s_cpus[i].online ? "online" : "offline");
    }
}

} // namespace smp
} // namespace vk
