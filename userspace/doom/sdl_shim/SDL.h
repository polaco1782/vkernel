/*
 * SDL.h - Minimal SDL2 type shim for building Chocolate Doom on vkernel
 *
 * This header provides ONLY the type definitions and constants that the
 * Chocolate Doom source code references.  All actual functionality is
 * provided by our vkernel-specific i_*.c replacements.
 */

#ifndef VK_SDL_SHIM_H
#define VK_SDL_SHIM_H

#include <stdint.h>

/* ---- Basic SDL types ---- */
typedef uint8_t  Uint8;
typedef uint16_t Uint16;
typedef uint32_t Uint32;
typedef int32_t  Sint32;
typedef int16_t  Sint16;

/* ---- SDL_bool ---- */
typedef enum { SDL_FALSE = 0, SDL_TRUE = 1 } SDL_bool;

/* ---- Init subsystem flags ---- */
#define SDL_INIT_TIMER       0x00000001u
#define SDL_INIT_AUDIO       0x00000010u
#define SDL_INIT_VIDEO       0x00000020u
#define SDL_INIT_JOYSTICK    0x00000200u
#define SDL_INIT_GAMECONTROLLER 0x00002000u
#define SDL_INIT_EVENTS      0x00004000u
#define SDL_INIT_EVERYTHING  0x0000FFFFu

static inline int SDL_Init(Uint32 flags) { (void)flags; return 0; }
static inline void SDL_Quit(void) {}
static inline int SDL_InitSubSystem(Uint32 flags) { (void)flags; return 0; }
static inline void SDL_QuitSubSystem(Uint32 flags) { (void)flags; }

/* ---- Hints ---- */
#define SDL_HINT_NO_SIGNAL_HANDLERS          "SDL_NO_SIGNAL_HANDLERS"
#define SDL_HINT_WINDOWS_DISABLE_THREAD_NAMING "SDL_WINDOWS_DISABLE_THREAD_NAMING"
static inline SDL_bool SDL_SetHint(const char *name, const char *value) {
    (void)name; (void)value; return SDL_TRUE;
}

/* ---- Timer ---- */
static inline Uint32 SDL_GetTicks(void);  /* implemented in i_timer_vk.c via extern */

/* ---- Events ---- */

/* Event types */
#define SDL_QUIT             0x100
#define SDL_KEYDOWN          0x300
#define SDL_KEYUP            0x301
#define SDL_MOUSEMOTION      0x400
#define SDL_MOUSEBUTTONDOWN  0x401
#define SDL_MOUSEBUTTONUP    0x402
#define SDL_MOUSEWHEEL       0x403
#define SDL_WINDOWEVENT      0x200
#define SDL_TEXTINPUT        0x303
#define SDL_ACTIVEEVENT      0x900   /* compat */

/* Window event subtypes */
#define SDL_WINDOWEVENT_EXPOSED       2
#define SDL_WINDOWEVENT_RESIZED       5
#define SDL_WINDOWEVENT_MINIMIZED     6
#define SDL_WINDOWEVENT_MAXIMIZED     7
#define SDL_WINDOWEVENT_RESTORED      8
#define SDL_WINDOWEVENT_FOCUS_GAINED  12
#define SDL_WINDOWEVENT_FOCUS_LOST    13
#define SDL_WINDOWEVENT_MOVED         4

/* SDL scancodes */
typedef enum {
    SDL_SCANCODE_UNKNOWN = 0,
    SDL_SCANCODE_A = 4, SDL_SCANCODE_B, SDL_SCANCODE_C, SDL_SCANCODE_D,
    SDL_SCANCODE_E, SDL_SCANCODE_F, SDL_SCANCODE_G, SDL_SCANCODE_H,
    SDL_SCANCODE_I, SDL_SCANCODE_J, SDL_SCANCODE_K, SDL_SCANCODE_L,
    SDL_SCANCODE_M, SDL_SCANCODE_N, SDL_SCANCODE_O, SDL_SCANCODE_P,
    SDL_SCANCODE_Q, SDL_SCANCODE_R, SDL_SCANCODE_S, SDL_SCANCODE_T,
    SDL_SCANCODE_U, SDL_SCANCODE_V, SDL_SCANCODE_W, SDL_SCANCODE_X,
    SDL_SCANCODE_Y, SDL_SCANCODE_Z,
    SDL_SCANCODE_1 = 30, SDL_SCANCODE_2, SDL_SCANCODE_3, SDL_SCANCODE_4,
    SDL_SCANCODE_5, SDL_SCANCODE_6, SDL_SCANCODE_7, SDL_SCANCODE_8,
    SDL_SCANCODE_9, SDL_SCANCODE_0,
    SDL_SCANCODE_RETURN = 40,
    SDL_SCANCODE_ESCAPE = 41,
    SDL_SCANCODE_BACKSPACE = 42,
    SDL_SCANCODE_TAB = 43,
    SDL_SCANCODE_SPACE = 44,
    SDL_SCANCODE_MINUS = 45,
    SDL_SCANCODE_EQUALS = 46,
    SDL_SCANCODE_LEFTBRACKET = 47,
    SDL_SCANCODE_RIGHTBRACKET = 48,
    SDL_SCANCODE_BACKSLASH = 49,
    SDL_SCANCODE_SEMICOLON = 51,
    SDL_SCANCODE_APOSTROPHE = 52,
    SDL_SCANCODE_GRAVE = 53,
    SDL_SCANCODE_COMMA = 54,
    SDL_SCANCODE_PERIOD = 55,
    SDL_SCANCODE_SLASH = 56,
    SDL_SCANCODE_CAPSLOCK = 57,
    SDL_SCANCODE_F1 = 58, SDL_SCANCODE_F2, SDL_SCANCODE_F3, SDL_SCANCODE_F4,
    SDL_SCANCODE_F5, SDL_SCANCODE_F6, SDL_SCANCODE_F7, SDL_SCANCODE_F8,
    SDL_SCANCODE_F9, SDL_SCANCODE_F10, SDL_SCANCODE_F11, SDL_SCANCODE_F12,
    SDL_SCANCODE_PRINTSCREEN = 70,
    SDL_SCANCODE_SCROLLLOCK = 71,
    SDL_SCANCODE_PAUSE = 72,
    SDL_SCANCODE_INSERT = 73,
    SDL_SCANCODE_HOME = 74,
    SDL_SCANCODE_PAGEUP = 75,
    SDL_SCANCODE_DELETE = 76,
    SDL_SCANCODE_END = 77,
    SDL_SCANCODE_PAGEDOWN = 78,
    SDL_SCANCODE_RIGHT = 79,
    SDL_SCANCODE_LEFT = 80,
    SDL_SCANCODE_DOWN = 81,
    SDL_SCANCODE_UP = 82,
    SDL_SCANCODE_NUMLOCKCLEAR = 83,
    SDL_SCANCODE_KP_DIVIDE = 84,
    SDL_SCANCODE_KP_MULTIPLY = 85,
    SDL_SCANCODE_KP_MINUS = 86,
    SDL_SCANCODE_KP_PLUS = 87,
    SDL_SCANCODE_KP_ENTER = 88,
    SDL_SCANCODE_KP_1 = 89, SDL_SCANCODE_KP_2, SDL_SCANCODE_KP_3,
    SDL_SCANCODE_KP_4, SDL_SCANCODE_KP_5, SDL_SCANCODE_KP_6,
    SDL_SCANCODE_KP_7, SDL_SCANCODE_KP_8, SDL_SCANCODE_KP_9,
    SDL_SCANCODE_KP_0 = 98,
    SDL_SCANCODE_KP_PERIOD = 99,
    SDL_SCANCODE_NONUSBACKSLASH = 100,
    SDL_SCANCODE_KP_EQUALS = 103,
    SDL_SCANCODE_LCTRL = 224,
    SDL_SCANCODE_LSHIFT = 225,
    SDL_SCANCODE_LALT = 226,
    SDL_SCANCODE_RCTRL = 228,
    SDL_SCANCODE_RSHIFT = 229,
    SDL_SCANCODE_RALT = 230,
    SDL_NUM_SCANCODES = 512
} SDL_Scancode;

/* Keymod flags */
typedef enum {
    KMOD_NONE  = 0x0000,
    KMOD_LSHIFT = 0x0001,
    KMOD_RSHIFT = 0x0002,
    KMOD_LCTRL  = 0x0040,
    KMOD_RCTRL  = 0x0080,
    KMOD_LALT   = 0x0100,
    KMOD_RALT   = 0x0200,
    KMOD_SHIFT  = 0x0003,
    KMOD_CTRL   = 0x00C0,
    KMOD_ALT    = 0x0300,
} SDL_Keymod;

typedef struct {
    SDL_Scancode scancode;
    int sym;
    Uint16 mod;
} SDL_Keysym;

typedef struct {
    Uint8 type;
    Uint8 event;
} SDL_WindowEvent;

typedef struct {
    Uint32 type;
    Uint32 timestamp;
    SDL_Keysym keysym;
    Uint8 repeat;
} SDL_KeyboardEvent;

typedef struct {
    Uint32 type;
    Sint32 x, y;
    Sint32 xrel, yrel;
} SDL_MouseMotionEvent;

typedef struct {
    Uint32 type;
    Uint8 button;
} SDL_MouseButtonEvent;

typedef struct {
    Uint32 type;
    Sint32 x, y;
} SDL_MouseWheelEvent;

typedef struct {
    Uint32 type;
    char text[32];
} SDL_TextInputEvent;

typedef union {
    Uint32 type;
    SDL_KeyboardEvent key;
    SDL_MouseMotionEvent motion;
    SDL_MouseButtonEvent button;
    SDL_MouseWheelEvent wheel;
    SDL_WindowEvent window;
    SDL_TextInputEvent text;
} SDL_Event;

static inline int  SDL_PollEvent(SDL_Event *ev) { (void)ev; return 0; }
static inline void SDL_PumpEvents(void) {}
static inline int  SDL_PushEvent(SDL_Event *ev) { (void)ev; return 0; }

/* ---- Mouse ---- */
#define SDL_BUTTON_LEFT   1
#define SDL_BUTTON_MIDDLE 2
#define SDL_BUTTON_RIGHT  3
static inline Uint32 SDL_GetRelativeMouseState(int *x, int *y) {
    if (x) *x = 0; if (y) *y = 0; return 0;
}
static inline int SDL_SetRelativeMouseMode(SDL_bool enabled) { (void)enabled; return 0; }
static inline SDL_bool SDL_GetRelativeMouseMode(void) { return SDL_FALSE; }

/* ---- Window / Renderer / Texture / Surface stubs ---- */
typedef struct SDL_Window SDL_Window;
typedef struct SDL_Renderer SDL_Renderer;
typedef struct SDL_Texture SDL_Texture;

typedef struct {
    Uint32 format;
    int w, h;
    int pitch;
    void *pixels;
    /* internal */
    void *_pixbuf;
} SDL_Surface;

typedef struct { Uint8 r, g, b, a; } SDL_Color;
typedef struct { int x, y, w, h; } SDL_Rect;

typedef struct {
    Uint32 format;
    void *palette;
    uint8_t BitsPerPixel;
    uint8_t BytesPerPixel;
    Uint32 Rmask, Gmask, Bmask, Amask;
} SDL_PixelFormat;

/* Pixel format constants */
#define SDL_PIXELFORMAT_ARGB8888  0x16362004u
#define SDL_PIXELFORMAT_ABGR8888  0x16762004u
#define SDL_PIXELFORMAT_INDEX8    0x13000001u
#define SDL_DEFINE_PIXELFORMAT(t,o,l,bt,by) (0)

/* Window flags */
#define SDL_WINDOW_FULLSCREEN          0x00000001u
#define SDL_WINDOW_FULLSCREEN_DESKTOP  0x00001001u
#define SDL_WINDOW_RESIZABLE           0x00000020u
#define SDL_WINDOW_ALLOW_HIGHDPI       0x00002000u

/* Renderer flags */
#define SDL_RENDERER_PRESENTVSYNC  0x00000004u
#define SDL_RENDERER_TARGETTEXTURE 0x00000008u
#define SDL_RENDERER_SOFTWARE      0x00000001u

/* Texture access */
#define SDL_TEXTUREACCESS_STREAMING 1
#define SDL_TEXTUREACCESS_TARGET    2

/* Blend modes */
#define SDL_BLENDMODE_NONE 0

/* Message box */
#define SDL_MESSAGEBOX_ERROR 0x00000010u
static inline int SDL_ShowSimpleMessageBox(Uint32 flags, const char *title,
    const char *message, SDL_Window *window) {
    (void)flags; (void)title; (void)message; (void)window; return 0;
}

/* Stub window/renderer/texture/surface functions */
static inline SDL_Window *SDL_CreateWindow(const char *t, int x, int y, int w, int h, Uint32 f) {
    (void)t;(void)x;(void)y;(void)w;(void)h;(void)f; return (SDL_Window*)1;
}
static inline void SDL_DestroyWindow(SDL_Window *w) { (void)w; }
static inline void SDL_SetWindowTitle(SDL_Window *w, const char *t) { (void)w;(void)t; }
static inline int  SDL_GetWindowDisplayIndex(SDL_Window *w) { (void)w; return 0; }
static inline void SDL_SetWindowFullscreen(SDL_Window *w, Uint32 f) { (void)w;(void)f; }
static inline void SDL_SetWindowSize(SDL_Window *w, int W, int H) { (void)w;(void)W;(void)H; }
static inline void SDL_GetWindowSize(SDL_Window *w, int *W, int *H) {
    (void)w; if (W) *W=320; if (H) *H=200;
}
static inline void SDL_SetWindowPosition(SDL_Window *w, int x, int y) { (void)w;(void)x;(void)y; }
static inline void SDL_SetWindowMinimumSize(SDL_Window *w, int x, int y) { (void)w;(void)x;(void)y; }
static inline SDL_Surface *SDL_GetWindowSurface(SDL_Window *w) { (void)w; return 0; }

static inline SDL_Renderer *SDL_CreateRenderer(SDL_Window *w, int i, Uint32 f) {
    (void)w;(void)i;(void)f; return (SDL_Renderer*)1;
}
static inline void SDL_DestroyRenderer(SDL_Renderer *r) { (void)r; }
static inline void SDL_RenderClear(SDL_Renderer *r) { (void)r; }
static inline void SDL_RenderCopy(SDL_Renderer *r, SDL_Texture *t, const SDL_Rect *s, const SDL_Rect *d) {
    (void)r;(void)t;(void)s;(void)d;
}
static inline void SDL_RenderPresent(SDL_Renderer *r) { (void)r; }
static inline int SDL_RenderSetLogicalSize(SDL_Renderer *r, int w, int h) { (void)r;(void)w;(void)h; return 0; }
static inline void SDL_SetRenderDrawColor(SDL_Renderer *r, Uint8 R, Uint8 G, Uint8 B, Uint8 A) {
    (void)r;(void)R;(void)G;(void)B;(void)A;
}
static inline int SDL_SetRenderTarget(SDL_Renderer *r, SDL_Texture *t) { (void)r;(void)t; return 0; }
static inline void SDL_RenderGetLogicalSize(SDL_Renderer *r, int *w, int *h) {
    (void)r; if(w)*w=320; if(h)*h=200;
}

static inline SDL_Texture *SDL_CreateTexture(SDL_Renderer *r, Uint32 f, int a, int w, int h) {
    (void)r;(void)f;(void)a;(void)w;(void)h; return (SDL_Texture*)1;
}
static inline SDL_Texture *SDL_CreateTextureFromSurface(SDL_Renderer *r, SDL_Surface *s) {
    (void)r;(void)s; return (SDL_Texture*)1;
}
static inline void SDL_DestroyTexture(SDL_Texture *t) { (void)t; }
static inline int SDL_UpdateTexture(SDL_Texture *t, const SDL_Rect *r, const void *p, int pitch) {
    (void)t;(void)r;(void)p;(void)pitch; return 0;
}
static inline int SDL_LockTexture(SDL_Texture *t, const SDL_Rect *r, void **p, int *pitch) {
    (void)t;(void)r;(void)p;(void)pitch; return -1;
}
static inline void SDL_UnlockTexture(SDL_Texture *t) { (void)t; }
static inline int SDL_SetTextureBlendMode(SDL_Texture *t, int m) { (void)t;(void)m; return 0; }
static inline int SDL_QueryTexture(SDL_Texture *t, Uint32 *f, int *a, int *w, int *h) {
    (void)t;(void)f;(void)a; if(w)*w=320; if(h)*h=200; return 0;
}

/* Surface functions */
static inline SDL_Surface *SDL_CreateRGBSurface(Uint32 f, int w, int h, int d,
    Uint32 rm, Uint32 gm, Uint32 bm, Uint32 am) {
    (void)f;(void)w;(void)h;(void)d;(void)rm;(void)gm;(void)bm;(void)am;
    return 0; /* our i_video_vk.c handles this */
}
static inline SDL_Surface *SDL_CreateRGBSurfaceFrom(void *p, int w, int h, int d, int pitch,
    Uint32 rm, Uint32 gm, Uint32 bm, Uint32 am) {
    (void)p;(void)w;(void)h;(void)d;(void)pitch;(void)rm;(void)gm;(void)bm;(void)am;
    return 0;
}
static inline void SDL_FreeSurface(SDL_Surface *s) { (void)s; }
static inline int  SDL_SetPaletteColors(void *p, const SDL_Color *c, int f, int n) {
    (void)p;(void)c;(void)f;(void)n; return 0;
}
static inline void SDL_SetSurfacePalette(SDL_Surface *s, void *p) { (void)s;(void)p; }
static inline int  SDL_BlitSurface(SDL_Surface *s, const SDL_Rect *sr, SDL_Surface *d, SDL_Rect *dr) {
    (void)s;(void)sr;(void)d;(void)dr; return 0;
}
static inline int SDL_FillRect(SDL_Surface *s, const SDL_Rect *r, Uint32 c) {
    (void)s;(void)r;(void)c; return 0;
}
static inline int SDL_LockSurface(SDL_Surface *s) { (void)s; return 0; }
static inline void SDL_UnlockSurface(SDL_Surface *s) { (void)s; }
static inline Uint32 SDL_MapRGB(const SDL_PixelFormat *f, Uint8 r, Uint8 g, Uint8 b) {
    (void)f; return ((Uint32)r<<16)|((Uint32)g<<8)|(Uint32)b;
}
static inline void *SDL_AllocPalette(int n) { (void)n; return (void*)1; }

/* Display */
typedef struct {
    Uint32 format;
    int w, h, refresh_rate;
    void *driverdata;
} SDL_DisplayMode;

static inline int SDL_GetNumVideoDisplays(void) { return 1; }
static inline int SDL_GetCurrentDisplayMode(int d, SDL_DisplayMode *m) {
    (void)d; if (m) { m->w = 320; m->h = 200; m->refresh_rate = 35; } return 0;
}
static inline int SDL_GetDesktopDisplayMode(int d, SDL_DisplayMode *m) {
    return SDL_GetCurrentDisplayMode(d, m);
}
static inline int SDL_GetNumDisplayModes(int d) { (void)d; return 1; }
static inline int SDL_GetDisplayMode(int d, int i, SDL_DisplayMode *m) {
    return SDL_GetCurrentDisplayMode(d, m);
}

/* Misc */
static inline const char *SDL_GetError(void) { return ""; }
static inline void SDL_Delay(Uint32 ms);  /* implemented externally */
static inline void SDL_StartTextInput(void) {}
static inline void SDL_StopTextInput(void) {}
static inline int SDL_GetDisplayBounds(int d, SDL_Rect *r) {
    (void)d; if(r){r->x=0;r->y=0;r->w=320;r->h=200;} return 0;
}

/* Joystick / gamecontroller stubs */
typedef struct SDL_Joystick SDL_Joystick;
typedef struct SDL_GameController SDL_GameController;
typedef int32_t SDL_JoystickID;
static inline int SDL_NumJoysticks(void) { return 0; }

/* Windowpos constants */
#define SDL_WINDOWPOS_CENTERED   0x2FFF0000u
#define SDL_WINDOWPOS_UNDEFINED  0x1FFF0000u

/* Audio - not used (we have our own sound path), but needed for compilation */
typedef uint16_t SDL_AudioFormat;
#define AUDIO_U8     0x0008
#define AUDIO_S16SYS 0x8010

typedef struct {
    int freq;
    SDL_AudioFormat format;
    Uint8 channels;
    Uint8 silence;
    Uint16 samples;
    Uint32 size;
    void (*callback)(void *userdata, Uint8 *stream, int len);
    void *userdata;
} SDL_AudioSpec;

typedef Uint32 SDL_AudioDeviceID;
static inline int SDL_OpenAudio(SDL_AudioSpec *d, SDL_AudioSpec *o) { (void)d;(void)o; return -1; }
static inline SDL_AudioDeviceID SDL_OpenAudioDevice(const char *d, int ic,
    const SDL_AudioSpec *des, SDL_AudioSpec *obt, int ac) {
    (void)d;(void)ic;(void)des;(void)obt;(void)ac; return 0;
}
static inline void SDL_CloseAudio(void) {}
static inline void SDL_CloseAudioDevice(SDL_AudioDeviceID dev) { (void)dev; }
static inline void SDL_PauseAudio(int p) { (void)p; }
static inline void SDL_PauseAudioDevice(SDL_AudioDeviceID dev, int p) { (void)dev;(void)p; }
static inline void SDL_LockAudio(void) {}
static inline void SDL_UnlockAudio(void) {}
static inline void SDL_LockAudioDevice(SDL_AudioDeviceID dev) { (void)dev; }
static inline void SDL_UnlockAudioDevice(SDL_AudioDeviceID dev) { (void)dev; }
static inline void SDL_MixAudioFormat(Uint8 *dst, const Uint8 *src, SDL_AudioFormat f, Uint32 len, int vol) {
    (void)dst;(void)src;(void)f;(void)len;(void)vol;
}

/* RWops stub (used by SDL_mixer which we disabled) */
typedef struct SDL_RWops SDL_RWops;

/* Versioning */
typedef struct { Uint8 major, minor, patch; } SDL_version;
static inline void SDL_GetVersion(SDL_version *v) { if(v){v->major=2;v->minor=0;v->patch=0;} }

/* Clipboard */
static inline char *SDL_GetClipboardText(void) { return (char*)""; }
static inline int SDL_SetClipboardText(const char *t) { (void)t; return 0; }

/* Filesystem */
static inline char *SDL_GetBasePath(void) { return (char*)"/"; }
static inline char *SDL_GetPrefPath(const char *org, const char *app) {
    (void)org;(void)app; return (char*)"/";
}
static inline void SDL_free(void *ptr) { (void)ptr; /* our stubs return literals */ }

/* Power */
typedef enum { SDL_POWERSTATE_UNKNOWN } SDL_PowerState;

/* Render scale quality hint */
#define SDL_HINT_RENDER_SCALE_QUALITY "SDL_RENDER_SCALE_QUALITY"

/* Log */
static inline void SDL_Log(const char *fmt, ...) { (void)fmt; }

#endif /* VK_SDL_SHIM_H */
