;
; vkernel - UEFI Microkernel
; Copyright (C) 2026 vkernel authors
;
; interrupts.asm - ISR stubs (MASM, x86_64, MS ABI)
;
; Each stub pushes a uniform register_state frame and calls
; interrupt_dispatch() in arch_init.cpp.
;

extern interrupt_dispatch : proc

.code

; ============================================================
; ISR stubs — 256 vectors, each aligned to 16 bytes
;
; Vectors with CPU-pushed error codes: 8, 10-14, 17, 21, 29, 30
; All others push a dummy 0.
;
; Each stub is exactly 16 bytes (padded with nops).
; ============================================================

; Macro for vectors WITHOUT CPU error code
ISR_NOERR MACRO vec
    ALIGN 16
    push 0              ; dummy error code
    push vec            ; interrupt number
    jmp isr_common
ENDM

; Macro for vectors WITH CPU error code
ISR_ERR MACRO vec
    ALIGN 16
                        ; error code already pushed by CPU
    push vec            ; interrupt number
    jmp isr_common
ENDM

; -- Stubs start here. isr_stub_0 is the global anchor. --
ALIGN 16
isr_stub_0 LABEL BYTE
PUBLIC isr_stub_0

; Vectors 0-7: no error code
ISR_NOERR 0
ISR_NOERR 1
ISR_NOERR 2
ISR_NOERR 3
ISR_NOERR 4
ISR_NOERR 5
ISR_NOERR 6
ISR_NOERR 7

ISR_ERR   8            ; Double Fault
ISR_NOERR 9
ISR_ERR   10           ; Invalid TSS
ISR_ERR   11           ; Segment Not Present
ISR_ERR   12           ; Stack-Segment Fault
ISR_ERR   13           ; General Protection Fault
ISR_ERR   14           ; Page Fault
ISR_NOERR 15
ISR_NOERR 16
ISR_ERR   17           ; Alignment Check
ISR_NOERR 18
ISR_NOERR 19
ISR_NOERR 20
ISR_ERR   21           ; Control Protection Exception
ISR_NOERR 22
ISR_NOERR 23
ISR_NOERR 24
ISR_NOERR 25
ISR_NOERR 26
ISR_NOERR 27
ISR_NOERR 28
ISR_ERR   29           ; VMM Communication Exception
ISR_ERR   30           ; Security Exception
ISR_NOERR 31

; Vectors 32-255: hardware IRQs / software — no error code
vec = 32
REPT 224
    ISR_NOERR %vec
    vec = vec + 1
ENDM

; ============================================================
; Common ISR path
; ============================================================
ALIGN 16
isr_common PROC
    ; Save segment registers
    push rax
    mov  ax, gs
    movzx rax, ax
    mov  [rsp], rax
    push rax
    mov  ax, fs
    movzx rax, ax
    mov  [rsp], rax

    ; Save general-purpose registers
    push r15
    push r14
    push r13
    push r12
    push r11
    push r10
    push r9
    push r8
    push rbp
    push rdi
    push rsi
    push rdx
    push rcx
    push rbx
    push rax

    ; MS ABI: first argument in RCX = pointer to register_state
    mov  rcx, rsp

    ; Align stack to 16 bytes for the call
    push rbp
    mov  rbp, rsp
    and  rsp, NOT 0Fh
    sub  rsp, 32         ; shadow space for MS ABI

    call interrupt_dispatch

    ; RAX = register_state* to restore (may be different task's stack)
    mov  rsp, rax

    ; Restore GPRs
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

    ; Restore segment registers
    pop rax              ; FS
    mov fs, ax
    pop rax              ; GS
    mov gs, ax

    ; Remove int_no and error_code
    add rsp, 16

    iretq
isr_common ENDP

END
