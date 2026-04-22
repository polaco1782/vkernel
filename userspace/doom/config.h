/*
 * config.h - Build configuration for Chocolate Doom on vkernel
 *
 * This replaces the autotools/cmake-generated config.h.
 */

#ifndef DOOM_CONFIG_H
#define DOOM_CONFIG_H

#define PACKAGE_NAME       "chocolate-doom"
#define PACKAGE_STRING     "chocolate-doom 3.1.0 (vkernel)"
#define PACKAGE_TARNAME    "chocolate-doom"
#define PACKAGE_VERSION    "3.1.0"

/* We have standard C string functions via newlib */
#define HAVE_DECL_STRCASECMP  1
#define HAVE_DECL_STRNCASECMP 1

/* Disable all optional features we don't support */
#define DISABLE_SDL2MIXER  1
#define DISABLE_SDL2NET    1

/* No libpng, no libsamplerate, no fluidsynth */
/* #undef HAVE_LIBPNG */
/* #undef HAVE_LIBSAMPLERATE */
/* #undef HAVE_FLUIDSYNTH */

/* Program prefix for binary names */
#define PROGRAM_PREFIX     "vk-"

/* Doom's IWAD search directory */
#define DOOMWADDIR         "/"

#endif /* DOOM_CONFIG_H */
