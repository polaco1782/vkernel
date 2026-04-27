/* SDL_joystick.h stub */
#ifndef VK_SDL_JOYSTICK_SHIM_H
#define VK_SDL_JOYSTICK_SHIM_H
#include "SDL.h"
static inline const char *SDL_JoystickNameForIndex(int i) { (void)i; return ""; }
static inline SDL_Joystick *SDL_JoystickOpen(int i) { (void)i; return 0; }
static inline void SDL_JoystickClose(SDL_Joystick *j) { (void)j; }
static inline int SDL_JoystickNumAxes(SDL_Joystick *j) { (void)j; return 0; }
static inline int SDL_JoystickNumButtons(SDL_Joystick *j) { (void)j; return 0; }
static inline int SDL_JoystickNumHats(SDL_Joystick *j) { (void)j; return 0; }
static inline Sint16 SDL_JoystickGetAxis(SDL_Joystick *j, int a) { (void)j;(void)a; return 0; }
static inline Uint8 SDL_JoystickGetButton(SDL_Joystick *j, int b) { (void)j;(void)b; return 0; }
static inline Uint8 SDL_JoystickGetHat(SDL_Joystick *j, int h) { (void)j;(void)h; return 0; }
static inline SDL_JoystickID SDL_JoystickInstanceID(SDL_Joystick *j) { (void)j; return 0; }
#define SDL_HAT_CENTERED  0x00
#define SDL_HAT_UP        0x01
#define SDL_HAT_RIGHT     0x02
#define SDL_HAT_DOWN      0x04
#define SDL_HAT_LEFT      0x08
#endif
