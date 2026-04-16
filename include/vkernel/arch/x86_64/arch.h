/*
 * vkernel - UEFI Microkernel
 * Copyright (C) 2026 vkernel authors
 *
 * arch/x86_64/arch.h - x86_64 architecture with C++26
 */

#ifndef VKERNEL_ARCH_X86_64_H
#define VKERNEL_ARCH_X86_64_H

#include "types.h"

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
struct [[gnu::packed]] gdt_entry {
    u16 limit_low;
    u16 base_low;
    u8 base_middle;
    u8 access;
    u8 granularity;
    u8 base_high;
};

/* GDT Pointer structure */
struct [[gnu::packed]] gdt_ptr {
    u16 limit;
    u64 base;
};

/* IDT entry structure */
struct [[gnu::packed]] idt_entry {
    u16 offset_low;
    u16 selector;
    u8 ist;
    u8 type_attr;
    u16 offset_middle;
    u32 offset_high;
    u32 zero;
};

/* IDT Pointer structure */
struct [[gnu::packed]] idt_ptr {
    u16 limit;
    u64 base;
};

/* Interrupt frame (pushed by CPU on interrupt/exception) */
struct [[gnu::packed]] interrupt_frame {
    u64 rip;
    u64 cs;
    u64 rflags;
    u64 rsp;
    u64 ss;
};

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
    
    /* Segment registers */
    u64 gs;
    u64 fs;
    
    /* Interrupt number and error code */
    u64 int_no;
    u64 error_code;
    
    /* Interrupt frame (pushed by CPU) */
    interrupt_frame frame;
};

/* TSS Structure (x86_64) */
struct [[gnu::packed]] tss {
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

/* Page table entry types */
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

/* Port I/O functions */
[[nodiscard]] inline auto inb(u16 port) -> u8 {
    u8 value;
    asm volatile("inb %1, %0" : "=a"(value) : "Nd"(port));
    return value;
}

inline auto outb(u16 port, u8 value) -> void {
    asm volatile("outb %0, %1" : : "a"(value), "Nd"(port));
}

[[nodiscard]] inline auto inw(u16 port) -> u16 {
    u16 value;
    asm volatile("inw %1, %0" : "=a"(value) : "Nd"(port));
    return value;
}

inline auto outw(u16 port, u16 value) -> void {
    asm volatile("outw %0, %1" : : "a"(value), "Nd"(port));
}

[[nodiscard]] inline auto inl(u16 port) -> u32 {
    u32 value;
    asm volatile("inl %1, %0" : "=a"(value) : "Nd"(port));
    return value;
}

inline auto outl(u16 port, u32 value) -> void {
    asm volatile("outl %0, %1" : : "a"(value), "Nd"(port));
}

/* MSR access */
[[nodiscard]] inline auto rdmsr(u32 msr) -> u64 {
    u32 low, high;
    asm volatile("rdmsr" : "=a"(low), "=d"(high) : "c"(msr));
    return (static_cast<u64>(high) << 32) | low;
}

inline auto wrmsr(u32 msr, u64 value) -> void {
    u32 low = static_cast<u32>(value & 0xFFFFFFFF);
    u32 high = static_cast<u32>(value >> 32);
    asm volatile("wrmsr" : : "a"(low), "d"(high), "c"(msr));
}

/* Control register access */
[[nodiscard]] inline auto read_cr0() -> u64 {
    u64 value;
    asm volatile("mov %%cr0, %0" : "=r"(value));
    return value;
}

inline auto write_cr0(u64 value) -> void {
    asm volatile("mov %0, %%cr0" : : "r"(value));
}

[[nodiscard]] inline auto read_cr2() -> u64 {
    u64 value;
    asm volatile("mov %%cr2, %0" : "=r"(value));
    return value;
}

[[nodiscard]] inline auto read_cr3() -> u64 {
    u64 value;
    asm volatile("mov %%cr3, %0" : "=r"(value));
    return value;
}

inline auto write_cr3(u64 value) -> void {
    asm volatile("mov %0, %%cr3" : : "r"(value));
}

[[nodiscard]] inline auto read_cr4() -> u64 {
    u64 value;
    asm volatile("mov %%cr4, %0" : "=r"(value));
    return value;
}

inline auto write_cr4(u64 value) -> void {
    asm volatile("mov %0, %%cr4" : : "r"(value));
}

/* Get RIP */
[[nodiscard]] inline auto read_rip() -> u64 {
    u64 rip;
    asm volatile("lea (%%rip), %0" : "=r"(rip));
    return rip;
}

/* Get RSP */
[[nodiscard]] inline auto read_rsp() -> u64 {
    u64 rsp;
    asm volatile("mov %%rsp, %0" : "=r"(rsp));
    return rsp;
}

/* Get RBP */
[[nodiscard]] inline auto read_rbp() -> u64 {
    u64 rbp;
    asm volatile("mov %%rbp, %0" : "=r"(rbp));
    return rbp;
}

/* Read RFLAGS */
[[nodiscard]] inline auto read_rflags() -> u64 {
    u64 rflags;
    asm volatile("pushfq; pop %0" : "=r"(rflags));
    return rflags;
}

/* Halt instruction */
inline auto cpu_halt() -> void {
    asm volatile("hlt");
}

/* No-operation */
inline auto cpu_nop() -> void {
    asm volatile("nop");
}

/* Memory barriers */
inline auto memory_barrier() -> void {
    asm volatile("mfence" ::: "memory");
}

inline auto read_memory_barrier() -> void {
    asm volatile("lfence" ::: "memory");
}

inline auto write_memory_barrier() -> void {
    asm volatile("sfence" ::: "memory");
}

/* Invalidate TLB entry */
inline auto invlpg(vaddr addr) -> void {
    asm volatile("invlpg (%0)" : : "r"(addr) : "memory");
}

/* Atomic operations */
[[nodiscard]] inline auto atomic_add(volatile u64* ptr, u64 value) -> u64 {
    asm volatile(
        "lock; xadd %0, %1"
        : "+r"(value), "+m"(*ptr)
        : : "memory"
    );
    return value;
}

[[nodiscard]] inline auto atomic_cmpxchg(volatile u64* ptr, u64 expected, u64 new_value) -> bool {
    u8 result;
    asm volatile(
        "lock; cmpxchg %2, %1; setz %0"
        : "=q"(result), "+m"(*ptr)
        : "r"(new_value), "a"(expected)
        : "memory"
    );
    return result != 0;
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

} // namespace arch
} // namespace vk

#endif /* VKERNEL_ARCH_X86_64_H */
