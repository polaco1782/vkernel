/*
 * vkernel - UEFI Microkernel
 * Copyright (C) 2026 vkernel authors
 *
 * arch/x86_64/arch_init.cpp - x86_64 architecture initialization
 */

#include "config.h"
#include "types.h"
#include "console.h"
#include "memory.h"
#include "scheduler.h"
#include "arch/x86_64/arch.h"

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

static gdt_entry g_gdt[7];
static gdt_ptr g_gdt_ptr;
static tss g_tss;

/* Helper: install a standard (8-byte) GDT descriptor */
static void gdt_set_entry(u32 idx, u32 base, u32 limit,
                           u8 access, u8 granularity) {
    g_gdt[idx].limit_low    = limit & 0xFFFF;
    g_gdt[idx].base_low     = base & 0xFFFF;
    g_gdt[idx].base_middle  = (base >> 16) & 0xFF;
    g_gdt[idx].access       = access;
    g_gdt[idx].granularity  = static_cast<u8>(((limit >> 16) & 0x0F) | (granularity & 0xF0));
    g_gdt[idx].base_high    = (base >> 24) & 0xFF;
}

/* Install the 16-byte TSS descriptor at g_gdt[5..6] */
static void gdt_set_tss(u32 idx, u64 base, u32 limit) {
    /* Low 8 bytes — identical to a normal descriptor */
    g_gdt[idx].limit_low   = limit & 0xFFFF;
    g_gdt[idx].base_low    = base & 0xFFFF;
    g_gdt[idx].base_middle = (base >> 16) & 0xFF;
    g_gdt[idx].access      = 0x89; /* Present, 64-bit TSS (Available) */
    g_gdt[idx].granularity = static_cast<u8>(((limit >> 16) & 0x0F));
    g_gdt[idx].base_high   = (base >> 24) & 0xFF;

    /* High 8 bytes — base[63:32] + reserved */
    auto* high = reinterpret_cast<u32*>(&g_gdt[idx + 1]);
    high[0] = static_cast<u32>(base >> 32);
    high[1] = 0;
}

void init_gdt() {
    log::info("Preparing GDT...");

    /* Null descriptor */
    gdt_set_entry(0, 0, 0, 0, 0);

    /*
     * Long-mode code segments:
     *   access  = 0x9A (Present, ring 0, code, readable)
     *   gran    = 0xA0 (L=1 64-bit, D=0)
     *
     * Long-mode data segments:
     *   access  = 0x92 (Present, ring 0, data, writable)
     *   gran    = 0x00 (L and D ignored for data in long mode)
     */
    gdt_set_entry(1, 0, 0xFFFFF, 0x9A, 0xA0); /* Kernel Code 64-bit */
    gdt_set_entry(2, 0, 0xFFFFF, 0x92, 0x00); /* Kernel Data */
    gdt_set_entry(3, 0, 0xFFFFF, 0xFA, 0xA0); /* User Code 64-bit */
    gdt_set_entry(4, 0, 0xFFFFF, 0xF2, 0x00); /* User Data */

    /* TSS */
    vk::memory::memory_set(&g_tss, 0, sizeof(tss));
    g_tss.iomap_base = sizeof(tss);
    gdt_set_tss(5, reinterpret_cast<u64>(&g_tss), sizeof(tss) - 1);

    g_gdt_ptr.limit = sizeof(g_gdt) - 1;
    g_gdt_ptr.base  = reinterpret_cast<u64>(&g_gdt);

    log::info("GDT prepared");
}

/* Load the GDT, reload segments, and load the TSS.
 * Must be called AFTER ExitBootServices — UEFI firmware
 * expects its own GDT while boot services are active. */
static void activate_gdt() {
    asm volatile("lgdt %0" : : "m"(g_gdt_ptr));

    /*
     * Reload segment registers.
     * CS is reloaded via a far-return; the data segments get a simple mov.
     */
    asm volatile(
        "pushq %[cs]\n\t"
        "lea 1f(%%rip), %%rax\n\t"
        "push %%rax\n\t"
        "lretq\n\t"
        "1:\n\t"
        "mov %[ds], %%ax\n\t"
        "mov %%ax, %%ds\n\t"
        "mov %%ax, %%es\n\t"
        "mov %%ax, %%ss\n\t"
        "xor %%ax, %%ax\n\t"
        "mov %%ax, %%fs\n\t"
        "mov %%ax, %%gs\n\t"
        :
        : [cs] "i"(static_cast<u64>(SEG_KERNEL_CODE)),
          [ds] "i"(SEG_KERNEL_DATA)
        : "rax", "memory"
    );

    /* Load the TSS selector */
    asm volatile("ltr %w0" : : "r"(SEG_TSS));
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

    if (vec < 32) {
        /* CPU exception */
        console::set_color(console_color::white, console_color::red);
        console::puts("\n*** EXCEPTION: ");
        console::puts(s_exception_names[vec]);
        console::puts(" (vector ");
        console::put_dec(vec);
        console::puts(") ***\n");

        console::puts("  Error code: ");
        console::put_hex(regs->error_code);
        console::puts("\n");

        console::puts("  RIP:    "); console::put_hex(regs->frame.rip);    console::puts("\n");
        console::puts("  CS:     "); console::put_hex(regs->frame.cs);     console::puts("\n");
        console::puts("  RFLAGS: "); console::put_hex(regs->frame.rflags); console::puts("\n");
        console::puts("  RSP:    "); console::put_hex(regs->frame.rsp);    console::puts("\n");
        console::puts("  SS:     "); console::put_hex(regs->frame.ss);     console::puts("\n");

        console::puts("\n  General purpose registers:\n");
        console::puts("  RAX: "); console::put_hex(regs->rax); console::puts("  RBX: "); console::put_hex(regs->rbx); console::puts("\n");
        console::puts("  RCX: "); console::put_hex(regs->rcx); console::puts("  RDX: "); console::put_hex(regs->rdx); console::puts("\n");
        console::puts("  RSI: "); console::put_hex(regs->rsi); console::puts("  RDI: "); console::put_hex(regs->rdi); console::puts("\n");
        console::puts("  RBP: "); console::put_hex(regs->rbp); console::puts("  R8:  "); console::put_hex(regs->r8);  console::puts("\n");
        console::puts("  R9:  "); console::put_hex(regs->r9);  console::puts("  R10: "); console::put_hex(regs->r10); console::puts("\n");
        console::puts("  R11: "); console::put_hex(regs->r11); console::puts("  R12: "); console::put_hex(regs->r12); console::puts("\n");
        console::puts("  R13: "); console::put_hex(regs->r13); console::puts("  R14: "); console::put_hex(regs->r14); console::puts("\n");
        console::puts("  R15: "); console::put_hex(regs->r15); console::puts("\n");

        if (vec == 14) {
            console::puts("  CR2 (fault addr): ");
            console::put_hex(read_cr2());
            console::puts("\n");
        }

        console::set_color(console_color::white, console_color::black);
        vk_panic("arch_init.cpp", __LINE__, "Unhandled CPU exception");
    }

    /* Vector 32: PIT timer (IRQ0) — scheduler preemption */
    if (vec == 32) {
        return sched::preempt(regs);
    }

    /* Vectors 33-255: other IRQs / software interrupts — not yet wired */
    if (vec >= 32) {
#if VK_DEBUG_LEVEL >= 4
        console::puts("[DEBUG] IRQ: unhandled vector ");
        console::put_dec(vec);
        console::puts("\n");
#endif
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
    u64 addr;
    asm volatile("lea isr_stub_0(%%rip), %0" : "=r"(addr));
    return addr;
}

void init_idt() {
    log::info("Preparing IDT...");

    g_idt_ptr.limit = sizeof(g_idt) - 1;
    g_idt_ptr.base  = reinterpret_cast<u64>(&g_idt);

    /* Compute runtime base address of the first ISR stub */
    u64 base = get_isr_stub_base();

#if VK_DEBUG_LEVEL >= 4
    console::puts("[DEBUG] IDT: isr_stub_0=0x");
    console::put_hex(base);
    console::puts(", stride=");
    console::put_dec(ISR_STUB_STRIDE);
    console::puts("\n");
#endif

    /* Wire all 256 vectors to their assembly stub */
    for (u32 i = 0; i < 256; ++i) {
        u8 type_attr = 0x8E; /* Present, ring 0, 64-bit interrupt gate */
        idt_set_gate(i, base + i * ISR_STUB_STRIDE, 0, type_attr);
    }

    log::info("IDT prepared (256 vectors)");
}

/* Load the IDT. Called after ExitBootServices. */
static void activate_idt() {
    asm volatile("lidt %0" : : "m"(g_idt_ptr));
}

/* ============================================================
 * Paging — validate and harden the UEFI-provided page tables
 * ============================================================ */

void init_paging() {
    log::info("Initializing paging...");

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
        log::info("  CR0.WP enabled");
    }

    /* EFER: enable NX (No-Execute) */
    constexpr u32 EFER_MSR = 0xC0000080;
    u64 efer = rdmsr(EFER_MSR);
    if (!(efer & EFER_NXE)) {
        wrmsr(EFER_MSR, efer | EFER_NXE);
        log::info("  EFER.NXE enabled");
    }

    log::info("Paging hardened (WP + NXE)");
}

/* ============================================================
 * Interrupt control
 * ============================================================ */

auto enable_interrupts() -> void {
    asm volatile("sti");
}

auto disable_interrupts() -> void {
    asm volatile("cli");
}

auto halt() -> void {
    disable_interrupts();
    while (true) {
        cpu_halt();
    }
}

/* ============================================================
 * Architecture entry point
 * ============================================================ */

void init() {
    log::info("Initializing x86_64 architecture...");

    /* Prepare descriptor tables (safe while UEFI boot services are active) */
    init_gdt();
    init_idt();

    log::info("x86_64 tables prepared (not yet loaded)");
}

void activate() {
    log::info("Activating x86_64 descriptor tables...");

    /* Load our own GDT, reload all segment registers, load TSS */
    activate_gdt();
    log::info("GDT loaded, segments reloaded, TSS active");

    /* Load our own IDT */
    activate_idt();
    log::info("IDT loaded");

    /* Harden paging (WP + NXE) */
    init_paging();

    log::info("x86_64 architecture fully active");
}

} // namespace arch
} // namespace vk
