/*
 * vid_vk.c - Video system for Chocolate Quake on vkernel
 * Replaces vid_window.c and vid_buffers.c
 *
 * Quake's software renderer writes into an 8-bit paletted buffer (vid.buffer).
 * We convert it to 32-bit XRGB and blit it to the VK native framebuffer,
 * scaling from the render resolution to the screen size.
 */

#include "vid_window.h"
#include "vid_buffers.h"
#include "d_local.h"
#include "render.h"
#include "sys.h"
#include "input.h"
#include "cvar.h"
#include "config.h"

#include <stdlib.h>
#include <string.h>

#include "../include/vk.h"

/* ---------------------------------------------------------------
 * Pixel format defined in vid_main.c; we need it here too.
 * We just use a fixed BGRX layout that matches what VK provides.
 * --------------------------------------------------------------- */
extern const u32 pixel_format; /* defined in vid_main.c (SDL_PIXELFORMAT_ARGB8888) */

/* ---------------------------------------------------------------
 * State
 * --------------------------------------------------------------- */

static vk_framebuffer_info_t vk_fb;

/* 32-bit palette converted from Quake's 8-bit palette.
 * Format depends on vk_fb.format: RGBX or BGRX.  */
static u32 palette_32[256];

/* The 8-bit paletted render surface that Quake draws into */
static byte *screen_buf = NULL;

/* Hunk mark for the z-buffer allocation */
static i32 VID_highhunkmark;
static i32 vid_surfcachesize;

static int fb_pixel_bytes; /* bytes per pixel in framebuffer */

/* ---------------------------------------------------------------
 * Palette helpers
 * --------------------------------------------------------------- */

static u32 make_pixel(byte r, byte g, byte b)
{
    if (vk_fb.format == VK_PIXEL_FORMAT_RGBX_8BPP)
        return ((u32)r << 16) | ((u32)g << 8) | (u32)b;
    else /* BGRX */
        return ((u32)b << 16) | ((u32)g << 8) | (u32)r;
}

void VID_SetPalette(const byte *palette)
{
    for (int i = 0; i < 256; i++) {
        byte r = palette[i * 3]     & ~3;
        byte g = palette[i * 3 + 1] & ~3;
        byte b = palette[i * 3 + 2] & ~3;
        palette_32[i] = make_pixel(r, g, b);
    }
}

void VID_ShiftPalette(const byte *palette)
{
    VID_SetPalette(palette);
}

/* ---------------------------------------------------------------
 * Buffer management — mirrors vid_buffers.c
 * --------------------------------------------------------------- */

void VID_FreeBuffers(void)
{
    if (screen_buf) {
        free(screen_buf);
        screen_buf = NULL;
        vid.buffer = NULL;
    }
    if (d_pzbuffer) {
        D_FlushCaches();
        Hunk_FreeToHighMark(VID_highhunkmark);
        d_pzbuffer = NULL;
    }
}

void VID_ReallocBuffers(void)
{
    VID_FreeBuffers();

    vid_surfcachesize = D_SurfaceCacheForRes(vid.width, vid.height);

    /* Allocate 8-bit render buffer */
    screen_buf = (byte *)malloc((size_t)(vid.width * vid.height));
    if (!screen_buf)
        Sys_Error("VID_ReallocBuffers: out of memory for screen buffer\n");
    memset(screen_buf, 0, (size_t)(vid.width * vid.height));
    vid.buffer = screen_buf;

    /* Allocate z-buffer + surface cache via Hunk */
    i32 chunk = (i32)(vid.width * vid.height * sizeof(*d_pzbuffer))
              + vid_surfcachesize;
    VID_highhunkmark = Hunk_HighMark();
    d_pzbuffer = Hunk_HighAllocName(chunk, "video");
    if (!d_pzbuffer)
        Sys_Error("VID_ReallocBuffers: not enough memory for video mode\n");

    byte *zbuf = (byte *)d_pzbuffer;
    size_t cache_offset = (size_t)vid.width * (size_t)vid.height
                          * sizeof(*d_pzbuffer);
    D_InitCaches(zbuf + cache_offset, vid_surfcachesize);
}

/* ---------------------------------------------------------------
 * Buffer access — no-ops on VK (no SDL_MUSTLOCK)
 * --------------------------------------------------------------- */

void VID_LockBuffer(void)   {}
void VID_UnlockBuffer(void) {}

/* ---------------------------------------------------------------
 * Blit 8-bit render buffer → VK framebuffer with scaling
 * --------------------------------------------------------------- */

/*
 * VID_UpdateTexture — signature kept for vid_main.c call site.
 * `texture` is always NULL on VK; we blit directly to fb.
 */
void VID_UpdateTexture(SDL_Texture *texture, vrect_t *rect)
{
    (void)texture;

    if (!screen_buf || !vk_fb.valid || !vk_fb.base)
        return;

    const int rw = (int)vid.width;
    const int rh = (int)vid.height;
    const int fw = (int)vk_fb.width;
    const int fh = (int)vk_fb.height;

    /* Scale factors (fixed-point 16.16) */
    int x_scale = (rw << 16) / fw;
    int y_scale = (rh << 16) / fh;

    /* Clip to dirty rect */
    int rx0 = rect->x, ry0 = rect->y;
    int rw1 = rect->width, rh1 = rect->height;
    if (rx0 < 0) { rw1 += rx0; rx0 = 0; }
    if (ry0 < 0) { rh1 += ry0; ry0 = 0; }
    if (rx0 + rw1 > rw) rw1 = rw - rx0;
    if (ry0 + rh1 > rh) rh1 = ry0 - ry0;

    /* Map render rect to framebuffer rect */
    int fx0 = (rx0 << 16) / x_scale;
    int fy0 = (ry0 << 16) / y_scale;
    int fx1 = ((rx0 + rw1) << 16) / x_scale;
    int fy1 = ((ry0 + rh1) << 16) / y_scale;
    if (fx1 > fw) fx1 = fw;
    if (fy1 > fh) fy1 = fh;

    u32 *fb = (u32 *)vk_fb.base;
    u32 fb_stride_u32 = vk_fb.stride / 4;

    for (int fy = fy0; fy < fy1; fy++) {
        int sy = ((fy * y_scale) >> 16);
        if (sy >= rh) sy = rh - 1;
        const byte *src_row = screen_buf + sy * rw;
        u32 *dst_row = fb + fy * fb_stride_u32;
        for (int fx = fx0; fx < fx1; fx++) {
            int sx = ((fx * x_scale) >> 16);
            if (sx >= rw) sx = rw - 1;
            dst_row[fx] = palette_32[src_row[sx]];
        }
    }
}

/* ---------------------------------------------------------------
 * Window lifecycle — maps to VK framebuffer setup
 * --------------------------------------------------------------- */

void VID_InitWindow(void)
{
    VK_CALL(framebuffer_info, &vk_fb);
    if (!vk_fb.valid) {
        Sys_Error("VID_InitWindow: no valid VK framebuffer\n");
    }
    fb_pixel_bytes = 4; /* VK framebuffers are always 32-bit */

    /* Set render resolution to 320×200 (Quake default) */
    vid.width  = 320;
    vid.height = 200;
    vid.aspect = (float)vid.height / (float)vid.width;
    vid.numpages = 1;
    vid.colormap = host_colormap;
}

void VID_ShutdownWindow(void)
{
    VID_FreeBuffers();
}

void VID_ResizeScreen(void)
{
    VID_ReallocBuffers();
}

void VID_UpdateWindow(vrect_t *rect)
{
    /* Full-screen blit when rect covers everything */
    vrect_t full = { .x = 0, .y = 0,
                     .width  = (i32)vid.width,
                     .height = (i32)vid.height };
    if (!rect) rect = &full;
    VID_UpdateTexture(NULL, rect);
}

void VID_MinimizeWindow(void) {}

/* ---------------------------------------------------------------
 * Mouse / grab stubs (no window manager on VK)
 * --------------------------------------------------------------- */

qboolean VID_WindowedMouse(void)     { return false; }
void     VID_ToggleMouseGrab(void)   {}
void     VID_HandlePause(qboolean p) { (void)p; }
