/*
 * vkernel - UEFI Microkernel
 * Copyright (C) 2026 vkernel authors
 *
 * arch/x86_64/arch.h - x86_64 architecture with C++26
 */

#ifndef VKERNEL_ARCH_X86_64_H
#define VKERNEL_ARCH_X86_64_H

#include "types.h"
#include "gcc_asm.h"    /* GCC/Clang asm declarations (no-op on MSVC) */
#include "msvc_asm.h"   /* MSVC asm declarations (no-op on GCC/Clang) */

namespace vk {
namespace arch {

/* CPU Flags register */
inline constexpr u64 FLAGS_INTERRUPT = (1ULL << 9);

/* Segment selectors */
inline constexpr u16 SEG_KERNEL_CODE = 0x08;
inline constexpr u16 SEG_KERNEL_DATA = 0x10;
inline constexpr u16 SEG_USER_CODE = 0x18;
inline constexpr u16 SEG_USER_DATA = 0x20;
inline constexpr u16 SEG_TSS = 0x28;

/* GDT entry structure */
#pragma pack(push, 1)
struct gdt_entry {
    u16 limit_low;
    u16 base_low;
    u8 base_middle;
    u8 access;
    u8 granularity;
    u8 base_high;
};
#pragma pack(pop)

/* GDT Pointer structure */
#pragma pack(push, 1)
struct gdt_ptr {
    u16 limit;
    u64 base;
};
#pragma pack(pop)

/* IDT entry structure */
#pragma pack(push, 1)
struct idt_entry {
    u16 offset_low;
    u16 selector;
    u8 ist;
    u8 type_attr;
    u16 offset_middle;
    u32 offset_high;
    u32 zero;
};
#pragma pack(pop)

/* IDT Pointer structure */
#pragma pack(push, 1)
struct idt_ptr {
    u16 limit;
    u64 base;
};
#pragma pack(pop)

/* Interrupt frame (pushed by CPU on interrupt/exception) */
#pragma pack(push, 1)
struct interrupt_frame {
    u64 rip;
    u64 cs;
    u64 rflags;
    u64 rsp;
    u64 ss;
};
#pragma pack(pop)

/* Full register state saved on context switch */
struct register_state {
    /* General purpose registers */
    u64 rax;
    u64 rbx;
    u64 rcx;
    u64 rdx;
    u64 rsi;
    u64 rdi;
    u64 rbp;
    u64 r8;
    u64 r9;
    u64 r10;
    u64 r11;
    u64 r12;
    u64 r13;
    u64 r14;
    u64 r15;
    
    /* Segment registers (pushed: FS at lower address, GS at higher) */
    u64 fs;
    u64 gs;
    
    /* Interrupt number and error code */
    u64 int_no;
    u64 error_code;
    
    /* Interrupt frame (pushed by CPU) */
    interrupt_frame frame;
};

/* TSS Structure (x86_64) */
#pragma pack(push, 1)
struct tss {
    u32 reserved0;
    u64 rsp0;
    u64 rsp1;
    u64 rsp2;
    u64 reserved1;
    u64 ist1;
    u64 ist2;
    u64 ist3;
    u64 ist4;
    u64 ist5;
    u64 ist6;
    u64 ist7;
    u64 reserved2;
    u16 reserved3;
    u16 iomap_base;
};
#pragma pack(pop)
using pte = u64;
using pde = u64;
using pdpe = u64;
using pml4e = u64;

/* Page table entry flags */
inline constexpr u64 PTE_PRESENT     = (1ULL << 0);
inline constexpr u64 PTE_WRITABLE    = (1ULL << 1);
inline constexpr u64 PTE_USER        = (1ULL << 2);
inline constexpr u64 PTE_ACCESSED    = (1ULL << 5);
inline constexpr u64 PTE_DIRTY       = (1ULL << 6);
inline constexpr u64 PTE_HUGE        = (1ULL << 7);  /* For 2MB/1GB pages */
inline constexpr u64 PTE_GLOBAL      = (1ULL << 8);
inline constexpr u64 PTE_XD          = (1ULL << 63);  /* Execute-disable (NX) */

/* CR0 flags */
inline constexpr u64 CR0_PROTECTED_MODE    = 0x00000001;
inline constexpr u64 CR0_COPROCESSOR_MONITOR = 0x00000002;
inline constexpr u64 CR0_EMULATION         = 0x00000004;
inline constexpr u64 CR0_WRITE_PROTECT     = 0x00010000;
inline constexpr u64 CR0_PAGING            = 0x80000000;

/* CR4 flags */
inline constexpr u64 CR4_PAE               = (1ULL << 5);
inline constexpr u64 CR4_PGE               = (1ULL << 7);
inline constexpr u64 CR4_OSFXSR            = (1ULL << 9);
inline constexpr u64 CR4_OSXMMEXCPT        = (1ULL << 10);
inline constexpr u64 CR4_OSXSAVE           = (1ULL << 18);
inline constexpr u64 CR4_SMEP              = (1ULL << 20);
inline constexpr u64 CR4_SMAP              = (1ULL << 21);

/* EFER MSR flags */
inline constexpr u64 EFER_SCE              = (1ULL << 0);  /* Syscall enable */
inline constexpr u64 EFER_LME              = (1ULL << 8);  /* Long mode enable */
inline constexpr u64 EFER_LMA              = (1ULL << 10); /* Long mode active */
inline constexpr u64 EFER_NXE              = (1ULL << 11); /* No-execute enable */

/* ============================================================
 * CPU primitives — thin inline wrappers around arch-specific
 * assembly routines.  All implementation lives in gcc_asm.S or
 * msvc_asm.asm; no inline asm or compiler intrinsics here.
 *
 * When porting to a new architecture, provide a new pair of
 * {gcc_asm.h, gcc_asm.S} (or msvc_asm equivalents) in
 * include/vkernel/arch/<target>/ and implement the same asm_*
 * symbols — no changes to this wrapper layer are needed.
 * ============================================================ */

/* Port I/O */
[[nodiscard]] inline auto inb(u16 port) -> u8  { return asm_inb(port); }
inline auto outb(u16 port, u8 value)  -> void  { asm_outb(port, value); }
[[nodiscard]] inline auto inw(u16 port) -> u16 { return asm_inw(port); }
inline auto outw(u16 port, u16 value) -> void  { asm_outw(port, value); }
[[nodiscard]] inline auto inl(u16 port) -> u32 { return asm_inl(port); }
inline auto outl(u16 port, u32 value) -> void  { asm_outl(port, value); }

/* MSR access */
[[nodiscard]] inline auto rdmsr(u32 msr) -> u64 { return asm_rdmsr(msr); }
inline auto wrmsr(u32 msr, u64 value) -> void   { asm_wrmsr(msr, value); }

/* Control register access */
[[nodiscard]] inline auto read_cr0() -> u64  { return asm_read_cr0(); }
inline auto write_cr0(u64 value) -> void     { asm_write_cr0(value); }
[[nodiscard]] inline auto read_cr2() -> u64  { return asm_read_cr2(); }
[[nodiscard]] inline auto read_cr3() -> u64  { return asm_read_cr3(); }
inline auto write_cr3(u64 value) -> void     { asm_write_cr3(value); }
[[nodiscard]] inline auto read_cr4() -> u64  { return asm_read_cr4(); }
inline auto write_cr4(u64 value) -> void     { asm_write_cr4(value); }

/* RIP / RSP / RBP */
[[nodiscard]] inline auto read_rip() -> u64  { return asm_read_rip(); }
[[nodiscard]] inline auto read_rsp() -> u64  { return asm_read_rsp(); }
[[nodiscard]] inline auto read_rbp() -> u64  { return asm_read_rbp(); }

/* RFLAGS */
[[nodiscard]] inline auto read_rflags() -> u64 { return asm_read_rflags(); }

/* Halt / NOP */
inline auto cpu_halt() -> void { asm_hlt(); }
inline auto cpu_nop()  -> void { asm_cpu_nop(); }

/* Memory barriers */
inline auto memory_barrier()       -> void { asm_memory_barrier(); }
inline auto read_memory_barrier()  -> void { asm_read_memory_barrier(); }
inline auto write_memory_barrier() -> void { asm_write_memory_barrier(); }

/* Invalidate TLB entry */
inline auto invlpg(vaddr addr) -> void {
    asm_invlpg(static_cast<unsigned long long>(addr));
}

/* Atomic operations */
[[nodiscard]] inline auto atomic_add(volatile u64* ptr, u64 value) -> u64 {
    return static_cast<u64>(asm_atomic_add(
        reinterpret_cast<volatile unsigned long long*>(ptr),
        static_cast<unsigned long long>(value)));
}
[[nodiscard]] inline auto atomic_cmpxchg(volatile u64* ptr, u64 expected, u64 new_value) -> bool {
    return asm_atomic_cmpxchg(
        reinterpret_cast<volatile unsigned long long*>(ptr),
        static_cast<unsigned long long>(expected),
        static_cast<unsigned long long>(new_value)) != 0;
}

/* Architecture initialization */
void init();           /* Prepare tables (safe during boot services) */
void activate();       /* Load GDT/IDT/TSS (call AFTER ExitBootServices) */
void init_gdt();
void init_idt();
void init_paging();
void dump_idt();       /* Print all installed IDT entries to the console */
auto enable_interrupts() -> void;
auto disable_interrupts() -> void;
auto halt() -> void;
auto reboot() -> void;

/*
 * Reload the BSP's GDT and IDT on an AP that started from the trampoline.
 * Must be called early in ap_init_secondary() before any C++ code that
 * assumes kernel selectors.
 */
void ap_activate();

/* PAUSE hint — thin inline wrapper around asm_pause() */
inline auto cpu_pause() -> void { asm_pause(); }

/* FXSAVE / FXRSTOR — save/restore x87+SSE state (512 bytes, 16B aligned) */
inline auto fxsave(void* area)        -> void { asm_fxsave(area); }
inline auto fxrstor(const void* area) -> void { asm_fxrstor(area); }

} // namespace arch
} // namespace vk

#endif /* VKERNEL_ARCH_X86_64_H */
