#ifndef VK_QUAKE_SDL_STDINC_H
#define VK_QUAKE_SDL_STDINC_H
#include "SDL.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
extern char *strdup(const char *);
static inline char *SDL_strdup(const char *s) { return strdup(s); }
#ifndef SDL_free
#define SDL_free free
#endif
#ifndef SDL_malloc
#define SDL_malloc malloc
#endif
#ifndef SDL_calloc
#define SDL_calloc calloc
#endif
#ifndef SDL_realloc
#define SDL_realloc realloc
#endif
#ifndef SDL_memset
#define SDL_memset memset
#endif
#ifndef SDL_memcpy
#define SDL_memcpy memcpy
#endif
#ifndef SDL_strlen
#define SDL_strlen strlen
#endif
#ifndef SDL_strtol
#define SDL_strtol strtol
#endif
#ifndef SDL_qsort
#define SDL_qsort qsort
#endif
#ifndef SDL_atoi
#define SDL_atoi atoi
#endif
#ifndef SDL_itoa
#define SDL_itoa(v, buf, base) ((void)sprintf((buf), "%d", (v)), (buf))
#endif
/* String functions */
#ifndef SDL_memmove
#define SDL_memmove memmove
#endif
#ifndef SDL_memcmp
#define SDL_memcmp memcmp
#endif
#ifndef SDL_strcmp
#define SDL_strcmp strcmp
#endif
#ifndef SDL_strncmp
#define SDL_strncmp strncmp
#endif
#ifndef SDL_strchr
#define SDL_strchr strchr
#endif
#ifndef SDL_strrchr
#define SDL_strrchr strrchr
#endif
#ifndef SDL_strstr
#define SDL_strstr strstr
#endif
#ifndef SDL_strncasecmp
#ifdef _WIN32
#define SDL_strncasecmp strnicmp
#else
#define SDL_strncasecmp strncasecmp
#endif
#endif
/* SDL_snprintf */
/* SDL_getenv */
#ifndef SDL_getenv
#define SDL_getenv getenv
#endif
/* SDL_snprintf */
#ifndef SDL_snprintf
#define SDL_snprintf snprintf
#endif
#endif
