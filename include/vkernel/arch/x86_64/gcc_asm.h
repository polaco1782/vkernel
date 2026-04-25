/*
 * vkernel - UEFI Microkernel
 * Copyright (C) 2026 vkernel authors
 *
 * arch/x86_64/gcc_asm.h - Declarations for GAS routines (GCC/Clang, x86_64)
 *
 * ALL raw CPU instruction sequences are implemented in gcc_asm.S.
 * No inline asm or compiler intrinsics belong in .h or .cpp files.
 *
 * Porting guide: for a new architecture, create
 *   include/vkernel/arch/<target>/gcc_asm.h   (this file's counterpart)
 *   src/arch/<target>/gcc_asm.S               (implementations)
 * and provide the same asm_* symbols listed here.
 */

#ifndef VKERNEL_ARCH_X86_64_GCC_ASM_H
#define VKERNEL_ARCH_X86_64_GCC_ASM_H

#if !defined(_MSC_VER)

extern "C" {

/* ---- System control ---- */
void asm_sti();
void asm_cli();
void asm_hlt();
void asm_int_timer();
void asm_lgdt(void* gdt_ptr);
void asm_lidt(void* idt_ptr);
void asm_ltr(unsigned short selector);
void asm_reload_segments(unsigned long long code_sel, unsigned long long data_sel);
void asm_int_0xff();
[[noreturn]] void asm_sched_switch_to(unsigned long long rsp);

/* ---- Port I/O ---- */
unsigned char  asm_inb(unsigned short port);
void           asm_outb(unsigned short port, unsigned char value);
unsigned short asm_inw(unsigned short port);
void           asm_outw(unsigned short port, unsigned short value);
unsigned int   asm_inl(unsigned short port);
void           asm_outl(unsigned short port, unsigned int value);

/* ---- MSR access ---- */
unsigned long long asm_rdmsr(unsigned int msr);
void               asm_wrmsr(unsigned int msr, unsigned long long value);

/* ---- Control register access ---- */
unsigned long long asm_read_cr0();
void               asm_write_cr0(unsigned long long value);
unsigned long long asm_read_cr2();
unsigned long long asm_read_cr3();
void               asm_write_cr3(unsigned long long value);
unsigned long long asm_read_cr4();
void               asm_write_cr4(unsigned long long value);

/* ---- General-purpose register reads ---- */
unsigned long long asm_read_rip();
unsigned long long asm_read_rsp();
unsigned long long asm_read_rbp();
unsigned long long asm_read_rflags();

/* ---- Misc CPU instructions ---- */
void asm_cpu_nop();

/* ---- Memory barriers ---- */
void asm_memory_barrier();
void asm_read_memory_barrier();
void asm_write_memory_barrier();

/* ---- TLB management ---- */
void asm_invlpg(unsigned long long addr);

/* ---- Atomic operations ---- */
unsigned long long asm_atomic_add(volatile unsigned long long* ptr,
                                   unsigned long long value);
int                asm_atomic_cmpxchg(volatile unsigned long long* ptr,
                                       unsigned long long expected,
                                       unsigned long long new_value);

/* ---- RIP-relative symbol address helpers ---- */
unsigned long long asm_get_isr_stub_base();
unsigned long long asm_get_image_base();
unsigned long long asm_get_data_start();
unsigned long long asm_get_data_end();
unsigned long long asm_get_got_start();
unsigned long long asm_get_got_end();
unsigned long long asm_get_end();

/* ---- FPU / SSE / AVX ---- */
void asm_fninit();
void asm_fldcw(u16 cw);
void asm_ldmxcsr(u32 mxcsr);
void asm_xsetbv(u32 xcr, u32 eax_val, u32 edx_val);
u64  asm_xgetbv(u32 xcr);
void asm_vzeroall();
void asm_cpuid(u32 leaf, u32* eax, u32* ebx, u32* ecx, u32* edx);

} /* extern "C" */

#endif /* !_MSC_VER */

#endif /* VKERNEL_ARCH_X86_64_GCC_ASM_H */
