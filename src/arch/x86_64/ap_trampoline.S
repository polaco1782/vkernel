/*
 * vkernel - UEFI Microkernel
 * Copyright (C) 2026 vkernel authors
 *
 * ap_trampoline.S - Application Processor (AP) 16-bit startup trampoline
 *
 * This blob is COPIED to physical address 0x8000 by the BSP before
 * sending the Startup IPI (SIPI vector = 0x08 → base = 0x8000).
 *
 * The AP starts in 16-bit real mode at CS:IP = 0x0800:0x0000
 * (physical 0x8000).  The trampoline:
 *   1. Loads a minimal temporary GDT
 *   2. Enables 32-bit protected mode
 *   3. Enables PAE, EFER.LME, and paging with the BSP's CR3
 *   4. Far-jumps (via a memory descriptor patched by the BSP)
 *      to the kernel's ap_entry_64 function in 64-bit long mode
 *
 *  Physical page layout at 0x8000 
 *
 *  Offset  Size  Description
 *  0x000   ~29   16-bit real-mode startup (cli/cld/lgdt/PE/far jump)
 *  0x040   ~60   32-bit protected-mode transition (PAE/LME/paging)
 *  0x100    6    GDT descriptor  { u16 limit=39, u32 base=0x8110 }
 *  0x110   40    5 × 8-byte temporary GDT entries (BSP fills these)
 *  0x138    8    CR3 value           (BSP fills before SIPI)
 *  0x140    8    AP stack pointer    (BSP fills before SIPI)
 *  0x148    6    Far-jump descriptor { u32 ap_entry_64_addr, u16 0x18 }
 *  0x150    4    AP ready flag       (AP writes 1; BSP polls)
 *
 *  Temporary GDT selectors 
 *  0x08 = 32-bit kernel code
 *  0x10 = 32-bit kernel data
 *  0x18 = 64-bit kernel code
 *  0x20 = 64-bit kernel data
 */

/* Physical address constants used from 32-bit code */
#define TRAM_GDT_DESC   0x8100  /* GDT descriptor  (6 bytes: u16 limit, u32 base) */
#define TRAM_CR3        0x8138  /* BSP's CR3 value (8 bytes) */
#define TRAM_STACK      0x8140  /* AP stack ptr    (8 bytes) */
#define TRAM_JUMP_FP    0x8148  /* Far-jump descriptor (6 bytes) */
#define TRAM_READY      0x8150  /* AP ready flag   (4 bytes) */

/* Temporary GDT selectors */
#define SEL_CODE32  0x08
#define SEL_DATA32  0x10
#define SEL_CODE64  0x18
#define SEL_DATA64  0x20

/* Physical address of ap_pm32 (BSP copies trampoline to 0x8000 and
 * ap_pm32 is forced to offset 0x40 by the .skip padding below,
 * so its physical address is always 0x8000 + 0x40 = 0x8040).     */
#define PHYS_AP_PM32    0x8040

.section .text.ap_trampoline, "ax"

/* ap_trampoline_start — exported so smp.cpp can copy & measure the blob */
.global ap_trampoline_start
ap_trampoline_start:

/*  16-bit real-mode entry  */
.code16
    cli
    cld

    /* DS = CS = 0x0800  (CS*16 = 0x8000 = our physical base) */
    movw    %cs, %ax
    movw    %ax, %ds

    /* Load temporary GDT from data page: DS:0x100 = physical 0x8100 */
    lgdt    0x100

    /* Set CR0.PE to enter 32-bit protected mode */
    movl    %cr0, %eax
    orl     $0x1, %eax
    movl    %eax, %cr0

    /*
     * Far jump with 32-bit operand size (0x66 0xEA prefix) to flush
     * the pipeline and load CS from the temporary GDT (SEL_CODE32).
     * Target is the physical address of ap_pm32 = PHYS_AP_PM32.
     */
    .byte   0x66, 0xEA
    .long   PHYS_AP_PM32
    .word   SEL_CODE32

    /* Pad so that ap_pm32 is exactly at offset 0x40 from this section start */
    .skip   (0x40 - (. - ap_trampoline_start))

/*  32-bit protected-mode transition  */
.code32
ap_pm32:
    /* Load flat 32-bit data selectors */
    movw    $SEL_DATA32, %ax
    movw    %ax, %ds
    movw    %ax, %es
    movw    %ax, %ss
    xorw    %ax, %ax
    movw    %ax, %fs
    movw    %ax, %gs

    /* Load BSP's CR3 (page tables) */
    movl    TRAM_CR3, %eax
    movl    %eax, %cr3

    /* Enable PAE (CR4 bit 5) */
    movl    %cr4, %eax
    orl     $(1 << 5), %eax
    movl    %eax, %cr4

    /* Enable Long Mode in EFER (MSR 0xC0000080, bit 8) */
    movl    $0xC0000080, %ecx
    rdmsr
    orl     $(1 << 8), %eax
    wrmsr

    /* Enable paging (CR0.PG bit 31) and write-protect (CR0.WP bit 16) */
    movl    %cr0, %eax
    orl     $0x80010000, %eax
    movl    %eax, %cr0

    /*
     * Far jump via memory descriptor at TRAM_JUMP_FP.
     * The BSP stores: { u32 phys_addr_of_ap_entry_64, u16 SEL_CODE64 }
     * This loads CS = 0x18 (64-bit code) and jumps to ap_entry_64,
     * activating 64-bit long mode.
     */
    ljmpl   *TRAM_JUMP_FP

/* Mark the end of the code blob (data area starts at page offset 0x100,
 * written directly by the BSP into the physical page — not part of the
 * copied binary).                                                       */
.global ap_trampoline_end
ap_trampoline_end:

/* Restore default 64-bit encoding for the remainder of the file */
.code64
