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

END
