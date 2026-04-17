/*
 * vkernel userspace - framebuffer demo
 * Copyright (C) 2026 vkernel authors
 *
 * framebuffer.c - Freestanding framebuffer painting example
 *
 * Build: see Makefile (Linux) or framebuffer.vcxproj (Visual Studio).
 * Run:   vk> run framebuffer.elf
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

static void paint_framebuffer(const vk_framebuffer_info_t* fb) {
    vk_u32* pixels = (vk_u32*)(unsigned long long)fb->base;

    for (vk_u32 y = 0; y < fb->height; ++y) {
        for (vk_u32 x = 0; x < fb->width; ++x) {
            vk_u32 r = (fb->width  > 1) ? (x * 255u) / (fb->width  - 1u) : 0u;
            vk_u32 g = (fb->height > 1) ? (y * 255u) / (fb->height - 1u) : 0u;
            vk_u32 b = ((x ^ y) & 0xFFu);

            if ((((x / 48u) + (y / 48u)) & 1u) != 0u) {
                r = 255u - r;
                g = 255u - g;
            }

            pixels[(vk_usize)y * fb->stride + x] = pack_pixel(
                (unsigned char)r,
                (unsigned char)g,
                (unsigned char)b,
                fb->format
            );
        }
    }

    /* Bright frame and center band for an obvious visual result. */
    for (vk_u32 x = 0; x < fb->width; ++x) {
        pixels[x] = pack_pixel(255, 255, 255, fb->format);
        pixels[(vk_usize)(fb->height - 1u) * fb->stride + x] = pack_pixel(255, 255, 255, fb->format);
    }
    for (vk_u32 y = 0; y < fb->height; ++y) {
        pixels[(vk_usize)y * fb->stride] = pack_pixel(255, 255, 255, fb->format);
        pixels[(vk_usize)y * fb->stride + (fb->width - 1u)] = pack_pixel(255, 255, 255, fb->format);
    }

    vk_u32 band_top = fb->height / 3u;
    vk_u32 band_bottom = (fb->height * 2u) / 3u;
    vk_u32 band_left = fb->width / 6u;
    vk_u32 band_right = (fb->width * 5u) / 6u;
    for (vk_u32 y = band_top; y < band_bottom; ++y) {
        for (vk_u32 x = band_left; x < band_right; ++x) {
            pixels[(vk_usize)y * fb->stride + x] = pack_pixel(32, 160, 255, fb->format);
        }
    }
}

int _start(const vk_api_t* api) {
    vk_init(api);

    printf("Framebuffer demo\n");
    printf("  Kernel API version : %llu\n", (unsigned long long)api->api_version);

    vk_framebuffer_info_t fb = { 0 };
    vk_get_framebuffer_info(&fb);

    if (!fb.valid || fb.base == 0 || fb.width == 0 || fb.height == 0) {
        printf("  No framebuffer available.\n");
        return 1;
    }

    printf("  Framebuffer        : %ux%u\n", fb.width, fb.height);
    printf("  Stride             : %u\n", fb.stride);
    printf("  Format             : %u\n", (unsigned int)fb.format);
    printf("  Base               : %p\n", (void*)(unsigned long long)fb.base);
    printf("  Painting screen...\n");

    paint_framebuffer(&fb);

    printf("  Done.\n");
    return 0;
}
