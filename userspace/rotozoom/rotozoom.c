/*
 * rotozoomer.c - Classic demoscene rotozoomer effect
 *
 * Ported from flightcrank/demo-effects rotozoomer to vkernel API.
 * Loads smile.bmp as the source texture, renders at half resolution
 * into a back buffer, then nearest-neighbour 2x stretches to the
 * real framebuffer on each flip — one pixel becomes a 2x2 block.
 *
 * Standard C (newlib) is used for all non-kernel operations.
 * vk_get_api() is used only for fb info, poll_key, and yield.
 */

#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "vk.h"

#define M_PI 3.14159265358979323846

/* ------------------------------------------------------------------ */
/* BMP loader — supports 24-bpp and 32-bpp uncompressed DIBs          */
/* Pixels are stored directly in FB-native format so no conversion    */
/* is needed at sample time.                                           */
/* ------------------------------------------------------------------ */

#pragma pack(push, 1)
typedef struct {
    uint16_t signature;   /* 'BM' */
    uint32_t file_size;
    uint16_t reserved1;
    uint16_t reserved2;
    uint32_t pixel_offset;
} bmp_file_header_t;

typedef struct {
    uint32_t header_size;
    int32_t  width;
    int32_t  height;      /* positive = bottom-up, negative = top-down */
    uint16_t planes;
    uint16_t bpp;         /* 24 or 32 supported */
    uint32_t compression; /* 0 = BI_RGB */
    uint32_t image_size;
    int32_t  x_ppm;
    int32_t  y_ppm;
    uint32_t colors_used;
    uint32_t colors_important;
} bmp_dib_header_t;
#pragma pack(pop)

typedef struct {
    uint32_t *pixels;   /* FB-native packed, row-major, top-down */
    int       width;
    int       height;
} texture_t;

/* Pack RGB into the FB's native word order once, at load time. */
static inline uint32_t pack_native(vk_pixel_format_t fmt,
                                   uint8_t r, uint8_t g, uint8_t b)
{
    return ((uint32_t)r << 16) | ((uint32_t)g << 8) | b;
}

/* Count trailing zero bits — used to find channel shift from a bitmask */
static inline int mask_shift(uint32_t mask)
{
    if (!mask) return 0;
    int s = 0;
    while (!(mask & 1)) { mask >>= 1; s++; }
    return s;
}
 
/* Expand an N-bit channel value to 8 bits */
static inline uint8_t expand_channel(uint32_t word, uint32_t mask, int shift)
{
    uint32_t val = (word & mask) >> shift;
    uint32_t maxval = mask >> shift;
    if (!maxval) return 0;
    /* Scale to 8-bit: val * 255 / maxval */
    return (uint8_t)(val * 255u / maxval);
}

static int load_bmp(const char *path, texture_t *out, vk_pixel_format_t fmt)
{
    FILE *f = fopen(path, "rb");
    if (!f) { fprintf(stderr, "rotozoomer: cannot open %s\n", path); return 0; }
 
    bmp_file_header_t fh;
    bmp_dib_header_t  dh;
 
    if (fread(&fh, sizeof(fh), 1, f) != 1 || fh.signature != 0x4D42) {
        fprintf(stderr, "rotozoomer: not a BMP: %s\n", path);
        fclose(f); return 0;
    }
    if (fread(&dh, sizeof(dh), 1, f) != 1) {
        fprintf(stderr, "rotozoomer: truncated header: %s\n", path);
        fclose(f); return 0;
    }
 
    /* Supported modes:
     *   compression=0 (BI_RGB):       16, 24, or 32 bpp
     *   compression=3 (BI_BITFIELDS): 16 or 32 bpp with explicit masks  */
    int use_bitfields = (dh.compression == 3);
    int bi_rgb        = (dh.compression == 0);
 
    if (!bi_rgb && !use_bitfields) {
        fprintf(stderr, "rotozoomer: unsupported BMP compression=%u\n",
                dh.compression);
        fclose(f); return 0;
    }
    if (dh.bpp != 16 && dh.bpp != 24 && dh.bpp != 32) {
        fprintf(stderr, "rotozoomer: unsupported BMP bpp=%u\n", dh.bpp);
        fclose(f); return 0;
    }
 
    /* Read channel masks.
     * For BI_BITFIELDS they follow immediately after the DIB header.
     * For BI_RGB 16-bpp the implicit masks are RGB555: 0x7C00/0x03E0/0x001F.
     * For BI_RGB 24/32-bpp we use the masks just as sentinels and handle
     * those separately in the pixel loop for clarity.                    */
    uint32_t rmask = 0, gmask = 0, bmask = 0;
    if (use_bitfields) {
        /* Masks are at offset 54 (right after the 40-byte BITMAPINFOHEADER) */
        fseek(f, 14 + 40, SEEK_SET);
        fread(&rmask, 4, 1, f);
        fread(&gmask, 4, 1, f);
        fread(&bmask, 4, 1, f);
    } else if (dh.bpp == 16) {
        /* BI_RGB 16-bpp implicit RGB555 */
        rmask = 0x7C00; gmask = 0x03E0; bmask = 0x001F;
    }
 
    int rshift = mask_shift(rmask);
    int gshift = mask_shift(gmask);
    int bshift = mask_shift(bmask);
 
    int w          = dh.width;
    int h          = abs(dh.height);
    int bottom_up  = (dh.height > 0);
    int bytes_pp   = dh.bpp / 8;
    int row_stride = (w * bytes_pp + 3) & ~3;
 
    uint32_t *buf = (uint32_t *)malloc((size_t)w * h * sizeof(uint32_t));
    if (!buf) { fprintf(stderr, "rotozoomer: OOM texture\n"); fclose(f); return 0; }
 
    fseek(f, (long)fh.pixel_offset, SEEK_SET);
    uint8_t *row_buf = (uint8_t *)malloc((size_t)row_stride);
    if (!row_buf) { free(buf); fclose(f); return 0; }
 
    for (int y = 0; y < h; y++) {
        int dst_y = bottom_up ? (h - 1 - y) : y;
        if (fread(row_buf, 1, (size_t)row_stride, f) != (size_t)row_stride) {
            fprintf(stderr, "rotozoomer: EOF at row %d\n", y);
            free(row_buf); free(buf); fclose(f); return 0;
        }
 
        for (int x = 0; x < w; x++) {
            uint8_t r, g, b;
 
            if (dh.bpp == 16) {
                /* 16-bpp: read a little-endian 16-bit word, apply masks */
                uint16_t word = (uint16_t)(row_buf[x * 2] |
                                           (row_buf[x * 2 + 1] << 8));
                r = expand_channel(word, rmask, rshift);
                g = expand_channel(word, gmask, gshift);
                b = expand_channel(word, bmask, bshift);
            } else if (dh.bpp == 24) {
                /* 24-bpp BI_RGB: on-disk order is always BGR */
                b = row_buf[x * 3 + 0];
                g = row_buf[x * 3 + 1];
                r = row_buf[x * 3 + 2];
            } else {
                /* 32-bpp: use masks if BI_BITFIELDS, else assume BGR0 */
                uint32_t word = (uint32_t)row_buf[x * 4    ]        |
                                ((uint32_t)row_buf[x * 4 + 1] <<  8) |
                                ((uint32_t)row_buf[x * 4 + 2] << 16) |
                                ((uint32_t)row_buf[x * 4 + 3] << 24);
                if (use_bitfields) {
                    r = expand_channel(word, rmask, rshift);
                    g = expand_channel(word, gmask, gshift);
                    b = expand_channel(word, bmask, bshift);
                } else {
                    b = (word      ) & 0xFF;
                    g = (word >>  8) & 0xFF;
                    r = (word >> 16) & 0xFF;
                }
            }
 
            buf[dst_y * w + x] = pack_native(fmt, r, g, b);
        }
    }
 
    free(row_buf);
    fclose(f);
    out->pixels = buf;
    out->width  = w;
    out->height = h;
    return 1;
}

/* ------------------------------------------------------------------ */
/* Texture sampler — pixels already in FB-native format               */
/* ------------------------------------------------------------------ */

static inline uint32_t tex_sample(const texture_t *tex, int tx, int ty)
{
    tx = ((tx % tex->width)  + tex->width)  % tex->width;
    ty = ((ty % tex->height) + tex->height) % tex->height;
    return tex->pixels[ty * tex->width + tx];
}

/* ------------------------------------------------------------------ */
/* 2x nearest-neighbour stretch: small buf -> full FB                 */
/* Each source pixel becomes a 2x2 block on the destination.         */
/* ------------------------------------------------------------------ */

static void stretch2x(const uint32_t *src, vk_u32 sw, vk_u32 sh,
                            uint32_t *dst, vk_u32 dst_stride)
{
    for (vk_u32 sy = 0; sy < sh; sy++) {
        const uint32_t *srow = src + sy * sw;
        uint32_t *drow0 = dst + (sy * 2    ) * dst_stride;
        uint32_t *drow1 = dst + (sy * 2 + 1) * dst_stride;
        for (vk_u32 sx = 0; sx < sw; sx++) {
            uint32_t p = srow[sx];
            drow0[sx * 2    ] = p;
            drow0[sx * 2 + 1] = p;
            drow1[sx * 2    ] = p;
            drow1[sx * 2 + 1] = p;
        }
    }
}

/* ------------------------------------------------------------------ */
/* Pre-computed sin table (360 entries, one per degree)               */
/* ------------------------------------------------------------------ */

static float sin_t[360];

static void build_sin_table(void)
{
    for (int i = 0; i < 360; i++)
        sin_t[i] = sinf((float)i * (float)M_PI / 180.0f);
}

static inline float lsin(int deg) { return sin_t[((deg % 360) + 360) % 360]; }
static inline float lcos(int deg) { return sin_t[((deg + 90) % 360 + 360) % 360]; }

/* ------------------------------------------------------------------ */
/* Entry point                                                         */
/* ------------------------------------------------------------------ */

int main(int argc, char **argv)
{
    (void)argc; (void)argv;

    vk_framebuffer_info_t fb;
    vk_get_api()->vk_framebuffer_info(&fb);

    if (!fb.valid) {
        fprintf(stderr, "rotozoomer: no framebuffer available\n");
        return 1;
    }

    const vk_u32 FB_W   = fb.width;
    const vk_u32 FB_H   = fb.height;
    const vk_u32 stride = fb.stride;          /* FB row pitch in pixels */
    uint32_t    *fb_ptr = (uint32_t *)(uintptr_t)fb.base;

    /* Render at half resolution — 4x fewer pixels to compute */
    const vk_u32 RW = FB_W / 2;
    const vk_u32 RH = FB_H / 2;

    /* Load BMP converted straight to FB-native pixel format */
    texture_t tex;
    if (!load_bmp("head.bmp", &tex, fb.format))
        return 1;

    build_sin_table();

    /* Small back buffer for the rotozoom render pass */
    size_t   small_bytes = (size_t)RW * RH * sizeof(uint32_t);
    uint32_t *backbuf    = (uint32_t *)malloc(small_bytes);
    if (!backbuf) {
        fprintf(stderr, "rotozoomer: out of memory for back buffer\n");
        free(tex.pixels);
        return 1;
    }

    /* Centre of the render resolution */
    const float cx = (float)(RW / 2);
    const float cy = (float)(RH / 2);

    int angle  = 0;
    float zoom_d = 0.0f; /* fractional degrees so step can vary smoothly */
    /* Orbit parameters: angle in degrees and radius in texture pixels */
    float orbit_angle = 0.0f;
    float orbit_radius = (RW < RH ? (float)RW : (float)RH) * 0.25f;

    while (1) {
        /* ESC or Q to quit */
        vk_key_event_t ke;
        if (vk_get_api()->vk_poll_key(&ke) && ke.pressed) {
            if (ke.ascii == 27 || ke.ascii == 'q' || ke.ascii == 'Q')
                break;
        }

        /* Zoom oscillates between 0.5x and 2.5x; use float-based sine
         * so we can vary the zoom step smoothly and slow near peaks. */
        float scale = 1.4f + sinf(zoom_d * (float)M_PI / 180.0f) * 1.35f;
        //if (scale < 0.1f) scale = 0.1f;

        float cos_a = lcos(angle) / scale;
        float sin_a = lsin(angle) / scale;

        /* Compute orbit offset (circular motion) and add to sampling origin */
        float orbit_rad = orbit_angle * (float)M_PI / 180.0f;
        float orbit_dx = cosf(orbit_rad) * orbit_radius;
        float orbit_dy = sinf(orbit_rad) * orbit_radius;

        float row_tx = -cos_a * cx + sin_a * cy + orbit_dx;
        float row_ty = -sin_a * cx - cos_a * cy + orbit_dy;

        /* Rotozoom into the small back buffer (tight stride = RW) */
        for (vk_u32 sy = 0; sy < RH; sy++) {
            float tx = row_tx;
            float ty = row_ty;
            uint32_t *row = backbuf + sy * RW;

            for (vk_u32 sx = 0; sx < RW; sx++) {
                row[sx] = tex_sample(&tex, (int)tx, (int)ty);
                tx += cos_a;
                ty += sin_a;
            }

            row_tx -= sin_a;
            row_ty += cos_a;
        }

        /* Stretch 2x directly into the real framebuffer */
        stretch2x(backbuf, RW, RH, fb_ptr, stride);

        angle = (angle + 1) % 360;

        /* Vary zoom step so it slows near the top/bottom of the sine wave.
         * The cosine is zero at peaks, so using |cos| as a multiplier
         * makes the step small at peaks and larger near zero crossings. */
        float cos_z = cosf(zoom_d * (float)M_PI / 180.0f);
        float step_mul = 0.3f + 0.8f * fabsf(cos_z); /* in [0.2, 1.0] */
        float zoom_step = 2.0f * step_mul;
        zoom_d = fmodf(zoom_d + zoom_step, 360.0f);
        /* Advance orbit angle for circular motion */
        orbit_angle = fmodf(orbit_angle + 3.5f, 360.0f);
    }

    free(backbuf);
    free(tex.pixels);
    return 0;
}