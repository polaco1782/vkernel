/*
 * vkernel - UEFI Microkernel
 * Copyright (C) 2026 vkernel authors
 *
 * arch/x86_64/arch_init.cpp - x86_64 architecture initialization
 */

#include "config.h"
#include "types.h"
#include "console.h"
#include "log.h"
#include "memory.h"
#include "scheduler.h"
#include "panic.h"
#include "process_internal.h"
#include "arch/x86_64/arch.h"
#include "smp.h"
#if defined(_MSC_VER)
#include "msvc_asm.h"
#else
#include "gcc_asm.h"
#endif

namespace vk {
namespace arch {

/* ============================================================
 * Exception name table (vectors 0-31)
 * ============================================================ */

static const char* const s_exception_names[32] = {
    "Division Error",           /*  0 */
    "Debug",                    /*  1 */
    "NMI",                      /*  2 */
    "Breakpoint",               /*  3 */
    "Overflow",                 /*  4 */
    "Bound Range Exceeded",     /*  5 */
    "Invalid Opcode",           /*  6 */
    "Device Not Available",     /*  7 */
    "Double Fault",             /*  8 */
    "Coprocessor Segment",      /*  9 */
    "Invalid TSS",              /* 10 */
    "Segment Not Present",      /* 11 */
    "Stack-Segment Fault",      /* 12 */
    "General Protection Fault", /* 13 */
    "Page Fault",               /* 14 */
    "Reserved",                 /* 15 */
    "x87 FP Exception",        /* 16 */
    "Alignment Check",          /* 17 */
    "Machine Check",            /* 18 */
    "SIMD FP Exception",       /* 19 */
    "Virtualisation Exception", /* 20 */
    "Control Protection",       /* 21 */
    "Reserved", "Reserved", "Reserved", "Reserved", "Reserved", "Reserved",
    "Hypervisor Injection",     /* 28 */
    "VMM Communication",        /* 29 */
    "Security Exception",       /* 30 */
    "Reserved",                 /* 31 */
};

/* ============================================================
 * GDT — 7 entries (long mode, 16-byte TSS descriptor)
 *
 *  [0] Null
 *  [1] Kernel Code  (ring 0, 64-bit)
 *  [2] Kernel Data  (ring 0)
 *  [3] User Code    (ring 3, 64-bit)
 *  [4] User Data    (ring 3)
 *  [5-6] TSS        (16-byte system segment descriptor)
 * ============================================================ */

static gdt_entry g_gdt[smp::MAX_CPUS][7];
static gdt_ptr g_gdt_ptr[smp::MAX_CPUS];
static tss g_tss[smp::MAX_CPUS];

/* Helper: install a standard (8-byte) GDT descriptor */
static void gdt_set_entry(u32 cpu, u32 idx, u32 base, u32 limit,
                           u8 access, u8 granularity) {
    g_gdt[cpu][idx].limit_low    = limit & 0xFFFF;
    g_gdt[cpu][idx].base_low     = base & 0xFFFF;
    g_gdt[cpu][idx].base_middle  = (base >> 16) & 0xFF;
    g_gdt[cpu][idx].access       = access;
    g_gdt[cpu][idx].granularity  = static_cast<u8>(((limit >> 16) & 0x0F) | (granularity & 0xF0));
    g_gdt[cpu][idx].base_high    = (base >> 24) & 0xFF;
}

/* Install the 16-byte TSS descriptor at g_gdt[5..6] */
static void gdt_set_tss(u32 cpu, u32 idx, u64 base, u32 limit) {
    /* Low 8 bytes — identical to a normal descriptor */
    g_gdt[cpu][idx].limit_low   = limit & 0xFFFF;
    g_gdt[cpu][idx].base_low    = base & 0xFFFF;
    g_gdt[cpu][idx].base_middle = (base >> 16) & 0xFF;
    g_gdt[cpu][idx].access      = 0x89; /* Present, 64-bit TSS (Available) */
    g_gdt[cpu][idx].granularity = static_cast<u8>(((limit >> 16) & 0x0F));
    g_gdt[cpu][idx].base_high   = (base >> 24) & 0xFF;

    /* High 8 bytes — base[63:32] + reserved */
    auto* high = reinterpret_cast<u32*>(&g_gdt[cpu][idx + 1]);
    high[0] = static_cast<u32>(base >> 32);
    high[1] = 0;
}

static void init_gdt_for_cpu(u32 cpu) {
    /* Null descriptor */
    gdt_set_entry(cpu, 0, 0, 0, 0, 0);

    /*
     * Long-mode code segments:
     *   access  = 0x9A (Present, ring 0, code, readable)
     *   gran    = 0xA0 (L=1 64-bit, D=0)
     *
     * Long-mode data segments:
     *   access  = 0x92 (Present, ring 0, data, writable)
     *   gran    = 0x00 (L and D ignored for data in long mode)
     */
    gdt_set_entry(cpu, 1, 0, 0xFFFFF, 0x9A, 0xA0); /* Kernel Code 64-bit */
    gdt_set_entry(cpu, 2, 0, 0xFFFFF, 0x92, 0x00); /* Kernel Data */
    gdt_set_entry(cpu, 3, 0, 0xFFFFF, 0xFA, 0xA0); /* User Code 64-bit */
    gdt_set_entry(cpu, 4, 0, 0xFFFFF, 0xF2, 0x00); /* User Data */

    /* TSS */
    memory::set(&g_tss[cpu], 0, sizeof(tss));
    g_tss[cpu].iomap_base = sizeof(tss);
    gdt_set_tss(cpu, 5, reinterpret_cast<u64>(&g_tss[cpu]), sizeof(tss) - 1);

    g_gdt_ptr[cpu].limit = sizeof(g_gdt[cpu]) - 1;
    g_gdt_ptr[cpu].base  = reinterpret_cast<u64>(&g_gdt[cpu]);
}

void init_gdt() {
    log::info() << "Preparing per-CPU GDTs...";

    for (u32 cpu = 0; cpu < smp::MAX_CPUS; ++cpu) {
        init_gdt_for_cpu(cpu);
    }

    log::info() << "GDT prepared";
}

/* Load the GDT descriptor table register only.
 * Keep this separate from selector/TSS reload so activate() can
 * place additional diagnostics around the risky transition steps. */
static void load_gdt() {
    asm_lgdt(&g_gdt_ptr[smp::current_cpu_index()]);
}

/* Reload CS/DS/ES/SS to the kernel descriptors from our GDT. */
static void reload_kernel_segments() {
    asm_reload_segments(static_cast<u64>(SEG_KERNEL_CODE), static_cast<u64>(SEG_KERNEL_DATA));
}

/* Load the task register with our TSS selector.
 * Seed rsp0 so privilege transitions have a valid ring-0 stack. */
static void activate_tss() {
    g_tss[smp::current_cpu_index()].rsp0 = read_rsp();
    asm_ltr(SEG_TSS);
}

/* ============================================================
 * IDT
 * ============================================================ */

static idt_entry g_idt[256];
static idt_ptr g_idt_ptr;

/* Set one IDT gate */
static void idt_set_gate(u32 vector, u64 handler, u8 ist, u8 type_attr) {
    g_idt[vector].offset_low    = handler & 0xFFFF;
    g_idt[vector].selector      = SEG_KERNEL_CODE;
    g_idt[vector].ist           = ist;
    g_idt[vector].type_attr     = type_attr;
    g_idt[vector].offset_middle = (handler >> 16) & 0xFFFF;
    g_idt[vector].offset_high   = static_cast<u32>(handler >> 32);
    g_idt[vector].zero          = 0;
}

/*
 * C-level interrupt dispatcher — called from the assembly ISR stubs.
 * The stubs push a uniform register_state onto the stack.
 */
extern "C" register_state* interrupt_dispatch(register_state* regs) {
    u64 vec = regs->int_no;

    /* Vector 2: NMI \u2014 used by vk_panic() to stop other CPUs.
     * Just disable interrupts and halt; do not log (the BSP holds
     * the log lock printing the panic message). */
    if (vec == 2) {
        disable_interrupts();
        while (true) { cpu_halt(); }
    }

    if (vec < 32) {
        u8 self_apic = smp::current_cpu_apic_id();
        u32 self_idx = smp::current_cpu_index();

        /*
         * Per-CPU re-entry guard FIRST.  If THIS CPU is already
         * handling an exception and another fires (e.g. cleanup
         * code itself faulted on corrupted heap), state is
         * compromised — halt this CPU.  We may still hold the
         * global exception lock at this point; release it so
         * other CPUs can make progress.
         */
        static volatile bool s_in_exception[smp::MAX_CPUS] = {};
        static spinlock s_exception_lock;  /* serialises exception handling across CPUs */

        if (s_in_exception[self_idx]) {
            if (s_exception_lock.held_by_self()) {
                s_exception_lock.release();
            }
            log::crash() << "\n*** NESTED EXCEPTION on CPU " << self_apic << " (vec " << static_cast<unsigned long long>(vec) << ") — halting CPU ***";
            disable_interrupts();
            while (true) { cpu_halt(); }
        }
        s_in_exception[self_idx] = true;

        /*
         * Global exception serializer: only one CPU at a time may
         * be in the exception dispatch / recovery path.  This
         * prevents two crashing CPUs from interleaving their
         * diagnostic output and from racing on shared cleanup
         * paths (heap free, scheduler state).
         */
        s_exception_lock.acquire();

        /* CPU exception \u2014 dump diagnostics */
        log::crash() << "\n*** EXCEPTION on CPU " << self_apic << ": " << s_exception_names[vec] << " (vector " << static_cast<unsigned long long>(vec) << ") ***";

        log::crash() << "  Error code: " << log::hex(static_cast<u64>(static_cast<unsigned long long>(regs->error_code)), 1, true, false);

        log::crash() << "  RIP:    " << log::hex(static_cast<u64>(static_cast<unsigned long long>(regs->frame.rip)), 1, true, false);
        log::crash() << "  CS:     " << log::hex(static_cast<u64>(static_cast<unsigned long long>(regs->frame.cs)), 1, true, false);
        log::crash() << "  RFLAGS: " << log::hex(static_cast<u64>(static_cast<unsigned long long>(regs->frame.rflags)), 1, true, false);
        log::crash() << "  RSP:    " << log::hex(static_cast<u64>(static_cast<unsigned long long>(regs->frame.rsp)), 1, true, false);
        log::crash() << "  SS:     " << log::hex(static_cast<u64>(static_cast<unsigned long long>(regs->frame.ss)), 1, true, false);

        log::crash() << "\n  General purpose registers:";
        log::crash() << "  RAX: " << log::hex(static_cast<u64>(static_cast<unsigned long long>(regs->rax)), 1, true, false) << "  RBX: " << log::hex(static_cast<u64>(static_cast<unsigned long long>(regs->rbx)), 1, true, false);
        log::crash() << "  RCX: " << log::hex(static_cast<u64>(static_cast<unsigned long long>(regs->rcx)), 1, true, false) << "  RDX: " << log::hex(static_cast<u64>(static_cast<unsigned long long>(regs->rdx)), 1, true, false);
        log::crash() << "  RSI: " << log::hex(static_cast<u64>(static_cast<unsigned long long>(regs->rsi)), 1, true, false) << "  RDI: " << log::hex(static_cast<u64>(static_cast<unsigned long long>(regs->rdi)), 1, true, false);
        log::crash() << "  RBP: " << log::hex(static_cast<u64>(static_cast<unsigned long long>(regs->rbp)), 1, true, false) << "  R8:  " << log::hex(static_cast<u64>(static_cast<unsigned long long>(regs->r8)), 1, true, false);
        log::crash() << "  R9:  " << log::hex(static_cast<u64>(static_cast<unsigned long long>(regs->r9)), 1, true, false) << "  R10: " << log::hex(static_cast<u64>(static_cast<unsigned long long>(regs->r10)), 1, true, false);
        log::crash() << "  R11: " << log::hex(static_cast<u64>(static_cast<unsigned long long>(regs->r11)), 1, true, false) << "  R12: " << log::hex(static_cast<u64>(static_cast<unsigned long long>(regs->r12)), 1, true, false);
        log::crash() << "  R13: " << log::hex(static_cast<u64>(static_cast<unsigned long long>(regs->r13)), 1, true, false) << "  R14: " << log::hex(static_cast<u64>(static_cast<unsigned long long>(regs->r14)), 1, true, false);
        log::crash() << "  R15: " << log::hex(static_cast<u64>(static_cast<unsigned long long>(regs->r15)), 1, true, false);

        if (vec == 14) {
            log::crash() << "  CR2 (fault addr): " << log::hex(static_cast<u64>(static_cast<unsigned long long>(read_cr2())), 1, true, false);
        }

        /* Print instruction bytes at RIP for diagnosis */
        {
            char bytes_buf[16 * 3 + 1];
            log::hex_bytes(bytes_buf, sizeof(bytes_buf),
                           reinterpret_cast<const u8*>(regs->frame.rip), 16);
            log::crash() << "  Bytes @ RIP:  " << bytes_buf;
        }

        /*
         * If the faulting task is a userspace process, kill just
         * that process instead of bringing down the whole kernel.
         *
         * Atomically detach the task from its ctx FIRST: this
         * read-and-clears user_data under the scheduler lock so two
         * CPUs cannot both observe the same non-null ctx and race
         * to free it twice.  Only the CPU that gets a non-null ctx
         * back is responsible for cleanup.
         */
        auto* ctx = static_cast<process::process_task_context*>(
            sched::detach_current_task());
        if (ctx != null) {
            log::warn() << "Terminating process '" << sched::current_task_name() << "' (task " << static_cast<unsigned long long>(sched::current_task_id()) << ") due to " << s_exception_names[vec];

            process::cleanup_process_context(ctx, -static_cast<int>(vec));

            /*
             * Cleanup completed without re-faulting.  Clear the
             * per-CPU re-entry guard and release the global
             * exception serializer so other CPUs can recover from
             * their own faults.  exit_task() switches to another
             * task and never returns to this dispatcher.
             */
            s_in_exception[self_idx] = false;
            s_exception_lock.release();

            sched::exit_task();
            /* exit_task never returns */
        }

        vk_panic("arch_init.cpp", __LINE__, "Unhandled CPU exception");
    }

    /* Vector 32: PIT timer (IRQ0) — scheduler preemption */
    if (vec == 32) {
        return sched::preempt(regs);
    }

    /* Vectors 33-255: other IRQs / software interrupts — not yet wired */
    if (vec >= 32) {
    log::debug() << "IRQ: unhandled vector " << static_cast<unsigned long long>(vec);
        /* Send EOI for any other IRQ */
        if (vec >= 40) arch::outb(0xA0, 0x20); /* PIC2 EOI */
        arch::outb(0x20, 0x20); /* PIC1 EOI */
        return regs;
    }

    return regs;
}

/* ISR stub anchor — defined in interrupts.S.
 * All 256 stubs are .align 16, so stub[i] = isr_stub_0 + i * 16. */
extern "C" void isr_stub_0();

static constexpr usize ISR_STUB_STRIDE = 16;

/* Force RIP-relative addressing to get the runtime address of isr_stub_0.
 * The compiler/linker will otherwise use an absolute link-time constant
 * because we're not linked as -pie. */
static inline auto get_isr_stub_base() -> u64 {
#if defined(_MSC_VER)
    return reinterpret_cast<u64>(&isr_stub_0);
#else
    return asm_get_isr_stub_base();
#endif
}

void init_idt() {
    log::info() << "Preparing IDT...";

    g_idt_ptr.limit = sizeof(g_idt) - 1;
    g_idt_ptr.base  = reinterpret_cast<u64>(&g_idt);

    /* Compute runtime base address of the first ISR stub */
    u64 base = get_isr_stub_base();

    log::debug() << "IDT: isr_stub_0=" << log::hex(static_cast<u64>(static_cast<unsigned long long>(base)), 1, true, false) << ", stride=" << ISR_STUB_STRIDE;

    /* Wire all 256 vectors to their assembly stub */
    for (u32 i = 0; i < 256; ++i) {
        u8 type_attr = 0x8E; /* Present, ring 0, 64-bit interrupt gate */
        idt_set_gate(i, base + i * ISR_STUB_STRIDE, 0, type_attr);
    }

    log::info() << "IDT prepared (256 vectors)";
}

/* Load the IDT. Called after ExitBootServices. */
static void activate_idt() {
    asm_lidt(&g_idt_ptr);
}

/* ============================================================
 * FPU / SSE / AVX
 * ============================================================ */

/*
 * Check CPUID for SSE, XSAVE and AVX support.
 * Returns 0 on success, or a non-zero error code:
 *   1 = no SSE
 *   2 = no XSAVE  (required to write XCR0)
 *   3 = no AVX
 */

/* Phase 1: pure hardware capability — called before any CR4 write */
static u32 fpu_avx_check_support() {
    u32 eax, ebx, ecx_out, edx_out;
    asm_cpuid(1, &eax, &ebx, &ecx_out, &edx_out);

    log::debug() << "FPU: CPUID ECX = " << log::hex(static_cast<u64>(ecx_out), 1, true, false) << "  EDX = " << log::hex(static_cast<u64>(edx_out), 1, true, false);

    if (!(edx_out & (1u << 25))) return 1; /* no SSE   */
    if (!(ecx_out & (1u << 26))) return 2; /* no XSAVE */
    if (!(ecx_out & (1u << 28))) return 3; /* no AVX   */
    return 0;
}

/* Phase 2: verify CR4.OSXSAVE actually took effect — call AFTER the write */
static bool fpu_osxsave_active() {
    u32 eax, ebx, ecx_out, edx_out;
    asm_cpuid(1, &eax, &ebx, &ecx_out, &edx_out);
    return (ecx_out & (1u << 27)) != 0; /* bit 27 mirrors CR4.OSXSAVE */
}

/*
 * Full FPU / SSE / AVX initialization.
 * Must be called in ring 0, after ExitBootServices.
 */
static void activate_fpu_state() {
    /* ── CPUID check ──────────────────────────────────────────── */
    log::info() << "FPU: checking CPUID...";
    u32 err = fpu_avx_check_support();

    log::debug() << "FPU: CPUID check result = " << err << " (0=OK, 1=no SSE, 2=no XSAVE, 3=no AVX)";

    log::debug() << "FPU: writing CR0...";
    u64 cr0 = read_cr0();
    cr0 &= ~(u64)(1u << 2);   /* clear EM */
    cr0 &= ~(u64)(1u << 3);   /* clear TS */
    cr0 |=  (u64)(1u << 1);   /* set   MP */
    cr0 |=  (u64)(1u << 5);   /* set   NE */
    write_cr0(cr0);

    log::debug() << "FPU: fninit...";
    asm_fninit();
    log::debug() << "FPU: fldcw...";
    asm_fldcw(0x037F);

    if (err == 1) {
        log::info() << "FPU: initialized (x87 only)";
        return;
    }

    log::debug() << "FPU: writing CR4 OSFXSR+OSXMMEXCPT...";
    write_cr4(read_cr4() | (u64)(1u << 9) | (u64)(1u << 10));

    log::debug() << "FPU: ldmxcsr...";
    asm_ldmxcsr(0x1F80u);

    if (err == 2) {
        log::info() << "FPU: initialized (x87 + SSE, no XSAVE)";
        return;
    }
    if (err == 3) {
        log::info() << "FPU: initialized (x87 + SSE, no AVX)";
        return;
    }

    log::debug() << "FPU: writing CR4 OSXSAVE...";
    {
        u64 cr4 = read_cr4();
        log::debug() << "FPU: CR4 before OSXSAVE = " << log::hex(static_cast<u64>(static_cast<unsigned long long>(cr4)), 1, true, false);

        if (!(cr4 & (u64)(1u << 18))) {
            write_cr4(cr4 | (u64)(1u << 18));
        }

        /* Verify via CPUID bit 27 — more reliable than reading CR4 back,
        * as a hypervisor may shadow CR4 reads but not CPUID              */
        if (!fpu_osxsave_active()) {
            log::warn() << "FPU: OSXSAVE did not become active after CR4 write — skipping AVX init";
            return;
        }
        log::debug() << "FPU: CR4.OSXSAVE active (confirmed via CPUID)";
    }

    log::debug() << "FPU: xgetbv...";
    u64 xcr0 = asm_xgetbv(0);
    log::debug() << "FPU: XCR0 before = 0x" << log::hex(static_cast<u64>(xcr0), 1, false, false);
    xcr0 |= (u64)(0x7);
    log::debug() << "FPU: xsetbv...";
    asm_xsetbv(0, static_cast<u32>(xcr0), static_cast<u32>(xcr0 >> 32));

    log::debug() << "FPU: vzeroall...";
    asm_vzeroall();

    log::info() << "FPU/SSE/AVX initialized";
}

/* ============================================================
 * Paging — validate and harden the UEFI-provided page tables
 * ============================================================ */

void init_paging() {
    log::info() << "Initializing paging...";

    /*
     * UEFI has already set up identity-mapped page tables in long mode
     * (CR0.PG=1, CR4.PAE=1, EFER.LME=1 are all already active).
     *
     * We just ensure additional protective features are turned on:
     *   CR0.WP  — write-protect supervisor pages
     *   EFER.NXE — enable execute-disable (NX) bit
     */

    /* CR0: enable Write-Protect */
    u64 cr0 = read_cr0();
    if (!(cr0 & CR0_WRITE_PROTECT)) {
        write_cr0(cr0 | CR0_WRITE_PROTECT);
        log::debug() << "  CR0.WP enabled";
    }

    /* EFER: enable NX (No-Execute) */
    constexpr u32 EFER_MSR = 0xC0000080;
    u64 efer = rdmsr(EFER_MSR);
    if (!(efer & EFER_NXE)) {
        wrmsr(EFER_MSR, efer | EFER_NXE);
        log::debug() << "  EFER.NXE enabled";
    }

    log::debug() << "Paging hardened (WP + NXE)";
}

/* ============================================================
 * make_region_writable — set R/W=1 in the UEFI identity-mapped
 * 4-level page tables for a physical address range.
 *
 * UEFI maps EFI_BOOT_SERVICES_CODE regions as non-writable
 * (R/W=0) in its page tables.  With CR0.WP active these pages
 * cause a protection fault on any supervisor write.  This
 * function walks the PML4→PDPT→PD→PT chain for every page
 * in [base, base+size) and sets the R/W bit at every level.
 * Non-present entries are skipped.  TLB entries for modified
 * pages are flushed with INVLPG.
 *
 * Assumes identity mapping (virtual address == physical address).
 * ============================================================ */
void make_region_writable(phys_addr base, size_phys size) {
    if (size == 0) return;

    /* Strip flags/XD bit; keep bits [51:12] as physical address. */
    static constexpr u64 PA_MASK = 0x000FFFFFFFFFF000ULL;

    /* Temporarily clear CR0.WP so we can write to page-table pages that
     * UEFI itself mapped read-only (e.g. the PML4 / PDPT / PD pages).
     * Without this, the first store to a PT entry faults because the page
     * containing that PT entry may itself be non-writable.  We restore CR0
     * exactly as we found it immediately after the walk. */
    const u64 saved_cr0 = read_cr0();
    write_cr0(saved_cr0 & ~CR0_WRITE_PROTECT);

    auto* pml4 = reinterpret_cast<pml4e*>(
        static_cast<usize>(read_cr3() & PA_MASK));

    /* Align start down to a page boundary. */
    phys_addr addr = align_down(base, static_cast<usize>(PAGE_SIZE_4K));
    const phys_addr end = base + size;

    while (addr < end) {
        const u32 pml4_idx = static_cast<u32>((addr >> 39) & 0x1FF);
        const u32 pdpt_idx = static_cast<u32>((addr >> 30) & 0x1FF);
        const u32 pd_idx   = static_cast<u32>((addr >> 21) & 0x1FF);
        const u32 pt_idx   = static_cast<u32>((addr >> 12) & 0x1FF);

        /* ---- PML4 ---- */
        if (!(pml4[pml4_idx] & PTE_PRESENT)) {
            /* No PML4 entry — advance past entire 512 GB slot. */
            addr = align_down(addr, static_cast<usize>(0x8000000000ULL))
                   + 0x8000000000ULL;
            continue;
        }
        pml4[pml4_idx] |= PTE_WRITABLE;

        /* ---- PDPT ---- */
        auto* pdpt = reinterpret_cast<pdpe*>(
            static_cast<usize>(pml4[pml4_idx] & PA_MASK));

        if (!(pdpt[pdpt_idx] & PTE_PRESENT)) {
            addr = align_down(addr, static_cast<usize>(PAGE_SIZE_1GB))
                   + PAGE_SIZE_1GB;
            continue;
        }
        if (pdpt[pdpt_idx] & PTE_HUGE) {
            /* 1 GB huge page */
            pdpt[pdpt_idx] |= PTE_WRITABLE;
            invlpg(addr);
            addr = align_down(addr, static_cast<usize>(PAGE_SIZE_1GB))
                   + PAGE_SIZE_1GB;
            continue;
        }
        pdpt[pdpt_idx] |= PTE_WRITABLE;

        /* ---- PD ---- */
        auto* pd = reinterpret_cast<pde*>(
            static_cast<usize>(pdpt[pdpt_idx] & PA_MASK));

        if (!(pd[pd_idx] & PTE_PRESENT)) {
            addr = align_down(addr, static_cast<usize>(PAGE_SIZE_2MB))
                   + PAGE_SIZE_2MB;
            continue;
        }
        if (pd[pd_idx] & PTE_HUGE) {
            /* 2 MB huge page (most common in OVMF) */
            pd[pd_idx] |= PTE_WRITABLE;
            invlpg(addr);
            addr = align_down(addr, static_cast<usize>(PAGE_SIZE_2MB))
                   + PAGE_SIZE_2MB;
            continue;
        }
        pd[pd_idx] |= PTE_WRITABLE;

        /* ---- PT ---- */
        auto* pt = reinterpret_cast<pte*>(
            static_cast<usize>(pd[pd_idx] & PA_MASK));

        if (pt[pt_idx] & PTE_PRESENT) {
            pt[pt_idx] |= PTE_WRITABLE;
            invlpg(addr);
        }
        addr += PAGE_SIZE_4K;
    }

    /* Restore CR0 (re-enables WP if it was set on entry). */
    write_cr0(saved_cr0);
}



auto enable_interrupts() -> void {
    asm_sti();
}

auto disable_interrupts() -> void {
    asm_cli();
}

auto halt() -> void {
    disable_interrupts();
    while (true) {
        cpu_halt();
    }
}

auto reboot() -> void {
    disable_interrupts();
    idt_ptr null_idt = { 0, 0 };
    asm_lidt(&null_idt);
    asm_int_0xff();

    // should not reach here, but if we do, halt the CPU
    while (true) {
        cpu_halt();
    }
}

/* ============================================================
 * IDT dump
 * ============================================================ */

void dump_idt() {
    log::info() << "Dumping IDT...";
    log::info() << "IDT base = " << log::hex(static_cast<u64>(static_cast<unsigned long long>(g_idt_ptr.base)), 1, true, false) << "  limit = " << log::hex(static_cast<u64>(static_cast<unsigned int>(g_idt_ptr.limit)), 1, true, false);

    for (u32 i = 0; i < 256; ++i) {
        const auto& e = g_idt[i];
        u64 handler = static_cast<u64>(e.offset_low)
                    | (static_cast<u64>(e.offset_middle) << 16)
                    | (static_cast<u64>(e.offset_high)   << 32);
        /* Skip null (uninstalled) entries */
        if (handler == 0 && e.selector == 0) continue;

        log::info() << "Vector " << i << ": handler=" << log::hex(static_cast<u64>(static_cast<unsigned long long>(handler)), 1, true, false) << " selector=" << log::hex(static_cast<u64>(e.selector), 1, true, false) << " ist=" << e.ist << " type_attr=" << log::hex(static_cast<u64>(e.type_attr), 1, true, false);
    }

    log::info() << "IDT dump complete.";
}

/* ============================================================
 * Architecture entry point
 * ============================================================ */

void init() {
    log::info() << "Initializing x86_64 architecture...";

    /* Prepare descriptor tables (safe while UEFI boot services are active) */
    init_gdt();
    init_idt();

    log::info() << "x86_64 tables prepared (not yet loaded)";
}

void activate() {
    log::info() << "Activating x86_64 descriptor tables...";

    /* Load our own GDT first so selector 0x08 in the IDT points at
     * our kernel code segment. */
    load_gdt();
    log::info() << "GDT loaded";

    /* Load our IDT before reloading CS/SS/TSS so faults during the
     * transition are handled by our exception path instead of dying
     * silently under whatever firmware state remained after EBS. */
    activate_idt();
    log::info() << "IDT loaded";

    /* Reload all visible segment selectors to our own descriptors. */
    reload_kernel_segments();
    log::info() << "Kernel code/data segments reloaded";

    /* Finally load the task register once GDT + IDT are already live. */
    activate_tss();
    log::info() << "TSS active";

    /* Initialize FPU/SSE/AVX state, if supported. Must be done after loading our own GDT/TSS. */
    activate_fpu_state();
    log::info() << "FPU/SSE/AVX state initialized";

    /* Harden paging (WP + NXE) */
    init_paging();

    log::info() << "x86_64 architecture fully active";
}

/*
 * Reload the BSP's GDT/IDT/segments on an Application Processor.
 *
 * Called from ap_init_secondary() (smp.cpp) immediately after the AP
 * enters 64-bit mode via the trampoline.  At that point the AP is
 * running with the temporary trampoline GDT, so we need to switch to
 * the kernel GDT before executing any selector-sensitive code.
 *
 * Each CPU loads its own GDT/TSS pair.  The TSS is required for any
 * interrupt or exception that enters the kernel from ring 3 on this AP.
 * 
 * EFER.NXE is per-CPU; APs must enable it before running address spaces
 * that use XD/NX PTEs for non-executable process heap pages.
 */

 void ap_activate() {
    load_gdt();
    reload_kernel_segments();
    activate_idt();
    activate_tss();
    activate_fpu_state();
    init_paging();
}

} // namespace arch
} // namespace vk
