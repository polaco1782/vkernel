/*
 * main_vk.c - MODPlay vkernel front-end
 * Copyright (C) 2026 vkernel authors
 *
 * Supports both MOD and S3M formats.
 * Features VU meters, oscilloscope, and blinking activity lights.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

#include "../include/vk.h"
#include "modplay.h"

/* ---- tunables ---- */
#define DEFAULT_SAMPLE_RATE  44100
#define BUFFER_SAMPLES       1024

static int16_t audio_buf[BUFFER_SAMPLES * 2];
vk_u32 *pixbuf;

/* ================================================================
 *  Framebuffer helpers
 * ================================================================ */

static vk_framebuffer_info_t g_fb;

static inline vk_u32 pack_pixel(uint8_t r, uint8_t g, uint8_t b) {
    return ((vk_u32)r << 16) | ((vk_u32)g << 8) | (vk_u32)b;
}

static inline void put_pixel(vk_u32 *pixels, int x, int y, vk_u32 color) {
    if (x >= 0 && x < (int)g_fb.width && y >= 0 && y < (int)g_fb.height)
        pixels[(vk_usize)y * g_fb.stride + x] = color;
}

static void fill_rect(vk_u32 *pixels, int x0, int y0, int w, int h, vk_u32 color) {
    for (int y = y0; y < y0 + h && y < (int)g_fb.height; y++) {
        if (y < 0) continue;
        for (int x = x0; x < x0 + w && x < (int)g_fb.width; x++) {
            if (x < 0) continue;
            pixels[(vk_usize)y * g_fb.stride + x] = color;
        }
    }
}

/* ================================================================
 *  Visualization: VU Bars + Oscilloscope + Blink Lights
 * ================================================================ */

#define VIS_MAX_CH  32
#define VU_DECAY    2

static int vu_peaks[VIS_MAX_CH];
static int vu_hold[VIS_MAX_CH];
static int vu_hold_timer[VIS_MAX_CH];
static uint8_t blink_state[VIS_MAX_CH];

static vk_u32 channel_color(int ch) {
    static const uint8_t colors[][3] = {
        {0,255,128}, {255,128,0}, {0,200,255}, {255,0,128},
        {128,255,0}, {255,255,0}, {0,128,255}, {255,0,255},
        {64,255,192},{255,192,0}, {0,255,64},  {192,0,255},
        {255,64,64}, {64,255,255},{192,255,0}, {255,0,64},
    };
    int idx = ch % 16;
    return pack_pixel(colors[idx][0], colors[idx][1], colors[idx][2]);
}

static vk_u32 dim_color(vk_u32 col, int brightness) {
    uint8_t r, g, b;
    if (g_fb.format == VK_PIXEL_FORMAT_BGRX_8BPP) {
        b = (col >> 16) & 0xFF;
        g = (col >> 8) & 0xFF;
        r = col & 0xFF;
    } else {
        r = (col >> 16) & 0xFF;
        g = (col >> 8) & 0xFF;
        b = col & 0xFF;
    }
    r = (uint8_t)((r * brightness) >> 8);
    g = (uint8_t)((g * brightness) >> 8);
    b = (uint8_t)((b * brightness) >> 8);
    return pack_pixel(r, g, b);
}

static void compute_vu_levels(ModPlayerStatus_t *mp_st) {
    for (int ch = 0; ch < mp_st->channels && ch < VIS_MAX_CH; ch++) {
        PaulaChannel_t *pch = &mp_st->ch[ch].samplegen;
        int level = 0;

        if (pch->sample && pch->period && pch->volume > 0 && pch->age < 0x7FFFFFFF) {
            level = pch->volume * 3;
            if (level > 192) level = 192;
        }

        if (level > vu_peaks[ch])
            vu_peaks[ch] = level;
        else if (vu_peaks[ch] > 0)
            vu_peaks[ch] -= VU_DECAY;

        if (vu_peaks[ch] > vu_hold[ch]) {
            vu_hold[ch] = vu_peaks[ch];
            vu_hold_timer[ch] = 30;
        } else if (vu_hold_timer[ch] > 0) {
            vu_hold_timer[ch]--;
        } else if (vu_hold[ch] > 0) {
            vu_hold[ch] -= 1;
        }

        if (pch->age < 3)
            blink_state[ch] = 255;
        else if (blink_state[ch] > 8)
            blink_state[ch] -= 8;
        else
            blink_state[ch] = 0;
    }
}

static void draw_visualizer(vk_u32 *pixels, ModPlayerStatus_t *mp_st,
                            const int16_t *buf, int buf_len) {
    int nch = mp_st->channels;
    if (nch > VIS_MAX_CH) nch = VIS_MAX_CH;
    int fb_w = (int)g_fb.width;
    int fb_h = (int)g_fb.height;

    /* Clear to dark background */
    vk_u32 bg = pack_pixel(8, 8, 16);
    for (int y = 0; y < fb_h; y++)
        for (int x = 0; x < fb_w; x++)
            pixels[(vk_usize)y * g_fb.stride + x] = bg;

    /* Layout */
    int blink_y = 10;
    int blink_h = 20;
    int vu_y = blink_y + blink_h + 10;
    int vu_h = fb_h * 50 / 100;
    int scope_y = vu_y + vu_h + 10;
    int scope_h = fb_h - scope_y - 10;
    if (scope_h < 20) scope_h = 20;

    int bar_gap = 2;
    int bar_w = (fb_w - 20) / nch - bar_gap;
    if (bar_w < 2) bar_w = 2;
    if (bar_w > 40) bar_w = 40;
    int total_w = nch * (bar_w + bar_gap);
    int x_start = (fb_w - total_w) / 2;

    /* ---- Blink lights ---- */
    for (int ch = 0; ch < nch; ch++) {
        int cx = x_start + ch * (bar_w + bar_gap) + bar_w / 2;
        int cy = blink_y + blink_h / 2;
        int radius = bar_w / 2;
        if (radius > 8) radius = 8;
        if (radius < 2) radius = 2;

        vk_u32 col = dim_color(channel_color(ch), blink_state[ch]);

        for (int dy = -radius; dy <= radius; dy++)
            for (int dx = -radius; dx <= radius; dx++)
                if (dx*dx + dy*dy <= radius*radius)
                    put_pixel(pixels, cx + dx, cy + dy, col);

        if (blink_state[ch] > 128) {
            int r2 = radius + 2;
            vk_u32 glow = dim_color(channel_color(ch), blink_state[ch] / 2);
            for (int dy = -r2; dy <= r2; dy++)
                for (int dx = -r2; dx <= r2; dx++) {
                    int d = dx*dx + dy*dy;
                    if (d > radius*radius && d <= r2*r2)
                        put_pixel(pixels, cx + dx, cy + dy, glow);
                }
        }
    }

    /* ---- VU bars ---- */
    for (int ch = 0; ch < nch; ch++) {
        int bx = x_start + ch * (bar_w + bar_gap);
        int peak_px = vu_peaks[ch] * vu_h / 200;
        if (peak_px > vu_h) peak_px = vu_h;

        for (int py = 0; py < peak_px; py++) {
            int by = vu_y + vu_h - 1 - py;
            int pct = py * 100 / (vu_h > 1 ? vu_h : 1);
            uint8_t r, g, b;
            if (pct < 60) {
                r = 0; g = (uint8_t)(128 + pct * 2); b = 0;
            } else if (pct < 85) {
                r = (uint8_t)((pct - 60) * 10); g = 255; b = 0;
            } else {
                r = 255; g = (uint8_t)(255 - (pct - 85) * 16); b = 0;
            }
            fill_rect(pixels, bx, by, bar_w, 1, pack_pixel(r, g, b));
        }

        /* Peak hold */
        int hold_px = vu_hold[ch] * vu_h / 200;
        if (hold_px > 0 && hold_px <= vu_h) {
            int hy = vu_y + vu_h - 1 - hold_px;
            fill_rect(pixels, bx, hy, bar_w, 2, pack_pixel(255, 255, 255));
        }

        /* Channel indicator dot */
        fill_rect(pixels, bx, vu_y + vu_h + 2, bar_w, 1,
                  dim_color(channel_color(ch), 180));
    }

    /* ---- Oscilloscope (stereo) ---- */
    if (scope_h > 4 && buf_len > 0) {
        int mid_l = scope_y + scope_h / 4;
        int mid_r = scope_y + scope_h * 3 / 4;
        int amplitude = scope_h / 4 - 2;
        if (amplitude < 1) amplitude = 1;

        // vk_u32 grid = pack_pixel(30, 30, 50);
        // for (int x = 0; x < fb_w; x++) {
        //     put_pixel(pixels, x, mid_l, grid);
        //     put_pixel(pixels, x, mid_r, grid);
        // }

        vk_u32 col_l = pack_pixel(0, 200, 255);
        vk_u32 col_r = pack_pixel(255, 100, 0);

        /* Expand the oscilloscope to fill the framebuffer width.
         * Map buffer samples across the full width using a floating
         * scale so the waveform stretches when there are fewer samples
         * than pixels, and decimates when there are more.
         */
        int draw_w = fb_w;
        int count = draw_w;
        double scale = buf_len > 0 ? (double)buf_len / (double)draw_w : 1.0;

        int prev_ly = mid_l, prev_ry = mid_r;

        for (int i = 0; i < count; i++) {
            int si = (int)(i * scale);
            if (si >= buf_len) si = buf_len - 1;

            int sl = buf[si * 2];
            int sr = buf[si * 2 + 1];

            int ly = mid_l - (sl * amplitude / 32768);
            int ry = mid_r - (sr * amplitude / 32768);
            int x = i;

            int y0, y1;
            y0 = prev_ly < ly ? prev_ly : ly;
            y1 = prev_ly > ly ? prev_ly : ly;
            for (int y = y0; y <= y1; y++) put_pixel(pixels, x, y, col_l);

            y0 = prev_ry < ry ? prev_ry : ry;
            y1 = prev_ry > ry ? prev_ry : ry;
            for (int y = y0; y <= y1; y++) put_pixel(pixels, x, y, col_r);

            prev_ly = ly;
            prev_ry = ry;
        }

        //fill_rect(pixels, 2, mid_l - 3, 3, 6, col_l);
        //fill_rect(pixels, 2, mid_r - 3, 3, 6, col_r);
    }

    /* Separator lines */
    vk_u32 sep = pack_pixel(40, 40, 60);
    for (int x = 0; x < fb_w; x++) {
        put_pixel(pixels, x, vu_y - 2, sep);
        put_pixel(pixels, x, scope_y - 2, sep);
    }
}

/* ================================================================ */

static void play_live(ModPlayerStatus_t *mp_ptr, const char *filename,
                      int sample_rate)
{
    VK_CALL(framebuffer_info, &g_fb);

    if (!g_fb.valid || g_fb.base == 0 || g_fb.width == 0 || g_fb.height == 0) {
        VK_CALL(puts, "  No framebuffer available.\n");
        exit(1);
    }

    // 2nd framebuffer
    pixbuf = (vk_u32 *)malloc((size_t)g_fb.width * g_fb.height * sizeof(vk_u32));

    memset(pixbuf, 0, (size_t)g_fb.width * g_fb.height * sizeof(vk_u32));
    memset(vu_peaks, 0, sizeof(vu_peaks));
    memset(vu_hold, 0, sizeof(vu_hold));
    memset(vu_hold_timer, 0, sizeof(vu_hold_timer));
    memset(blink_state, 0, sizeof(blink_state));

    printf("Playing '%s'  (%d ch, %d Hz)\n",
           filename, mp_ptr->channels, sample_rate);
    printf("Press any key to stop.\n\n");

    VK_CALL(snd_set_sample_rate, (vk_u32)sample_rate);
    VK_CALL(snd_set_volume, 255, 255);

    for (;;) {
        vk_key_event_t key;
        if (VK_CALL(poll_key, &key) && key.pressed) break;

        if (!VK_CALL(snd_is_playing)) {
            RenderMOD(audio_buf, BUFFER_SAMPLES);
            VK_CALL(snd_play, audio_buf,
                    (vk_u32)(BUFFER_SAMPLES * 2 * sizeof(int16_t)),
                    VK_SND_FORMAT_SIGNED_16);

            compute_vu_levels(mp_ptr);
            draw_visualizer(pixbuf, mp_ptr, audio_buf, BUFFER_SAMPLES);

            // swap buffers
            memcpy((void *)(unsigned long long)g_fb.base, pixbuf,
                   (size_t)g_fb.width * g_fb.height * sizeof(vk_u32));

        } else {
            VK_CALL(yield);
        }
    }

    VK_CALL(snd_stop);
    printf("\nStopped.\n");
}

/* ================================================================
 *  main
 * ================================================================ */

int main(int argc, char *argv[])
{
    (void)argc; (void)argv;

    const char *filename = "makemove.mod";
    int sample_rate = DEFAULT_SAMPLE_RATE;

    FILE *f = fopen(filename, "rb");
    if (!f) {
        printf("Error: cannot open '%s'\n", filename);
        return 1;
    }

    fseek(f, 0, SEEK_END);
    long tune_len = ftell(f);
    fseek(f, 0, SEEK_SET);

    if (tune_len <= 0) {
        printf("Error: empty or unreadable file\n");
        fclose(f);
        return 1;
    }

    uint8_t *tune = malloc((size_t)tune_len);
    if (!tune) {
        printf("Error: out of memory\n");
        fclose(f);
        return 1;
    }

    fread(tune, 1, (size_t)tune_len, f);
    fclose(f);

    ModPlayerStatus_t *mp_ptr = InitMOD(tune, (uint32_t)sample_rate);

    if (!mp_ptr) {
        printf("Error: '%s' is not a valid MOD file\n", filename);
        free(tune);
        return 1;
    }

    play_live(mp_ptr, filename, sample_rate);

    free(tune);
    return 0;
}
