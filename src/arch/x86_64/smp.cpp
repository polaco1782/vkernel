/*
 * vkernel - UEFI Microkernel
 * Copyright (C) 2026 vkernel authors
 *
 * smp.cpp - Symmetric Multi-Processing initialization (x86_64)
 */

#include "config.h"
#include "types.h"
#include "smp.h"
#include "acpi.h"
#include "memory.h"
#include "console.h"
#include "log.h"
#include "arch/x86_64/arch.h"
#include "scheduler.h"

namespace vk {
namespace smp {

/* ============================================================
 * Local APIC (LAPIC) MMIO access
 * ============================================================ */

static constexpr u32 MSR_IA32_APIC_BASE = 0x1B;
static constexpr u64 APIC_BASE_ENABLE   = (1ULL << 11);
static constexpr u64 APIC_BASE_PHYS_MASK = 0x0000'0000'FFFF'F000ULL;

/* LAPIC register offsets from the MMIO base. */
static constexpr u32 LAPIC_ID          = 0x020;  /* Identification */
static constexpr u32 LAPIC_VER         = 0x030;  /* Version */
static constexpr u32 LAPIC_SVR         = 0x0F0;  /* Spurious Interrupt Vector */
static constexpr u32 LAPIC_ESR         = 0x280;  /* Error Status */
static constexpr u32 LAPIC_ICR_LOW     = 0x300;  /* Interrupt Command (low)  */
static constexpr u32 LAPIC_ICR_HIGH    = 0x310;  /* Interrupt Command (high) */

static constexpr u32 LAPIC_SVR_ENABLE  = (1u << 8);
static constexpr u32 LAPIC_SVR_VECTOR  = 0xFF;   /* spurious vector */

static constexpr u32 LAPIC_ICR_INIT   = 0x00000500u; /* INIT IPI */
static constexpr u32 LAPIC_ICR_SIPI   = 0x00000600u; /* Startup IPI */

static constexpr u32 LAPIC_ICR_ASSERT  = (1u << 14);
static constexpr u32 LAPIC_ICR_LEVEL   = (1u << 15);

static constexpr u32 ICR_INIT_ASSERT =
    LAPIC_ICR_INIT | LAPIC_ICR_ASSERT | LAPIC_ICR_LEVEL;

static constexpr u32 ICR_SIPI_BASE =
    LAPIC_ICR_SIPI | LAPIC_ICR_ASSERT;

/* SIPI vector for the trampoline page at 0x8000. */
static constexpr u8  SIPI_VECTOR      = 0x08;

static volatile u32* s_lapic_base = null;

static inline auto lapic_read(u32 offset) -> u32 {
    return s_lapic_base[offset / 4];
}

static inline void lapic_write(u32 offset, u32 value) {
    s_lapic_base[offset / 4] = value;
    (void)lapic_read(LAPIC_ID);   /* serialize the MMIO write */
}

static void lapic_init_local() {
    u32 svr = lapic_read(LAPIC_SVR);
    svr &= ~LAPIC_SVR_VECTOR;
    svr |= LAPIC_SVR_VECTOR | LAPIC_SVR_ENABLE;
    lapic_write(LAPIC_SVR, svr);

    lapic_write(LAPIC_ESR, 0);
    (void)lapic_read(LAPIC_ESR);
}

/* Low page used for the AP trampoline and per-AP handoff data. */
static constexpr u64  TRAM_PHYS_BASE  = 0x8000;

static constexpr u64 TRAM_GDT_DESC  = TRAM_PHYS_BASE + 0x100;
static constexpr u64 TRAM_GDT_DATA  = TRAM_PHYS_BASE + 0x110; /* 5 × 8 = 40 bytes */
static constexpr u64 TRAM_CR3       = TRAM_PHYS_BASE + 0x138;
static constexpr u64 TRAM_STACK     = TRAM_PHYS_BASE + 0x140;
static constexpr u64 TRAM_JUMP_FP   = TRAM_PHYS_BASE + 0x148; /* { u32 addr, u16 sel } */
static constexpr u64 TRAM_READY     = TRAM_PHYS_BASE + 0x150;

template<typename T>
static inline T* phys_ptr(u64 addr) {
    return reinterpret_cast<T*>(static_cast<usize>(addr));
}

/* Build the tiny GDT the trampoline needs before long mode is stable. */
static void build_gdt_entry(u64* entry, u32 base, u32 limit,
                              u8 access, u8 flags) {
    *entry =
        ( static_cast<u64>(limit  & 0xFFFF)      )       |
        ( static_cast<u64>(base   & 0xFFFF) << 16 )       |
        ( static_cast<u64>((base  >> 16) & 0xFF) << 32 )  |
        ( static_cast<u64>(access)               << 40 )  |
        ( static_cast<u64>(((limit >> 16) & 0xF) | (flags & 0xF0)) << 48 ) |
        ( static_cast<u64>((base  >> 24) & 0xFF) << 56 );
}

static void write_trampoline_gdt() {
    auto* gdt_desc_limit = phys_ptr<u16>(TRAM_GDT_DESC);
    auto* gdt_desc_base  = phys_ptr<u32>(TRAM_GDT_DESC + 2);
    *gdt_desc_limit = 5 * 8 - 1;
    *gdt_desc_base  = static_cast<u32>(TRAM_GDT_DATA);

    auto* gdt = phys_ptr<u64>(TRAM_GDT_DATA);

    gdt[0] = 0;

    /* 32-bit code/data first, then 64-bit selectors for the final jump. */
    build_gdt_entry(&gdt[1], 0, 0xFFFFF, 0x9A, 0xCF);
    build_gdt_entry(&gdt[2], 0, 0xFFFFF, 0x92, 0xCF);
    build_gdt_entry(&gdt[3], 0, 0xFFFFF, 0x9A, 0xA0);
    build_gdt_entry(&gdt[4], 0, 0xFFFFF, 0x92, 0x00);
}

/* ============================================================
 * AP trampoline blob symbols (from ap_trampoline.S)
 * ============================================================ */

#if defined(_MSC_VER)
extern "C" const u8 g_ap_trampoline_blob[];
extern "C" const usize g_ap_trampoline_blob_size;
#else
extern "C" u8 ap_trampoline_start[];
extern "C" u8 ap_trampoline_end[];
#endif

/* Defined in assembly; loads RSP and tail-calls ap_init_secondary(). */
extern "C" void ap_entry_64();

/* ============================================================
 * Per-CPU state
 * ============================================================ */

static cpu_info s_cpus[MAX_CPUS];
static u32      s_cpu_count = 0;
static u8       s_bsp_apic_id = 0;

static constexpr usize AP_STACK_SIZE = 65536;  /* 64 KB per AP */
#if defined(_MSC_VER)
static __declspec(align(16)) u8 s_ap_stacks[MAX_CPUS][AP_STACK_SIZE];
#else
static u8 s_ap_stacks[MAX_CPUS][AP_STACK_SIZE] __attribute__((aligned(16)));
#endif

/* Coarse delay for INIT/SIPI timing via the POST port. */
static void io_delay_us(u32 us) {
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

/* Wait for the trampoline handshake to report the AP online. */
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
    /* Switch from the trampoline tables to the kernel's real CPU state. */
    arch::ap_activate();

    lapic_init_local();

    arch::memory_barrier();
    *phys_ptr<volatile u32>(TRAM_READY) = 1;

    log::info() << "AP APIC " << current_cpu_apic_id() << ": online, entering scheduler AP loop";

    sched::start_ap();
}

/* ============================================================
 * smp::init() — BSP-side SMP bringup
 * ============================================================ */

void init() {
    log::info() << "SMP: initializing...";

    if (!acpi::is_initialized()) {
        log::warn() << "SMP: ACPI not initialized — skipping AP bringup";
        return;
    }

    /* Locate the LAPIC base address */
    u64 apic_base_msr = arch::rdmsr(MSR_IA32_APIC_BASE);
    if (!(apic_base_msr & APIC_BASE_ENABLE)) {
        log::warn() << "SMP: LAPIC globally disabled — skipping AP bringup";
        return;
    }

    u64 lapic_phys = apic_base_msr & APIC_BASE_PHYS_MASK;
    s_lapic_base = reinterpret_cast<volatile u32*>(static_cast<usize>(lapic_phys));
    log::debug() << "SMP: LAPIC MMIO at " << s_lapic_base;

    /* Enable the BSP's LAPIC */
    lapic_init_local();

    /* Record the BSP's APIC ID */
    s_bsp_apic_id = static_cast<u8>((lapic_read(LAPIC_ID) >> 24) & 0xFF);
    log::debug() << "SMP: BSP APIC ID = " << s_bsp_apic_id;

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
    log::info() << "SMP: found " << s_cpu_count << " CPU(s) in MADT";

    if (s_cpu_count == 0) {
        /* No LAPIC entries — treat BSP as CPU 0 */
        s_cpus[0] = { s_bsp_apic_id, 0, true };
        s_cpu_count = 1;
        log::warn() << "SMP: no MADT LAPIC entries found; single-CPU mode";
        return;
    }

    /* Copy the trampoline into low memory and fill its handoff slots. */
    #if defined(_MSC_VER)
    const u8* trampoline_blob = g_ap_trampoline_blob;
    const usize blob_size = g_ap_trampoline_blob_size;
    #else
    const u8* trampoline_blob = ap_trampoline_start;
    const usize blob_size =
        static_cast<usize>(ap_trampoline_end - ap_trampoline_start);
    #endif
    memory::copy(phys_ptr<void>(TRAM_PHYS_BASE),
                        trampoline_blob, blob_size);
    log::debug() << "SMP: trampoline blob (" << blob_size << " bytes) copied to " << log::hex(static_cast<u64>(static_cast<unsigned long long>(TRAM_PHYS_BASE)), 1, true, false);

    write_trampoline_gdt();

    *phys_ptr<u64>(TRAM_CR3) = arch::read_cr3();

    /* Real-mode trampoline uses a 32-bit far pointer into ap_entry_64. */
    const u64 entry_addr = reinterpret_cast<u64>(&ap_entry_64);
    *phys_ptr<u32>(TRAM_JUMP_FP)     = static_cast<u32>(entry_addr);
    *phys_ptr<u16>(TRAM_JUMP_FP + 4) = 0x18; /* SEL_CODE64 */

    log::debug() << "SMP: ap_entry_64 @ " << log::hex(static_cast<u64>(static_cast<unsigned long long>(entry_addr)), 1, true, false);

    u32 ap_count = 0;
    for (u32 i = 0; i < s_cpu_count; ++i) {
        if (s_cpus[i].apic_id == s_bsp_apic_id) continue; /* skip BSP */

        log::info() << "SMP - Booting processor #" << i << ": APIC ID " << s_cpus[i].apic_id << ", ACPI UID " << s_cpus[i].acpi_uid;

        const u8 apic_id = s_cpus[i].apic_id;
        const u32 ap_idx = i;

        const u64 stack_top =
            reinterpret_cast<u64>(&s_ap_stacks[ap_idx][AP_STACK_SIZE]);
        *phys_ptr<u64>(TRAM_STACK) = stack_top;

        *phys_ptr<volatile u32>(TRAM_READY) = 0;
        arch::memory_barrier();

        log::debug() << "SMP: starting AP APIC ID " << apic_id << " (stack top " << log::hex(static_cast<u64>(static_cast<unsigned long long>(stack_top)), 1, true, false) << ")...";

        send_init_ipi(apic_id);
        io_delay_us(10000);   /* 10 ms */

        send_sipi(apic_id, SIPI_VECTOR);
        io_delay_us(200);     /* 200 µs */

        /* Send the architectural second SIPI in case the first was missed. */
        send_sipi(apic_id, SIPI_VECTOR);
        if (wait_ap_ready(ap_idx, 1000)) {
            ++ap_count;
            log::info() << "SMP: AP APIC " << apic_id << " up";
        } else {
            log::warn() << "SMP: AP APIC " << apic_id << " did not respond within 1 s";
            s_cpus[ap_idx].online = false;
        }
    }

    log::info() << "SMP: " << ap_count << " AP(s) started; total " << ap_count + 1 /* BSP */ << " CPU(s) online";
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

u32 current_cpu_index() {
    u8 apic_id = current_cpu_apic_id();
    for (u32 i = 0; i < s_cpu_count; ++i) {
        if (s_cpus[i].apic_id == apic_id) {
            return i;
        }
    }

    return (apic_id < MAX_CPUS) ? apic_id : 0;
}

const cpu_info* get_cpu_info(u32 idx) {
    if (idx >= s_cpu_count) return null;
    return &s_cpus[idx];
}

void dump_cpus() {
    log::info() << "SMP: " << s_cpu_count << " CPU(s) detected:";
    for (u32 i = 0; i < s_cpu_count; ++i) {
        log::info() << "  CPU " << i << ": APIC ID=" << s_cpus[i].apic_id
                    << ", ACPI UID=" << s_cpus[i].acpi_uid << ", "
                    << (s_cpus[i].online ? "online" : "offline");
    }
}

} // namespace smp
} // namespace vk
