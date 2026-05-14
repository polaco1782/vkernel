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

typedef struct vk_task_info {
    vk_u64 id;
    vk_u32 state;       /* 0=ready, 1=running, 2=blocked, 3=terminated */
    vk_u32 cpu;         /* APIC ID while running, VK_TASK_CPU_NONE otherwise */
    vk_u64 cpu_ticks;   /* Scheduler ticks spent running on any CPU */
    char   name[32];
} vk_task_info_t;

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
    int   (*vk_memcmp)(const void* lhs, const void* rhs, vk_usize n);

    /* ---- filesystem / ramfs ---- */
    int         (*vk_file_exists)(const char* name);
    vk_usize    (*vk_file_size)(const char* name);

    /* ---- process ---- */
    void (*vk_exit)(int code);
    void (*vk_yield)(void);
    void (*vk_sleep)(vk_u64 ticks);
    vk_i64 (*vk_run_with_fb)(const char* path, const vk_framebuffer_info_t* fb);
    vk_i64 (*vk_run_auto)(const char* path);

    /* ---- framebuffer ---- */
    void (*vk_framebuffer_info)(vk_framebuffer_info_t* out);

    /* ---- file streams and filesystem ops ---- */
    vk_file_handle_t (*vk_file_open)(const char* path, const char* mode);
    int              (*vk_file_close)(vk_file_handle_t handle);
    vk_usize         (*vk_file_read_handle)(vk_file_handle_t handle, void* buf, vk_usize buf_size);
    vk_usize         (*vk_file_write_handle)(vk_file_handle_t handle, const void* buf, vk_usize buf_size);
    int              (*vk_file_seek)(vk_file_handle_t handle, vk_i64 offset, int whence);
    vk_i64           (*vk_file_tell)(vk_file_handle_t handle);
    int              (*vk_file_remove)(const char* path);

    /* ---- process utilities ---- */
    vk_i64 (*vk_run)(const char* path);
    vk_u64 (*vk_tick_count)(void);

    /* ---- raw keyboard input ---- */
    int    (*vk_poll_key)(vk_key_event_t* out);
    int    (*vk_send_key)(vk_u64 task_id, const vk_key_event_t* ev);
    int    (*vk_set_task_framebuffer)(vk_u64 task_id, const vk_framebuffer_info_t* fb);
    vk_u32 (*vk_ticks_per_sec)(void);

    /* ---- task synchronisation ---- */
    void (*vk_wait_task)(vk_i64 task_id);

    /* ---- sound ---- */
    int  (*vk_snd_play)(const void* samples, vk_u32 length, vk_u32 format);
    void (*vk_snd_stop)(void);
    int  (*vk_snd_is_playing)(void);
    int  (*vk_snd_set_sample_rate)(vk_u32 rate_hz);
    void (*vk_snd_set_volume)(vk_u32 left, vk_u32 right);

    /* ---- software mixer ---- */
    int  (*vk_snd_mix_play)(int channel, const void* data, vk_u32 num_samples,
                             vk_u32 format, vk_u32 sample_rate,
                             vk_u32 vol_left, vk_u32 vol_right);
    void (*vk_snd_mix_stop)(int channel);
    int  (*vk_snd_mix_is_playing)(int channel);
    void (*vk_snd_mix_update)(void);

    /* ---- mouse input ---- */
    int  (*vk_poll_mouse)(vk_mouse_event_t* out);

    /* ---- task stats ---- */
    vk_usize (*vk_task_snapshot)(vk_task_info_t* out, vk_usize max_tasks);

    /* ---- compositor control ---- */
    int (*vk_set_compositor_active)(vk_u32 active);
    int (*vk_set_compositor_default_fb)(const vk_framebuffer_info_t* fb);

    /* ---- kobj ---- */
    vk_usize (*vk_kobj_rpc)(const char* req_json, char* out, vk_usize out_cap);

    /* ---- input routing ---- */
    int (*vk_send_mouse)(vk_u64 task_id, const vk_mouse_event_t* ev);

    /* ---- process command line ---- */
    vk_usize (*vk_get_cmdline)(char* out, vk_usize out_cap);
    vk_i64   (*vk_run_cmdline)(const char* command_line);
    int      (*vk_terminate_task)(vk_u64 task_id);
    int      (*vk_exec_cmdline)(const char* command_line);

} vk_api_t;

/* Current API version */
#define VK_API_VERSION 29ULL

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

/* Sound format constants for vk_snd_play() / vk_snd_mix_play() */
#define VK_SND_FORMAT_UNSIGNED_8   0
#define VK_SND_FORMAT_SIGNED_16    1
#define VK_SND_FORMAT_SIGNED_16_STEREO 2

#define VK_TASK_CPU_NONE 0xFFFFFFFFu

/* Maximum simultaneous mixer channels */
#define VK_SND_MIX_CHANNELS        8

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
