/*
 * es_window_vk.c - End screen window stub for vkernel
 * Replaces src/end_screen/src/es_window.c
 *
 * The end screen (exit screen) requires an SDL renderer/window.
 * On VK we simply skip it by returning false from ES_InitWindow.
 */

#include "es_window.h"
#include <stdio.h>

i32 ES_InitWindow(void)
{
    return false;
}

void ES_ShutdownWindow(void)
{
}

void ES_RefreshWindow(void)
{
}
