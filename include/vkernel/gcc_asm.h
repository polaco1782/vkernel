/*
 * vkernel - UEFI Microkernel
 * Copyright (C) 2026 vkernel authors
 *
 * gcc_asm.h - Declarations for GAS routines (GCC/Clang only)
 *
 * This is the GCC/Clang counterpart of msvc_asm.h.
 */

#ifndef VKERNEL_GCC_ASM_H
#define VKERNEL_GCC_ASM_H

#if !defined(_MSC_VER)

extern "C" {

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

/* RIP-relative address helpers */
unsigned long long asm_get_isr_stub_base();
unsigned long long asm_get_image_base();
unsigned long long asm_get_data_start();
unsigned long long asm_get_data_end();
unsigned long long asm_get_end();

} /* extern "C" */

#endif /* !_MSC_VER */

#endif /* VKERNEL_GCC_ASM_H */
