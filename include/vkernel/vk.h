/*
 * vkernel - UEFI Microkernel
 * Copyright (C) 2026 vkernel authors
 *
 * vk.h - Kernel API table passed to loaded ELF programs
 *
 * This header is the single source of truth for the kernel/userspace
 * ABI. Userspace code should include it directly, or via the thin
 * wrapper in userspace/include/vk.h, so the build always sees the
 * current layout.
 *
 * The kernel fills in a vk_api struct and passes a pointer to it
 * as the first argument of every ELF entry point:
 *
 *   int _start(const vk_api_t* api);
 */

#ifndef VKERNEL_VK_H
#define VKERNEL_VK_H

#ifdef __cplusplus
extern "C" {
#endif

typedef unsigned long long vk_u64;
typedef long long          vk_i64;
typedef unsigned int       vk_u32;
typedef int                vk_i32;
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

typedef struct vk_key_event {
    vk_u32 scancode;   /* PS/2 scan code set 1 make code (0x01-0x58) */
    vk_u32 pressed;    /* 1 = key down (make), 0 = key up (break)   */
    char   ascii;      /* ASCII translation if available, '\0' else */
    char   _pad[3];
    vk_u32 modifiers;  /* bit 0=shift, bit 1=ctrl, bit 2=alt         */
} vk_key_event_t;

typedef struct vk_mouse_event {
    vk_i32 dx;         /* relative X movement (pixels)                */
    vk_i32 dy;         /* relative Y movement, positive = down        */
    vk_u32 buttons;    /* bit 0=left, bit 1=right, bit 2=middle       */
} vk_mouse_event_t;

typedef vk_u64 vk_file_handle_t;

/* ============================================================
 * Add new fields only at the END to preserve ABI compatibility.
 * Bump VK_API_VERSION when the layout changes in a breaking way.
 * ============================================================ */

typedef struct vk_api {
    /* ---- header ---- */
    vk_u64 api_version;

    /* ---- console output ---- */
    void (*vk_puts)(const char* s);
    void (*vk_putc)(char c);
    void (*vk_put_hex)(vk_u64 v);
    void (*vk_put_dec)(vk_u64 v);
    void (*vk_clear)(void);

    /* ---- console input ---- */
    char (*vk_getc)(void);
    char (*vk_try_getc)(void);

    /* ---- memory ---- */
    void* (*vk_malloc)(vk_usize size);
    void  (*vk_free)(void* ptr);
    void* (*vk_memset)(void* dest, int c, vk_usize n);
    void* (*vk_memcpy)(void* dest, const void* src, vk_usize n);
    void* (*vk_memmove)(void* dest, const void* src, vk_usize n);
    void* (*vk_memchr)(const void* ptr, int ch, vk_usize n);
    int   (*vk_memcmp)(const void* lhs, const void* rhs, vk_usize n);
    void* (*vk_realloc)(void* ptr, vk_usize size);

    /* ---- filesystem / ramfs ---- */
    int         (*vk_file_exists)(const char* name);
    vk_usize    (*vk_file_size)(const char* name);
    vk_usize    (*vk_file_read)(const char* name, void* buf, vk_usize buf_size);

    /* ---- process ---- */
    void (*vk_exit)(int code);
    void (*vk_yield)(void);
    void (*vk_sleep)(vk_u64 ticks);

    /* ---- framebuffer ---- */
    void (*vk_framebuffer_info)(vk_framebuffer_info_t* out);

    /* ---- file streams and filesystem ops ---- */
    vk_file_handle_t (*vk_file_open)(const char* path, const char* mode);
    int              (*vk_file_close)(vk_file_handle_t handle);
    vk_usize         (*vk_file_read_handle)(vk_file_handle_t handle, void* buf, vk_usize buf_size);
    vk_usize         (*vk_file_write_handle)(vk_file_handle_t handle, const void* buf, vk_usize buf_size);
    int              (*vk_file_seek)(vk_file_handle_t handle, vk_i64 offset, int whence);
    vk_i64           (*vk_file_tell)(vk_file_handle_t handle);
    int              (*vk_file_eof)(vk_file_handle_t handle);
    int              (*vk_file_error)(vk_file_handle_t handle);
    int              (*vk_file_flush)(vk_file_handle_t handle);
    int              (*vk_file_remove)(const char* path);
    int              (*vk_file_rename)(const char* old_path, const char* new_path);

    /* ---- process utilities ---- */
    vk_i64 (*vk_run)(const char* path);
    vk_u64 (*vk_tick_count)(void);
    void (*vk_dump_memory)(void);
    void (*vk_dump_tasks)(void);
    void (*vk_dump_idt)(void);
    void (*vk_reboot)(void);

    /* ---- raw keyboard input ---- */
    int    (*vk_poll_key)(vk_key_event_t* out);
    vk_u32 (*vk_ticks_per_sec)(void);

    /* ---- task synchronisation ---- */
    void (*vk_wait_task)(vk_i64 task_id);

    /* ---- sound ---- */
    int  (*vk_snd_play)(const void* samples, vk_u32 length, vk_u32 format);
    void (*vk_snd_stop)(void);
    int  (*vk_snd_is_playing)(void);
    int  (*vk_snd_set_sample_rate)(vk_u32 rate_hz);
    void (*vk_snd_set_volume)(vk_u32 left, vk_u32 right);

    /* ---- driver management ---- */
    int  (*vk_drv_load)(const char* name);
    int  (*vk_drv_unload)(const char* name);

    /* ---- mouse input ---- */
    int  (*vk_poll_mouse)(vk_mouse_event_t* out);

} vk_api_t;

/* Current API version */
#define VK_API_VERSION 12ULL

#if defined(_MSC_VER)
__declspec(selectany) const vk_api_t* _vk_api_ptr = 0;
#elif defined(__GNUC__) || defined(__clang__)
__attribute__((weak)) const vk_api_t* _vk_api_ptr = 0;
#else

/* Fallback: exactly one TU must define VK_IMPLEMENT */
#if defined(VK_IMPLEMENT)
const vk_api_t* _vk_api_ptr = 0;
#else
extern const vk_api_t* _vk_api_ptr;
#endif
#endif

static inline void vk_init(const vk_api_t* api) {
    _vk_api_ptr = api;
}

static inline const vk_api_t* vk_get_api(void) {
    return _vk_api_ptr;
}

/* Sound format constants for vk_snd_play() */
#define VK_SND_FORMAT_UNSIGNED_8   0
#define VK_SND_FORMAT_SIGNED_16    1

#ifdef __cplusplus
} /* extern "C" */

namespace vk {
namespace kernel_api {

void init();
auto get_api() -> const vk_api_t*;

} // namespace kernel_api
} // namespace vk
#endif

#endif /* VKERNEL_VK_H */
