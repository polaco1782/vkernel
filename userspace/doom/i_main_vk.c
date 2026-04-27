/*
 * i_main_vk.c - Chocolate Doom entry point for vkernel
 *
 * Replaces i_main.c.  Sets up arguments and calls D_DoomMain.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "config.h"
#include "doomtype.h"
#include "i_system.h"
#include "m_argv.h"
#include "m_misc.h"

void D_DoomMain(void);

int main(int argc, char **argv)
{
    /* Set up argument parsing */
    myargc = argc;
    myargv = malloc(argc * sizeof(char *));

    for (int i = 0; i < argc; i++)
        myargv[i] = M_StringDuplicate(argv[i]);

    if (M_ParmExists("-version") || M_ParmExists("--version")) {
        puts(PACKAGE_STRING);
        exit(0);
    }

    M_FindResponseFile();
    M_SetExeDir();

    /* Start doom */
    D_DoomMain();

    return 0;
}
