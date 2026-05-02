/*
 * SDL.h - Fully self-contained SDL2 shim for Chocolate Quake on vkernel.
 *
 * Does NOT pull in the Doom SDL shim — it defines everything itself so that:
 *   - SDL_Surface.format is SDL_PixelFormat* (pointer), not Uint32
 *   - SDL_PixelFormat.palette is SDL_Palette* (pointer), not void*
 *   - Sint8/Sint64/Uint64 are available for common.h type aliases
 */
#ifndef VK_QUAKE_SDL_H
#define VK_QUAKE_SDL_H

#include <stdint.h>
#include <stddef.h>

/* ---- Basic SDL integer types ---- */
typedef uint8_t  Uint8;
typedef uint16_t Uint16;
typedef uint32_t Uint32;
typedef uint64_t Uint64;
typedef int8_t   Sint8;
typedef int16_t  Sint16;
typedef int32_t  Sint32;
typedef int64_t  Sint64;

/* ---- SDL_bool ---- */
typedef enum { SDL_FALSE = 0, SDL_TRUE = 1 } SDL_bool;

/* ---- SDLCALL (unused on Linux/x86-64 but headers reference it) ---- */
#define SDLCALL

/* ---- Init flags ---- */
#define SDL_INIT_TIMER          0x00000001u
#define SDL_INIT_AUDIO          0x00000010u
#define SDL_INIT_VIDEO          0x00000020u
#define SDL_INIT_JOYSTICK       0x00000200u
#define SDL_INIT_GAMECONTROLLER 0x00002000u
#define SDL_INIT_EVENTS         0x00004000u
#define SDL_INIT_EVERYTHING     0x0000FFFFu

static inline int  SDL_Init(Uint32 f)           { (void)f; return 0; }
static inline void SDL_Quit(void)               {}
static inline int  SDL_InitSubSystem(Uint32 f)  { (void)f; return 0; }
static inline void SDL_QuitSubSystem(Uint32 f)  { (void)f; }
static inline const char *SDL_GetError(void)    { return ""; }
static inline void SDL_ClearError(void)         {}

/* ---- Hints ---- */
#define SDL_HINT_NO_SIGNAL_HANDLERS              "SDL_NO_SIGNAL_HANDLERS"
#define SDL_HINT_WINDOWS_DISABLE_THREAD_NAMING   "SDL_WINDOWS_DISABLE_THREAD_NAMING"
static inline SDL_bool SDL_SetHint(const char *n, const char *v) { (void)n;(void)v; return SDL_TRUE; }

/* ---- Scancodes (USB HID usage-page 7 values) ---- */
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
    SDL_SCANCODE_RETURN    = 40,
    SDL_SCANCODE_ESCAPE    = 41,
    SDL_SCANCODE_BACKSPACE = 42,
    SDL_SCANCODE_TAB       = 43,
    SDL_SCANCODE_SPACE     = 44,
    SDL_SCANCODE_MINUS     = 45,
    SDL_SCANCODE_EQUALS    = 46,
    SDL_SCANCODE_LEFTBRACKET  = 47,
    SDL_SCANCODE_RIGHTBRACKET = 48,
    SDL_SCANCODE_BACKSLASH = 49,
    SDL_SCANCODE_SEMICOLON = 51,
    SDL_SCANCODE_APOSTROPHE = 52,
    SDL_SCANCODE_GRAVE     = 53,
    SDL_SCANCODE_COMMA     = 54,
    SDL_SCANCODE_PERIOD    = 55,
    SDL_SCANCODE_SLASH     = 56,
    SDL_SCANCODE_CAPSLOCK  = 57,
    SDL_SCANCODE_F1  = 58, SDL_SCANCODE_F2,  SDL_SCANCODE_F3,  SDL_SCANCODE_F4,
    SDL_SCANCODE_F5,       SDL_SCANCODE_F6,  SDL_SCANCODE_F7,  SDL_SCANCODE_F8,
    SDL_SCANCODE_F9,       SDL_SCANCODE_F10, SDL_SCANCODE_F11, SDL_SCANCODE_F12,
    SDL_SCANCODE_PRINTSCREEN = 70,
    SDL_SCANCODE_SCROLLLOCK  = 71,
    SDL_SCANCODE_PAUSE     = 72,
    SDL_SCANCODE_INSERT    = 73,
    SDL_SCANCODE_HOME      = 74,
    SDL_SCANCODE_PAGEUP    = 75,
    SDL_SCANCODE_DELETE    = 76,
    SDL_SCANCODE_END       = 77,
    SDL_SCANCODE_PAGEDOWN  = 78,
    SDL_SCANCODE_RIGHT     = 79,
    SDL_SCANCODE_LEFT      = 80,
    SDL_SCANCODE_DOWN      = 81,
    SDL_SCANCODE_UP        = 82,
    SDL_SCANCODE_NUMLOCKCLEAR  = 83,
    SDL_SCANCODE_KP_DIVIDE    = 84,
    SDL_SCANCODE_KP_MULTIPLY  = 85,
    SDL_SCANCODE_KP_MINUS     = 86,
    SDL_SCANCODE_KP_PLUS      = 87,
    SDL_SCANCODE_KP_ENTER     = 88,
    SDL_SCANCODE_KP_1 = 89, SDL_SCANCODE_KP_2, SDL_SCANCODE_KP_3,
    SDL_SCANCODE_KP_4,      SDL_SCANCODE_KP_5, SDL_SCANCODE_KP_6,
    SDL_SCANCODE_KP_7,      SDL_SCANCODE_KP_8, SDL_SCANCODE_KP_9,
    SDL_SCANCODE_KP_0      = 98,
    SDL_SCANCODE_KP_PERIOD = 99,
    SDL_SCANCODE_NONUSBACKSLASH = 100,
    SDL_SCANCODE_KP_EQUALS = 103,
    SDL_SCANCODE_LCTRL     = 224,
    SDL_SCANCODE_LSHIFT    = 225,
    SDL_SCANCODE_LALT      = 226,
    SDL_SCANCODE_RCTRL     = 228,
    SDL_SCANCODE_RSHIFT    = 229,
    SDL_SCANCODE_RALT      = 230,
    SDL_NUM_SCANCODES      = 512
} SDL_Scancode;

/* ---- Keymod ---- */
typedef enum {
    KMOD_NONE   = 0x0000,
    KMOD_LSHIFT = 0x0001, KMOD_RSHIFT = 0x0002,
    KMOD_LCTRL  = 0x0040, KMOD_RCTRL  = 0x0080,
    KMOD_LALT   = 0x0100, KMOD_RALT   = 0x0200,
    KMOD_SHIFT  = 0x0003, KMOD_CTRL   = 0x00C0, KMOD_ALT = 0x0300,
} SDL_Keymod;

typedef struct {
    SDL_Scancode scancode;
    int          sym;
    Uint16       mod;
} SDL_Keysym;

/* ---- Event types ---- */
#define SDL_QUIT             0x100
#define SDL_KEYDOWN          0x300
#define SDL_KEYUP            0x301
#define SDL_TEXTINPUT        0x303
#define SDL_MOUSEMOTION      0x400
#define SDL_MOUSEBUTTONDOWN  0x401
#define SDL_MOUSEBUTTONUP    0x402
#define SDL_MOUSEWHEEL       0x403
#define SDL_WINDOWEVENT      0x200
#define SDL_CONTROLLERAXISMOTION    0x650
#define SDL_CONTROLLERBUTTONDOWN    0x651
#define SDL_CONTROLLERBUTTONUP      0x652
#define SDL_CONTROLLERDEVICEADDED   0x653
#define SDL_CONTROLLERDEVICEREMOVED 0x654
#define SDL_ACTIVEEVENT             0x900

/* Window event subtypes */
#define SDL_WINDOWEVENT_EXPOSED      2
#define SDL_WINDOWEVENT_RESIZED      5
#define SDL_WINDOWEVENT_MINIMIZED    6
#define SDL_WINDOWEVENT_MAXIMIZED    7
#define SDL_WINDOWEVENT_RESTORED     8
#define SDL_WINDOWEVENT_FOCUS_GAINED 12
#define SDL_WINDOWEVENT_FOCUS_LOST   13
#define SDL_WINDOWEVENT_MOVED        4

typedef struct { Uint8 type; Uint8 event; } SDL_WindowEvent;

typedef struct {
    Uint32     type;
    Uint32     timestamp;
    SDL_Keysym keysym;
    Uint8      repeat;
} SDL_KeyboardEvent;

typedef struct {
    Uint32 type;
    Sint32 x, y;
    Sint32 xrel, yrel;
} SDL_MouseMotionEvent;

typedef struct {
    Uint32 type;
    Uint8  button;
    Uint8  state;
} SDL_MouseButtonEvent;

typedef struct {
    Uint32 type;
    Sint32 x, y;
} SDL_MouseWheelEvent;

typedef struct {
    Uint32 type;
    char   text[32];
} SDL_TextInputEvent;

typedef struct {
    Uint32 type;
    Sint32 which;
    Uint8  axis;
    Sint16 value;
} SDL_ControllerAxisEvent;

typedef struct {
    Uint32 type;
    Sint32 which;
    Uint8  button;
    Uint8  state;
} SDL_ControllerButtonEvent;

typedef struct {
    Uint32 type;
    Sint32 which;
} SDL_ControllerDeviceEvent;

typedef union {
    Uint32                  type;
    SDL_KeyboardEvent       key;
    SDL_MouseMotionEvent    motion;
    SDL_MouseButtonEvent    button;
    SDL_MouseWheelEvent     wheel;
    SDL_WindowEvent         window;
    SDL_TextInputEvent      text;
    SDL_ControllerAxisEvent caxis;
    SDL_ControllerButtonEvent cbutton;
    SDL_ControllerDeviceEvent cdevice;
} SDL_Event;

static inline int  SDL_PollEvent(SDL_Event *ev)  { (void)ev; return 0; }
static inline void SDL_PumpEvents(void)          {}
static inline int  SDL_PushEvent(SDL_Event *ev)  { (void)ev; return 0; }
static inline int  SDL_WaitEvent(SDL_Event *ev)  { (void)ev; return 0; }

/* ---- Mouse ---- */
#define SDL_BUTTON_LEFT   1
#define SDL_BUTTON_MIDDLE 2
#define SDL_BUTTON_RIGHT  3
#define SDL_BUTTON_X1     4
#define SDL_BUTTON_X2     5
#define SDL_PRESSED  1
#define SDL_RELEASED 0

static inline Uint32 SDL_GetMouseState(int *x, int *y) {
    if (x) *x = 0; if (y) *y = 0; return 0;
}
static inline int SDL_GetRelativeMouseState(int *x, int *y) {
    if (x) *x = 0; if (y) *y = 0; return 0;
}
static inline int  SDL_ShowCursor(int t)                        { (void)t; return 0; }
static inline int  SDL_SetRelativeMouseMode(SDL_bool e)         { (void)e; return 0; }
static inline int  SDL_WarpMouseInWindow(void *w, int x, int y) { (void)w;(void)x;(void)y; return 0; }

/* ---- Keyboard ---- */
static inline const Uint8 *SDL_GetKeyboardState(int *n) { (void)n; return NULL; }

/* ---- Window / Renderer (stubs) ---- */
typedef struct SDL_Window   SDL_Window;
typedef struct SDL_Renderer SDL_Renderer;
typedef struct SDL_Texture  SDL_Texture;

#define SDL_WINDOW_FULLSCREEN          0x00000001u
#define SDL_WINDOW_FULLSCREEN_DESKTOP  0x00001001u
#define SDL_WINDOW_RESIZABLE           0x00000020u
#define SDL_WINDOW_ALLOW_HIGHDPI       0x00002000u
#define SDL_WINDOW_INPUT_FOCUS         0x00000200u
#define SDL_WINDOWPOS_CENTERED         0x2FFF0000u
#define SDL_WINDOWPOS_UNDEFINED        0x1FFF0000u
#define SDL_RENDERER_PRESENTVSYNC      0x00000004u
#define SDL_RENDERER_TARGETTEXTURE     0x00000008u
#define SDL_RENDERER_SOFTWARE          0x00000001u
#define SDL_TEXTUREACCESS_STREAMING    1
#define SDL_PIXELFORMAT_ARGB8888       0x16362004u
#define SDL_PIXELFORMAT_ABGR8888       0x16762004u
#define SDL_PIXELFORMAT_INDEX8         0x13000001u
#define SDL_DEFINE_PIXELFORMAT(t,o,l,bt,by) (0)

static inline SDL_Window *SDL_CreateWindow(const char *t, int x, int y, int w, int h, Uint32 f) {
    (void)t;(void)x;(void)y;(void)w;(void)h;(void)f; return NULL;
}
static inline void SDL_DestroyWindow(SDL_Window *w) { (void)w; }
static inline SDL_Renderer *SDL_CreateRenderer(SDL_Window *w, int i, Uint32 f) {
    (void)w;(void)i;(void)f; return NULL;
}
static inline void SDL_DestroyRenderer(SDL_Renderer *r) { (void)r; }
static inline int SDL_SetRenderDrawColor(SDL_Renderer *r, Uint8 rr, Uint8 g, Uint8 b, Uint8 a) {
    (void)r;(void)rr;(void)g;(void)b;(void)a; return 0;
}
static inline int SDL_RenderClear(SDL_Renderer *r) { (void)r; return 0; }
static inline void SDL_RenderPresent(SDL_Renderer *r) { (void)r; }
static inline int SDL_RenderCopy(SDL_Renderer *r, SDL_Texture *t, const void *src, const void *dst) {
    (void)r;(void)t;(void)src;(void)dst; return 0;
}
static inline int SDL_SetWindowTitle(SDL_Window *w, const char *t) { (void)w;(void)t; return 0; }
static inline void SDL_SetWindowIcon(SDL_Window *w, void *s) { (void)w;(void)s; }
static inline int SDL_GetWindowSize(SDL_Window *w, int *wp, int *hp) {
    (void)w; if(wp)*wp=0; if(hp)*hp=0; return 0;
}
static inline SDL_Window *SDL_GetWindowFromID(Uint32 id) { (void)id; return NULL; }
static inline Uint32 SDL_GetWindowFlags(SDL_Window *w) { (void)w; return 0; }
static inline void SDL_StartTextInput(void) {}
static inline void SDL_StopTextInput(void) {}
static inline SDL_Texture *SDL_CreateTexture(SDL_Renderer *r, Uint32 f, int ac, int w, int h) {
    (void)r;(void)f;(void)ac;(void)w;(void)h; return NULL;
}
static inline void SDL_DestroyTexture(SDL_Texture *t) { (void)t; }
static inline int SDL_UpdateTexture(SDL_Texture *t, const void *r, const void *p, int pitch) {
    (void)t;(void)r;(void)p;(void)pitch; return 0;
}
static inline SDL_Texture *SDL_CreateTextureFromSurface(SDL_Renderer *r, void *s) {
    (void)r;(void)s; return NULL;
}

/* ---- Surface / Pixels ---- */
typedef struct { Uint8 r, g, b, a; } SDL_Color;
typedef struct { int x, y, w, h;  } SDL_Rect;

typedef struct SDL_Palette {
    int        ncolors;
    SDL_Color *colors;
} SDL_Palette;

/* SDL_PixelFormat: format->palette MUST be a pointer (es_palette.c dereferences it) */
typedef struct SDL_PixelFormat {
    Uint32       format;
    SDL_Palette *palette;
    Uint8        BitsPerPixel;
    Uint8        BytesPerPixel;
    Uint32       Rmask, Gmask, Bmask, Amask;
} SDL_PixelFormat;

typedef uint32_t SDL_PixelFormatEnum;

/* SDL_Surface: format is SDL_PixelFormat* pointer (required by es_palette.c) */
typedef struct SDL_Surface {
    SDL_PixelFormat *format;
    int              w, h;
    int              pitch;
    void            *pixels;
    void            *_pixbuf;
} SDL_Surface;

#define SDL_MUSTLOCK(s)    (0)
#define SDL_ALPHA_OPAQUE   255

static inline SDL_Surface *SDL_GetWindowSurface(SDL_Window *w) { (void)w; return NULL; }
static inline SDL_Surface *SDL_CreateRGBSurface(Uint32 f, int w, int h, int d,
    Uint32 rm, Uint32 gm, Uint32 bm, Uint32 am) {
    (void)f;(void)w;(void)h;(void)d;(void)rm;(void)gm;(void)bm;(void)am; return NULL;
}
static inline SDL_Surface *SDL_CreateRGBSurfaceFrom(void *p, int w, int h, int d, int pitch,
    Uint32 rm, Uint32 gm, Uint32 bm, Uint32 am) {
    (void)p;(void)w;(void)h;(void)d;(void)pitch;(void)rm;(void)gm;(void)bm;(void)am; return NULL;
}
static inline SDL_Surface *SDL_CreateRGBSurfaceWithFormatFrom(
    void *p, int w, int h, int d, int pitch, Uint32 fmt) {
    (void)p;(void)w;(void)h;(void)d;(void)pitch;(void)fmt; return NULL;
}
static inline void SDL_FreeSurface(SDL_Surface *s)              { (void)s; }
static inline int  SDL_LockSurface(SDL_Surface *s)              { (void)s; return 0; }
static inline void SDL_UnlockSurface(SDL_Surface *s)            { (void)s; }
static inline void SDL_SetSurfacePalette(SDL_Surface *s, void *p) { (void)s;(void)p; }
static inline int SDL_BlitSurface(SDL_Surface *s, const SDL_Rect *sr, SDL_Surface *d, SDL_Rect *dr) {
    (void)s;(void)sr;(void)d;(void)dr; return 0;
}
static inline int SDL_LowerBlit(SDL_Surface *s, SDL_Rect *sr, SDL_Surface *d, SDL_Rect *dr) {
    (void)s;(void)sr;(void)d;(void)dr; return 0;
}
static inline int SDL_FillRect(SDL_Surface *s, const SDL_Rect *r, Uint32 c) {
    (void)s;(void)r;(void)c; return 0;
}
static inline Uint32 SDL_MapRGB(const SDL_PixelFormat *f, Uint8 r, Uint8 g, Uint8 b) {
    (void)f;(void)r;(void)g;(void)b; return 0;
}
static inline int SDL_SetPaletteColors(void *p, const SDL_Color *c, int f, int n) {
    (void)p;(void)c;(void)f;(void)n; return 0;
}
static inline SDL_Palette *SDL_AllocPalette(int n) { (void)n; return NULL; }
static inline void SDL_FreePalette(SDL_Palette *p) { (void)p; }

/* ---- Audio ---- */
#define AUDIO_U8      0x0008
#define AUDIO_S8      0x8008
#define AUDIO_U16SYS  0x0010
#define AUDIO_S16SYS  0x8010

typedef Uint32 SDL_AudioDeviceID;
typedef void (SDLCALL *SDL_AudioCallback)(void *userdata, Uint8 *stream, int len);
typedef struct {
    int              freq;
    Uint16           format;
    Uint8            channels;
    Uint16           samples;
    SDL_AudioCallback callback;
    void            *userdata;
} SDL_AudioSpec;

static inline int  SDL_OpenAudio(SDL_AudioSpec *d, SDL_AudioSpec *o) { (void)d;(void)o; return -1; }
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
static inline const char *SDL_GetCurrentAudioDriver(void) { return "vk"; }
static inline const char *SDL_GetAudioDeviceName(int i, int c) {
    (void)i;(void)c; return "vk-snd";
}

/* ---- Game controller / Joystick (stubs) ---- */
typedef struct SDL_Joystick    SDL_Joystick;
typedef struct SDL_GameController SDL_GameController;
typedef int32_t SDL_JoystickID;
typedef struct SDL_Haptic SDL_Haptic;

#define SDL_ENABLE  1
#define SDL_DISABLE 0
#define SDL_QUERY  -1

typedef enum {
    SDL_CONTROLLER_AXIS_INVALID = -1,
    SDL_CONTROLLER_AXIS_LEFTX,
    SDL_CONTROLLER_AXIS_LEFTY,
    SDL_CONTROLLER_AXIS_RIGHTX,
    SDL_CONTROLLER_AXIS_RIGHTY,
    SDL_CONTROLLER_AXIS_TRIGGERLEFT,
    SDL_CONTROLLER_AXIS_TRIGGERRIGHT,
    SDL_CONTROLLER_AXIS_MAX
} SDL_GameControllerAxis;

typedef enum {
    SDL_CONTROLLER_BUTTON_INVALID = -1,
    SDL_CONTROLLER_BUTTON_A,
    SDL_CONTROLLER_BUTTON_B,
    SDL_CONTROLLER_BUTTON_X,
    SDL_CONTROLLER_BUTTON_Y,
    SDL_CONTROLLER_BUTTON_BACK,
    SDL_CONTROLLER_BUTTON_GUIDE,
    SDL_CONTROLLER_BUTTON_START,
    SDL_CONTROLLER_BUTTON_LEFTSTICK,
    SDL_CONTROLLER_BUTTON_RIGHTSTICK,
    SDL_CONTROLLER_BUTTON_LEFTSHOULDER,
    SDL_CONTROLLER_BUTTON_RIGHTSHOULDER,
    SDL_CONTROLLER_BUTTON_DPAD_UP,
    SDL_CONTROLLER_BUTTON_DPAD_DOWN,
    SDL_CONTROLLER_BUTTON_DPAD_LEFT,
    SDL_CONTROLLER_BUTTON_DPAD_RIGHT,
    SDL_CONTROLLER_BUTTON_MAX
} SDL_GameControllerButton;

static inline SDL_GameController *SDL_GameControllerOpen(int i) { (void)i; return NULL; }
static inline void SDL_GameControllerClose(SDL_GameController *g) { (void)g; }
static inline SDL_bool SDL_IsGameController(int i) { (void)i; return SDL_FALSE; }
static inline Sint16 SDL_GameControllerGetAxis(SDL_GameController *g, SDL_GameControllerAxis a) {
    (void)g;(void)a; return 0;
}
static inline Uint8 SDL_GameControllerGetButton(SDL_GameController *g, SDL_GameControllerButton b) {
    (void)g;(void)b; return 0;
}
static inline int SDL_GameControllerEventState(int s) { (void)s; return 0; }
static inline SDL_Joystick *SDL_GameControllerGetJoystick(SDL_GameController *g) {
    (void)g; return NULL;
}
static inline SDL_JoystickID SDL_JoystickInstanceID(SDL_Joystick *j) { (void)j; return -1; }
static inline int SDL_NumJoysticks(void) { return 0; }

/* ---- Performance timer ---- */
static inline Uint64 SDL_GetPerformanceCounter(void) { return 0; }
static inline Uint64 SDL_GetPerformanceFrequency(void) { return 1000000ULL; }

/* ---- SDL_clamp ---- */
#ifndef SDL_clamp
#define SDL_clamp(x, lo, hi) ((x) < (lo) ? (lo) : (x) > (hi) ? (hi) : (x))
#endif

/* ---- Timer ---- */
extern Uint32 SDL_GetTicks(void);
extern void   SDL_Delay(Uint32 ms);

/* ---- Math macros ---- */
#ifndef SDL_min
#define SDL_min(a, b) ((a) < (b) ? (a) : (b))
#endif
#ifndef SDL_max
#define SDL_max(a, b) ((a) > (b) ? (a) : (b))
#endif
#ifndef SDL_abs
#define SDL_abs(x) ((x) < 0 ? -(x) : (x))
#endif

/* ---- Display mode ---- */
typedef struct { int format; int w; int h; int refresh_rate; void *driverdata; } SDL_DisplayMode;
static inline int SDL_GetCurrentDisplayMode(int di, SDL_DisplayMode *mode) {
    (void)di; if (mode) { mode->w = 640; mode->h = 480; mode->refresh_rate = 60; } return 0;
}
static inline int SDL_GetNumVideoDisplays(void) { return 1; }

/* ---- Extra render hints / texture ops ---- */
#define SDL_HINT_RENDER_SCALE_QUALITY "SDL_RENDER_SCALE_QUALITY"

static inline int SDL_LockTexture(SDL_Texture *t, const SDL_Rect *r, void **p, int *pitch) {
    (void)t;(void)r;(void)p;(void)pitch; return -1;
}
static inline void SDL_UnlockTexture(SDL_Texture *t) { (void)t; }

/* ---- getenv ---- */
static inline char *SDL_getenv(const char *n) { (void)n; return NULL; }

#endif /* VK_QUAKE_SDL_H */
