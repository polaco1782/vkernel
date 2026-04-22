/*
 * vkernel userspace - framebuffer text demo
 * Copyright (C) 2026 vkernel authors
 *
 * framebuffer_text.c - Freestanding framebuffer text rendering example
 *
 * Build: see Makefile (Linux) or framebuffer_text.vcxproj (Visual Studio).
 * Run:   vk> run framebuffer_text.elf
 */

#include "../include/vk.h"

static vk_u32 pack_pixel(unsigned char r, unsigned char g, unsigned char b, vk_pixel_format_t format) {
    switch (format) {
        case VK_PIXEL_FORMAT_BGRX_8BPP:
            return ((vk_u32)b << 16) | ((vk_u32)g << 8) | (vk_u32)r;
        case VK_PIXEL_FORMAT_RGBX_8BPP:
        case VK_PIXEL_FORMAT_BITMASK:
        case VK_PIXEL_FORMAT_BLT_ONLY:
        default:
            return ((vk_u32)r << 16) | ((vk_u32)g << 8) | (vk_u32)b;
    }
}

typedef struct glyph_entry {
    char ch;
    unsigned char rows[8];
} glyph_entry_t;

static const glyph_entry_t k_font[] = {
    { ' ', { 0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00 } },
    { 'A', { 0x18,0x24,0x42,0x7E,0x42,0x42,0x42,0x00 } },
    { 'B', { 0x7C,0x42,0x42,0x7C,0x42,0x42,0x7C,0x00 } },
    { 'C', { 0x3C,0x42,0x40,0x40,0x40,0x42,0x3C,0x00 } },
    { 'D', { 0x78,0x44,0x42,0x42,0x42,0x44,0x78,0x00 } },
    { 'E', { 0x7E,0x40,0x40,0x7C,0x40,0x40,0x7E,0x00 } },
    { 'F', { 0x7E,0x40,0x40,0x7C,0x40,0x40,0x40,0x00 } },
    { 'K', { 0x42,0x44,0x48,0x70,0x48,0x44,0x42,0x00 } },
    { 'L', { 0x40,0x40,0x40,0x40,0x40,0x40,0x7E,0x00 } },
    { 'M', { 0x42,0x66,0x5A,0x5A,0x42,0x42,0x42,0x00 } },
    { 'N', { 0x42,0x62,0x52,0x4A,0x46,0x42,0x42,0x00 } },
    { 'O', { 0x3C,0x42,0x42,0x42,0x42,0x42,0x3C,0x00 } },
    { 'P', { 0x7C,0x42,0x42,0x7C,0x40,0x40,0x40,0x00 } },
    { 'R', { 0x7C,0x42,0x42,0x7C,0x48,0x44,0x42,0x00 } },
    { 'S', { 0x3C,0x42,0x40,0x3C,0x02,0x42,0x3C,0x00 } },
    { 'T', { 0x7E,0x18,0x18,0x18,0x18,0x18,0x18,0x00 } },
    { 'U', { 0x42,0x42,0x42,0x42,0x42,0x42,0x3C,0x00 } },
    { 'V', { 0x42,0x42,0x42,0x42,0x24,0x24,0x18,0x00 } },
    { 'X', { 0x42,0x24,0x18,0x18,0x18,0x24,0x42,0x00 } },
    { '0', { 0x3C,0x42,0x46,0x4A,0x52,0x62,0x3C,0x00 } },
    { '1', { 0x18,0x38,0x18,0x18,0x18,0x18,0x3C,0x00 } },
    { '2', { 0x3C,0x42,0x02,0x0C,0x30,0x40,0x7E,0x00 } },
    { '3', { 0x7E,0x04,0x08,0x1C,0x02,0x42,0x3C,0x00 } },
    { '4', { 0x0C,0x14,0x24,0x44,0x7E,0x04,0x04,0x00 } },
    { '5', { 0x7E,0x40,0x7C,0x02,0x02,0x42,0x3C,0x00 } },
    { '6', { 0x3C,0x40,0x7C,0x42,0x42,0x42,0x3C,0x00 } },
    { '7', { 0x7E,0x02,0x04,0x08,0x10,0x10,0x10,0x00 } },
    { '8', { 0x3C,0x42,0x42,0x3C,0x42,0x42,0x3C,0x00 } },
    { '9', { 0x3C,0x42,0x42,0x3E,0x02,0x02,0x3C,0x00 } },
    { ':', { 0x00,0x18,0x18,0x00,0x18,0x18,0x00,0x00 } },
    { '(', { 0x0C,0x10,0x20,0x20,0x20,0x10,0x0C,0x00 } },
    { ')', { 0x30,0x08,0x04,0x04,0x04,0x08,0x30,0x00 } },
    { '-', { 0x00,0x00,0x00,0x7E,0x00,0x00,0x00,0x00 } },
    { '/', { 0x02,0x04,0x08,0x10,0x20,0x40,0x00,0x00 } },
};

static const unsigned int k_font_count = sizeof(k_font) / sizeof(k_font[0]);

static const unsigned char* glyph_for(char ch) {
    for (unsigned int i = 0; i < k_font_count; ++i) {
        if (k_font[i].ch == ch) {
            return k_font[i].rows;
        }
    }
    return k_font[0].rows;
}

static void put_pixel(vk_framebuffer_info_t* fb, vk_u32 x, vk_u32 y, vk_u32 color) {
    if (x >= fb->width || y >= fb->height) {
        return;
    }
    vk_u32* pixels = (vk_u32*)(unsigned long long)fb->base;
    pixels[(vk_usize)y * fb->stride + x] = color;
}

static void fill_rect(vk_framebuffer_info_t* fb, vk_u32 x, vk_u32 y, vk_u32 w, vk_u32 h, vk_u32 color) {
    for (vk_u32 row = 0; row < h; ++row) {
        for (vk_u32 col = 0; col < w; ++col) {
            put_pixel(fb, x + col, y + row, color);
        }
    }
}

static void draw_char(vk_framebuffer_info_t* fb, vk_u32 x, vk_u32 y, char ch, vk_u32 fg, vk_u32 bg) {
    const unsigned char* glyph = glyph_for(ch);
    for (vk_u32 row = 0; row < 8; ++row) {
        unsigned char bits = glyph[row];
        for (vk_u32 col = 0; col < 8; ++col) {
            vk_u32 color = (bits & (0x80u >> col)) ? fg : bg;
            put_pixel(fb, x + col, y + row, color);
        }
    }
}

static void draw_text(vk_framebuffer_info_t* fb, vk_u32 x, vk_u32 y, const char* text, vk_u32 fg, vk_u32 bg) {
    vk_u32 cursor_x = x;
    for (const char* p = text; *p != '\0'; ++p) {
        draw_char(fb, cursor_x, y, *p, fg, bg);
        cursor_x += 8;
    }
}

static void draw_text_centered(vk_framebuffer_info_t* fb, vk_u32 y, const char* text, vk_u32 fg, vk_u32 bg) {
    vk_usize len = 0;
    while (text[len]) ++len;
    vk_u32 width = (vk_u32)len * 8u;
    vk_u32 x = fb->width > width ? (fb->width - width) / 2u : 0u;
    draw_text(fb, x, y, text, fg, bg);
}

static void paint_background(vk_framebuffer_info_t* fb) {
    vk_u32* pixels = (vk_u32*)(unsigned long long)fb->base;

    for (vk_u32 y = 0; y < fb->height; ++y) {
        for (vk_u32 x = 0; x < fb->width; ++x) {
            vk_u32 r = (x * 32u) / (fb->width  ? fb->width  : 1u) + 8u;
            vk_u32 g = (y * 48u) / (fb->height ? fb->height : 1u) + 12u;
            vk_u32 b = 36u + ((x ^ y) & 0x1Fu);
            pixels[(vk_usize)y * fb->stride + x] = pack_pixel((unsigned char)r, (unsigned char)g, (unsigned char)b, fb->format);
        }
    }
}

int _start(const vk_api_t* api) {
    vk_init(api);

    VK_CALL(puts, "Framebuffer text demo\n");

    vk_framebuffer_info_t fb = { 0 };
    VK_CALL(framebuffer_info, &fb);

    if (!fb.valid || fb.base == 0 || fb.width == 0 || fb.height == 0) {
        VK_CALL(puts, "  No framebuffer available.\n");
        return 1;
    }

    vk_u32 bg = pack_pixel(12, 18, 36, fb.format);
    vk_u32 panel = pack_pixel(28, 40, 70, fb.format);
    vk_u32 accent = pack_pixel(96, 196, 255, fb.format);
    vk_u32 accent2 = pack_pixel(255, 210, 96, fb.format);
    vk_u32 text_col = pack_pixel(245, 245, 245, fb.format);

    paint_background(&fb);

    vk_u32 panel_w = fb.width > 48u ? fb.width - 48u : fb.width;
    vk_u32 panel_h = fb.height > 72u ? fb.height - 72u : fb.height;
    vk_u32 panel_x = (fb.width - panel_w) / 2u;
    vk_u32 panel_y = (fb.height - panel_h) / 2u;
    fill_rect(&fb, panel_x, panel_y, panel_w, panel_h, panel);

    for (vk_u32 y = panel_y; y < panel_y + panel_h; ++y) {
        put_pixel(&fb, panel_x, y, accent);
        put_pixel(&fb, panel_x + panel_w - 1u, y, accent);
    }
    for (vk_u32 x = panel_x; x < panel_x + panel_w; ++x) {
        put_pixel(&fb, x, panel_y, accent);
        put_pixel(&fb, x, panel_y + panel_h - 1u, accent);
    }

    fill_rect(&fb, panel_x + 16u, panel_y + 16u, panel_w - 32u, 18u, bg);
    fill_rect(&fb, panel_x + 16u, panel_y + panel_h - 40u, panel_w - 32u, 12u, bg);

    draw_text_centered(&fb, panel_y + 24u, "FRAMEBUFFER TEXT DEMO", text_col, bg);
    draw_text_centered(&fb, panel_y + 54u, "VKERNEL USERSPACE", accent2, bg);
    draw_text_centered(&fb, panel_y + panel_h - 32u, "RESOLUTION:", accent, bg);

    /* Build "WIDTHxHEIGHT" string manually (no snprintf in freestanding env) */
    char size_line[32];
    {
        int pos = 0;
        vk_u32 v = fb.width;
        char tmp[12]; int tlen = 0;
        if (v == 0) { tmp[tlen++] = '0'; } else { while (v) { tmp[tlen++] = '0' + (char)(v % 10u); v /= 10u; } }
        for (int i = tlen - 1; i >= 0; --i) size_line[pos++] = tmp[i];
        size_line[pos++] = 'X';
        v = fb.height; tlen = 0;
        if (v == 0) { tmp[tlen++] = '0'; } else { while (v) { tmp[tlen++] = '0' + (char)(v % 10u); v /= 10u; } }
        for (int i = tlen - 1; i >= 0; --i) size_line[pos++] = tmp[i];
        size_line[pos] = '\0';
    }

    draw_text(&fb, panel_x + 16u, panel_y + panel_h - 32u, size_line, text_col, bg);

    VK_CALL(sleep, 500); /* Sleep for 500 ticks (5 seconds) */

    VK_CALL(puts, "  Framebuffer        : ");
    VK_CALL(put_dec, fb.width); VK_CALL(puts, "x"); VK_CALL(put_dec, fb.height); VK_CALL(puts, "\n");
    VK_CALL(puts, "  Stride             : "); VK_CALL(put_dec, fb.stride); VK_CALL(puts, "\n");
    VK_CALL(puts, "  Format             : "); VK_CALL(put_dec, (vk_u64)fb.format); VK_CALL(puts, "\n");
    VK_CALL(puts, "  Base               : 0x"); VK_CALL(put_hex, fb.base); VK_CALL(puts, "\n");
    VK_CALL(puts, "  Text rendered.\n");

    return 0;
}
