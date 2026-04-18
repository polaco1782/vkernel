/*
 * vkernel - UEFI Microkernel
 * Copyright (C) 2026 vkernel authors
 *
 * interrupts.S - ISR stubs (GAS, x86_64, SysV ABI)
 *
 * Each stub pushes a uniform register_state frame and calls
 * interrupt_dispatch() in arch_init.cpp.
 *
 * register_state layout (grows downward on stack):
 *   --- pushed by CPU ---
 *   SS, RSP, RFLAGS, CS, RIP           (interrupt_frame)
 *   error_code   (real or dummy 0)
 *   --- pushed by stub ---
 *   int_no
 *   FS, GS
 *   R15..R8, RBP, RDI, RSI, RDX, RCX, RBX, RAX
 */

.code64
.altmacro

/* ------------------------------------------------------------ */
/* ISR stub that pushes a dummy error code (for vectors that    */
/* don't push one automatically).                               */
/* ------------------------------------------------------------ */
.macro ISR_NOERR vector
    .align 16
    isr_stub_\vector:
        pushq $0                /* dummy error code  */
        pushq $\vector          /* interrupt number  */
        jmp   isr_common
.endm

/* ------------------------------------------------------------ */
/* ISR stub for vectors where the CPU pushes an error code.     */
/* ------------------------------------------------------------ */
.macro ISR_ERR vector
    .align 16
    isr_stub_\vector:
        /* error code already on stack */
        pushq $\vector          /* interrupt number  */
        jmp   isr_common
.endm

/* ------------------------------------------------------------ */
/* Generate 256 stubs                                           */
/* Vectors 8, 10-14, 17, 21, 29, 30 push error codes.          */
/* ------------------------------------------------------------ */

.text

.global isr_stub_0

/* 0-7: no error code */
.irp i,0,1,2,3,4,5,6,7
    ISR_NOERR \i
.endr

ISR_ERR   8        /* Double Fault */
ISR_NOERR 9

ISR_ERR  10        /* Invalid TSS */
ISR_ERR  11        /* Segment Not Present */
ISR_ERR  12        /* Stack-Segment Fault */
ISR_ERR  13        /* General Protection Fault */
ISR_ERR  14        /* Page Fault */

ISR_NOERR 15
ISR_NOERR 16

ISR_ERR  17        /* Alignment Check */

ISR_NOERR 18
ISR_NOERR 19
ISR_NOERR 20

ISR_ERR  21        /* Control Protection Exception */

.irp i,22,23,24,25,26,27,28
    ISR_NOERR \i
.endr

ISR_ERR  29        /* VMM Communication Exception */
ISR_ERR  30        /* Security Exception */

ISR_NOERR 31

/* 32-255: hardware IRQs / software — no error code */
.set i, 32
.rept 224
    ISR_NOERR %i
    .set i, i+1
.endr

/* ------------------------------------------------------------ */
/* Common path: save all GPRs, call C dispatcher, restore, iret */
/* ------------------------------------------------------------ */
isr_common:
    /* Allocate slots for FS and GS on the stack */
    subq  $16, %rsp
    /* Stack: [FS_slot][GS_slot][int_no][error_code][iretq frame] */

    /* Temporarily store original RAX in FS slot so we can use RAX as scratch */
    movq  %rax, (%rsp)

    /* Read and store GS */
    movw  %gs, %ax
    movzwq %ax, %rax
    movq  %rax, 8(%rsp)          /* GS slot = GS value */

    /* Read FS, then swap with original RAX to restore it */
    movw  %fs, %ax
    movzwq %ax, %rax
    xchgq %rax, (%rsp)           /* FS slot = FS value, RAX = original value */

    /* Save general-purpose registers (RAX is the correct original value) */
    pushq %r15
    pushq %r14
    pushq %r13
    pushq %r12
    pushq %r11
    pushq %r10
    pushq %r9
    pushq %r8
    pushq %rbp
    pushq %rdi
    pushq %rsi
    pushq %rdx
    pushq %rcx
    pushq %rbx
    pushq %rax

    /* First argument = pointer to register_state (RSP) */
    movq %rsp, %rdi
    /* Ensure 16-byte stack alignment for the call. */
    pushq %rbp
    movq  %rsp, %rbp
    andq  $~0xF, %rsp
    call  interrupt_dispatch
    /* RAX = register_state* to restore (may be a different task's stack) */
    movq  %rax, %rsp

    /* -- We only get here for non-fatal interrupts -- */

    /* Restore GPRs */
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

    /* Restore segment registers without corrupting RAX */
    /* Stack: [FS][GS][int_no][error_code][iretq frame] */
    pushq %rax                   /* temporarily save restored RAX */
    movq  8(%rsp), %rax          /* read FS value */
    movw  %ax, %fs
    movq  16(%rsp), %rax         /* read GS value */
    movw  %ax, %gs
    popq  %rax                   /* restore RAX */

    /* Remove FS, GS, int_no and error_code (4 * 8 = 32 bytes) */
    addq  $32, %rsp

    iretq

/* ------------------------------------------------------------ */
/* ISR stub stride: each stub is .align 16, so exactly 16 bytes */
/* The C++ code computes: &isr_stub_0 + vector * 16             */
/* No isr_stub_table needed — absolute addresses are broken in  */
/* a position-independent PE loaded by UEFI.                    */
/* ------------------------------------------------------------ */
