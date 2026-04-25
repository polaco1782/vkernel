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

/* u64 asm_get_got_start() — runtime address of _got_start */
.global asm_get_got_start
asm_get_got_start:
    lea  _got_start(%rip), %rax
    ret

/* u64 asm_get_got_end() — runtime address of _got_end */
.global asm_get_got_end
asm_get_got_end:
    lea  _got_end(%rip), %rax
    ret

/* ============================================================
 * Port I/O
 * First argument (port, u16) arrives in %rdi/%di (SysV AMD64).
 * The IN/OUT instructions require the port in %dx, so we move
 * %di → %dx before issuing the instruction.
 * ============================================================ */

/* u8 asm_inb(u16 port) */
.global asm_inb
asm_inb:
    movw  %di, %dx
    xorl  %eax, %eax
    inb   %dx, %al
    ret

/* void asm_outb(u16 port, u8 value) */
.global asm_outb
asm_outb:
    movb  %sil, %al         /* value (arg2) → AL */
    movw  %di,  %dx         /* port  (arg1) → DX */
    outb  %al,  %dx
    ret

/* u16 asm_inw(u16 port) */
.global asm_inw
asm_inw:
    movw  %di, %dx
    xorl  %eax, %eax
    inw   %dx, %ax
    ret

/* void asm_outw(u16 port, u16 value) */
.global asm_outw
asm_outw:
    movw  %si, %ax          /* value → AX */
    movw  %di, %dx          /* port  → DX */
    outw  %ax, %dx
    ret

/* u32 asm_inl(u16 port) */
.global asm_inl
asm_inl:
    movw  %di, %dx
    inl   %dx, %eax
    ret

/* void asm_outl(u16 port, u32 value) */
.global asm_outl
asm_outl:
    movl  %esi, %eax        /* value → EAX */
    movw  %di,  %dx         /* port  → DX */
    outl  %eax, %dx
    ret

/* ============================================================
 * MSR access
 * ============================================================ */

/* u64 asm_rdmsr(u32 msr) */
.global asm_rdmsr
asm_rdmsr:
    movl  %edi, %ecx        /* msr → ECX */
    rdmsr                   /* EDX:EAX = MSR[ECX] */
    shlq  $32, %rdx
    orq   %rdx, %rax        /* RAX = (RDX<<32)|EAX */
    ret

/* void asm_wrmsr(u32 msr, u64 value) */
.global asm_wrmsr
asm_wrmsr:
    movl  %edi, %ecx        /* msr   → ECX */
    movl  %esi, %eax        /* low32 → EAX */
    movq  %rsi, %rdx
    shrq  $32,  %rdx        /* high32 → EDX */
    wrmsr
    ret

/* ============================================================
 * Control register access
 * ============================================================ */

.global asm_read_cr0
asm_read_cr0:
    movq  %cr0, %rax
    ret

.global asm_write_cr0
asm_write_cr0:
    movq  %rdi, %cr0
    ret

.global asm_read_cr2
asm_read_cr2:
    movq  %cr2, %rax
    ret

.global asm_read_cr3
asm_read_cr3:
    movq  %cr3, %rax
    ret

.global asm_write_cr3
asm_write_cr3:
    movq  %rdi, %cr3
    ret

.global asm_read_cr4
asm_read_cr4:
    movq  %cr4, %rax
    ret

.global asm_write_cr4
asm_write_cr4:
    movq  %rdi, %cr4
    ret

/* ============================================================
 * General-purpose register reads
 * ============================================================ */

/* u64 asm_read_rip() — returns the return address (caller's next RIP) */
.global asm_read_rip
asm_read_rip:
    movq  (%rsp), %rax
    ret

/* u64 asm_read_rsp() — returns caller's stack pointer */
.global asm_read_rsp
asm_read_rsp:
    leaq  8(%rsp), %rax     /* +8: skip the return address slot */
    ret

/* u64 asm_read_rbp() */
.global asm_read_rbp
asm_read_rbp:
    movq  %rbp, %rax
    ret

/* u64 asm_read_rflags() */
.global asm_read_rflags
asm_read_rflags:
    pushfq
    popq  %rax
    ret

/* ============================================================
 * Misc CPU instructions
 * ============================================================ */

/* void asm_cpu_nop() */
.global asm_cpu_nop
asm_cpu_nop:
    nop
    ret

/* ============================================================
 * Memory barriers
 * ============================================================ */

.global asm_memory_barrier
asm_memory_barrier:
    mfence
    ret

.global asm_read_memory_barrier
asm_read_memory_barrier:
    lfence
    ret

.global asm_write_memory_barrier
asm_write_memory_barrier:
    sfence
    ret

/* ============================================================
 * TLB management
 * ============================================================ */

/* void asm_invlpg(u64 addr) — %rdi = virtual address to invalidate */
.global asm_invlpg
asm_invlpg:
    invlpg (%rdi)
    ret

/* ============================================================
 * Atomic operations
 * ============================================================ */

/*
 * u64 asm_atomic_add(volatile u64* ptr, u64 value)
 *   %rdi = ptr, %rsi = value
 *   Returns the value that was in *ptr before the add.
 */
.global asm_atomic_add
asm_atomic_add:
    movq  %rsi, %rax
    lock xaddq %rax, (%rdi)
    ret

/*
 * int asm_atomic_cmpxchg(volatile u64* ptr, u64 expected, u64 new_value)
 *   %rdi = ptr, %rsi = expected, %rdx = new_value
 *   Returns 1 if exchange was performed, 0 otherwise.
 */
.global asm_atomic_cmpxchg
asm_atomic_cmpxchg:
    movq  %rsi, %rax
    lock cmpxchgq %rdx, (%rdi)
    sete  %al
    movzbl %al, %eax
    ret

/* ============================================================
 * FPU / SSE / AVX initialization helpers
 * ============================================================ */

/* void asm_fninit() — reset x87 FPU to default state */
.global asm_fninit
asm_fninit:
    fninit
    ret

/* void asm_fldcw(u16 cw) — load x87 control word from %di */
.global asm_fldcw
asm_fldcw:
    subq  $8, %rsp
    movw  %di, (%rsp)
    fldcw (%rsp)
    addq  $8, %rsp
    ret

/* void asm_ldmxcsr(u32 mxcsr) — load SSE MXCSR from %edi */
.global asm_ldmxcsr
asm_ldmxcsr:
    subq    $8, %rsp
    movl    %edi, (%rsp)
    ldmxcsr (%rsp)
    addq    $8, %rsp
    ret

/* void asm_xsetbv(u32 xcr, u32 eax, u32 edx)
 *   %edi = XCR index, %esi = low 32 bits (EAX), %edx = high 32 bits (EDX) */
.global asm_xsetbv
asm_xsetbv:
    movl  %edi, %ecx        /* XCR index */
    movl  %esi, %eax        /* low 32  */
    movl  %edx, %edx        /* high 32 (already in EDX) */
    xsetbv
    ret

/* u64 asm_xgetbv(u32 xcr) — returns EDX:EAX of XCR[xcr] as u64 */
.global asm_xgetbv
asm_xgetbv:
    movl  %edi, %ecx
    xgetbv                  /* EDX:EAX = XCR[ECX] */
    shlq  $32, %rdx
    orq   %rdx, %rax
    ret

/* void asm_vzeroall() — zero all YMM registers */
.global asm_vzeroall
asm_vzeroall:
    vzeroall
    ret

/* void asm_cpuid(u32 leaf, u32* eax, u32* ebx, u32* ecx, u32* edx)
 *   %edi = leaf, %rsi = eax*, %rdx = ebx*, %rcx = ecx*, %r8 = edx*  */
/* void asm_cpuid(u32 leaf, u32* eax, u32* ebx, u32* ecx, u32* edx)
 *   SysV: %edi=leaf, %rsi=eax*, %rdx=ebx*, %rcx=ecx*, %r8=edx*
 *   CPUID clobbers EAX/EBX/ECX/EDX, so all pointer args must be
 *   saved to callee-saved registers before the instruction.       */
.global asm_cpuid
asm_cpuid:
    push  %rbx          /* callee-saved, clobbered by cpuid */
    push  %r12          /* callee-saved scratch for eax*    */
    push  %r13          /* callee-saved scratch for ebx*    */
    push  %r14          /* callee-saved scratch for ecx*    */
    push  %r15          /* callee-saved scratch for edx*    */

    /* Save all output pointers before cpuid clobbers their registers */
    movq  %rsi, %r12    /* eax* */
    movq  %rdx, %r13    /* ebx* */
    movq  %rcx, %r14    /* ecx* */
    movq  %r8,  %r15    /* edx* */

    movl  %edi, %eax    /* leaf → EAX */
    xorl  %ecx, %ecx    /* subleaf 0  */
    cpuid

    movl  %eax, (%r12)  /* *eax */
    movl  %ebx, (%r13)  /* *ebx */
    movl  %ecx, (%r14)  /* *ecx */
    movl  %edx, (%r15)  /* *edx */

    pop   %r15
    pop   %r14
    pop   %r13
    pop   %r12
    pop   %rbx
    ret

/* void asm_pause() — PAUSE hint for spin-wait loops (reduces CPU power,
 *                    improves hyper-threading performance) */
.global asm_pause
asm_pause:
    pause
    ret

/* ============================================================
 * AP 64-bit entry point
 *
 * Called via far jump from the 32-bit AP trampoline (ap_trampoline.S)
 * after the AP has entered 64-bit long mode.
 *
 * On entry:
 *   CS = 0x18  (64-bit code, temporary trampoline GDT)
 *   SS = 0x10  (32-bit data, temporary trampoline GDT — valid in 64-bit)
 *   RSP = indeterminate (no stack yet)
 *   Page tables = BSP's CR3 (identity-mapped)
 *
 * We must load RSP from TRAM_STACK (physical 0x8140) before any
 * push/call instruction.
 * ============================================================ */
.global ap_entry_64
ap_entry_64:
    /* Load 64-bit data selectors from the temporary GDT */
    movw    $0x20, %ax      /* SEL_DATA64 from temporary trampoline GDT */
    movw    %ax, %ss
    movw    %ax, %ds
    movw    %ax, %es
    xorw    %ax, %ax
    movw    %ax, %fs
    movw    %ax, %gs

    /* Load the AP stack pointer stored by the BSP at physical 0x8140 */
    movabsq $0x8140, %rax
    movq    (%rax), %rsp

    /* Zero the frame pointer for clean stack traces */
    xorq    %rbp, %rbp

    /* Call the C++ AP initialization function */
    callq   ap_init_secondary

    /* ap_init_secondary should not return, but halt if it does */
.ap_entry_halt:
    cli
    hlt
    jmp .ap_entry_halt