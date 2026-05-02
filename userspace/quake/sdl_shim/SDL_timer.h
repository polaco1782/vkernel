#ifndef VK_QUAKE_SDL_TIMER_H
#define VK_QUAKE_SDL_TIMER_H
#include "SDL.h"
/* SDL_GetPerformanceCounter/Frequency already in SDL.h */
/* SDL_Delay declared in SDL.h */
typedef Uint32 SDL_TimerID;
typedef Uint32 (*SDL_TimerCallback)(Uint32 interval, void *param);
static inline SDL_TimerID SDL_AddTimer(Uint32 i, SDL_TimerCallback c, void *p) {
    (void)i; (void)c; (void)p; return 0;
}
static inline SDL_bool SDL_RemoveTimer(SDL_TimerID id) { (void)id; return SDL_TRUE; }
#endif
