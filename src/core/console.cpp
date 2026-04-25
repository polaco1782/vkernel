/*
 * vkernel - UEFI Microkernel
 * Copyright (C) 2026 vkernel authors
 *
 * console.cpp - Console: UEFI ConOut → GOP framebuffer + COM1 serial
 */

#include "config.h"
#include "types.h"
#include "uefi.h"
#include "console.h"
#include "arch/x86_64/arch.h"

using vk_va_list = __builtin_va_list;

namespace vk {

/* ============================================================
 * 8×16 bitmap font — ASCII 32 (space) through 127 (DEL)
 * Each glyph is 16 bytes (one byte per row, MSB = leftmost pixel).
 * ============================================================ */
static constexpr u8 k_font[96][16] = {
    /* 32 space */  { 0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00 },
    /* 33 !     */  { 0x00,0x00,0x18,0x3C,0x3C,0x3C,0x18,0x18,0x18,0x00,0x18,0x18,0x00,0x00,0x00,0x00 },
    /* 34 "     */  { 0x00,0x66,0x66,0x66,0x24,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00 },
    /* 35 #     */  { 0x00,0x00,0x00,0x6C,0x6C,0xFE,0x6C,0x6C,0x6C,0xFE,0x6C,0x6C,0x00,0x00,0x00,0x00 },
    /* 36 $     */  { 0x18,0x18,0x7C,0xC6,0xC2,0xC0,0x7C,0x06,0x06,0x86,0xC6,0x7C,0x18,0x18,0x00,0x00 },
    /* 37 %     */  { 0x00,0x00,0x00,0x00,0xC2,0xC6,0x0C,0x18,0x30,0x60,0xC6,0x86,0x00,0x00,0x00,0x00 },
    /* 38 &     */  { 0x00,0x00,0x38,0x6C,0x6C,0x38,0x76,0xDC,0xCC,0xCC,0xCC,0x76,0x00,0x00,0x00,0x00 },
    /* 39 '     */  { 0x00,0x30,0x30,0x30,0x60,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00 },
    /* 40 (     */  { 0x00,0x00,0x0C,0x18,0x30,0x30,0x30,0x30,0x30,0x30,0x18,0x0C,0x00,0x00,0x00,0x00 },
    /* 41 )     */  { 0x00,0x00,0x30,0x18,0x0C,0x0C,0x0C,0x0C,0x0C,0x0C,0x18,0x30,0x00,0x00,0x00,0x00 },
    /* 42 *     */  { 0x00,0x00,0x00,0x00,0x00,0x66,0x3C,0xFF,0x3C,0x66,0x00,0x00,0x00,0x00,0x00,0x00 },
    /* 43 +     */  { 0x00,0x00,0x00,0x18,0x18,0x18,0xFF,0x18,0x18,0x18,0x00,0x00,0x00,0x00,0x00,0x00 },
    /* 44 ,     */  { 0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x18,0x18,0x18,0x30,0x00,0x00,0x00 },
    /* 45 -     */  { 0x00,0x00,0x00,0x00,0x00,0x00,0xFF,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00 },
    /* 46 .     */  { 0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x18,0x18,0x00,0x00,0x00,0x00 },
    /* 47 /     */  { 0x00,0x00,0x00,0x00,0x02,0x06,0x0C,0x18,0x30,0x60,0xC0,0x80,0x00,0x00,0x00,0x00 },
    /* 48 0     */  { 0x00,0x00,0x7C,0xC6,0xC6,0xCE,0xDE,0xF6,0xE6,0xC6,0xC6,0x7C,0x00,0x00,0x00,0x00 },
    /* 49 1     */  { 0x00,0x00,0x18,0x38,0x78,0x18,0x18,0x18,0x18,0x18,0x18,0x7E,0x00,0x00,0x00,0x00 },
    /* 50 2     */  { 0x00,0x00,0x7C,0xC6,0x06,0x0C,0x18,0x30,0x60,0xC0,0xC6,0xFE,0x00,0x00,0x00,0x00 },
    /* 51 3     */  { 0x00,0x00,0x7C,0xC6,0x06,0x06,0x3C,0x06,0x06,0x06,0xC6,0x7C,0x00,0x00,0x00,0x00 },
    /* 52 4     */  { 0x00,0x00,0x0C,0x1C,0x3C,0x6C,0xCC,0xFE,0x0C,0x0C,0x0C,0x1E,0x00,0x00,0x00,0x00 },
    /* 53 5     */  { 0x00,0x00,0xFE,0xC0,0xC0,0xC0,0xFC,0x06,0x06,0x06,0xC6,0x7C,0x00,0x00,0x00,0x00 },
    /* 54 6     */  { 0x00,0x00,0x38,0x60,0xC0,0xC0,0xFC,0xC6,0xC6,0xC6,0xC6,0x7C,0x00,0x00,0x00,0x00 },
    /* 55 7     */  { 0x00,0x00,0xFE,0xC6,0x06,0x06,0x0C,0x18,0x30,0x30,0x30,0x30,0x00,0x00,0x00,0x00 },
    /* 56 8     */  { 0x00,0x00,0x7C,0xC6,0xC6,0xC6,0x7C,0xC6,0xC6,0xC6,0xC6,0x7C,0x00,0x00,0x00,0x00 },
    /* 57 9     */  { 0x00,0x00,0x7C,0xC6,0xC6,0xC6,0x7E,0x06,0x06,0x06,0x0C,0x78,0x00,0x00,0x00,0x00 },
    /* 58 :     */  { 0x00,0x00,0x00,0x00,0x18,0x18,0x00,0x00,0x00,0x18,0x18,0x00,0x00,0x00,0x00,0x00 },
    /* 59 ;     */  { 0x00,0x00,0x00,0x00,0x18,0x18,0x00,0x00,0x00,0x18,0x18,0x30,0x00,0x00,0x00,0x00 },
    /* 60 <     */  { 0x00,0x00,0x00,0x06,0x0C,0x18,0x30,0x60,0x30,0x18,0x0C,0x06,0x00,0x00,0x00,0x00 },
    /* 61 =     */  { 0x00,0x00,0x00,0x00,0x00,0xFF,0x00,0x00,0xFF,0x00,0x00,0x00,0x00,0x00,0x00,0x00 },
    /* 62 >     */  { 0x00,0x00,0x00,0x60,0x30,0x18,0x0C,0x06,0x0C,0x18,0x30,0x60,0x00,0x00,0x00,0x00 },
    /* 63 ?     */  { 0x00,0x00,0x7C,0xC6,0xC6,0x0C,0x18,0x18,0x18,0x00,0x18,0x18,0x00,0x00,0x00,0x00 },
    /* 64 @     */  { 0x00,0x00,0x7C,0xC6,0xC6,0xDE,0xDE,0xDE,0xDC,0xC0,0xC0,0x7C,0x00,0x00,0x00,0x00 },
    /* 65 A     */  { 0x00,0x00,0x10,0x38,0x6C,0xC6,0xC6,0xFE,0xC6,0xC6,0xC6,0xC6,0x00,0x00,0x00,0x00 },
    /* 66 B     */  { 0x00,0x00,0xFC,0x66,0x66,0x66,0x7C,0x66,0x66,0x66,0x66,0xFC,0x00,0x00,0x00,0x00 },
    /* 67 C     */  { 0x00,0x00,0x3C,0x66,0xC2,0xC0,0xC0,0xC0,0xC0,0xC2,0x66,0x3C,0x00,0x00,0x00,0x00 },
    /* 68 D     */  { 0x00,0x00,0xF8,0x6C,0x66,0x66,0x66,0x66,0x66,0x66,0x6C,0xF8,0x00,0x00,0x00,0x00 },
    /* 69 E     */  { 0x00,0x00,0xFE,0x66,0x62,0x68,0x78,0x68,0x60,0x62,0x66,0xFE,0x00,0x00,0x00,0x00 },
    /* 70 F     */  { 0x00,0x00,0xFE,0x66,0x62,0x68,0x78,0x68,0x60,0x60,0x60,0xF0,0x00,0x00,0x00,0x00 },
    /* 71 G     */  { 0x00,0x00,0x3C,0x66,0xC2,0xC0,0xC0,0xDE,0xC6,0xC6,0x66,0x3A,0x00,0x00,0x00,0x00 },
    /* 72 H     */  { 0x00,0x00,0xC6,0xC6,0xC6,0xC6,0xFE,0xC6,0xC6,0xC6,0xC6,0xC6,0x00,0x00,0x00,0x00 },
    /* 73 I     */  { 0x00,0x00,0x3C,0x18,0x18,0x18,0x18,0x18,0x18,0x18,0x18,0x3C,0x00,0x00,0x00,0x00 },
    /* 74 J     */  { 0x00,0x00,0x1E,0x0C,0x0C,0x0C,0x0C,0x0C,0xCC,0xCC,0xCC,0x78,0x00,0x00,0x00,0x00 },
    /* 75 K     */  { 0x00,0x00,0xE6,0x66,0x66,0x6C,0x78,0x78,0x6C,0x66,0x66,0xE6,0x00,0x00,0x00,0x00 },
    /* 76 L     */  { 0x00,0x00,0xF0,0x60,0x60,0x60,0x60,0x60,0x60,0x62,0x66,0xFE,0x00,0x00,0x00,0x00 },
    /* 77 M     */  { 0x00,0x00,0xC6,0xEE,0xFE,0xFE,0xD6,0xC6,0xC6,0xC6,0xC6,0xC6,0x00,0x00,0x00,0x00 },
    /* 78 N     */  { 0x00,0x00,0xC6,0xE6,0xF6,0xFE,0xDE,0xCE,0xC6,0xC6,0xC6,0xC6,0x00,0x00,0x00,0x00 },
    /* 79 O     */  { 0x00,0x00,0x7C,0xC6,0xC6,0xC6,0xC6,0xC6,0xC6,0xC6,0xC6,0x7C,0x00,0x00,0x00,0x00 },
    /* 80 P     */  { 0x00,0x00,0xFC,0x66,0x66,0x66,0x7C,0x60,0x60,0x60,0x60,0xF0,0x00,0x00,0x00,0x00 },
    /* 81 Q     */  { 0x00,0x00,0x7C,0xC6,0xC6,0xC6,0xC6,0xC6,0xC6,0xD6,0xDE,0x7C,0x0C,0x0E,0x00,0x00 },
    /* 82 R     */  { 0x00,0x00,0xFC,0x66,0x66,0x66,0x7C,0x6C,0x66,0x66,0x66,0xE6,0x00,0x00,0x00,0x00 },
    /* 83 S     */  { 0x00,0x00,0x7C,0xC6,0xC6,0x60,0x38,0x0C,0x06,0xC6,0xC6,0x7C,0x00,0x00,0x00,0x00 },
    /* 84 T     */  { 0x00,0x00,0xFF,0xDB,0x99,0x18,0x18,0x18,0x18,0x18,0x18,0x3C,0x00,0x00,0x00,0x00 },
    /* 85 U     */  { 0x00,0x00,0xC6,0xC6,0xC6,0xC6,0xC6,0xC6,0xC6,0xC6,0xC6,0x7C,0x00,0x00,0x00,0x00 },
    /* 86 V     */  { 0x00,0x00,0xC6,0xC6,0xC6,0xC6,0xC6,0xC6,0xC6,0x6C,0x38,0x10,0x00,0x00,0x00,0x00 },
    /* 87 W     */  { 0x00,0x00,0xC6,0xC6,0xC6,0xC6,0xD6,0xD6,0xD6,0xFE,0xEE,0x6C,0x00,0x00,0x00,0x00 },
    /* 88 X     */  { 0x00,0x00,0xC6,0xC6,0x6C,0x7C,0x38,0x38,0x7C,0x6C,0xC6,0xC6,0x00,0x00,0x00,0x00 },
    /* 89 Y     */  { 0x00,0x00,0xCC,0xCC,0xCC,0xCC,0x78,0x30,0x30,0x30,0x30,0x78,0x00,0x00,0x00,0x00 },
    /* 90 Z     */  { 0x00,0x00,0xFE,0xC6,0x86,0x0C,0x18,0x30,0x60,0xC2,0xC6,0xFE,0x00,0x00,0x00,0x00 },
    /* 91 [     */  { 0x00,0x00,0x3C,0x30,0x30,0x30,0x30,0x30,0x30,0x30,0x30,0x3C,0x00,0x00,0x00,0x00 },
    /* 92 \     */  { 0x00,0x00,0x00,0x80,0xC0,0x60,0x30,0x18,0x0C,0x06,0x02,0x00,0x00,0x00,0x00,0x00 },
    /* 93 ]     */  { 0x00,0x00,0x3C,0x0C,0x0C,0x0C,0x0C,0x0C,0x0C,0x0C,0x0C,0x3C,0x00,0x00,0x00,0x00 },
    /* 94 ^     */  { 0x10,0x38,0x6C,0xC6,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00 },
    /* 95 _     */  { 0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0xFF,0x00,0x00 },
    /* 96 `     */  { 0x30,0x30,0x18,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00 },
    /* 97 a     */  { 0x00,0x00,0x00,0x00,0x00,0x78,0x0C,0x7C,0xCC,0xCC,0xCC,0x76,0x00,0x00,0x00,0x00 },
    /* 98 b     */  { 0x00,0x00,0xE0,0x60,0x60,0x78,0x6C,0x66,0x66,0x66,0x66,0x7C,0x00,0x00,0x00,0x00 },
    /* 99 c     */  { 0x00,0x00,0x00,0x00,0x00,0x7C,0xC6,0xC0,0xC0,0xC0,0xC6,0x7C,0x00,0x00,0x00,0x00 },
    /* 100 d    */  { 0x00,0x00,0x1C,0x0C,0x0C,0x3C,0x6C,0xCC,0xCC,0xCC,0xCC,0x76,0x00,0x00,0x00,0x00 },
    /* 101 e    */  { 0x00,0x00,0x00,0x00,0x00,0x7C,0xC6,0xFE,0xC0,0xC0,0xC6,0x7C,0x00,0x00,0x00,0x00 },
    /* 102 f    */  { 0x00,0x00,0x1C,0x36,0x32,0x30,0x7C,0x30,0x30,0x30,0x30,0x78,0x00,0x00,0x00,0x00 },
    /* 103 g    */  { 0x00,0x00,0x00,0x00,0x00,0x76,0xCC,0xCC,0xCC,0xCC,0xCC,0x7C,0x0C,0xCC,0x78,0x00 },
    /* 104 h    */  { 0x00,0x00,0xE0,0x60,0x60,0x6C,0x76,0x66,0x66,0x66,0x66,0xE6,0x00,0x00,0x00,0x00 },
    /* 105 i    */  { 0x00,0x00,0x18,0x18,0x00,0x38,0x18,0x18,0x18,0x18,0x18,0x3C,0x00,0x00,0x00,0x00 },
    /* 106 j    */  { 0x00,0x00,0x06,0x06,0x00,0x0E,0x06,0x06,0x06,0x06,0x06,0x06,0x66,0x66,0x3C,0x00 },
    /* 107 k    */  { 0x00,0x00,0xE0,0x60,0x60,0x66,0x6C,0x78,0x78,0x6C,0x66,0xE6,0x00,0x00,0x00,0x00 },
    /* 108 l    */  { 0x00,0x00,0x38,0x18,0x18,0x18,0x18,0x18,0x18,0x18,0x18,0x3C,0x00,0x00,0x00,0x00 },
    /* 109 m    */  { 0x00,0x00,0x00,0x00,0x00,0xEC,0xFE,0xD6,0xD6,0xD6,0xD6,0xC6,0x00,0x00,0x00,0x00 },
    /* 110 n    */  { 0x00,0x00,0x00,0x00,0x00,0xDC,0x66,0x66,0x66,0x66,0x66,0x66,0x00,0x00,0x00,0x00 },
    /* 111 o    */  { 0x00,0x00,0x00,0x00,0x00,0x7C,0xC6,0xC6,0xC6,0xC6,0xC6,0x7C,0x00,0x00,0x00,0x00 },
    /* 112 p    */  { 0x00,0x00,0x00,0x00,0x00,0xDC,0x66,0x66,0x66,0x66,0x66,0x7C,0x60,0x60,0xF0,0x00 },
    /* 113 q    */  { 0x00,0x00,0x00,0x00,0x00,0x76,0xCC,0xCC,0xCC,0xCC,0xCC,0x7C,0x0C,0x0C,0x1E,0x00 },
    /* 114 r    */  { 0x00,0x00,0x00,0x00,0x00,0xDC,0x76,0x66,0x60,0x60,0x60,0xF0,0x00,0x00,0x00,0x00 },
    /* 115 s    */  { 0x00,0x00,0x00,0x00,0x00,0x7C,0xC6,0x60,0x38,0x0C,0xC6,0x7C,0x00,0x00,0x00,0x00 },
    /* 116 t    */  { 0x00,0x00,0x10,0x30,0x30,0xFC,0x30,0x30,0x30,0x30,0x36,0x1C,0x00,0x00,0x00,0x00 },
    /* 117 u    */  { 0x00,0x00,0x00,0x00,0x00,0xCC,0xCC,0xCC,0xCC,0xCC,0xCC,0x76,0x00,0x00,0x00,0x00 },
    /* 118 v    */  { 0x00,0x00,0x00,0x00,0x00,0x66,0x66,0x66,0x66,0x66,0x3C,0x18,0x00,0x00,0x00,0x00 },
    /* 119 w    */  { 0x00,0x00,0x00,0x00,0x00,0xC6,0xC6,0xD6,0xD6,0xD6,0xFE,0x6C,0x00,0x00,0x00,0x00 },
    /* 120 x    */  { 0x00,0x00,0x00,0x00,0x00,0xC6,0x6C,0x38,0x38,0x38,0x6C,0xC6,0x00,0x00,0x00,0x00 },
    /* 121 y    */  { 0x00,0x00,0x00,0x00,0x00,0xC6,0xC6,0xC6,0xC6,0xC6,0xC6,0x7E,0x06,0x0C,0xF8,0x00 },
    /* 122 z    */  { 0x00,0x00,0x00,0x00,0x00,0xFE,0xCC,0x18,0x30,0x60,0xC6,0xFE,0x00,0x00,0x00,0x00 },
    /* 123 {    */  { 0x00,0x00,0x0E,0x18,0x18,0x18,0x70,0x18,0x18,0x18,0x18,0x0E,0x00,0x00,0x00,0x00 },
    /* 124 |    */  { 0x00,0x00,0x18,0x18,0x18,0x18,0x00,0x18,0x18,0x18,0x18,0x18,0x00,0x00,0x00,0x00 },
    /* 125 }    */  { 0x00,0x00,0x70,0x18,0x18,0x18,0x0E,0x18,0x18,0x18,0x18,0x70,0x00,0x00,0x00,0x00 },
    /* 126 ~    */  { 0x00,0x76,0xDC,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00 },
    /* 127 DEL  */  { 0x00,0x00,0x00,0x00,0x10,0x38,0x6C,0xC6,0xC6,0xFE,0x00,0x00,0x00,0x00,0x00,0x00 },
};

static constexpr u32 FONT_W = 8;
static constexpr u32 FONT_H = 16;

/* ============================================================
 * Serial port (COM1 = 0x3F8)
 * ============================================================ */

static constexpr u16 COM1 = 0x3F8;

static inline void serial_outb(u16 port, u8 val) {
    arch::outb(port, val);
}
static inline auto serial_inb(u16 port) -> u8 {
    return arch::inb(port);
}
static void serial_init() {
    serial_outb(COM1 + 1, 0x00);
    serial_outb(COM1 + 3, 0x80);
    serial_outb(COM1 + 0, 0x01); /* 115200 baud */
    serial_outb(COM1 + 1, 0x00);
    serial_outb(COM1 + 3, 0x03); /* 8N1 */
    serial_outb(COM1 + 2, 0xC7);
    serial_outb(COM1 + 4, 0x0B);
}
static void serial_putc_raw(char c) {
    while (!(serial_inb(COM1 + 5) & 0x20)) {}
    serial_outb(COM1, static_cast<u8>(c));
}

/* ============================================================
 * Framebuffer state
 * ============================================================ */

struct fb_state {
    u32* base    = null;
    u32  width   = 0;
    u32  height  = 0;
    u32  stride  = 0;   /* pixels per scan line */
    u32  cols    = 0;   /* text columns */
    u32  rows    = 0;   /* text rows */
    u32  col     = 0;   /* current cursor column */
    u32  row     = 0;   /* current cursor row */
    u32  fg      = 0x00FFFFFF; /* white */
    u32  bg      = 0x00000000; /* black */
    uefi::pixel_format fmt = uefi::pixel_format::bgrx_8bpp;
};

static fb_state g_fb;

/* Convert an RGB triplet to the framebuffer's native pixel word */
static auto rgb_to_pixel(u8 r, u8 g, u8 b) -> u32 {
    if (g_fb.fmt == uefi::pixel_format::rgbx_8bpp) {
        return (static_cast<u32>(r)) |
               (static_cast<u32>(g) << 8) |
               (static_cast<u32>(b) << 16);
    }
    /* BGRX (default on most firmware) */
    return (static_cast<u32>(b)) |
           (static_cast<u32>(g) << 8) |
           (static_cast<u32>(r) << 16);
}

/* Scroll the framebuffer up one text row */
static void fb_scroll() {
    u32 row_pixels = FONT_H * g_fb.stride;
    /* Move all rows up by one */
    for (u32 y = 0; y < (g_fb.rows - 1) * FONT_H; ++y) {
        u32* dst = g_fb.base + y * g_fb.stride;
        const u32* src = g_fb.base + (y + FONT_H) * g_fb.stride;
        for (u32 x = 0; x < g_fb.width; ++x) {
            dst[x] = src[x];
        }
    }
    /* Clear the last row */
    for (u32 y = (g_fb.rows - 1) * FONT_H; y < g_fb.rows * FONT_H; ++y) {
        u32* row = g_fb.base + y * g_fb.stride;
        for (u32 x = 0; x < g_fb.width; ++x) {
            row[x] = g_fb.bg;
        }
    }
    (void)row_pixels;
}

/* Draw one glyph at (col, row) in text-cell coordinates */
static void fb_draw_char(u32 col, u32 row, char c) {
    u32 glyph_idx = (static_cast<u8>(c) >= 32 && static_cast<u8>(c) <= 127)
                  ? static_cast<u8>(c) - 32 : 0;
    const u8* glyph = k_font[glyph_idx];

    u32 px = col * FONT_W;
    u32 py = row * FONT_H;

    for (u32 gy = 0; gy < FONT_H; ++gy) {
        u8 row_bits = glyph[gy];
        u32* line = g_fb.base + (py + gy) * g_fb.stride + px;
        for (u32 gx = 0; gx < FONT_W; ++gx) {
            line[gx] = (row_bits & (0x80 >> gx)) ? g_fb.fg : g_fb.bg;
        }
    }
}

/* Advance the cursor, scrolling if necessary */
static void fb_advance() {
    ++g_fb.col;
    if (g_fb.col >= g_fb.cols) {
        g_fb.col = 0;
        ++g_fb.row;
        if (g_fb.row >= g_fb.rows) {
            fb_scroll();
            g_fb.row = g_fb.rows - 1;
        }
    }
}

static void fb_putc(char c) {
    if (c == '\n') {
        g_fb.col = 0;
        ++g_fb.row;
        if (g_fb.row >= g_fb.rows) {
            fb_scroll();
            g_fb.row = g_fb.rows - 1;
        }
        return;
    }
    if (c == '\r') {
        g_fb.col = 0;
        return;
    }
    if (c == '\t') {
        /* Tab stop every 8 columns */
        u32 next = (g_fb.col + 8) & ~7u;
        while (g_fb.col < next) {
            fb_draw_char(g_fb.col, g_fb.row, ' ');
            fb_advance();
        }
        return;
    }
    fb_draw_char(g_fb.col, g_fb.row, c);
    fb_advance();
}

/* ============================================================
 * Output backend flags
 * ============================================================ */

static bool g_use_serial = false;
static bool g_use_fb     = false;

/* ============================================================
 * Public API
 * ============================================================ */

void console::switch_to_serial() {
    serial_init();
    g_use_serial = true;
}

void console::putc_serial(char c) {
    if (c == '\n') {
        serial_putc_raw('\r');
    }
    serial_putc_raw(c);
}

void console::puts_serial(const char* str) {
    if (str == null) {
        return;
    }
    while (*str != '\0') {
        putc_serial(*str++);
    }
}

void console::clear_serial() {
    puts_serial("\x1b[2J\x1b[H");
}

void console::init_framebuffer(const uefi::framebuffer_info& fb) {
    if (!fb.valid || fb.base == 0) return;
    g_fb.base   = reinterpret_cast<u32*>(fb.base);
    g_fb.width  = fb.width;
    g_fb.height = fb.height;
    g_fb.stride = fb.stride;
    g_fb.cols   = fb.width  / FONT_W;
    g_fb.rows   = fb.height / FONT_H;
    g_fb.col    = 0;
    g_fb.row    = 0;
    g_fb.fmt    = fb.format;
    g_fb.fg     = rgb_to_pixel(0xFF, 0xFF, 0xFF); /* white text */
    g_fb.bg     = rgb_to_pixel(0x00, 0x00, 0x00); /* black background */

    /* Clear screen */
    for (u32 y = 0; y < fb.height; ++y) {
        u32* line = g_fb.base + y * g_fb.stride;
        for (u32 x = 0; x < fb.width; ++x) {
            line[x] = g_fb.bg;
        }
    }
}

void console::switch_to_framebuffer() {
    g_use_fb = true;
}

auto console::framebuffer() -> uefi::framebuffer_info {
    uefi::framebuffer_info fb{};
    fb.base   = (phys_addr)(u64)g_fb.base;
    fb.width  = g_fb.width;
    fb.height = g_fb.height;
    fb.stride = g_fb.stride;
    fb.format = g_fb.fmt;
    fb.valid  = g_fb.base != null && g_fb.width != 0 && g_fb.height != 0;
    return fb;
}

void console::putc_framebuffer(char c) {
    if (!g_use_fb || g_fb.base == null) {
        return;
    }
    fb_putc(c);
}

void console::puts_framebuffer(const char* str) {
    if (str == null || !g_use_fb || g_fb.base == null) {
        return;
    }
    while (*str != '\0') {
        putc_framebuffer(*str++);
    }
}

void console::clear_framebuffer() {
    if (!g_use_fb || g_fb.base == null) {
        return;
    }
    for (u32 y = 0; y < g_fb.height; ++y) {
        u32* line = g_fb.base + y * g_fb.stride;
        for (u32 x = 0; x < g_fb.width; ++x) {
            line[x] = g_fb.bg;
        }
    }
    g_fb.col = 0;
    g_fb.row = 0;
}

/* Initialize the UEFI ConOut subsystem (pre-EBS phase) */
auto console::init() -> status_code {
    if (uefi::g_system_table == null) {
        return status_code::error;
    }
    auto con_out = uefi::g_system_table->con_out;
    if (con_out == null) {
        return status_code::error;
    }
    con_out->reset(con_out, false);
    con_out->clear_screen(con_out);
    return status_code::success;
}

/* Core character output — writes to every active backend */
void console::putc(char c) {
    if (g_use_serial) {
        if (c == '\n') serial_putc_raw('\r');
        serial_putc_raw(c);
    }

    if (g_use_fb) {
        fb_putc(c);
        return; /* fb is now the primary visual output */
    }

    /* Fall back to UEFI ConOut (pre-EBS only) */
    if (!g_use_serial && uefi::g_system_table != null) {
        auto con_out = uefi::g_system_table->con_out;
        if (con_out != null) {
            if (c == '\n') {
                char16_t crlf[3] = { u'\r', u'\n', 0 };
                con_out->output_string(con_out, crlf);
                return;
            }
            char16_t str[2] = { static_cast<char16_t>(c), 0 };
            con_out->output_string(con_out, str);
        }
    }
}

/* Output a null-terminated ASCII string */
void console::puts(const char* str) {
    if (str == null) return;
    while (*str != '\0') putc(*str++);
}

/* Output a null-terminated UCS-2 string */
void console::putw(const char16_t* str) {
    if (str == null || uefi::g_system_table == null) {
        return;
    }
    auto con_out = uefi::g_system_table->con_out;
    if (con_out == null) {
        return;
    }
    con_out->output_string(con_out, str);
}

/* Print a 64-bit value as "0x" + 16 hex digits */
void console::put_hex(u64 value) {
    static constexpr char hex_chars[] = "0123456789ABCDEF";
    char buf[19];
    buf[0] = '0'; buf[1] = 'x';
    for (i32 i = 15; i >= 0; --i) {
        buf[2 + (15 - i)] = hex_chars[(value >> (i * 4)) & 0xF];
    }
    buf[18] = '\0';
    puts(buf);
}

/* Print a 64-bit value as unsigned decimal */
void console::put_dec(u64 value) {
    if (value == 0) { putc('0'); return; }
    char buf[21];
    i32 i = 0;
    while (value > 0 && i < 20) {
        buf[i++] = static_cast<char>('0' + (value % 10));
        value /= 10;
    }
    for (i32 j = i - 1; j >= 0; --j) putc(buf[j]);
}

/* Clear the console */
void console::clear() {
    if (g_use_fb) {
        for (u32 y = 0; y < g_fb.height; ++y) {
            u32* line = g_fb.base + y * g_fb.stride;
            for (u32 x = 0; x < g_fb.width; ++x) line[x] = g_fb.bg;
        }
        g_fb.col = 0; g_fb.row = 0;
        return;
    }
    if (uefi::g_system_table == null) return;
    auto con_out = uefi::g_system_table->con_out;
    if (con_out != null) con_out->clear_screen(con_out);
}

/* Set text colour — maps UEFI palette to 24-bit RGB on the framebuffer */
void console::set_color(console_color foreground, console_color background) {
    if (g_use_fb) {
        /* Map the 16-colour UEFI palette to RGB */
        static constexpr u32 palette[16] = {
            0x000000, 0x0000AA, 0x00AA00, 0x00AAAA,
            0xAA0000, 0xAA00AA, 0xAA5500, 0xAAAAAA,
            0x555555, 0x5555FF, 0x55FF55, 0x55FFFF,
            0xFF5555, 0xFF55FF, 0xFFFF55, 0xFFFFFF,
        };
        u32 fi = static_cast<u32>(foreground) & 0xF;
        u32 bi = static_cast<u32>(background) & 0xF;
        u8 fr = (palette[fi] >> 16) & 0xFF;
        u8 fg = (palette[fi] >>  8) & 0xFF;
        u8 fb = (palette[fi]      ) & 0xFF;
        u8 br = (palette[bi] >> 16) & 0xFF;
        u8 bg = (palette[bi] >>  8) & 0xFF;
        u8 bb = (palette[bi]      ) & 0xFF;
        g_fb.fg = rgb_to_pixel(fr, fg, fb);
        g_fb.bg = rgb_to_pixel(br, bg, bb);
        return;
    }

    // serial does not supports framebuffer-like color attributes, so we skip it
    if (g_use_serial)
        return;
    if (uefi::g_system_table == null)
        return;
    auto con_out = uefi::g_system_table->con_out;
    if (con_out == null)
        return;

    usize attribute = static_cast<usize>(background) << 4 | static_cast<usize>(foreground);
    con_out->set_attribute(con_out, attribute);
}

/* Get cursor position */
auto console::get_position() -> console_state {
    if (g_use_fb) {
        return { g_fb.col, g_fb.row, g_fb.cols, g_fb.rows,
                 console_color::white, console_color::black, true };
    }
    if (uefi::g_system_table == null || uefi::g_system_table->con_out == null) {
        return {0, 0, 0, 0, console_color::white, console_color::black, false};
    }
    auto mode = uefi::g_system_table->con_out->mode;
    if (mode == null) {
        return {0, 0, 0, 0, console_color::white, console_color::black, false};
    }
    return { static_cast<u32>(mode->cursor_column), static_cast<u32>(mode->cursor_row),
             80, 25, console_color::white, console_color::black, mode->cursor_visible };
}

/* Set cursor position */
void console::set_position(u32 column, u32 row) {
    if (g_use_fb) {
        g_fb.col = column < g_fb.cols ? column : g_fb.cols - 1;
        g_fb.row = row    < g_fb.rows ? row    : g_fb.rows - 1;
        return;
    }
    if (uefi::g_system_table == null) return;
    auto con_out = uefi::g_system_table->con_out;
    if (con_out != null)
        con_out->set_cursor_position(con_out, column, row);
}

/* Logging implementation */
namespace log {

namespace {

enum class log_level : u8 {
    printk = 0,
    error = 1,
    warn = 2,
    info = 3,
    debug = 4,
    verbose = 5,
    crash = 6,
};

struct format_state {
    char last_char = '\0';
};

enum class length_modifier : u8 {
    none,
    l,
    ll,
    z,
};

static void format_putc(format_state& state, char c) {
    console::putc(c);
    state.last_char = c;
}

static void format_puts(format_state& state, const char* str) {
    if (str == null) {
        str = "(null)";
    }
    while (*str != '\0') {
        format_putc(state, *str++);
    }
}

static void format_unsigned(format_state& state, u64 value, u32 base,
                            bool uppercase, bool prefix, usize min_digits = 1) {
    static constexpr char lower_digits[] = "0123456789abcdef";
    static constexpr char upper_digits[] = "0123456789ABCDEF";
    const char* digits = uppercase ? upper_digits : lower_digits;

    char buffer[32];
    constexpr usize buffer_capacity = sizeof(buffer) / sizeof(buffer[0]);
    usize count = 0;

    do {
        buffer[count++] = digits[value % base];
        value /= base;
    } while (value != 0 && count < buffer_capacity);

    while (count < min_digits && count < buffer_capacity) {
        buffer[count++] = '0';
    }

    if (prefix) {
        format_puts(state, uppercase ? "0X" : "0x");
    }

    while (count > 0) {
        format_putc(state, buffer[--count]);
    }
}

static void format_signed(format_state& state, i64 value) {
    u64 magnitude = static_cast<u64>(value);
    if (value < 0) {
        format_putc(state, '-');
        magnitude = static_cast<u64>(-(value + 1)) + 1;
    }
    format_unsigned(state, magnitude, 10, false, false);
}

static auto read_unsigned_arg(vk_va_list args, length_modifier length) -> u64 {
    switch (length) {
        case length_modifier::ll:
            return __builtin_va_arg(args, unsigned long long);
        case length_modifier::l:
            return __builtin_va_arg(args, unsigned long);
        case length_modifier::z:
            return __builtin_va_arg(args, usize);
        case length_modifier::none:
        default:
            return __builtin_va_arg(args, unsigned int);
    }
}

static auto read_signed_arg(vk_va_list args, length_modifier length) -> i64 {
    switch (length) {
        case length_modifier::ll:
            return __builtin_va_arg(args, long long);
        case length_modifier::l:
            return __builtin_va_arg(args, long);
        case length_modifier::z:
            return __builtin_va_arg(args, isize);
        case length_modifier::none:
        default:
            return __builtin_va_arg(args, int);
    }
}

static void vformat_to_console(format_state& state, const char* format, vk_va_list args) {
    if (format == null) {
        return;
    }

    while (*format != '\0') {
        if (*format != '%') {
            format_putc(state, *format++);
            continue;
        }

        ++format;
        if (*format == '%') {
            format_putc(state, *format++);
            continue;
        }

        bool alternate = false;
        while (*format == '-' || *format == '+' || *format == ' ' || *format == '#' || *format == '0') {
            if (*format == '#') {
                alternate = true;
            }
            ++format;
        }

        while (*format >= '0' && *format <= '9') {
            ++format;
        }

        if (*format == '.') {
            ++format;
            while (*format >= '0' && *format <= '9') {
                ++format;
            }
        }

        length_modifier length = length_modifier::none;
        if (*format == 'l') {
            ++format;
            if (*format == 'l') {
                ++format;
                length = length_modifier::ll;
            } else {
                length = length_modifier::l;
            }
        } else if (*format == 'z') {
            ++format;
            length = length_modifier::z;
        }

        char spec = *format;
        if (spec == '\0') {
            break;
        }
        ++format;

        switch (spec) {
            case 'c':
                format_putc(state, static_cast<char>(__builtin_va_arg(args, int)));
                break;
            case 's':
                format_puts(state, __builtin_va_arg(args, const char*));
                break;
            case 'd':
            case 'i':
                format_signed(state, read_signed_arg(args, length));
                break;
            case 'u':
                format_unsigned(state, read_unsigned_arg(args, length), 10, false, false);
                break;
            case 'x':
                format_unsigned(state, read_unsigned_arg(args, length), 16, false, alternate);
                break;
            case 'X':
                format_unsigned(state, read_unsigned_arg(args, length), 16, true, alternate);
                break;
            case 'p': {
                auto value = reinterpret_cast<usize>(__builtin_va_arg(args, const void*));
                format_unsigned(state, value, 16, false, true, sizeof(void*) * 2);
                break;
            }
            default:
                format_putc(state, '%');
                format_putc(state, spec);
                break;
        }
    }
}

static auto level_enabled(log_level level) -> bool {
    switch (level) {
        case log_level::printk:  return true;
        case log_level::crash:   return true;
        case log_level::error:   return error_enabled();
        case log_level::warn:    return warn_enabled();
        case log_level::info:    return info_enabled();
        case log_level::debug:   return debug_enabled();
        case log_level::verbose: return verbose_enabled();
        default:                 return false;
    }
}

static auto level_prefix(log_level level) -> const char* {
    switch (level) {
        case log_level::crash:   return null;
        case log_level::error:   return "[ERROR] ";
        case log_level::warn:    return "[WARN] ";
        case log_level::info:    return "[INFO] ";
        case log_level::debug:   return "[DEBUG] ";
        case log_level::verbose: return "[VERBOSE] ";
        case log_level::printk:
        default:
            return null;
    }
}

static auto level_color(log_level level) -> console_color {
    switch (level) {
        case log_level::crash:   return console_color::white;
        case log_level::error:   return console_color::light_red;
        case log_level::warn:    return console_color::yellow;
        case log_level::info:    return console_color::light_green;
        case log_level::debug:   return console_color::blue;
        case log_level::verbose: return console_color::gray;
        case log_level::printk:
        default:
            return console_color::white;
    }
}

static void vlog(log_level level, bool append_newline, const char* format, vk_va_list args) {
    if (!level_enabled(level)) {
        return;
    }

    format_state state{};

    const char* prefix = level_prefix(level);
    if (level == log_level::crash) {
        /* Keep existing behaviour: entire crash message highlighted */
        console::set_color(console_color::white, console_color::red);
    } else if (prefix != null) {
        /* Print only the coloured prefix, then reset to white for the message */
        console::set_color(level_color(level), console_color::black);
        format_puts(state, prefix);
        console::set_color(console_color::white, console_color::black);
    }

    vformat_to_console(state, format, args);

    if (append_newline && state.last_char != '\n') {
        format_putc(state, '\n');
    }

    if (level != log_level::printk) {
        console::set_color(console_color::white, console_color::black);
    }
}

} // namespace

void printk(const char* format, ...) {
    vk_va_list args;
    __builtin_va_start(args, format);
    vlog(log_level::printk, false, format, args);
    __builtin_va_end(args);
}

void crash(const char* format, ...) {
    vk_va_list args;
    __builtin_va_start(args, format);
    vlog(log_level::crash, true, format, args);
    __builtin_va_end(args);
}

void error(const char* format, ...) {
    vk_va_list args;
    __builtin_va_start(args, format);
    vlog(log_level::error, true, format, args);
    __builtin_va_end(args);
}

void warn(const char* format, ...) {
    vk_va_list args;
    __builtin_va_start(args, format);
    vlog(log_level::warn, true, format, args);
    __builtin_va_end(args);
}

void info(const char* format, ...) {
    vk_va_list args;
    __builtin_va_start(args, format);
    vlog(log_level::info, true, format, args);
    __builtin_va_end(args);
}

void debug(const char* format, ...) {
    vk_va_list args;
    __builtin_va_start(args, format);
    vlog(log_level::debug, true, format, args);
    __builtin_va_end(args);
}

void verbose(const char* format, ...) {
    vk_va_list args;
    __builtin_va_start(args, format);
    vlog(log_level::verbose, true, format, args);
    __builtin_va_end(args);
}

} // namespace log

} // namespace vk
