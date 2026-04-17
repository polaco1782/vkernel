/*
 * vkernel - UEFI Microkernel
 * Copyright (C) 2026 vkernel authors
 *
 * userapi.h - Kernel API table passed to loaded ELF programs
 *
 * This header is intentionally written in C so it can be included
 * from both freestanding C ELF programs and the C++ kernel.
 *
 * The kernel fills in a vk_api struct and passes a pointer to it
 * as the first argument of every ELF entry point:
 *
 *   int _start(const vk_api_t* api);
 */

#ifndef VKERNEL_USERAPI_H
#define VKERNEL_USERAPI_H

#ifdef __cplusplus
extern "C" {
#endif

typedef unsigned long long vk_u64;
typedef unsigned int       vk_u32;
#if defined(_MSC_VER)
typedef unsigned long long vk_usize;
#else
typedef unsigned long      vk_usize;
#endif

typedef enum vk_pixel_format {
    VK_PIXEL_FORMAT_RGBX_8BPP = 0,
    VK_PIXEL_FORMAT_BGRX_8BPP = 1,
    VK_PIXEL_FORMAT_BITMASK   = 2,
    VK_PIXEL_FORMAT_BLT_ONLY  = 3,
} vk_pixel_format_t;

typedef struct vk_framebuffer_info {
    vk_u64            base;
    vk_u32            width;
    vk_u32            height;
    vk_u32            stride;
    vk_pixel_format_t format;
    vk_u32            valid;
} vk_framebuffer_info_t;

/* ============================================================
 * vk_api_t — version 3
 *
 * Add new fields only at the END to preserve ABI compatibility.
 * Bump VK_API_VERSION when the layout changes in a breaking way.
 * ============================================================ */

typedef struct vk_api {
    /* ---- header (offset 0) ---- */
    vk_u64 api_version;

    /* ---- console output (offset 8) ---- */
    void (*puts)(const char* s);
    void (*putc)(char c);
    void (*put_hex)(vk_u64 v);
    void (*put_dec)(vk_u64 v);
    void (*clear)(void);

    /* ---- console input (offset 48) ---- */
    char (*getc)(void);
    char (*try_getc)(void);

    /* ---- memory (offset 64) ---- */
    void* (*malloc)(vk_usize size);
    void  (*free)(void* ptr);
    void* (*memset)(void* dest, int c, vk_usize n);
    void* (*memcpy)(void* dest, const void* src, vk_usize n);

    /* ---- filesystem / ramfs (offset 96) ---- */
    int         (*file_exists)(const char* name);
    vk_usize    (*file_size)(const char* name);
    vk_usize    (*file_read)(const char* name, void* buf, vk_usize buf_size);

    /* ---- process (offset 120) ---- */
    void (*exit)(int code);
    void (*yield)(void);
    void (*sleep)(vk_u64 ticks);

    /* ---- framebuffer (offset 144) ---- */
    void (*framebuffer_info)(vk_framebuffer_info_t* out);

} vk_api_t;

/* Current API version */
#define VK_API_VERSION 3ULL

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* VKERNEL_USERAPI_H */
