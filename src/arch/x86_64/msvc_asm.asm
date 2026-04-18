;
; vkernel - UEFI Microkernel
; Copyright (C) 2026 vkernel authors
;
; msvc_asm.asm - MASM x64 assembly routines for MSVC
;
; MSVC x64 does not support inline assembly, so all raw CPU
; instructions (int, lgdt, lidt, ltr, iretq, sti, cli, hlt, etc.)
; that cannot be expressed via compiler intrinsics live here.
;

EXTERN isr_stub_0:PROC

.code

; void asm_sti()
asm_sti PROC
    sti
    ret
asm_sti ENDP

; void asm_cli()
asm_cli PROC
    cli
    ret
asm_cli ENDP

; void asm_hlt()
asm_hlt PROC
    hlt
    ret
asm_hlt ENDP

; void asm_int_timer()  — issues INT 32 (0x20) for scheduler yield
asm_int_timer PROC
    int 20h
    ret
asm_int_timer ENDP

; void asm_lgdt(void* gdt_ptr)  — RCX = pointer to GDT descriptor
asm_lgdt PROC
    lgdt FWORD PTR [rcx]
    ret
asm_lgdt ENDP

; void asm_lidt(void* idt_ptr)  — RCX = pointer to IDT descriptor
asm_lidt PROC
    lidt FWORD PTR [rcx]
    ret
asm_lidt ENDP

; void asm_ltr(u16 selector)  — CX = TSS selector
asm_ltr PROC
    ltr cx
    ret
asm_ltr ENDP

; void asm_reload_segments(u64 code_sel, u64 data_sel)
;   RCX = code selector, RDX = data selector
;   Reloads CS via far return, DS/ES/SS via mov, zeroes FS/GS.
asm_reload_segments PROC
    ; Reload data segments
    mov ax, dx
    mov ds, ax
    mov es, ax
    mov ss, ax
    xor ax, ax
    mov fs, ax
    mov gs, ax

    ; Reload CS via far return
    pop rax             ; return address
    push rcx            ; code selector
    push rax            ; return address
    retfq               ; far return → reloads CS
asm_reload_segments ENDP

; void asm_int_0xff()  — trigger INT 0xFF (for triple-fault reboot)
asm_int_0xff PROC
    int 0FFh
    ret
asm_int_0xff ENDP

; __declspec(noreturn) void asm_sched_switch_to(u64 rsp)
;   RCX = new RSP pointing to a register_state frame
;   Restores all GPRs, skips segment/interrupt fields, then iretq.
asm_sched_switch_to PROC
    mov rsp, rcx
    ; Restore GPRs (order matches ISR stub push order)
    pop rax
    pop rbx
    pop rcx
    pop rdx
    pop rsi
    pop rdi
    pop rbp
    pop r8
    pop r9
    pop r10
    pop r11
    pop r12
    pop r13
    pop r14
    pop r15
    ; Skip FS, GS, int_no, error_code (4 * 8 = 32 bytes)
    add rsp, 32
    iretq
asm_sched_switch_to ENDP

; u64 asm_lea_symbol(void* sym_addr)
;   Returns the address passed in RCX (identity, used as a helper).
;   For MSVC the linker handles relocations, so we just return the address.
asm_lea_symbol PROC
    mov rax, rcx
    ret
asm_lea_symbol ENDP

; ==============================================================
; Port I/O
; Windows x64: first arg in RCX, second in RDX.
; The IN/OUT instructions require the port in DX, so we move
; CX (low 16 of RCX) → DX before issuing the instruction.
; ==============================================================

; u8 asm_inb(u16 port)  — RCX = port
asm_inb PROC
    mov dx, cx
    xor eax, eax
    in al, dx
    ret
asm_inb ENDP

; void asm_outb(u16 port, u8 value)  — RCX = port, RDX = value
asm_outb PROC
    mov al, dl              ; save value byte before clobbering DX
    mov dx, cx              ; port → DX
    out dx, al
    ret
asm_outb ENDP

; u16 asm_inw(u16 port)  — RCX = port
asm_inw PROC
    mov dx, cx
    xor eax, eax
    in ax, dx
    ret
asm_inw ENDP

; void asm_outw(u16 port, u16 value)  — RCX = port, RDX = value
asm_outw PROC
    mov ax, dx              ; value → AX
    mov dx, cx              ; port  → DX
    out dx, ax
    ret
asm_outw ENDP

; u32 asm_inl(u16 port)  — RCX = port
asm_inl PROC
    mov dx, cx
    in eax, dx
    ret
asm_inl ENDP

; void asm_outl(u16 port, u32 value)  — RCX = port, RDX = value
asm_outl PROC
    mov eax, edx            ; value → EAX
    mov dx, cx              ; port  → DX
    out dx, eax
    ret
asm_outl ENDP

; ==============================================================
; MSR access
; ==============================================================

; u64 asm_rdmsr(u32 msr)  — RCX = msr (ECX already correct for RDMSR)
asm_rdmsr PROC
    rdmsr                   ; EDX:EAX = MSR[ECX]
    shl rdx, 32
    or  rax, rdx            ; RAX = (RDX<<32)|EAX
    ret
asm_rdmsr ENDP

; void asm_wrmsr(u32 msr, u64 value)  — RCX = msr, RDX = value
asm_wrmsr PROC
    ; ECX = msr (already set by calling convention)
    ; Need: EAX = low32(value), EDX = high32(value)
    mov rax, rdx            ; rax = full 64-bit value
    shr rdx, 32             ; rdx = high 32 bits (EDX for WRMSR)
    ; eax = low 32 bits of value (EAX for WRMSR)
    wrmsr
    ret
asm_wrmsr ENDP

; ==============================================================
; Control register access
; ==============================================================

asm_read_cr0 PROC
    mov rax, cr0
    ret
asm_read_cr0 ENDP

; void asm_write_cr0(u64 value)  — RCX = value
asm_write_cr0 PROC
    mov cr0, rcx
    ret
asm_write_cr0 ENDP

asm_read_cr2 PROC
    mov rax, cr2
    ret
asm_read_cr2 ENDP

asm_read_cr3 PROC
    mov rax, cr3
    ret
asm_read_cr3 ENDP

; void asm_write_cr3(u64 value)  — RCX = value
asm_write_cr3 PROC
    mov cr3, rcx
    ret
asm_write_cr3 ENDP

asm_read_cr4 PROC
    mov rax, cr4
    ret
asm_read_cr4 ENDP

; void asm_write_cr4(u64 value)  — RCX = value
asm_write_cr4 PROC
    mov cr4, rcx
    ret
asm_write_cr4 ENDP

; ==============================================================
; General-purpose register reads
; ==============================================================

; u64 asm_read_rip() — returns the return address (caller's next RIP)
asm_read_rip PROC
    mov rax, QWORD PTR [rsp]
    ret
asm_read_rip ENDP

; u64 asm_read_rsp() — returns caller's stack pointer
asm_read_rsp PROC
    lea rax, QWORD PTR [rsp+8]  ; +8: skip the return address slot
    ret
asm_read_rsp ENDP

; u64 asm_read_rbp()
asm_read_rbp PROC
    mov rax, rbp
    ret
asm_read_rbp ENDP

; u64 asm_read_rflags()
asm_read_rflags PROC
    pushfq
    pop rax
    ret
asm_read_rflags ENDP

; ==============================================================
; Misc CPU instructions
; ==============================================================

; void asm_cpu_nop()
asm_cpu_nop PROC
    nop
    ret
asm_cpu_nop ENDP

; ==============================================================
; Memory barriers
; ==============================================================

asm_memory_barrier PROC
    mfence
    ret
asm_memory_barrier ENDP

asm_read_memory_barrier PROC
    lfence
    ret
asm_read_memory_barrier ENDP

asm_write_memory_barrier PROC
    sfence
    ret
asm_write_memory_barrier ENDP

; ==============================================================
; TLB management
; ==============================================================

; void asm_invlpg(u64 addr)  — RCX = virtual address to invalidate
asm_invlpg PROC
    invlpg [rcx]
    ret
asm_invlpg ENDP

; ==============================================================
; Atomic operations
; ==============================================================

; u64 asm_atomic_add(volatile u64* ptr, u64 value)  — RCX=ptr, RDX=value
; Returns the value that was in *ptr before the add.
asm_atomic_add PROC
    mov rax, rdx
    lock xadd QWORD PTR [rcx], rax
    ret
asm_atomic_add ENDP

; int asm_atomic_cmpxchg(volatile u64* ptr, u64 expected, u64 new_value)
;   RCX=ptr, RDX=expected, R8=new_value
;   Returns 1 if exchange was performed, 0 otherwise.
asm_atomic_cmpxchg PROC
    mov rax, rdx
    lock cmpxchg QWORD PTR [rcx], r8
    sete al
    movzx eax, al
    ret
asm_atomic_cmpxchg ENDP

; ==============================================================
; Symbol address helper
; ==============================================================

; u64 asm_get_isr_stub_base()
; Returns the runtime address of isr_stub_0 via RIP-relative LEA.
asm_get_isr_stub_base PROC
    lea rax, isr_stub_0
    ret
asm_get_isr_stub_base ENDP

END
