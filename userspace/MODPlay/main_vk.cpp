/*
 * main_vk.cpp - MODPlay vkernel front-end
 * Copyright (C) 2026 vkernel authors
 *
 * Supports both MOD and S3M formats.
 * Uses the Visual Player UI shell adapted from ramfs_reader and feeds it
 * live tracker data, VU meters, spectrum bars and waveform lines.
 */

#include <math.h>
#include <iostream>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

extern "C" {
#include "../include/vk.h"
#include "libmodplay/modplay.h"
}

/* ---- tunables ---- */
#define DEFAULT_SAMPLE_RATE   44100
#define RENDER_SAMPLES        512
#define PLAY_SAMPLES          3072
#define QUEUE_SAMPLES         12288
#define QUEUE_TARGET_SAMPLES  9216
#define VIS_UPDATE_DIV_FAST   2
#define VIS_UPDATE_DIV_SLOW   3
#define VIS_MIN_QUEUE         (PLAY_SAMPLES * 2)
#define VIS_MAX_CH            32
#define VIS_TOP_SCOPES        4
#define VIS_SCOPE_HISTORY     96
#define VIS_SPECTRUM_BINS     48
#define VIS_SPECTRUM_SAMPLES  96
#define VIS_PEAK_DECAY        2

static int16_t render_buf[RENDER_SAMPLES * 2];
static int16_t play_buf[PLAY_SAMPLES * 2];
static int16_t queue_buf[QUEUE_SAMPLES * 2];
static vk_u32 queue_rd = 0;
static vk_u32 queue_wr = 0;
static vk_u32 queue_count = 0;
static vk_u32 vis_div_ctr = 0;
static vk_u32 *pixbuf = NULL;
static vk_usize pixbuf_pixels = 0;
static vk_u32 vis_frame = 0;
static int vis_audio_drive = 0;
static const char *g_current_filename = NULL;

static vk_framebuffer_info_t g_fb;

static int vu_peaks[VIS_MAX_CH];
static int vu_hold[VIS_MAX_CH];
static int vu_hold_timer[VIS_MAX_CH];
static uint8_t blink_state[VIS_MAX_CH];

static int master_vu[2];
static int master_hold[2];
static int master_hold_timer[2];

static uint8_t scope_history[VIS_TOP_SCOPES][VIS_SCOPE_HISTORY];
static uint8_t spectrum_levels[VIS_SPECTRUM_BINS];
static uint8_t spectrum_peaks[VIS_SPECTRUM_BINS];
static uint8_t spectrum_peak_timer[VIS_SPECTRUM_BINS];
static float spectrum_window[VIS_SPECTRUM_SAMPLES];
static float spectrum_cos[VIS_SPECTRUM_BINS][VIS_SPECTRUM_SAMPLES];
static float spectrum_sin[VIS_SPECTRUM_BINS][VIS_SPECTRUM_SAMPLES];
static int spectrum_tables_ready = 0;

static inline int clamp_int(int value, int min_value, int max_value)
{
    if (value < min_value)
        return min_value;
    if (value > max_value)
        return max_value;
    return value;
}

static inline int abs_int(int value)
{
    return value < 0 ? -value : value;
}

static inline int max_int(int lhs, int rhs)
{
    return lhs > rhs ? lhs : rhs;
}

static inline int min_int(int lhs, int rhs)
{
    return lhs < rhs ? lhs : rhs;
}

static int sample_buffer_mono(const int16_t *buf, int buf_len, int sample_index)
{
    if (buf == NULL || buf_len <= 0)
        return 0;

    sample_index = clamp_int(sample_index, 0, buf_len - 1);
    return (buf[sample_index * 2] + buf[sample_index * 2 + 1]) / 2;
}

static int compute_buffer_energy(const int16_t *buf, int buf_len)
{
    if (buf == NULL || buf_len <= 0)
        return 0;

    vk_u32 total = 0;
    vk_u32 peak = 0;

    for (int i = 0; i < buf_len; ++i) {
        int sl = abs_int(buf[i * 2]);
        int sr = abs_int(buf[i * 2 + 1]);
        vk_u32 amp = (vk_u32)((sl + sr) >> 9);

        total += amp;
        if (amp > peak)
            peak = amp;
    }

    total /= (vk_u32)buf_len;
    total = (total + peak * 3u) / 4u;
    if (total > 255u)
        total = 255u;
    return (int)total;
}

static void analyze_visual_audio(const int16_t *buf, int buf_len,
                                 int *bass_out, int *mid_out, int *treble_out)
{
    int bass = 0;
    int mid = 0;
    int treble = 0;
    int lowpass_fast = 0;
    int lowpass_slow = 0;

    if (bass_out != NULL)
        *bass_out = 0;
    if (mid_out != NULL)
        *mid_out = 0;
    if (treble_out != NULL)
        *treble_out = 0;

    if (buf == NULL || buf_len <= 0)
        return;

    for (int i = 0; i < buf_len; ++i) {
        int mono = sample_buffer_mono(buf, buf_len, i);

        lowpass_fast += (mono - lowpass_fast) >> 2;
        lowpass_slow += (mono - lowpass_slow) >> 4;

        bass += abs_int(lowpass_slow) >> 7;
        mid += abs_int(lowpass_fast - lowpass_slow) >> 7;
        treble += abs_int(mono - lowpass_fast) >> 7;
    }

    bass = clamp_int((bass * 3) / buf_len, 0, 255);
    mid = clamp_int((mid * 4) / buf_len, 0, 255);
    treble = clamp_int((treble * 5) / buf_len, 0, 255);

    if (bass_out != NULL)
        *bass_out = bass;
    if (mid_out != NULL)
        *mid_out = mid;
    if (treble_out != NULL)
        *treble_out = treble;
}

static void compute_master_vu(const int16_t *buf, int buf_len)
{
    int avg_l = 0;
    int avg_r = 0;
    int peak_l = 0;
    int peak_r = 0;

    if (buf == NULL || buf_len <= 0) {
        master_vu[0] = max_int(0, master_vu[0] - VIS_PEAK_DECAY);
        master_vu[1] = max_int(0, master_vu[1] - VIS_PEAK_DECAY);
        return;
    }

    for (int i = 0; i < buf_len; ++i) {
        int sl = abs_int(buf[i * 2]) >> 7;
        int sr = abs_int(buf[i * 2 + 1]) >> 7;

        avg_l += sl;
        avg_r += sr;
        if (sl > peak_l)
            peak_l = sl;
        if (sr > peak_r)
            peak_r = sr;
    }

    avg_l /= buf_len;
    avg_r /= buf_len;

    master_vu[0] = clamp_int((avg_l + peak_l * 3) / 4, 0, 255);
    master_vu[1] = clamp_int((avg_r + peak_r * 3) / 4, 0, 255);

    for (int i = 0; i < 2; ++i) {
        if (master_vu[i] > master_hold[i]) {
            master_hold[i] = master_vu[i];
            master_hold_timer[i] = 30;
        } else if (master_hold_timer[i] > 0) {
            master_hold_timer[i]--;
        } else if (master_hold[i] > 0) {
            master_hold[i]--;
        }
    }
}

static void sanitize_font_string(char *dst, size_t cap, const char *src)
{
    size_t out = 0;

    if (cap == 0)
        return;

    if (src == NULL) {
        dst[0] = '\0';
        return;
    }

    while (*src && out + 1 < cap) {
        unsigned char ch = (unsigned char)*src++;

        if (ch >= 'a' && ch <= 'z')
            ch = (unsigned char)(ch - 'a' + 'A');

        if ((ch >= 'A' && ch <= 'Z') ||
            (ch >= '0' && ch <= '9') ||
            ch == ' ' || ch == '/' || ch == ':' ||
            ch == '.' || ch == '-' || ch == '#') {
            dst[out++] = (char)ch;
        } else {
            dst[out++] = ' ';
        }
    }

    while (out > 0 && dst[out - 1] == ' ')
        --out;

    dst[out] = '\0';
}

static void basename_no_ext(char *dst, size_t cap, const char *path)
{
    const char *base = path;
    size_t out = 0;

    if (cap == 0)
        return;

    if (path == NULL) {
        dst[0] = '\0';
        return;
    }

    for (const char *p = path; *p; ++p) {
        if (*p == '/' || *p == '\\')
            base = p + 1;
    }

    while (base[out] && base[out] != '.' && out + 1 < cap) {
        dst[out] = base[out];
        ++out;
    }

    dst[out] = '\0';
}

static void copy_song_title(char *dst, size_t cap, const MODFILE *mod, const char *filename)
{
    char raw[29];
    char fallback[29];
    int len = 28;

    memcpy(raw, mod->songname, 28);
    raw[28] = '\0';

    while (len > 0 && (raw[len - 1] == '\0' || raw[len - 1] == ' ')) {
        raw[len - 1] = '\0';
        --len;
    }

    sanitize_font_string(dst, cap, raw);
    if (dst[0] != '\0')
        return;

    basename_no_ext(fallback, sizeof(fallback), filename);
    sanitize_font_string(dst, cap, fallback);
    if (dst[0] == '\0')
        sanitize_font_string(dst, cap, "MODPLAY");
}

static const char *module_format_name(int filetype)
{
    switch (filetype) {
    case MODULE_MOD:
        return "MOD";
    case MODULE_S3M:
        return "S3M";
    case MODULE_XM:
        return "XM";
    default:
        return "UNK";
    }
}

static void vis_init_spectrum_tables(void)
{
    const float tau = 6.28318530718f;

    if (spectrum_tables_ready)
        return;

    for (int i = 0; i < VIS_SPECTRUM_SAMPLES; ++i) {
        float phase = (float)i / (float)(VIS_SPECTRUM_SAMPLES - 1);
        spectrum_window[i] = 0.54f - 0.46f * cosf(tau * phase);
    }

    for (int bin = 0; bin < VIS_SPECTRUM_BINS; ++bin) {
        float norm = (float)(bin + 1) / (float)VIS_SPECTRUM_BINS;
        float freq = 1.0f + norm * norm * ((float)VIS_SPECTRUM_SAMPLES / 2.4f);

        for (int i = 0; i < VIS_SPECTRUM_SAMPLES; ++i) {
            float angle = tau * freq * (float)i / (float)VIS_SPECTRUM_SAMPLES;
            spectrum_cos[bin][i] = cosf(angle);
            spectrum_sin[bin][i] = sinf(angle);
        }
    }

    spectrum_tables_ready = 1;
}

static void vis_reset_dynamic_state(void)
{
    memset(vu_peaks, 0, sizeof(vu_peaks));
    memset(vu_hold, 0, sizeof(vu_hold));
    memset(vu_hold_timer, 0, sizeof(vu_hold_timer));
    memset(blink_state, 0, sizeof(blink_state));
    memset(master_vu, 0, sizeof(master_vu));
    memset(master_hold, 0, sizeof(master_hold));
    memset(master_hold_timer, 0, sizeof(master_hold_timer));
    memset(scope_history, 0, sizeof(scope_history));
    memset(spectrum_levels, 0, sizeof(spectrum_levels));
    memset(spectrum_peaks, 0, sizeof(spectrum_peaks));
    memset(spectrum_peak_timer, 0, sizeof(spectrum_peak_timer));
    vis_frame = 0;
    vis_audio_drive = 0;
    vis_init_spectrum_tables();
}

static void compute_vu_levels(MODFILE *mod)
{
    for (int ch = 0; ch < mod->nChannels && ch < VIS_MAX_CH; ++ch) {
        MOD_Channel *channel = &mod->channels[ch];
        int level = 0;

        if (channel->voiceInfo.enabled &&
            channel->voiceInfo.playing &&
            channel->sample != NULL &&
            channel->sample->sampleInfo.sampledata != NULL) {
            int fade = (int)(channel->volumeFade >> 9);
            level = (channel->voiceInfo.volume * fade * 3) / 64;
            if (level > 192)
                level = 192;
        }

        if (level > vu_peaks[ch])
            vu_peaks[ch] = level;
        else if (vu_peaks[ch] > 0)
            vu_peaks[ch] -= VIS_PEAK_DECAY;

        if (vu_peaks[ch] > vu_hold[ch]) {
            vu_hold[ch] = vu_peaks[ch];
            vu_hold_timer[ch] = 30;
        } else if (vu_hold_timer[ch] > 0) {
            vu_hold_timer[ch]--;
        } else if (vu_hold[ch] > 0) {
            vu_hold[ch]--;
        }

        if ((mod->notebeats & (1u << ch)) != 0) {
            blink_state[ch] = 255;
        } else if (blink_state[ch] > 10) {
            blink_state[ch] -= 10;
        } else {
            blink_state[ch] = 0;
        }
    }

    for (int ch = mod->nChannels; ch < VIS_MAX_CH; ++ch) {
        if (vu_peaks[ch] > 0)
            vu_peaks[ch] -= VIS_PEAK_DECAY;
        if (vu_hold_timer[ch] > 0)
            vu_hold_timer[ch]--;
        else if (vu_hold[ch] > 0)
            vu_hold[ch]--;
        if (blink_state[ch] > 10)
            blink_state[ch] -= 10;
        else
            blink_state[ch] = 0;
    }
}

static void update_scope_history(MODFILE *mod)
{
    for (int panel = 0; panel < VIS_TOP_SCOPES; ++panel) {
        int level = 0;

        memmove(scope_history[panel], scope_history[panel] + 1, VIS_SCOPE_HISTORY - 1);

        if (panel < mod->nChannels) {
            level = (vu_peaks[panel] * 255) / 192;
            if (blink_state[panel] > 0 && level < 20)
                level = 20 + blink_state[panel] / 8;
        }

        scope_history[panel][VIS_SCOPE_HISTORY - 1] = (uint8_t)clamp_int(level, 0, 255);
    }
}

static void update_spectrum(const int16_t *buf, int buf_len, int energy)
{
    int sample_count = min_int(buf_len, VIS_SPECTRUM_SAMPLES);

    if (buf == NULL || buf_len <= 0 || sample_count < 8) {
        for (int i = 0; i < VIS_SPECTRUM_BINS; ++i) {
            if (spectrum_levels[i] > 4)
                spectrum_levels[i] -= 4;
            else
                spectrum_levels[i] = 0;
        }
        return;
    }

    for (int bin = 0; bin < VIS_SPECTRUM_BINS; ++bin) {
        float real = 0.0f;
        float imag = 0.0f;
        float gain = 1.0f + ((float)bin / (float)VIS_SPECTRUM_BINS) * 2.0f;
        int level;

        for (int i = 0; i < sample_count; ++i) {
            int sample_index = (i * (buf_len - 1)) / (sample_count - 1);
            float mono = (float)sample_buffer_mono(buf, buf_len, sample_index) / 32768.0f;
            float weighted = mono * spectrum_window[i];

            real += weighted * spectrum_cos[bin][i];
            imag -= weighted * spectrum_sin[bin][i];
        }

        level = (int)(sqrtf(real * real + imag * imag) * (44.0f + gain * 18.0f));
        level += energy / 10;
        level = clamp_int(level, 0, 255);

        if (level > spectrum_levels[bin])
            spectrum_levels[bin] = (uint8_t)level;
        else if (spectrum_levels[bin] > 5)
            spectrum_levels[bin] -= 5;
        else
            spectrum_levels[bin] = 0;

        if (spectrum_levels[bin] > spectrum_peaks[bin]) {
            spectrum_peaks[bin] = spectrum_levels[bin];
            spectrum_peak_timer[bin] = 18;
        } else if (spectrum_peak_timer[bin] > 0) {
            spectrum_peak_timer[bin]--;
        } else if (spectrum_peaks[bin] > 0) {
            spectrum_peaks[bin]--;
        }
    }
}

namespace vp {

class Color {
public:
    constexpr Color() : m_rgb(0) {}
    constexpr explicit Color(unsigned int rgb) : m_rgb(rgb & 0x00FFFFFFu) {}
    constexpr Color(unsigned int r, unsigned int g, unsigned int b)
        : m_rgb(((r & 0xFFu) << 16) | ((g & 0xFFu) << 8) | (b & 0xFFu)) {}

    constexpr unsigned int r() const { return (m_rgb >> 16) & 0xFFu; }
    constexpr unsigned int g() const { return (m_rgb >> 8) & 0xFFu; }
    constexpr unsigned int b() const { return m_rgb & 0xFFu; }

    constexpr Color lighten(unsigned int delta) const {
        return Color(
            r() + delta > 255u ? 255u : r() + delta,
            g() + delta > 255u ? 255u : g() + delta,
            b() + delta > 255u ? 255u : b() + delta
        );
    }

    constexpr Color scale(unsigned int brightness) const {
        return Color(
            (r() * brightness) / 255u,
            (g() * brightness) / 255u,
            (b() * brightness) / 255u
        );
    }

    vk_u32 to_pixel() const {
        return ((vk_u32)r() << 16) | ((vk_u32)g() << 8) | (vk_u32)b();
    }

private:
    unsigned int m_rgb;
};

namespace Palette {
    constexpr Color BG_PANEL   { 0xBA8E7Du };
    constexpr Color BG_DARK    { 0x000000u };
    constexpr Color BORDER_LT  { 0xD0A898u };
    constexpr Color BORDER_DK  { 0x604030u };
    constexpr Color GREEN_LINE { 0x00FF88u };
    constexpr Color RED_LED    { 0xCC2020u };
    constexpr Color LED_SHADOW { 0x551010u };
    constexpr Color LED_HILITE { 0xFF6666u };
    constexpr Color INFO_BG    { 0x000000u };
    constexpr Color INFO_CYAN  { 0x00CCCCu };
    constexpr Color INFO_WHITE { 0xDDDDDDu };
    constexpr Color BTN_FACE   { 0xA07060u };
    constexpr Color BTN_TEXT   { 0xEEEEEEu };
    constexpr Color TITLE_TEXT { 0x7C4A3Eu };
    constexpr Color VU_LOW     { 0x2244AAu };
    constexpr Color VU_MID     { 0x00AAAAu };
    constexpr Color VU_HIGH    { 0xFFAA00u };
    constexpr Color VU_PEAK    { 0xFFFFFFu };
    constexpr Color LIGHT_RED  { 0xAA2222u };
    constexpr Color LIGHT_AMB  { 0xAA8822u };
    constexpr Color LIGHT_GRN  { 0x33AA44u };
    constexpr Color GRID_RED   { 0x8B0000u };
    constexpr Color GRID_BLUE  { 0x00008Bu };
}

struct Rect {
    int x;
    int y;
    int w;
    int h;

    constexpr Rect() : x(0), y(0), w(0), h(0) {}
    constexpr Rect(int px, int py, int pw, int ph) : x(px), y(py), w(pw), h(ph) {}

    constexpr int right() const { return x + w; }
    constexpr int bottom() const { return y + h; }
    constexpr int cx() const { return x + w / 2; }
    constexpr int cy() const { return y + h / 2; }
    constexpr Rect inset(int d) const { return Rect(x + d, y + d, w - d * 2, h - d * 2); }
};

class Painter {
public:
    enum class BevelStyle { Raised, Inset };

    Painter(vk_u32 *pixels, int width, int height, int stride)
        : m_pixels(pixels), m_width(width), m_height(height), m_stride(stride) {}

    int width() const { return m_width; }
    int height() const { return m_height; }

    void put_pixel(int x, int y, Color color) {
        if (x < 0 || y < 0 || x >= m_width || y >= m_height)
            return;
        m_pixels[y * m_stride + x] = color.to_pixel();
    }

    void fill_rect(int x, int y, int w, int h, Color color) {
        if (w <= 0 || h <= 0)
            return;

        int x0 = x;
        int y0 = y;
        int x1 = x + w;
        int y1 = y + h;

        if (x0 < 0) x0 = 0;
        if (y0 < 0) y0 = 0;
        if (x1 > m_width) x1 = m_width;
        if (y1 > m_height) y1 = m_height;
        if (x0 >= x1 || y0 >= y1)
            return;

        vk_u32 px = color.to_pixel();
        for (int py = y0; py < y1; ++py) {
            vk_u32 *row = m_pixels + py * m_stride;
            for (int px_idx = x0; px_idx < x1; ++px_idx)
                row[px_idx] = px;
        }
    }

    void fill_rect(const Rect& rect, Color color) { fill_rect(rect.x, rect.y, rect.w, rect.h, color); }
    void draw_hline(int x, int y, int w, Color color) { fill_rect(x, y, w, 1, color); }
    void draw_vline(int x, int y, int h, Color color) { fill_rect(x, y, 1, h, color); }

    void draw_line(int x0, int y0, int x1, int y1, Color color) {
        int dx = abs_int(x1 - x0);
        int sx = x0 < x1 ? 1 : -1;
        int dy = -abs_int(y1 - y0);
        int sy = y0 < y1 ? 1 : -1;
        int err = dx + dy;

        for (;;) {
            put_pixel(x0, y0, color);
            if (x0 == x1 && y0 == y1)
                break;

            int err2 = err << 1;
            if (err2 >= dy) {
                err += dy;
                x0 += sx;
            }
            if (err2 <= dx) {
                err += dx;
                y0 += sy;
            }
        }
    }

    void fill_circle(int cx, int cy, int radius, Color color) {
        for (int dy = -radius; dy <= radius; ++dy) {
            for (int dx = -radius; dx <= radius; ++dx) {
                if (dx * dx + dy * dy <= radius * radius)
                    put_pixel(cx + dx, cy + dy, color);
            }
        }
    }

    void draw_bevel(const Rect& rect, BevelStyle style,
                    Color light = Palette::BORDER_LT,
                    Color dark = Palette::BORDER_DK,
                    int thickness = 5)
    {
        if (style == BevelStyle::Inset) {
            Color tmp = light;
            light = dark;
            dark = tmp;
        }

        for (int i = 0; i < thickness; ++i) {
            int x = rect.x + i;
            int y = rect.y + i;
            int w = rect.w - i * 2;
            int h = rect.h - i * 2;

            if (w <= 0 || h <= 0)
                break;

            draw_hline(x, y, w, light);
            draw_vline(x, y, h, light);
            draw_hline(x, rect.bottom() - 1 - i, w, dark);
            draw_vline(rect.right() - 1 - i, y, h, dark);
        }
    }

private:
    vk_u32 *m_pixels;
    int m_width;
    int m_height;
    int m_stride;
};

class Font {
public:
    static constexpr int GLYPH_W = 5;
    static constexpr int GLYPH_H = 7;

    static constexpr int char_advance(int scale = 1) {
        return (GLYPH_W + 1) * scale;
    }

    static void draw_char(Painter& painter, int x, int y, char c, Color color, int scale = 1)
    {
        const unsigned char *glyph = glyph_for(c);

        for (int row = 0; row < GLYPH_H; ++row) {
            for (int col = 0; col < GLYPH_W; ++col) {
                if (glyph[row] & (0x01u << col)) {
                    painter.fill_rect(x + col * scale, y + row * scale, scale, scale, color);
                }
            }
        }
    }

    static int draw_string(Painter& painter, int x, int y, const char *text,
                           Color color, int scale = 1)
    {
        int cursor = x;

        while (*text) {
            draw_char(painter, cursor, y, *text, color, scale);
            cursor += char_advance(scale);
            ++text;
        }

        return cursor;
    }

    static void draw_centered(Painter& painter, const Rect& rect, int y,
                              const char *text, Color color, int scale = 1)
    {
        int width = (int)strlen(text) * char_advance(scale);
        draw_string(painter, rect.x + (rect.w - width) / 2, y, text, color, scale);
    }

private:
    static const unsigned char *glyph_for(char c)
    {
        int idx = 0;

        if (c == '/') idx = 1;
        else if (c == ':') idx = 2;
        else if (c == '.') idx = 3;
        else if (c == '-') idx = 4;
        else if (c == '#') idx = 5;
        else if (c >= '0' && c <= '9') idx = 6 + (c - '0');
        else if (c >= 'A' && c <= 'Z') idx = 16 + (c - 'A');
        else if (c >= 'a' && c <= 'z') idx = 16 + (c - 'a');

        return k_glyphs[idx];
    }

    static const unsigned char k_glyphs[][GLYPH_H];
};

const unsigned char Font::k_glyphs[][Font::GLYPH_H] = {
    {0x00,0x00,0x00,0x00,0x00,0x00,0x00},
    {0x10,0x08,0x04,0x02,0x01,0x00,0x00},
    {0x00,0x0C,0x0C,0x00,0x0C,0x0C,0x00},
    {0x00,0x00,0x00,0x00,0x00,0x0C,0x00},
    {0x00,0x00,0x00,0x1F,0x00,0x00,0x00},
    {0x0A,0x1F,0x0A,0x1F,0x0A,0x00,0x00},
    {0x0E,0x11,0x19,0x15,0x13,0x11,0x0E},
    {0x04,0x06,0x04,0x04,0x04,0x04,0x0E},
    {0x0E,0x11,0x10,0x0C,0x02,0x01,0x1F},
    {0x1F,0x08,0x04,0x0C,0x10,0x11,0x0E},
    {0x08,0x0C,0x0A,0x09,0x1F,0x08,0x08},
    {0x1F,0x01,0x0F,0x10,0x10,0x11,0x0E},
    {0x0C,0x02,0x01,0x0F,0x11,0x11,0x0E},
    {0x1F,0x10,0x08,0x04,0x02,0x02,0x02},
    {0x0E,0x11,0x11,0x0E,0x11,0x11,0x0E},
    {0x0E,0x11,0x11,0x1E,0x10,0x08,0x06},
    {0x04,0x0A,0x11,0x11,0x1F,0x11,0x11},
    {0x0F,0x11,0x11,0x0F,0x11,0x11,0x0F},
    {0x0E,0x11,0x01,0x01,0x01,0x11,0x0E},
    {0x0F,0x11,0x11,0x11,0x11,0x11,0x0F},
    {0x1F,0x01,0x01,0x0F,0x01,0x01,0x1F},
    {0x1F,0x01,0x01,0x0F,0x01,0x01,0x01},
    {0x0E,0x11,0x01,0x1D,0x11,0x11,0x1E},
    {0x11,0x11,0x11,0x1F,0x11,0x11,0x11},
    {0x0E,0x04,0x04,0x04,0x04,0x04,0x0E},
    {0x1C,0x08,0x08,0x08,0x08,0x09,0x06},
    {0x11,0x09,0x05,0x03,0x05,0x09,0x11},
    {0x01,0x01,0x01,0x01,0x01,0x01,0x1F},
    {0x11,0x1B,0x15,0x11,0x11,0x11,0x11},
    {0x11,0x13,0x15,0x19,0x11,0x11,0x11},
    {0x0E,0x11,0x11,0x11,0x11,0x11,0x0E},
    {0x0F,0x11,0x11,0x0F,0x01,0x01,0x01},
    {0x0E,0x11,0x11,0x11,0x15,0x09,0x16},
    {0x0F,0x11,0x11,0x0F,0x05,0x09,0x11},
    {0x0E,0x11,0x01,0x0E,0x10,0x11,0x0E},
    {0x1F,0x04,0x04,0x04,0x04,0x04,0x04},
    {0x11,0x11,0x11,0x11,0x11,0x11,0x0E},
    {0x11,0x11,0x11,0x11,0x0A,0x0A,0x04},
    {0x11,0x11,0x11,0x15,0x15,0x1B,0x11},
    {0x11,0x11,0x0A,0x04,0x0A,0x11,0x11},
    {0x11,0x11,0x0A,0x04,0x04,0x04,0x04},
    {0x1F,0x10,0x08,0x04,0x02,0x01,0x1F},
};

struct VisualState {
    char song_text[28];
    char file_text[28];
    char type_text[20];
    char bpm_text[20];
    char pos_text[20];
    char pat_text[20];
    char row_text[20];
    char voice_text[VIS_TOP_SCOPES][16];
    int channel_level[VIS_TOP_SCOPES];
    int channel_led[VIS_TOP_SCOPES];
    int master_level[2];
    int master_peak[2];
    int bass_level;
    int mid_level;
    int treble_level;
    int row;
    int row_total;
};

class Layout {
public:
    static constexpr int MARGIN = 16;
    static constexpr int LABEL_H = 20;
    static constexpr int BTN_H = 28;
    static constexpr int BTN_W = 70;
    static constexpr int VU_W = 40;
    static constexpr int TITLE_SCALE = 2;
    static constexpr int TITLE_H = 7 * TITLE_SCALE + 6;

    Layout(int fbw, int fbh)
        : m_w(fbw), m_h(fbh),
          m_scope_h((fbh * 28) / 100),
          m_voice_w((fbw - MARGIN * 2) / 4),
          m_mid_y(MARGIN + m_scope_h + LABEL_H + 6),
          m_mid_h((fbh * 28) / 100)
    {
        m_grid_y = m_mid_y + m_mid_h + TITLE_H + MARGIN;
        m_grid_h = fbh - m_grid_y - BTN_H - MARGIN * 2;
    }

    Rect window() const { return Rect(0, 0, m_w, m_h); }

    Rect voice_scope(int index) const {
        return Rect(MARGIN + index * m_voice_w, MARGIN, m_voice_w - 16, m_scope_h);
    }

    Rect voice_label(int index) const {
        Rect scope = voice_scope(index);
        return Rect(scope.x, scope.bottom() + 2, scope.w, LABEL_H);
    }

    Rect info_panel() const {
        return Rect(MARGIN, m_mid_y, m_voice_w - 16, m_mid_h);
    }

    Rect vu_left() const {
        return Rect(MARGIN + m_voice_w, m_mid_y, VU_W, m_mid_h);
    }

    Rect center_scope() const {
        int x = MARGIN + m_voice_w + VU_W + 2;
        int right = MARGIN + m_voice_w * 3 - VU_W - 2;
        return Rect(x, m_mid_y, right - x, m_mid_h);
    }

    Rect vu_right() const {
        return Rect(MARGIN + m_voice_w * 3 - VU_W, m_mid_y, VU_W, m_mid_h);
    }

    Rect lights() const {
        return Rect(MARGIN + m_voice_w * 3, m_mid_y, m_voice_w - 4, m_mid_h);
    }

    int title_y() const {
        return m_mid_y + m_mid_h + 4;
    }

    Rect spectrum_panel() const {
        int gx = MARGIN + BTN_W + MARGIN;
        int gr = m_w - MARGIN - BTN_W - MARGIN;
        return Rect(gx, m_grid_y, gr - gx, m_grid_h);
    }

    Rect btn_quit() const { return Rect(MARGIN, m_h - MARGIN - BTN_H, BTN_W, BTN_H); }
    Rect btn_help() const { return Rect(m_w - MARGIN - BTN_W, m_h - MARGIN - BTN_H, BTN_W, BTN_H); }

private:
    int m_w;
    int m_h;
    int m_scope_h;
    int m_voice_w;
    int m_mid_y;
    int m_mid_h;
    int m_grid_y;
    int m_grid_h;
};

static Color channel_color(int index)
{
    static const unsigned int colors[][3] = {
        {0, 255, 128}, {255, 176, 64}, {0, 216, 255}, {255, 72, 176}
    };
    return Color(colors[index % 4][0], colors[index % 4][1], colors[index % 4][2]);
}

static void draw_scope_frame(Painter& painter, const Rect& rect)
{
    painter.fill_rect(rect, Palette::BG_DARK);
    painter.draw_bevel(rect, Painter::BevelStyle::Inset);
}

static void draw_scope_trace(Painter& painter, const Rect& rect,
                             const uint8_t *history, int length, Color line)
{
    Rect inner = rect.inset(3);
    Color glow = line.scale(96);
    int prev_x = inner.x;
    int prev_y = inner.cy();

    if (inner.w <= 1 || inner.h <= 2)
        return;

    painter.draw_hline(inner.x, inner.cy(), inner.w, Palette::GREEN_LINE.scale(80));

    for (int i = 0; i < length; ++i) {
        int x = inner.x + (i * (inner.w - 1)) / (length - 1);
        int y = inner.bottom() - 2 - ((int)history[i] * (inner.h - 3)) / 255;

        if (i > 0) {
            painter.draw_line(prev_x, prev_y - 1, x, y - 1, glow);
            painter.draw_line(prev_x, prev_y, x, y, line);
        }

        prev_x = x;
        prev_y = y;
    }
}

static void draw_audio_line(Painter& painter, const Rect& rect,
                            const int16_t *buf, int buf_len, int energy)
{
    Rect inner = rect.inset(3);
    int amplitude = inner.h / 3;
    int accent_amplitude = amplitude / 2;
    int prev_x = inner.x;
    int prev_main_y = inner.cy();
    int prev_accent_y = inner.cy();

    if (buf == NULL || buf_len <= 0 || inner.w <= 1 || inner.h <= 4)
        return;

    if (amplitude < 1)
        amplitude = 1;
    if (accent_amplitude < 1)
        accent_amplitude = 1;

    for (int x = inner.x; x < inner.right(); x += 4)
        painter.put_pixel(x, inner.cy(), Palette::INFO_CYAN.scale(70));

    for (int i = 0; i < inner.w; ++i) {
        int sample_index = (i * (buf_len - 1)) / (inner.w - 1);
        int left = buf[sample_index * 2];
        int right = buf[sample_index * 2 + 1];
        int mono = (left + right) / 2;
        int side = (left - right) / 2;
        int main_y = inner.cy() - (mono * amplitude / 32768);
        int accent_y = inner.cy() - (side * accent_amplitude / 32768);
        int gradient = (i * 255) / (inner.w - 1);
        Color core(255 - gradient, 20 + (energy >> 4), 64 + ((gradient * 3) >> 2));
        Color glow = core.scale(92);
        Color accent = Color(180, 220, 255).scale(128);
        int x = inner.x + i;

        main_y = clamp_int(main_y, inner.y + 1, inner.bottom() - 2);
        accent_y = clamp_int(accent_y, inner.y + 1, inner.bottom() - 2);

        if (i > 0) {
            painter.draw_line(prev_x, prev_main_y - 1, x, main_y - 1, glow);
            painter.draw_line(prev_x, prev_main_y + 1, x, main_y + 1, glow);
            painter.draw_line(prev_x, prev_main_y, x, main_y, core);
            painter.draw_line(prev_x, prev_accent_y, x, accent_y, accent);
        }

        if ((i % 30) == 0)
            painter.put_pixel(x, main_y, Color(255, 255, 255));

        prev_x = x;
        prev_main_y = main_y;
        prev_accent_y = accent_y;
    }
}

namespace widgets {

class VoiceLabel {
public:
    void draw(Painter& painter, const Rect& rect, const char *text, Color led_color, int led_brightness) const
    {
        Rect led(rect.x + 6, rect.y + (rect.h - 6) / 2, 6, 6);
        Rect text_area(led.right() + 5, rect.y, rect.w - (led.right() - rect.x) - 5, rect.h);

        painter.fill_rect(rect, Palette::BTN_FACE);
        painter.draw_bevel(rect, Painter::BevelStyle::Raised, Palette::BORDER_LT, Palette::BORDER_DK, 1);
        painter.fill_rect(led, led_color.scale((unsigned int)led_brightness));
        painter.draw_bevel(led, Painter::BevelStyle::Raised, Palette::LED_HILITE, Palette::LED_SHADOW, 1);
        Font::draw_centered(painter, text_area, rect.y + (rect.h - Font::GLYPH_H) / 2,
                            text, Palette::BTN_TEXT);
    }
};

class VuMeter {
public:
    void draw(Painter& painter, const Rect& rect, int level, int peak, const char *label) const
    {
        Rect inner = rect.inset(4);
        int lit = (level * (inner.h - 2)) / 255;
        int hold = (peak * (inner.h - 2)) / 255;

        painter.fill_rect(rect, Palette::BG_PANEL);
        painter.draw_bevel(rect, Painter::BevelStyle::Raised);
        painter.fill_rect(inner, Palette::BG_DARK);
        painter.draw_bevel(inner, Painter::BevelStyle::Inset, Palette::BORDER_LT, Palette::BORDER_DK, 2);

        for (int y = 0; y < lit; ++y) {
            int py = inner.bottom() - 2 - y;
            int pct = (y * 255) / max_int(1, inner.h - 2);
            Color color = pct < 120 ? Palette::VU_LOW.lighten((unsigned int)(pct / 3)) :
                          pct < 200 ? Palette::VU_MID.lighten((unsigned int)((pct - 120) / 2)) :
                                      Palette::VU_HIGH.lighten((unsigned int)((pct - 200) / 3));
            painter.draw_hline(inner.x + 2, py, inner.w - 4, color);
        }

        if (hold > 0)
            painter.draw_hline(inner.x + 1, inner.bottom() - 2 - hold, inner.w - 2, Palette::VU_PEAK);

        Font::draw_centered(painter, rect, rect.y + 4, label, Palette::TITLE_TEXT);
    }
};

class TrafficLights {
public:
    void draw(Painter& painter, const Rect& rect, int low, int mid, int high) const
    {
        int radius = min_int(rect.w / 6, rect.h / 5);
        int cx1 = rect.x + rect.w / 3;
        int cx2 = rect.x + (rect.w * 2) / 3 + 2;
        int cy1 = rect.y + rect.h / 4;
        int cy2 = rect.y + (rect.h * 3) / 4;

        painter.fill_rect(rect, Palette::BG_DARK);
        painter.draw_bevel(rect, Painter::BevelStyle::Inset);

        painter.fill_circle(cx1, cy1, radius + 2, Palette::BORDER_DK);
        painter.fill_circle(cx2, cy1, radius + 2, Palette::BORDER_DK);
        painter.fill_circle((cx1 + cx2) / 2, cy2, radius + 2, Palette::BORDER_DK);

        painter.fill_circle(cx1, cy1, radius, Palette::LIGHT_RED.scale((unsigned int)(64 + low * 191 / 255)));
        painter.fill_circle(cx2, cy1, radius, Palette::LIGHT_AMB.scale((unsigned int)(64 + mid * 191 / 255)));
        painter.fill_circle((cx1 + cx2) / 2, cy2, radius, Palette::LIGHT_GRN.scale((unsigned int)(64 + high * 191 / 255)));
    }
};

class InfoPanel {
public:
    void draw(Painter& painter, const Rect& rect, const VisualState& state) const
    {
        static const char *labels[] = {
            "SONG:", "FILE:", "TYPE:", "BPM:", "POS:", "PAT:", "ROW:"
        };
        static constexpr int line_h = 13;
        static constexpr int value_x = 42;
        const char *values[] = {
            state.song_text,
            state.file_text,
            state.type_text,
            state.bpm_text,
            state.pos_text,
            state.pat_text,
            state.row_text,
        };

        painter.fill_rect(rect, Palette::INFO_BG);
        painter.draw_bevel(rect, Painter::BevelStyle::Inset);

        for (int i = 0; i < 7; ++i) {
            int y = rect.y + 6 + i * line_h;
            Font::draw_string(painter, rect.x + 6, y, labels[i], Palette::INFO_CYAN);
            Font::draw_string(painter, rect.x + 6 + value_x, y, values[i], Palette::INFO_WHITE);
        }
    }
};

class SpectrumPanel {
public:
    void draw(Painter& painter, const Rect& rect,
              const uint8_t *levels, const uint8_t *peaks, int row, int row_total) const
    {
        Rect inner = rect.inset(4);

        painter.fill_rect(rect, Palette::BG_DARK);
        painter.draw_bevel(rect, Painter::BevelStyle::Inset);

        for (int y = 1; y < 4; ++y) {
            int gy = inner.y + (y * inner.h) / 4;
            painter.draw_hline(inner.x, gy, inner.w, Palette::INFO_CYAN.scale(36));
        }

        for (int bin = 0; bin < VIS_SPECTRUM_BINS; ++bin) {
            int x0 = inner.x + (bin * inner.w) / VIS_SPECTRUM_BINS;
            int x1 = inner.x + ((bin + 1) * inner.w) / VIS_SPECTRUM_BINS;
            int bw = max_int(1, x1 - x0 - 1);
            int bar_h = (levels[bin] * (inner.h - 1)) / 255;
            int peak_h = (peaks[bin] * (inner.h - 1)) / 255;

            for (int y = 0; y < bar_h; ++y) {
                int py = inner.bottom() - 1 - y;
                int pct = (y * 255) / max_int(1, inner.h - 1);
                Color color = pct < 120 ? Palette::GRID_BLUE.lighten((unsigned int)(pct / 2)) :
                              pct < 200 ? Palette::INFO_CYAN.lighten((unsigned int)((pct - 120) / 2)) :
                                          Palette::GRID_RED.lighten((unsigned int)((pct - 200) / 2));
                painter.fill_rect(x0, py, bw, 1, color);
            }

            if (peak_h > 0)
                painter.draw_hline(x0, inner.bottom() - 1 - peak_h, bw, Palette::VU_PEAK);
        }

        if (row_total > 0) {
            int cursor_x = inner.x + (row * inner.w) / row_total;
            painter.draw_vline(cursor_x, inner.y, inner.h, Palette::TITLE_TEXT.scale(180));
        }
    }
};

class Button {
public:
    explicit Button(const char *label) : m_label(label) {}

    void draw(Painter& painter, const Rect& rect) const
    {
        painter.fill_rect(rect, Palette::BTN_FACE);
        painter.draw_bevel(rect, Painter::BevelStyle::Raised, Palette::BORDER_LT, Palette::BORDER_DK, 1);
        Font::draw_centered(painter, rect, rect.y + (rect.h - Font::GLYPH_H) / 2, m_label, Palette::BTN_TEXT);
    }

private:
    const char *m_label;
};

} // namespace widgets

class VisualPlayer {
public:
    void render(Painter& painter, const VisualState& state,
                const int16_t *buf, int buf_len,
                const uint8_t history[][VIS_SCOPE_HISTORY],
                const uint8_t *levels, const uint8_t *peaks) const
    {
        Layout layout(painter.width(), painter.height());
        widgets::VoiceLabel voice_label;
        widgets::VuMeter vu_meter;
        widgets::TrafficLights traffic_lights;
        widgets::InfoPanel info_panel;
        widgets::SpectrumPanel spectrum_panel;
        widgets::Button quit_button("QUIT");
        widgets::Button help_button("HELP");

        painter.fill_rect(layout.window(), Palette::BG_PANEL);
        painter.draw_bevel(layout.window(), Painter::BevelStyle::Raised);

        for (int i = 0; i < VIS_TOP_SCOPES; ++i) {
            draw_scope_frame(painter, layout.voice_scope(i));
            draw_scope_trace(painter, layout.voice_scope(i), history[i], VIS_SCOPE_HISTORY, channel_color(i));
            voice_label.draw(painter, layout.voice_label(i), state.voice_text[i],
                             channel_color(i), 48 + state.channel_led[i] * 207 / 255);
        }

        info_panel.draw(painter, layout.info_panel(), state);
        vu_meter.draw(painter, layout.vu_left(), state.master_level[0], state.master_peak[0], "LEFT");

        draw_scope_frame(painter, layout.center_scope());
        draw_audio_line(painter, layout.center_scope(), buf, buf_len, state.bass_level + state.mid_level + state.treble_level);

        vu_meter.draw(painter, layout.vu_right(), state.master_level[1], state.master_peak[1], "RIGHT");
        traffic_lights.draw(painter, layout.lights(), state.bass_level, state.mid_level, state.treble_level);

        Font::draw_centered(painter, layout.window(), layout.title_y(),
                            "MODPLAY VISUAL PLAYER", Palette::TITLE_TEXT, Layout::TITLE_SCALE);

        spectrum_panel.draw(painter, layout.spectrum_panel(), levels, peaks, state.row, state.row_total);
        quit_button.draw(painter, layout.btn_quit());
        help_button.draw(painter, layout.btn_help());
    }
};

} // namespace vp

static void build_visual_state(vp::VisualState *state, MODFILE *mod,
                               const char *filename, const int16_t *buf, int buf_len)
{
    char raw[32];
    char file_base[28];
    int bass = 0;
    int mid = 0;
    int treble = 0;
    int play_position = clamp_int(mod->play_position, 0, max_int(0, mod->songlength - 1));
    int pattern = mod->songlength > 0 ? mod->playlist[play_position] : 0;
    int row_total = 64;

    memset(state, 0, sizeof(*state));

    analyze_visual_audio(buf, buf_len, &bass, &mid, &treble);
    state->bass_level = bass;
    state->mid_level = mid;
    state->treble_level = treble;
    state->master_level[0] = master_vu[0];
    state->master_level[1] = master_vu[1];
    state->master_peak[0] = master_hold[0];
    state->master_peak[1] = master_hold[1];

    copy_song_title(state->song_text, sizeof(state->song_text), mod, filename);
    basename_no_ext(file_base, sizeof(file_base), filename);
    sanitize_font_string(state->file_text, sizeof(state->file_text), file_base);

    snprintf(raw, sizeof(raw), "%s %02dCH", module_format_name(mod->filetype), mod->nChannels);
    sanitize_font_string(state->type_text, sizeof(state->type_text), raw);

    snprintf(raw, sizeof(raw), "%03d/%02d", mod->bpm, mod->speed);
    sanitize_font_string(state->bpm_text, sizeof(state->bpm_text), raw);

    snprintf(raw, sizeof(raw), "%03d/%03d", play_position + 1, max_int(1, mod->songlength));
    sanitize_font_string(state->pos_text, sizeof(state->pos_text), raw);

    if (pattern >= 0 && pattern < mod->nPatterns && mod->patternLengths != NULL)
        row_total = max_int(1, mod->patternLengths[pattern]);

    snprintf(raw, sizeof(raw), "%03d/%03d", pattern + 1, max_int(1, mod->nPatterns));
    sanitize_font_string(state->pat_text, sizeof(state->pat_text), raw);

    snprintf(raw, sizeof(raw), "%03d/%03d", mod->pattern_line + 1, row_total);
    sanitize_font_string(state->row_text, sizeof(state->row_text), raw);
    state->row = clamp_int(mod->pattern_line, 0, max_int(0, row_total - 1));
    state->row_total = row_total;

    for (int i = 0; i < VIS_TOP_SCOPES; ++i) {
        if (i < mod->nChannels) {
            const char *note = mod->channels[i].cur_note ? MODFILE_GetNoteString(mod->channels[i].cur_note) : "...";

            snprintf(raw, sizeof(raw), "CH%d %s", i + 1, note);
            sanitize_font_string(state->voice_text[i], sizeof(state->voice_text[i]), raw);
            state->channel_level[i] = clamp_int((vu_peaks[i] * 255) / 192, 0, 255);
            state->channel_led[i] = blink_state[i];
        } else {
            snprintf(raw, sizeof(raw), "CH%d EMPTY", i + 1);
            sanitize_font_string(state->voice_text[i], sizeof(state->voice_text[i]), raw);
            state->channel_level[i] = 0;
            state->channel_led[i] = 0;
        }
    }
}

static void draw_visualizer(vk_u32 *pixels, MODFILE *mod,
                            const char *filename,
                            const int16_t *buf, int buf_len)
{
    int energy = compute_buffer_energy(buf, buf_len);
    vp::Painter painter(pixels, (int)g_fb.width, (int)g_fb.height, (int)g_fb.stride);
    vp::VisualPlayer player;
    vp::VisualState state;

    if (energy > vis_audio_drive)
        vis_audio_drive += ((energy - vis_audio_drive) * 3 + 3) / 4;
    else if (vis_audio_drive > energy)
        vis_audio_drive -= (vis_audio_drive - energy + 5) / 6;

    compute_master_vu(buf, buf_len);
    update_scope_history(mod);
    update_spectrum(buf, buf_len, vis_audio_drive);
    build_visual_state(&state, mod, filename, buf, buf_len);
    player.render(painter, state, buf, buf_len, scope_history, spectrum_levels, spectrum_peaks);
}

static int visual_update_divisor(void)
{
    if (queue_count >= (QUEUE_TARGET_SAMPLES - PLAY_SAMPLES))
        return VIS_UPDATE_DIV_FAST;
    return VIS_UPDATE_DIV_SLOW;
}

static void render_module_block(MODFILE *mod, int16_t *buf, int samples)
{
    mod->mixingbuf = (u16 *)buf;
    mod->mixingbuflen = samples * 2 * (int)sizeof(int16_t);
    MODFILE_Player(mod);
}

static int queue_push_render_block(MODFILE *mod)
{
    if (queue_count + RENDER_SAMPLES > QUEUE_SAMPLES)
        return 0;

    render_module_block(mod, render_buf, RENDER_SAMPLES);

    for (int i = 0; i < RENDER_SAMPLES; ++i) {
        vk_u32 dst = (queue_wr + (vk_u32)i) % QUEUE_SAMPLES;
        queue_buf[dst * 2u] = render_buf[i * 2];
        queue_buf[dst * 2u + 1u] = render_buf[i * 2 + 1];
    }

    queue_wr = (queue_wr + RENDER_SAMPLES) % QUEUE_SAMPLES;
    queue_count += RENDER_SAMPLES;
    return 1;
}

static int queue_pop_play_block(void)
{
    if (queue_count < PLAY_SAMPLES)
        return 0;

    for (int i = 0; i < PLAY_SAMPLES; ++i) {
        vk_u32 src = (queue_rd + (vk_u32)i) % QUEUE_SAMPLES;
        play_buf[i * 2] = queue_buf[src * 2u];
        play_buf[i * 2 + 1] = queue_buf[src * 2u + 1u];
    }

    queue_rd = (queue_rd + PLAY_SAMPLES) % QUEUE_SAMPLES;
    queue_count -= PLAY_SAMPLES;
    return 1;
}

static void update_visualizer(MODFILE *mod)
{
    compute_vu_levels(mod);
    draw_visualizer(pixbuf, mod, g_current_filename, render_buf, RENDER_SAMPLES);
    vis_frame++;

    memcpy((void *)(unsigned long long)g_fb.base, pixbuf, pixbuf_pixels * sizeof(vk_u32));
}

static void play_live(MODFILE *mod, const char *filename, int sample_rate)
{
    VK_CALL(framebuffer_info, &g_fb);

    if (!g_fb.valid || g_fb.base == 0 || g_fb.width == 0 || g_fb.height == 0) {
        std::cout << "  No framebuffer available.\n";
        exit(1);
    }

    pixbuf_pixels = (vk_usize)g_fb.stride * g_fb.height;
    pixbuf = (vk_u32 *)malloc(pixbuf_pixels * sizeof(vk_u32));
    if (pixbuf == NULL) {
        std::cout << "  Failed to allocate MODPlay backbuffer.\n";
        exit(1);
    }

    memset(pixbuf, 0, pixbuf_pixels * sizeof(vk_u32));
    vis_reset_dynamic_state();
    g_current_filename = filename;

    std::cout << "Playing '" << filename << "'  ("
              << module_format_name(mod->filetype) << ", "
              << mod->nChannels << " ch, "
              << sample_rate << " Hz)\n";
    std::cout << "Press any key to stop.\n\n";

    VK_CALL(snd_set_sample_rate, (vk_u32)sample_rate);
    VK_CALL(snd_set_volume, 255, 255);
    queue_rd = queue_wr = queue_count = 0;
    vis_div_ctr = 0;

    while (queue_count < QUEUE_TARGET_SAMPLES) {
        if (!queue_push_render_block(mod))
            break;
    }

    if (queue_pop_play_block()) {
        VK_CALL(snd_play, play_buf,
                (vk_u32)(PLAY_SAMPLES * 2 * sizeof(int16_t)),
                VK_SND_FORMAT_SIGNED_16);
    }

    for (;;) {
        vk_key_event_t key;

        if (VK_CALL(poll_key, &key) && key.pressed)
            break;

        if (!VK_CALL(snd_is_playing) && queue_pop_play_block()) {
            VK_CALL(snd_play, play_buf,
                    (vk_u32)(PLAY_SAMPLES * 2 * sizeof(int16_t)),
                    VK_SND_FORMAT_SIGNED_16);
        }

        for (int i = 0; i < 2 && queue_count < QUEUE_TARGET_SAMPLES; ++i) {
            if (!queue_push_render_block(mod))
                break;
        }

        if (!VK_CALL(snd_is_playing) && queue_count >= PLAY_SAMPLES) {
            if (queue_pop_play_block()) {
                VK_CALL(snd_play, play_buf,
                        (vk_u32)(PLAY_SAMPLES * 2 * sizeof(int16_t)),
                        VK_SND_FORMAT_SIGNED_16);
            }
        }

        if (queue_count >= VIS_MIN_QUEUE &&
            (vis_div_ctr++ % (vk_u32)visual_update_divisor()) == 0) {
            update_visualizer(mod);
        }

        VK_CALL(yield);
    }

    VK_CALL(snd_stop);
    free(pixbuf);
    pixbuf = NULL;
    pixbuf_pixels = 0;
    g_current_filename = NULL;
    std::cout << "\nStopped.\n";
}

int main(int argc, char *argv[])
{
    //const char *filename = (argc > 1) ? argv[1] : "UNREALPM.S3M";
    const char *filename = (argc > 1) ? argv[1] : "makemove.mod";
    int sample_rate = DEFAULT_SAMPLE_RATE;
    MODFILE mod;
    FILE *f = fopen(filename, "rb");

    if (!f) {
        std::cout << "Error: cannot open '" << filename << "'\n";
        return 1;
    }

    fseek(f, 0, SEEK_END);
    long tune_len = ftell(f);
    fseek(f, 0, SEEK_SET);

    if (tune_len <= 0) {
        std::cout << "Error: empty or unreadable file\n";
        fclose(f);
        return 1;
    }

    uint8_t *tune = (uint8_t *)malloc((size_t)tune_len);
    if (!tune) {
        std::cout << "Error: out of memory\n";
        fclose(f);
        return 1;
    }

    fread(tune, 1, (size_t)tune_len, f);
    fclose(f);

    MODFILE_Init(&mod);
    if (MODFILE_Set(tune, (int)tune_len, &mod) < 0) {
        std::cout << "Error: '" << filename << "' is not a supported module file\n";
        free(tune);
        return 1;
    }

    mod.musicvolume = 64;
    mod.sfxvolume = 64;
    MODFILE_SetFormat(&mod, sample_rate, 2, 16, TRUE);
    MODFILE_Start(&mod);
    play_live(&mod, filename, sample_rate);
    MODFILE_Stop(&mod);
    MODFILE_Free(&mod);

    free(tune);
    return 0;
}