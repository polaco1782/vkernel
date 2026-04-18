/*
 * vkernel - UEFI Microkernel
 * Copyright (C) 2026 vkernel authors
 *
 * gcc_asm.S - GAS x86_64 assembly routines for GCC/Clang (SysV ABI)
 *
 * GCC x86_64 inline assembly is avoided in .cpp files; all raw CPU
 * instructions (lgdt, lidt, ltr, iretq, sti, cli, hlt, etc.) live here.
 * This is the GCC/Clang counterpart of msvc_asm.asm.
 */

.code64
.text

/* void asm_sti() */
.global asm_sti
asm_sti:
    sti
    ret

/* void asm_cli() */
.global asm_cli
asm_cli:
    cli
    ret

/* void asm_hlt() */
.global asm_hlt
asm_hlt:
    hlt
    ret

/* void asm_int_timer()  — issues INT 32 (0x20) for scheduler yield */
.global asm_int_timer
asm_int_timer:
    int $32
    ret

/* void asm_lgdt(void* gdt_ptr)  — %rdi = pointer to GDT descriptor */
.global asm_lgdt
asm_lgdt:
    lgdt (%rdi)
    ret

/* void asm_lidt(void* idt_ptr)  — %rdi = pointer to IDT descriptor */
.global asm_lidt
asm_lidt:
    lidt (%rdi)
    ret

/* void asm_ltr(u16 selector)  — %di = TSS selector */
.global asm_ltr
asm_ltr:
    ltr %di
    ret

/*
 * void asm_reload_segments(u64 code_sel, u64 data_sel)
 *   %rdi = code selector, %rsi = data selector
 *   Reloads CS via far return, DS/ES/SS via mov, zeroes FS/GS.
 */
.global asm_reload_segments
asm_reload_segments:
    /* Reload data segments */
    movw  %si, %ax
    movw  %ax, %ds
    movw  %ax, %es
    movw  %ax, %ss
    xorw  %ax, %ax
    movw  %ax, %fs
    movw  %ax, %gs

    /* Reload CS via far return */
    popq  %rax              /* return address  */
    pushq %rdi              /* code selector   */
    pushq %rax              /* return address  */
    lretq

/* void asm_int_0xff()  — trigger INT 0xFF (for triple-fault reboot) */
.global asm_int_0xff
asm_int_0xff:
    int $0xFF
    ret

/*
 * __attribute__((noreturn)) void asm_sched_switch_to(u64 rsp)
 *   %rdi = new RSP pointing to a register_state frame
 *   Restores all GPRs, skips segment/interrupt fields, then iretq.
 */
.global asm_sched_switch_to
asm_sched_switch_to:
    movq  %rdi, %rsp
    /* Restore GPRs (order matches ISR stub push order) */
    popq %rax
    popq %rbx
    popq %rcx
    popq %rdx
    popq %rsi
    popq %rdi
    popq %rbp
    popq %r8
    popq %r9
    popq %r10
    popq %r11
    popq %r12
    popq %r13
    popq %r14
    popq %r15
    /* Skip FS, GS, int_no, error_code (4 * 8 = 32 bytes) */
    addq $32, %rsp
    iretq

/*
 * u64 asm_get_isr_stub_base()
 *   Returns the RIP-relative runtime address of isr_stub_0.
 *   Needed because the PE loaded by UEFI may be relocated.
 */
.global asm_get_isr_stub_base
asm_get_isr_stub_base:
    lea  isr_stub_0(%rip), %rax
    ret

/* ---- Linker symbol address helpers for self_relocate() ---- */

/* u64 asm_get_image_base() — runtime address of ImageBase */
.global asm_get_image_base
asm_get_image_base:
    lea  ImageBase(%rip), %rax
    ret

/* u64 asm_get_data_start() — runtime address of .data section start */
.global asm_get_data_start
asm_get_data_start:
    lea  _data(%rip), %rax
    ret

/* u64 asm_get_data_end() — runtime address of _edata (end of .data) */
.global asm_get_data_end
asm_get_data_end:
    lea  _edata(%rip), %rax
    ret

/* u64 asm_get_end() — runtime address of _end (end of image incl. BSS) */
.global asm_get_end
asm_get_end:
    lea  _end(%rip), %rax
    ret
