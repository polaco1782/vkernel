/*
 * SDL.h - SDL2 shim for Chocolate Quake on vkernel
 * Extends the Doom SDL shim with quake-specific types.
 */
#ifndef VK_QUAKE_SDL_H
#define VK_QUAKE_SDL_H

/* Pull in the base doom shim */
#include "../../doom/sdl_shim/SDL.h"

/* Additional types quake needs */
typedef uint64_t Uint64;
typedef int64_t  Sint64;

/* SDL_clamp used in in_mouse.c */
#ifndef SDL_clamp
#define SDL_clamp(x, lo, hi) ((x) < (lo) ? (lo) : (x) > (hi) ? (hi) : (x))
#endif

/* SDL_BUTTON_X1/X2 */
#define SDL_BUTTON_X1 4
#define SDL_BUTTON_X2 5

/* SDL_PRESSED / SDL_RELEASED */
#define SDL_PRESSED  1
#define SDL_RELEASED 0

/* SDL_GetMouseState stub */
static inline Uint32 SDL_GetMouseState(int *x, int *y) {
    if (x) *x = 0; if (y) *y = 0; return 0;
}
static inline int SDL_ShowCursor(int toggle) { (void)toggle; return 0; }
static inline int SDL_WarpMouseInWindow(SDL_Window *w, int x, int y) {
    (void)w; (void)x; (void)y; return 0;
}
static inline const char *SDL_GetCurrentAudioDriver(void) { return "vk"; }
static inline const char *SDL_GetAudioDeviceName(int i, int c) {
    (void)i; (void)c; return "vk-snd";
}

/* SDL_GetPerformanceCounter/Frequency – stubs; vk provides this via its own timer */
static inline Uint64 SDL_GetPerformanceCounter(void) { return 0; }
static inline Uint64 SDL_GetPerformanceFrequency(void) { return 1000000ULL; }

/* SDL_CreateRGBSurfaceWithFormatFrom – needed by end_screen */
static inline SDL_Surface *SDL_CreateRGBSurfaceWithFormatFrom(
    void *p, int w, int h, int d, int pitch, Uint32 fmt) {
    (void)p; (void)w; (void)h; (void)d; (void)pitch; (void)fmt;
    return NULL;
}

/* SDL_LowerBlit – needed by vid_buffers stub */
static inline int SDL_LowerBlit(SDL_Surface *src, SDL_Rect *sr,
                                SDL_Surface *dst, SDL_Rect *dr) {
    (void)src; (void)sr; (void)dst; (void)dr; return 0;
}
#define SDL_MUSTLOCK(s) (0)
#define SDL_ALPHA_OPAQUE 255

/* SDL palette */
static inline SDL_Surface *SDL_CreateRGBSurface(Uint32 f, int w, int h, int d,
    Uint32 rm, Uint32 gm, Uint32 bm, Uint32 am) {
    (void)f;(void)w;(void)h;(void)d;(void)rm;(void)gm;(void)bm;(void)am;
    return NULL;
}

/* SDL_Palette – used by es_palette.h */
typedef struct { int ncolors; SDL_Color *colors; } SDL_Palette;
/* SDL_PixelFormatEnum - used by es_buffer.c */
typedef uint32_t SDL_PixelFormatEnum;
static inline SDL_Palette *SDL_AllocPaletteImpl(int n) {
    (void)n; return NULL;
}

/* SDL audio – needed for snd_sdl.c (we replace it but es_time.c etc. might pull headers) */
#define AUDIO_U8      0x0008
#define AUDIO_S8      0x8008
#define AUDIO_U16SYS  0x0010
#define AUDIO_S16SYS  0x8010
typedef void (SDLCALL *SDL_AudioCallback)(void *userdata, Uint8 *stream, int len);
typedef struct {
    int freq;
    Uint16 format;
    Uint8 channels;
    Uint16 samples;
    SDL_AudioCallback callback;
    void *userdata;
} SDL_AudioSpec;
static inline int  SDL_OpenAudio(SDL_AudioSpec *d, SDL_AudioSpec *o) {
    (void)d; (void)o; return -1;
}
static inline void SDL_CloseAudio(void) {}
static inline void SDL_PauseAudio(int p) { (void)p; }
static inline void SDL_LockAudio(void) {}
static inline void SDL_UnlockAudio(void) {}

/* SDL_Video constants needed by es_font.c */
#define SDL_WINDOW_INPUT_FOCUS 0x00000200u

/* SDL_GetTicks is declared forward in base shim; implement it here */
#undef SDL_GetTicks
/* Implemented externally in sys_vk.c; just declare */
Uint32 SDL_GetTicks(void);

/* SDL_Delay implemented in sys_vk.c */
#undef SDL_Delay
void SDL_Delay(Uint32 ms);

#endif /* VK_QUAKE_SDL_H */
