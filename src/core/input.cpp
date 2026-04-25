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

/* Forward declaration: defined in the PS/2 mouse section below. */
static void ps2_pump_mouse_byte(u8 byte);

/*
 * Try to read one character from the PS/2 port.
 * Returns '\0' if nothing is available or the scancode is a modifier.
 *
 * Aux (mouse) bytes are consumed immediately and fed into ps2_pump_mouse_byte()
 * so the controller output buffer is never blocked by unread mouse data.
 */
static auto ps2_try_read() -> char {
    while (true) {
        /* Bit 0 of status: output buffer full */
        u8 status = arch::inb(PS2_STATUS);
        if (!(status & 0x01)) return '\0';

        u8 sc = arch::inb(PS2_DATA);

        /* Aux (mouse) byte: consume it, pump it, keep looking for keyboard. */
        if (status & 0x20) { ps2_pump_mouse_byte(sc); continue; }

        if (sc == 0xE0) { s_ext = true; continue; }   /* extended prefix */

        bool ext = s_ext;
        s_ext = false;

        if (ext) continue;   /* ignore extended make/break codes for now */

        /* Break code (key release) — update shift state, no character */
        if (sc & 0x80) {
            u8 make = sc & 0x7F;
            if (make == 0x2A || make == 0x36) s_shift = false;
            continue;
        }

        /* Make codes — modifiers */
        if (sc == 0x2A || sc == 0x36) { s_shift = true;  continue; }
        if (sc == 0x3A)               { s_caps = !s_caps; continue; }
        if (sc == 0x1D || sc == 0x38) continue;   /* ctrl / alt */

        if (sc >= 128) continue;

        char base_n = s_sc_normal[sc];
        char base_s = s_sc_shifted[sc];

        /* Letters: caps lock and shift XOR to determine case */
        bool letter = (base_n >= 'a' && base_n <= 'z');
        if (letter) {
            bool upper = s_caps ^ s_shift;
            return upper ? base_s : base_n;
        }

        char c = s_shift ? base_s : base_n;
        if (c != '\0') return c;
        /* Non-printable make code — keep draining */
    }
}

/* ============================================================
 * Raw PS/2 scancode reader — for poll_key()
 * Returns the full scancode with make/break info preserved.
 * ============================================================ */

static bool s_ctrl = false;
static bool s_alt  = false;

static auto ps2_try_read_raw(vk_key_event_t& ev) -> bool {
    while (true) {
    u8 status = arch::inb(PS2_STATUS);
    if (!(status & 0x01)) return false;

    u8 sc = arch::inb(PS2_DATA);

    /* Aux (mouse) byte: consume it, pump it, keep looking for keyboard. */
    if (status & 0x20) { ps2_pump_mouse_byte(sc); continue; }

    if (sc == 0xE0) { s_ext = true; continue; }

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
    } /* while */
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
 * PS/2 Mouse — i8042 auxiliary port
 *
 * Protocol: 3-byte packets
 *   Byte 0: [YO|XO|YS|XS|1|MB|RB|LB]  (bit 3 always 1)
 *   Byte 1: X movement (two's complement, sign in byte 0 bit 4)
 *   Byte 2: Y movement (two's complement, sign in byte 0 bit 5, +Y = up)
 * ============================================================ */

static constexpr u8  PS2_CMD_ENABLE_AUX    = 0xA8;
static constexpr u8  PS2_CMD_GET_CONFIG    = 0x20;
static constexpr u8  PS2_CMD_SET_CONFIG    = 0x60;
static constexpr u8  PS2_CMD_WRITE_AUX     = 0xD4;  /* next byte → aux */
static constexpr u8  PS2_MOUSE_ENABLE_RPT  = 0xF4;  /* Enable Data Reporting */
static constexpr u8  PS2_MOUSE_ACK         = 0xFA;

static bool s_mouse_ready = false;
static u8   s_mouse_buf[3];
static int  s_mouse_phase = 0;   /* which byte within a 3-byte packet */

/*
 * Ring buffer of fully-decoded mouse packets.
 * ps2_pump_mouse_byte() assembles bytes into packets and enqueues them.
 * Both the keyboard readers and poll_mouse() call pump, so the PS/2
 * output buffer is always drained immediately — keyboard bytes are never
 * blocked by pending mouse data.
 */
static constexpr int    MOUSE_QUEUE_SIZE = 8;
static vk_mouse_event_t s_mouse_queue[MOUSE_QUEUE_SIZE];
static int              s_mouse_q_head = 0;   /* next write slot */
static int              s_mouse_q_tail = 0;   /* next read slot  */

static bool mouse_q_full()  { return ((s_mouse_q_head + 1) % MOUSE_QUEUE_SIZE) == s_mouse_q_tail; }
static bool mouse_q_empty() { return s_mouse_q_head == s_mouse_q_tail; }

static void mouse_q_push(const vk_mouse_event_t& ev)
{
    if (mouse_q_full())
        s_mouse_q_tail = (s_mouse_q_tail + 1) % MOUSE_QUEUE_SIZE; /* drop oldest */
    s_mouse_queue[s_mouse_q_head] = ev;
    s_mouse_q_head = (s_mouse_q_head + 1) % MOUSE_QUEUE_SIZE;
}

static bool mouse_q_pop(vk_mouse_event_t& ev)
{
    if (mouse_q_empty()) return false;
    ev = s_mouse_queue[s_mouse_q_tail];
    s_mouse_q_tail = (s_mouse_q_tail + 1) % MOUSE_QUEUE_SIZE;
    return true;
}

/*
 * Feed one raw byte from the aux device into the 3-byte packet state machine.
 * Enqueues a decoded vk_mouse_event_t when a complete packet is ready.
 */
static void ps2_pump_mouse_byte(u8 byte)
{
    /* Re-sync: first byte of a packet always has bit 3 set. */
    if (s_mouse_phase == 0 && !(byte & 0x08u))
        return;   /* out-of-sync, discard */

    s_mouse_buf[s_mouse_phase++] = byte;
    if (s_mouse_phase < 3) return;   /* packet incomplete */

    s_mouse_phase = 0;

    u8 flags = s_mouse_buf[0];
    u8 raw_x = s_mouse_buf[1];
    u8 raw_y = s_mouse_buf[2];

    if (flags & 0xC0u) return;   /* overflow bits set — discard */

    i32 dx =  (i32)raw_x - (i32)((flags & 0x10u) ? 256 : 0);
    i32 dy = -(i32)raw_y + (i32)((flags & 0x20u) ? 256 : 0);  /* invert Y */

    vk_mouse_event_t ev;
    ev.dx      = dx;
    ev.dy      = dy;
    ev.buttons = flags & 0x07u;
    mouse_q_push(ev);
}

/* Wait until the controller input buffer is empty (ready for write). */
static void ps2_ctrl_wait_write() {
    for (int i = 0; i < 100000; ++i)
        if (!(arch::inb(PS2_STATUS) & 0x02)) return;
}

/* Wait until the controller output buffer has a byte ready. */
static void ps2_ctrl_wait_read() {
    for (int i = 0; i < 100000; ++i)
        if (arch::inb(PS2_STATUS) & 0x01) return;
}

/* Send a command byte to the i8042 controller command port. */
static void ps2_send_ctrl_cmd(u8 cmd) {
    ps2_ctrl_wait_write();
    arch::outb(PS2_STATUS, cmd);   /* 0x64 write = command */
}

/* Send a data byte to the i8042 data port (port 1 / keyboard). */
static void ps2_send_data(u8 data) {
    ps2_ctrl_wait_write();
    arch::outb(PS2_DATA, data);
}

/* Read one byte from the data port (with wait). */
static u8 ps2_read_data() {
    ps2_ctrl_wait_read();
    return arch::inb(PS2_DATA);
}

/* Route a byte to the aux (mouse) device: 0xD4 to cmd, then byte to data. */
static void ps2_send_mouse(u8 data) {
    ps2_send_ctrl_cmd(PS2_CMD_WRITE_AUX);
    ps2_send_data(data);
}

auto mouse_init() -> status_code {
    /* 1. Enable the aux device (PS/2 mouse port). */
    ps2_send_ctrl_cmd(PS2_CMD_ENABLE_AUX);

    /* 2. Read current controller configuration byte. */
    ps2_send_ctrl_cmd(PS2_CMD_GET_CONFIG);
    u8 cfg = ps2_read_data();

    /* 3. Enable aux interrupt (bit 1) and aux clock (bit 5 must be 0). */
    cfg |=  0x02u;   /* enable aux IRQ */
    cfg &= ~0x20u;   /* enable aux clock (0 = enabled) */
    ps2_send_ctrl_cmd(PS2_CMD_SET_CONFIG);
    ps2_send_data(cfg);

    /* 4. Enable mouse data reporting (mouse must ACK with 0xFA). */
    ps2_send_mouse(PS2_MOUSE_ENABLE_RPT);
    u8 ack = ps2_read_data();

    if (ack != PS2_MOUSE_ACK) {
        log::warn("input: PS/2 mouse init: unexpected ACK 0x%02x (expected 0xFA)",
                  static_cast<unsigned>(ack));
        /* Not a fatal error — mouse packets may still arrive. */
    }

    s_mouse_phase = 0;
    s_mouse_ready = true;
    log::debug("Input: PS/2 mouse ready");
    return status_code::success;
}

/*
 * poll_mouse: drain any remaining aux bytes from port 0x60 (in case
 * poll_key hasn't been called recently), then pop one packet from the
 * internal queue assembled by ps2_pump_mouse_byte().
 */
auto poll_mouse(vk_mouse_event_t& ev) -> bool {
    if (!s_mouse_ready) return false;

    /* Drain aux bytes that poll_key may not have consumed yet. */
    while (true) {
        u8 status = arch::inb(PS2_STATUS);
        if ((status & 0x21u) != 0x21u) break;   /* no aux byte available */
        ps2_pump_mouse_byte(arch::inb(PS2_DATA));
    }

    return mouse_q_pop(ev);
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

    /* Initialise the PS/2 mouse port */
    mouse_init();

    log::debug("Input subsystem ready (PS/2 keyboard + mouse + COM1)");
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
