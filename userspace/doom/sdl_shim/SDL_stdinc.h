/* SDL_stdinc.h stub */
#ifndef VK_SDL_STDINC_SHIM_H
#define VK_SDL_STDINC_SHIM_H
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include "SDL.h"
extern char *strdup(const char *);
static inline char *SDL_strdup(const char *s) { return strdup(s); }
#endif
