/* SDL_gamecontroller.h stub */
#ifndef VK_SDL_GAMECONTROLLER_SHIM_H
#define VK_SDL_GAMECONTROLLER_SHIM_H
#include "SDL.h"
#include "SDL_joystick.h"

typedef enum {
    SDL_CONTROLLER_AXIS_LEFTX = 0,
    SDL_CONTROLLER_AXIS_LEFTY,
    SDL_CONTROLLER_AXIS_RIGHTX,
    SDL_CONTROLLER_AXIS_RIGHTY,
    SDL_CONTROLLER_AXIS_TRIGGERLEFT,
    SDL_CONTROLLER_AXIS_TRIGGERRIGHT,
    SDL_CONTROLLER_AXIS_MAX
} SDL_GameControllerAxis;

typedef enum {
    SDL_CONTROLLER_BUTTON_A = 0,
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

static inline SDL_bool SDL_IsGameController(int i) { (void)i; return SDL_FALSE; }
static inline SDL_GameController *SDL_GameControllerOpen(int i) { (void)i; return 0; }
static inline void SDL_GameControllerClose(SDL_GameController *c) { (void)c; }
static inline const char *SDL_GameControllerName(SDL_GameController *c) { (void)c; return ""; }
static inline Sint16 SDL_GameControllerGetAxis(SDL_GameController *c, SDL_GameControllerAxis a) {
    (void)c;(void)a; return 0;
}
static inline Uint8 SDL_GameControllerGetButton(SDL_GameController *c, SDL_GameControllerButton b) {
    (void)c;(void)b; return 0;
}
static inline SDL_Joystick *SDL_GameControllerGetJoystick(SDL_GameController *c) { (void)c; return 0; }

#endif
