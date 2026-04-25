/*
 * vkernel - UEFI Microkernel
 * Copyright (C) 2026 vkernel authors
 *
 * input.cpp - Kernel input subsystem implementation
 *
 * Polls two sources:
 *   1. PS/2 keyboard — 8042 controller (ports 0x60/0x64)
 *      Scan code set 1, with shift / caps-lock state tracking.
 *   2. COM1 serial  — 0x3F8
 *      Any byte arriving on the serial line is forwarded directly.
 *
 * input::getc() yields to the scheduler between polls so the idle
 * task can HLT and other tasks can make progress.
 */

#include "config.h"
#include "types.h"
#include "console.h"
#include "scheduler.h"
#include "input.h"
#include "arch/x86_64/arch.h"
#include "vk.h"

namespace vk {
namespace input {

/* ============================================================
 * PS/2 keyboard — 8042 controller
 * ============================================================ */

static constexpr u16 PS2_DATA   = 0x60;
static constexpr u16 PS2_STATUS = 0x64;

/* Scan code set 1 → ASCII (unshifted) — index is the make code */
static constexpr char s_sc_normal[128] = {
/*00*/  0,   '\x1B', '1', '2', '3', '4', '5', '6', '7', '8',
/*0A*/  '9', '0',    '-', '=', '\b', '\t',
/*10*/  'q', 'w', 'e', 'r', 't', 'y', 'u', 'i', 'o', 'p', '[', ']', '\r',
/*1D*/  0,   /* L-Ctrl */
/*1E*/  'a', 's', 'd', 'f', 'g', 'h', 'j', 'k', 'l', ';', '\'', '`',
/*2A*/  0,   /* L-Shift */
/*2B*/  '\\',
/*2C*/  'z', 'x', 'c', 'v', 'b', 'n', 'm', ',', '.', '/',
/*36*/  0,   /* R-Shift */
/*37*/  '*',
/*38*/  0,   /* L-Alt */
/*39*/  ' ',
/*3A*/  0,   /* Caps Lock */
/*3B-44*/ 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, /* F1-F10 */
/*45*/  0, 0, /* Num Lock, Scroll Lock */
/*47*/  0, 0, 0, /* numpad 7, 8, 9 */
/*4A*/  '-',
/*4B*/  0, 0, 0, /* numpad 4, 5, 6 */
/*4E*/  '+',
/*4F*/  0, 0, 0, /* numpad 1, 2, 3 */
/*52*/  0, /* numpad 0 */
/*53*/  0, /* numpad . */
/*54-57*/ 0, 0, 0, 0,
/*58-7F*/ 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
          0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
          0, 0, 0, 0, 0, 0, 0,
};

/* Scan code set 1 → ASCII (shifted) */
static constexpr char s_sc_shifted[128] = {
/*00*/  0,   '\x1B', '!', '@', '#', '$', '%', '^', '&', '*',
/*0A*/  '(', ')',    '_', '+', '\b', '\t',
/*10*/  'Q', 'W', 'E', 'R', 'T', 'Y', 'U', 'I', 'O', 'P', '{', '}', '\r',
/*1D*/  0,
/*1E*/  'A', 'S', 'D', 'F', 'G', 'H', 'J', 'K', 'L', ':', '"', '~',
/*2A*/  0,
/*2B*/  '|',
/*2C*/  'Z', 'X', 'C', 'V', 'B', 'N', 'M', '<', '>', '?',
/*36*/  0,
/*37*/  '*',
/*38*/  0,
/*39*/  ' ',
/*3A*/  0,
/*3B-44*/ 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
/*45*/  0, 0,
/*47*/  0, 0, 0,
/*4A*/  '-',
/*4B*/  0, 0, 0,
/*4E*/  '+',
/*4F*/  0, 0, 0,
/*52*/  0,
/*53*/  0,
/*54-57*/ 0, 0, 0, 0,
/*58-7F*/ 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
          0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
          0, 0, 0, 0, 0, 0, 0,
};

static bool s_shift = false;
static bool s_caps  = false;
static bool s_ext   = false;   /* consumed 0xE0 prefix */

/*
 * Try to read one character from the PS/2 port.
 * Returns '\0' if nothing is available or the scancode is a modifier.
 */
static auto ps2_try_read() -> char {
    /* Bit 0 of status: output buffer full */
    if (!(arch::inb(PS2_STATUS) & 0x01)) return '\0';

    u8 sc = arch::inb(PS2_DATA);

    if (sc == 0xE0) { s_ext = true; return '\0'; }   /* extended prefix */

    bool ext = s_ext;
    s_ext = false;

    if (ext) return '\0';   /* ignore extended make/break codes for now */

    /* Break code (key release) — update shift state, no character */
    if (sc & 0x80) {
        u8 make = sc & 0x7F;
        if (make == 0x2A || make == 0x36) s_shift = false;
        return '\0';
    }

    /* Make codes — modifiers */
    if (sc == 0x2A || sc == 0x36) { s_shift = true;  return '\0'; }
    if (sc == 0x3A)               { s_caps = !s_caps; return '\0'; }
    if (sc == 0x1D || sc == 0x38) return '\0';   /* ctrl / alt */

    if (sc >= 128) return '\0';

    char base_n = s_sc_normal[sc];
    char base_s = s_sc_shifted[sc];

    /* Letters: caps lock and shift XOR to determine case */
    bool letter = (base_n >= 'a' && base_n <= 'z');
    if (letter) {
        bool upper = s_caps ^ s_shift;
        return upper ? base_s : base_n;
    }

    return s_shift ? base_s : base_n;
}

/* ============================================================
 * Raw PS/2 scancode reader — for poll_key()
 * Returns the full scancode with make/break info preserved.
 * ============================================================ */

static bool s_ctrl = false;
static bool s_alt  = false;

static auto ps2_try_read_raw(vk_key_event_t& ev) -> bool {
    if (!(arch::inb(PS2_STATUS) & 0x01)) return false;

    u8 sc = arch::inb(PS2_DATA);

    if (sc == 0xE0) { s_ext = true; return false; }

    bool ext = s_ext;
    s_ext = false;

    bool released = (sc & 0x80) != 0;
    u8 make = sc & 0x7F;

    /* Track modifier state */
    if (make == 0x2A || make == 0x36) s_shift = released ? false : true;
    if (make == 0x1D)                 s_ctrl  = released ? false : true;
    if (make == 0x38)                 s_alt   = released ? false : true;
    if (make == 0x3A && !released)    s_caps  = !s_caps;

    /* For extended keys, offset scancode by 0x80 to distinguish */
    u32 code = ext ? (make | 0x80u) : make;

    ev.scancode  = code;
    ev.pressed   = released ? 0u : 1u;
    ev.modifiers = (s_shift ? 1u : 0u)
                 | (s_ctrl  ? 2u : 0u)
                 | (s_alt   ? 4u : 0u);

    /* ASCII translation for printable make codes (non-extended only) */
    ev.ascii = '\0';
    if (!ext && !released && make < 128) {
        char c = s_shift ? s_sc_shifted[make] : s_sc_normal[make];
        bool letter = (s_sc_normal[make] >= 'a' && s_sc_normal[make] <= 'z');
        if (letter) {
            bool upper = s_caps ^ s_shift;
            c = upper ? s_sc_shifted[make] : s_sc_normal[make];
        }
        ev.ascii = c;
    }
    ev._pad[0] = ev._pad[1] = ev._pad[2] = 0;

    return true;
}

/* ============================================================
 * COM1 serial input
 * ============================================================ */

static constexpr u16 COM1      = 0x3F8;
static constexpr u16 COM1_LSR  = COM1 + 5;   /* Line Status Register */

static auto serial_try_read() -> char {
    if (arch::inb(COM1_LSR) & 0x01)
        return static_cast<char>(arch::inb(COM1));
    return '\0';
}

/* ============================================================
 * Public API
 * ============================================================ */

auto init() -> status_code {
    /* Flush any stale PS/2 bytes left by the UEFI firmware */
    while (arch::inb(PS2_STATUS) & 0x01)
        (void)arch::inb(PS2_DATA);

    /* Reset modifier state */
    s_shift = false;
    s_caps  = false;
    s_ext   = false;

    log::debug("Input subsystem ready (PS/2 + COM1)");
    return status_code::success;
}

auto try_getc() -> char {
    char c = ps2_try_read();
    if (c != '\0') return c;
    return serial_try_read();
}

auto getc() -> char {
    while (true) {
        char c = try_getc();
        if (c != '\0') return c;
        sched::yield();
    }
}

auto poll_key(vk_key_event_t& ev) -> bool {
    return ps2_try_read_raw(ev);
}

auto try_getc_ps2() -> char {
    return ps2_try_read();
}

auto getc_ps2() -> char {
    while (true) {
        char c = try_getc_ps2();
        if (c != '\0') return c;
        sched::yield();
    }
}

auto try_getc_serial() -> char {
    return serial_try_read();
}

auto getc_serial() -> char {
    while (true) {
        char c = try_getc_serial();
        if (c != '\0') return c;
        sched::yield();
    }
}

} // namespace input
} // namespace vk
