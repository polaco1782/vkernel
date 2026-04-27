/*
 * visual_player.cpp
 * DOS-style "Visual Player v2.0" UI for vkernel framebuffer.
 *
 * Build constraints:
 *   - newlib C functions only  (string.h, stdlib.h, …)
 *   - No C++ STL whatsoever
 *   - Drawing exclusively through the vk_api framebuffer
 *
 * Architecture:
 *   vp::Color          – immutable RGB value type with format conversion
 *   vp::Rect           – value type for axis-aligned rectangles
 *   vp::Painter        – stateful drawing context wrapping the framebuffer
 *   vp::Font           – 5×7 bitmap font, renders glyphs through a Painter
 *   vp::Palette        – compile-time color constants (all constexpr)
 *   vp::widgets::*     – self-contained UI components (Scope, VuMeter, …)
 *   vp::Layout         – computes panel geometry from framebuffer dimensions
 *   vp::VisualPlayer   – top-level orchestrator
 */

#include "../include/vk.h"
#include <string.h>
#include <stdio.h>
#include <stdint.h>

// ============================================================
// Namespace vp  –  Visual Player
// ============================================================
namespace vp {

// ------------------------------------------------------------
// Color  –  0x00RRGGBB immutable value type
// ------------------------------------------------------------
class Color {
public:
    constexpr Color() : m_rgb(0) {}
    constexpr explicit Color(unsigned int rgb) : m_rgb(rgb & 0x00FFFFFFu) {}
    constexpr Color(unsigned int r, unsigned int g, unsigned int b)
        : m_rgb(((r & 0xFFu) << 16) | ((g & 0xFFu) << 8) | (b & 0xFFu)) {}

    constexpr unsigned int r()   const { return (m_rgb >> 16) & 0xFFu; }
    constexpr unsigned int g()   const { return (m_rgb >>  8) & 0xFFu; }
    constexpr unsigned int b()   const { return  m_rgb        & 0xFFu; }
    constexpr unsigned int rgb() const { return m_rgb; }

    /* Return a brightened copy (each channel clamped at 255) */
    constexpr Color lighten(unsigned int delta) const {
        return Color(
            r() + delta > 255u ? 255u : r() + delta,
            g() + delta > 255u ? 255u : g() + delta,
            b() + delta > 255u ? 255u : b() + delta
        );
    }

    /* Convert to the native 32-bit pixel word for a given framebuffer format */
    unsigned int to_pixel(vk_pixel_format_t fmt) const {
        return (r() << 16) | (g() << 8) | b(); // RGBX and default
    }

    constexpr bool operator==(const Color& o) const { return m_rgb == o.m_rgb; }
    constexpr bool operator!=(const Color& o) const { return m_rgb != o.m_rgb; }

private:
    unsigned int m_rgb;
};

// ------------------------------------------------------------
// Palette  –  compile-time named color constants
// ------------------------------------------------------------
namespace Palette {
    constexpr Color BG_PANEL   { 0xBA8E7Du }; // warm tan/beige – main chrome
    constexpr Color BG_DARK    { 0x000000u }; // scope / waveform background
    constexpr Color BORDER_LT  { 0xD0A898u }; // bevel highlight (top-left)
    constexpr Color BORDER_DK  { 0x604030u }; // bevel shadow  (bottom-right)
    constexpr Color GREEN_LINE { 0x00FF88u }; // idle flat waveform
    constexpr Color RED_LED    { 0xCC2020u }; // mute indicator dot
    constexpr Color LED_SHADOW { 0x882020u }; // LED bevel dark
    constexpr Color LED_HILITE { 0xFF4444u }; // LED bevel light
    constexpr Color INFO_BG    { 0x000000u }; // info panel background
    constexpr Color INFO_CYAN  { 0x00CCCCu }; // info label text
    constexpr Color INFO_WHITE { 0xDDDDDDu }; // info value text
    constexpr Color BTN_FACE   { 0xA07060u }; // push-button face
    constexpr Color BTN_TEXT   { 0xEEEEEEu }; // push-button label
    constexpr Color TITLE_TEXT { 0xC09080u }; // etched title
    constexpr Color VU_RED     { 0xAA0000u }; // VU meter top half
    constexpr Color VU_BLUE    { 0x2244AAu }; // VU meter bottom half
    constexpr Color LIGHT_RED  { 0xAA2222u }; // traffic light – red (dim)
    constexpr Color LIGHT_GDM  { 0x2A4A22u }; // traffic light – green dim
    constexpr Color LIGHT_GBT  { 0x44AA33u }; // traffic light – green bright
    constexpr Color SEQ_RED    { 0x8B0000u }; // sequencer top row
    constexpr Color SEQ_BLUE   { 0x00008Bu }; // sequencer bottom row
} // namespace Palette

// ------------------------------------------------------------
// Rect  –  axis-aligned rectangle value type
// ------------------------------------------------------------
struct Rect {
    int x, y, w, h;

    constexpr Rect() : x(0), y(0), w(0), h(0) {}
    constexpr Rect(int x, int y, int w, int h) : x(x), y(y), w(w), h(h) {}

    constexpr int right()  const { return x + w; }
    constexpr int bottom() const { return y + h; }
    constexpr int cx()     const { return x + w / 2; }
    constexpr int cy()     const { return y + h / 2; }

    constexpr Rect inset(int d)               const { return { x+d,  y+d,  w-2*d, h-2*d }; }
    constexpr Rect translated(int dx, int dy) const { return { x+dx, y+dy, w,     h     }; }

    constexpr bool contains(int px, int py) const {
        return px >= x && px < right() && py >= y && py < bottom();
    }
};

// ------------------------------------------------------------
// Painter  –  stateful drawing context bound to one framebuffer
// ------------------------------------------------------------
class Painter {
public:
    enum class BevelStyle { Raised, Inset };

    Painter(unsigned int* pixels, int width, int height,
            int stride, vk_pixel_format_t fmt)
        : m_pixels(pixels), m_width(width), m_height(height),
          m_stride(stride), m_fmt(fmt) {}

    static Painter from_info(const vk_framebuffer_info_t& info) {
        return Painter(
            reinterpret_cast<unsigned int*>(static_cast<vk_usize>(info.base)),
            static_cast<int>(info.width),
            static_cast<int>(info.height),
            static_cast<int>(info.stride),
            info.format
        );
    }

    int width()  const { return m_width;  }
    int height() const { return m_height; }

    // ---- primitives ----

    void put_pixel(int x, int y, Color c) {
        if (x < 0 || y < 0 || x >= m_width || y >= m_height) return;
        m_pixels[y * m_stride + x] = c.to_pixel(m_fmt);
    }

    void fill_rect(int x, int y, int w, int h, Color c) {
        unsigned int px = c.to_pixel(m_fmt);
        for (int row = 0; row < h; ++row) {
            int py = y + row;
            if (py < 0 || py >= m_height) continue;
            unsigned int* line = m_pixels + py * m_stride;
            for (int col = 0; col < w; ++col) {
                int qx = x + col;
                if (qx >= 0 && qx < m_width) line[qx] = px;
            }
        }
    }

    void fill_rect(const Rect& r, Color c) { fill_rect(r.x, r.y, r.w, r.h, c); }

    void draw_hline(int x, int y, int w, Color c) { fill_rect(x, y, w, 1, c); }
    void draw_vline(int x, int y, int h, Color c) { fill_rect(x, y, 1, h, c); }

    /* Midpoint circle – filled disk */
    void fill_circle(int cx, int cy, int r, Color c) {
        for (int dy = -r; dy <= r; ++dy)
            for (int dx = -r; dx <= r; ++dx)
                if (dx*dx + dy*dy <= r*r)
                    put_pixel(cx + dx, cy + dy, c);
    }

    /* Bevel around a rectangle with thickness >= 6 pixels */
    void draw_bevel(const Rect& r, BevelStyle style,
                    Color lt = Palette::BORDER_LT,
                    Color dk = Palette::BORDER_DK,
                    const int thickness = 5)
    {
        if (style == BevelStyle::Inset) { Color tmp = lt; lt = dk; dk = tmp; }

        int x0 = r.x;
        int y0 = r.y;
        int w0 = r.w;
        int h0 = r.h;

        for (int i = 0; i < thickness; ++i) {
            int xi = x0 + i;
            int yi = y0 + i;
            int wi = w0 - i * 2;
            int hi = h0 - i * 2;
            if (wi <= 0 || hi <= 0) break;

            draw_hline(xi, yi, wi, lt);                       // top
            draw_vline(xi, yi, hi, lt);                       // left
            draw_hline(xi, y0 + h0 - 1 - i, wi, dk);          // bottom
            draw_vline(x0 + w0 - 1 - i, yi, hi, dk);          // right
        }
    }

private:
    unsigned int*     m_pixels;
    int               m_width;
    int               m_height;
    int               m_stride;
    vk_pixel_format_t m_fmt;
};

// ------------------------------------------------------------
// Font  –  5×7 bitmap font (0-9, A-Z, space / : .)
// ------------------------------------------------------------
class Font {
public:
    static constexpr int GLYPH_W = 5;
    static constexpr int GLYPH_H = 7;

    static constexpr int char_advance(int scale = 1) {
        return (GLYPH_W + 1) * scale;
    }

    static void draw_char(Painter& p, int x, int y, char c,
                          Color col, int scale = 1)
    {
        const unsigned char* glyph = glyph_for(c);
        for (int row = 0; row < GLYPH_H; ++row)
            for (int col_idx = 0; col_idx < GLYPH_W; ++col_idx)
                if (glyph[row] & (0x10u >> col_idx))  // bit4=leftmost, bit0=rightmost
                    p.fill_rect(x + col_idx * scale, y + row * scale,
                                scale, scale, col);
    }

    /* Returns x position after last glyph */
    static int draw_string(Painter& p, int x, int y, const char* s,
                           Color col, int scale = 1)
    {
        int cx = x;
        while (*s) {
            draw_char(p, cx, y, *s, col, scale);
            cx += char_advance(scale);
            ++s;
        }
        return cx;
    }

    /* Horizontally centred inside rect */
    static void draw_centered(Painter& p, const Rect& rect, int y,
                              const char* s, Color col, int scale = 1)
    {
        int len = static_cast<int>(strlen(s));
        int tw  = len * char_advance(scale);
        draw_string(p, rect.x + (rect.w - tw) / 2, y, s, col, scale);
    }

private:
    static const unsigned char* glyph_for(char c) {
        int idx = 0;
        if      (c == '/')              idx = 1;
        else if (c == ':')              idx = 2;
        else if (c == '.')              idx = 3;
        else if (c >= '0' && c <= '9') idx = 4  + (c - '0');
        else if (c >= 'A' && c <= 'Z') idx = 14 + (c - 'A');
        else if (c >= 'a' && c <= 'z') idx = 14 + (c - 'a');
        return k_glyphs[idx];
    }

    /* Column-bits, 7 rows, LSB = leftmost pixel of each row */
    static const unsigned char k_glyphs[][GLYPH_H];
};

const unsigned char Font::k_glyphs[][Font::GLYPH_H] = {
    // Encoding: bit4=leftmost pixel, bit0=rightmost pixel (MSB-first within 5 columns)
    /*  0 space */  {0x00,0x00,0x00,0x00,0x00,0x00,0x00},
    /*  1 /     */  {0x10,0x08,0x04,0x02,0x01,0x00,0x00},
    /*  2 :     */  {0x00,0x0C,0x0C,0x00,0x0C,0x0C,0x00},
    /*  3 .     */  {0x00,0x00,0x00,0x00,0x00,0x0C,0x00},
    /*  4 0     */  {0x0E,0x11,0x19,0x15,0x13,0x11,0x0E},
    /*  5 1     */  {0x04,0x06,0x04,0x04,0x04,0x04,0x0E},
    /*  6 2     */  {0x0E,0x11,0x10,0x0C,0x02,0x01,0x1F},
    /*  7 3     */  {0x1F,0x08,0x04,0x0C,0x10,0x11,0x0E},
    /*  8 4     */  {0x08,0x0C,0x0A,0x09,0x1F,0x08,0x08},
    /*  9 5     */  {0x1F,0x01,0x0F,0x10,0x10,0x11,0x0E},
    /* 10 6     */  {0x0C,0x02,0x01,0x0F,0x11,0x11,0x0E},
    /* 11 7     */  {0x1F,0x10,0x08,0x04,0x02,0x02,0x02},
    /* 12 8     */  {0x0E,0x11,0x11,0x0E,0x11,0x11,0x0E},
    /* 13 9     */  {0x0E,0x11,0x11,0x1E,0x10,0x08,0x06},
    /* 14 A     */  {0x04,0x0A,0x11,0x11,0x1F,0x11,0x11},
    /* 15 B     */  {0x0F,0x11,0x11,0x0F,0x11,0x11,0x0F},
    /* 16 C     */  {0x0E,0x11,0x01,0x01,0x01,0x11,0x0E},
    /* 17 D     */  {0x0F,0x11,0x11,0x11,0x11,0x11,0x0F},
    /* 18 E     */  {0x1F,0x01,0x01,0x0F,0x01,0x01,0x1F},
    /* 19 F     */  {0x1F,0x01,0x01,0x0F,0x01,0x01,0x01},
    /* 20 G     */  {0x0E,0x11,0x01,0x1D,0x11,0x11,0x1E},
    /* 21 H     */  {0x11,0x11,0x11,0x1F,0x11,0x11,0x11},
    /* 22 I     */  {0x0E,0x04,0x04,0x04,0x04,0x04,0x0E},
    /* 23 J     */  {0x1C,0x08,0x08,0x08,0x08,0x09,0x06},
    /* 24 K     */  {0x11,0x09,0x05,0x03,0x05,0x09,0x11},
    /* 25 L     */  {0x01,0x01,0x01,0x01,0x01,0x01,0x1F},
    /* 26 M     */  {0x11,0x1B,0x15,0x11,0x11,0x11,0x11},
    /* 27 N     */  {0x11,0x13,0x15,0x19,0x11,0x11,0x11},
    /* 28 O     */  {0x0E,0x11,0x11,0x11,0x11,0x11,0x0E},
    /* 29 P     */  {0x0F,0x11,0x11,0x0F,0x01,0x01,0x01},
    /* 30 Q     */  {0x0E,0x11,0x11,0x11,0x15,0x09,0x16},
    /* 31 R     */  {0x0F,0x11,0x11,0x0F,0x05,0x09,0x11},
    /* 32 S     */  {0x0E,0x11,0x01,0x0E,0x10,0x11,0x0E},
    /* 33 T     */  {0x1F,0x04,0x04,0x04,0x04,0x04,0x04},
    /* 34 U     */  {0x11,0x11,0x11,0x11,0x11,0x11,0x0E},
    /* 35 V     */  {0x11,0x11,0x11,0x11,0x0A,0x0A,0x04},
    /* 36 W     */  {0x11,0x11,0x11,0x15,0x15,0x1B,0x11},
    /* 37 X     */  {0x11,0x11,0x0A,0x04,0x0A,0x11,0x11},
    /* 38 Y     */  {0x11,0x11,0x0A,0x04,0x04,0x04,0x04},
    /* 39 Z     */  {0x1F,0x10,0x08,0x04,0x02,0x01,0x1F},
};

// ============================================================
// Namespace vp::widgets  –  self-contained UI components
// ============================================================
namespace widgets {

// ------------------------------------------------------------
// Scope  –  black oscilloscope panel with idle flat-line
// ------------------------------------------------------------
class Scope {
public:
    void draw(Painter& p, const Rect& r) const {
        p.fill_rect(r, Palette::BG_DARK);
        p.draw_bevel(r, Painter::BevelStyle::Inset);
        Rect inner = r.inset(2);
        p.draw_hline(inner.x, inner.cy(), inner.w, Palette::GREEN_LINE);
    }
};

// ------------------------------------------------------------
// VoiceLabel  –  labelled strip with red LED mute indicator
// ------------------------------------------------------------
class VoiceLabel {
public:
    explicit VoiceLabel(const char* text) : m_text(text) {}

    void draw(Painter& p, const Rect& r) const {
        p.fill_rect(r, Palette::BTN_FACE);
        p.draw_bevel(r, Painter::BevelStyle::Raised, Palette::BORDER_LT, Palette::BORDER_DK, 1);

        // LED dot
        Rect led { r.x + 6, r.y + (r.h - 6) / 2, 6, 6 };
        p.fill_rect(led, Palette::RED_LED);
        p.draw_bevel(led, Painter::BevelStyle::Raised,
                     Palette::LED_SHADOW, Palette::LED_HILITE, 1);

        // Label right of LED
        Rect text_area { led.right() + 4, r.y, r.right() - led.right() - 4, r.h };
        int ty = r.y + (r.h - Font::GLYPH_H) / 2;
        Font::draw_centered(p, text_area, ty, m_text, Palette::BTN_TEXT);
    }

private:
    const char* m_text;
};

// ------------------------------------------------------------
// VuMeter  –  two-bar level display (red top, blue bottom)
// ------------------------------------------------------------
class VuMeter {
public:
    void draw(Painter& p, const Rect& r) const {
        p.fill_rect(r, Palette::BG_PANEL);
        p.draw_bevel(r, Painter::BevelStyle::Raised);

        static constexpr int k_pad = 4;
        static constexpr int k_gap = 3;
        int bw = (r.w - k_pad * 2 - k_gap) / 2;
        if (bw < 1) bw = 1;

        Rect bar1 { r.x + k_pad,               r.y + k_pad, bw, r.h - k_pad * 2 };
        Rect bar2 { r.x + k_pad + bw + k_gap,  r.y + k_pad, bw, r.h - k_pad * 2 };
        draw_bar(p, bar1);
        draw_bar(p, bar2);
    }

private:
    static void draw_bar(Painter& p, const Rect& r) {
        p.fill_rect(r, Palette::BG_DARK);
        p.draw_bevel(r, Painter::BevelStyle::Inset);
        Rect inner = r.inset(2);
        if (inner.h < 2) return;
        int half = inner.h / 2;
        p.fill_rect(inner.x, inner.y,        inner.w, half,           Palette::VU_RED);
        p.fill_rect(inner.x, inner.y + half, inner.w, inner.h - half, Palette::VU_BLUE);
    }
};

// ------------------------------------------------------------
// TrafficLights  –  three-circle status indicator
// ------------------------------------------------------------
class TrafficLights {
public:
    void draw(Painter& p, const Rect& r) const {
        p.fill_rect(r, Palette::BG_DARK);
        p.draw_bevel(r, Painter::BevelStyle::Inset);

        static constexpr int k_radius = 18;
        int cx1 = r.x + r.w / 3;
        int cx2 = r.x + (r.w * 2) / 3 + 2;
        int cy1 = r.y + r.h / 4;
        int cy2 = r.y + (r.h * 3) / 4;

        p.fill_circle(cx1,             cy1, k_radius, Palette::LIGHT_RED);
        p.fill_circle(cx2,             cy1, k_radius, Palette::LIGHT_GDM);
        p.fill_circle((cx1 + cx2) / 2, cy2, k_radius, Palette::LIGHT_GBT);
    }
};

// ------------------------------------------------------------
// InfoPanel  –  Freq / Seq / Pattern readout
// ------------------------------------------------------------
class InfoPanel {
public:
    struct Row { const char* label; const char* value; };

    void draw(Painter& p, const Rect& r) const {
        p.fill_rect(r, Palette::INFO_BG);
        p.draw_bevel(r, Painter::BevelStyle::Inset);

        static const Row k_rows[] = {
            { "FREQ:",    "18000" },
            { "PC SPEAKER", ""   },
            { "SEQ:",     "88/80"},
            { "PATTERN:", "88/80"},
            { "NOTE:",    "88/63"},
        };
        static constexpr int k_n      = 5;
        static constexpr int k_line_h = 14;
        static constexpr int k_val_x  = 60;

        int tx = r.x + 6;
        int ty = r.y + 8;

        for (int i = 0; i < k_n; ++i) {
            Font::draw_string(p, tx,          ty + i * k_line_h,
                              k_rows[i].label, Palette::INFO_CYAN);
            if (k_rows[i].value[0])
                Font::draw_string(p, tx + k_val_x, ty + i * k_line_h,
                                  k_rows[i].value, Palette::INFO_WHITE);
        }
        Font::draw_string(p, tx, ty + k_n * k_line_h + 2,
                          "88:88/00:00", Palette::INFO_CYAN);
    }
};

// ------------------------------------------------------------
// SeqGrid  –  pattern / sequencer grid (red + blue rows)
// ------------------------------------------------------------
class SeqGrid {
public:
    void draw(Painter& p, const Rect& r) const {
        p.fill_rect(r, Palette::BG_DARK);
        p.draw_bevel(r, Painter::BevelStyle::Inset);

        static constexpr int   k_rows    = 2;
        static constexpr int   k_cols    = 64;
        static constexpr int   k_pad     = 3;
        static constexpr unsigned int k_bright = 30u;

        Rect   inner { r.x + k_pad, r.y + k_pad, r.w - 2*k_pad, r.h - 2*k_pad };
        int    cw = inner.w / k_cols;
        int    ch = inner.h / k_rows;

        static const Color k_base[k_rows] = { Palette::SEQ_RED, Palette::SEQ_BLUE };

        for (int row = 0; row < k_rows; ++row)
            for (int col = 0; col < k_cols; ++col) {
                Color c = ((col & 3) == 0) ? k_base[row].lighten(k_bright)
                                           : k_base[row];
                p.fill_rect(inner.x + col * cw + 1,
                            inner.y + row * ch + 1,
                            cw - 1, ch - 1, c);
            }
    }
};

// ------------------------------------------------------------
// Button  –  bevelled push button with centred text label
// ------------------------------------------------------------
class Button {
public:
    explicit Button(const char* label) : m_label(label) {}

    void draw(Painter& p, const Rect& r) const {
        p.fill_rect(r, Palette::BTN_FACE);
        p.draw_bevel(r, Painter::BevelStyle::Raised, Palette::BORDER_LT, Palette::BORDER_DK, 1);
        int ty = r.y + (r.h - Font::GLYPH_H) / 2;
        Font::draw_centered(p, r, ty, m_label, Palette::BTN_TEXT);
    }

private:
    const char* m_label;
};

} // namespace widgets

// ============================================================
// Layout  –  proportional geometry derived from fb dimensions
// ============================================================
class Layout {
public:
    static constexpr int MARGIN      = 16;
    static constexpr int LABEL_H     = 20;
    static constexpr int BTN_H       = 28;
    static constexpr int BTN_W       = 70;   // wide enough for 4-char label at scale=1
    static constexpr int VU_W        = 36;   // each VU column width
    static constexpr int TITLE_SCALE = 2;
    static constexpr int TITLE_H     = 7 * TITLE_SCALE + 6; // glyph_h * scale + padding

    explicit Layout(int fbw, int fbh)
        : m_w(fbw), m_h(fbh)
        , m_scope_h((fbh * 28) / 100)
        , m_voice_w((fbw - MARGIN * 2) / 4)
        , m_mid_y(MARGIN + m_scope_h + LABEL_H + 6)
        , m_mid_h((fbh * 28) / 100)
    {
        m_grid_y = m_mid_y + m_mid_h + TITLE_H + MARGIN;
        m_grid_h = fbh - m_grid_y - BTN_H - MARGIN * 2;
    }

    Rect window()           const { return { 0, 0, m_w, m_h }; }

    // Top row: 4 equally-spaced voice scopes
    Rect voice_scope(int v) const {
        return { MARGIN + v * m_voice_w, MARGIN, m_voice_w - 16, m_scope_h };
    }
    Rect voice_label(int v) const {
        Rect s = voice_scope(v);
        return { s.x, s.bottom() + 2, s.w, LABEL_H };
    }

    // Middle row: info | vu_left | center_scope | vu_right | lights
    // info and lights each occupy one voice_w column.
    // vu_left and vu_right each occupy VU_W.
    // center_scope fills the remaining space in the middle.
    Rect info_panel()   const {
        return { MARGIN, m_mid_y, m_voice_w - 16, m_mid_h };
    }
    Rect vu_left()      const {
        int x = MARGIN + m_voice_w;
        return { x, m_mid_y, VU_W, m_mid_h };
    }
    Rect center_scope() const {
        int x = MARGIN + m_voice_w + VU_W + 2;
        int right = MARGIN + m_voice_w * 3 - VU_W - 2;
        return { x, m_mid_y, right - x, m_mid_h };
    }
    Rect vu_right()     const {
        int x = MARGIN + m_voice_w * 3 - VU_W;
        return { x, m_mid_y, VU_W, m_mid_h };
    }
    Rect lights()       const {
        return { MARGIN + m_voice_w * 3, m_mid_y, m_voice_w - 4, m_mid_h };
    }

    // Title strip sits between middle row and seq grid
    int title_y()       const { return m_mid_y + m_mid_h + 4; }

    // Seq grid spans from after info-panel area to before lights area
    Rect seq_grid()     const {
        int gx = MARGIN + BTN_W + MARGIN;
        int gr = m_w - MARGIN - BTN_W - MARGIN;
        return { gx, m_grid_y, gr - gx, m_grid_h };
    }

    Rect btn_quit()     const { return { MARGIN, m_h - MARGIN - BTN_H, BTN_W, BTN_H }; }
    Rect btn_help()     const { return { m_w - MARGIN - BTN_W, m_h - MARGIN - BTN_H, BTN_W, BTN_H }; }

private:
    int m_w, m_h;
    int m_scope_h;
    int m_voice_w;
    int m_mid_y, m_mid_h;
    int m_grid_y, m_grid_h;
};

// ============================================================
// VisualPlayer  –  top-level render orchestrator
// ============================================================
class VisualPlayer {
public:
    void render(Painter& p) const {
        const Layout lay(p.width(), p.height());

        // Background + outer chrome
        p.fill_rect(lay.window(), Palette::BG_PANEL);
        p.draw_bevel(lay.window(), Painter::BevelStyle::Raised);

        // Voice names (stack allocated, no STL)
        static const char* k_voice_names[4] = {
            "Voice 1", "Voice 2", "Voice 3", "Voice 4"
        };

        widgets::Scope     scope;
        widgets::VuMeter   vu;
        widgets::InfoPanel info;
        widgets::SeqGrid   seq;
        widgets::TrafficLights lights;

        // Top row: 4 oscilloscopes + labels
        for (int v = 0; v < 4; ++v) {
            scope.draw(p, lay.voice_scope(v));
            widgets::VoiceLabel lbl(k_voice_names[v]);
            lbl.draw(p, lay.voice_label(v));
        }

        // Middle row
        info.draw  (p, lay.info_panel());
        scope.draw (p, lay.center_scope());
        vu.draw    (p, lay.vu_left());
        vu.draw    (p, lay.vu_right());
        lights.draw(p, lay.lights());

        // Title strip centred over full window in the gap below middle row
        {
            static const char* k_title = "Visual Player  v2.0";
            Font::draw_centered(p, lay.window(), lay.title_y(),
                                k_title, Palette::TITLE_TEXT, Layout::TITLE_SCALE);
        }

        // Sequencer grid
        seq.draw(p, lay.seq_grid());

        // Buttons
        widgets::Button quit_btn("Quit");
        widgets::Button help_btn("Help");
        quit_btn.draw(p, lay.btn_quit());
        help_btn.draw(p, lay.btn_help());
    }
};

} // namespace vp

int main()
{
    vk_framebuffer_info_t info;
    VK_CALL(framebuffer_info, &info);

    if (!info.valid || !info.base) {
        printf("visual_player: no framebuffer available\n");
        return 1;
    }

    vp::Painter      painter = vp::Painter::from_info(info);
    vp::VisualPlayer player;
    player.render(painter);

    getchar(); // block until any key is pressed
    return 0;
}