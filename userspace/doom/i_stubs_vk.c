/*
 * i_stubs_vk.c - Stub implementations for optional subsystems on vkernel
 *
 * Provides no-op implementations for: joystick, CD music, endoom display,
 * high-res video, file globbing, PC speaker sound, OPL music, music packs,
 * SDL music, SDL sound, and network modules that Chocolate Doom normally
 * compiles but which we don't need on vkernel.
 */

#include <stdlib.h>
#include <string.h>
#include <stdio.h>

#include "config.h"
#include "doomtype.h"
#include "d_event.h"
#include "d_mode.h"
#include "i_sound.h"
#include "m_config.h"

/* ---- Joystick (i_joystick.c) ---- */

int usejoystick = 0;
int joystick_index = -1;

void I_InitJoystick(void) {}
void I_ShutdownJoystick(void) {}
void I_UpdateJoystick(void) {}
void I_BindJoystickVariables(void)
{
    M_BindIntVariable("use_joystick", &usejoystick);
    M_BindIntVariable("joystick_index", &joystick_index);
}

/* ---- CD Music (i_cdmus.c) ---- */
/* nothing needed - header only */

/* ---- Endoom (i_endoom.c) ---- */
void I_Endoom(byte *data) { (void)data; }

/* ---- i_videohr.c ---- */
void I_SetVideoMode(void) {}

/* ---- i_glob.c stubs ---- */
/* glob_t structure used for file enumeration */
typedef struct { void *_opaque; } glob_t_vk;

void *I_StartGlob(const char *dir, const char *glob, int flags)
{
    (void)dir; (void)glob; (void)flags;
    return NULL;
}

const char *I_NextGlob(void *handle)
{
    (void)handle;
    return NULL;
}

void I_EndGlob(void *handle)
{
    (void)handle;
}

/* ---- PC speaker sound module (i_pcsound.c) ---- */
static const snddevice_t pcsound_devices[] = { SNDDEVICE_PCSPEAKER };

static boolean PCSnd_Init(GameMission_t m) { (void)m; return false; }
static void    PCSnd_Shutdown(void) {}
static int     PCSnd_GetSfxLumpNum(sfxinfo_t *s) { (void)s; return 0; }
static void    PCSnd_Update(void) {}
static void    PCSnd_UpdateParams(int c, int v, int s) { (void)c;(void)v;(void)s; }
static int     PCSnd_Start(sfxinfo_t *s, int c, int v, int sp, int p) {
    (void)s;(void)c;(void)v;(void)sp;(void)p; return -1;
}
static void    PCSnd_Stop(int c) { (void)c; }
static boolean PCSnd_Playing(int c) { (void)c; return false; }
static void    PCSnd_Precache(sfxinfo_t *s, int n) { (void)s;(void)n; }

const sound_module_t sound_pcsound_module = {
    pcsound_devices, 1,
    PCSnd_Init, PCSnd_Shutdown, PCSnd_GetSfxLumpNum,
    PCSnd_Update, PCSnd_UpdateParams, PCSnd_Start,
    PCSnd_Stop, PCSnd_Playing, PCSnd_Precache,
};

/* ---- OPL music module (i_oplmusic.c) ---- */
static const snddevice_t opl_devices[] = { SNDDEVICE_ADLIB, SNDDEVICE_SB };

static boolean OPLMus_Init(void) { return false; }
static void    OPLMus_Shutdown(void) {}
static void    OPLMus_SetVol(int v) { (void)v; }
static void    OPLMus_Pause(void) {}
static void    OPLMus_Resume(void) {}
static void   *OPLMus_Register(void *d, int l) { (void)d;(void)l; return NULL; }
static void    OPLMus_Unregister(void *h) { (void)h; }
static void    OPLMus_Play(void *h, boolean l) { (void)h;(void)l; }
static void    OPLMus_Stop(void) {}
static boolean OPLMus_Playing(void) { return false; }

const music_module_t music_opl_module = {
    opl_devices, 2,
    OPLMus_Init, OPLMus_Shutdown, OPLMus_SetVol,
    OPLMus_Pause, OPLMus_Resume, OPLMus_Register, OPLMus_Unregister,
    OPLMus_Play, OPLMus_Stop, OPLMus_Playing, NULL,
};

/* ---- GUS config (gusconf.c) ---- */
char *gus_patch_path = "";
int gus_ram_kb = 1024;

/* ---- Timidity config stubs ---- */
char *timidity_cfg_path = "";

/* ---- pcsound library stubs ---- */
/* From pcsound/pcsound.c */
int pcsound_sample_rate = 0;

void PCSound_SetSampleRate(int rate) { (void)rate; }
int  PCSound_Init(void (*callback)(int *duration, int *freq)) {
    (void)callback; return 0;
}
void PCSound_Shutdown(void) {}

/* ---- OPL library stubs ---- */
typedef void *opl_callback_t;
int  OPL_Init(unsigned int port_base) { (void)port_base; return 0; }
void OPL_Shutdown(void) {}
void OPL_WritePort(unsigned int port, unsigned int val) { (void)port;(void)val; }
unsigned int OPL_ReadPort(unsigned int port) { (void)port; return 0; }
void OPL_SetCallback(uint64_t us, void *cb, void *data) { (void)us;(void)cb;(void)data; }
void OPL_ClearCallbacks(void) {}
void OPL_Lock(void) {}
void OPL_Unlock(void) {}
void OPL_SetPaused(int paused) { (void)paused; }
void OPL_AdjustCallbacks(float val) { (void)val; }
void OPL_InitRegisters(int opl3) { (void)opl3; }

/* ---- net_sdl.c stubs ---- */
/* The net_sdl module is needed for compilation but we disable networking */

void *net_sdl_module = NULL;

/* ---- net_gui.c stubs ---- */
#include "net_client.h"
#include "net_gui.h"
#include "net_server.h"
#include "net_query.h"
#include "net_defs.h"

void NET_WaitForLaunch(void) {}

/* ---- net_query.c stubs ---- */
net_addr_t *NET_Query_ResolveMaster(net_context_t *ctx) { (void)ctx; return NULL; }
void NET_Query_AddToMaster(net_addr_t *addr) { (void)addr; }
void NET_Query_AddResponse(net_packet_t *pkt) { (void)pkt; }
int NET_Query_MasterQuery(net_context_t *ctx) { (void)ctx; return 0; }
int NET_Query_SendQuery(net_addr_t *addr) { (void)addr; return 0; }
int NET_Query_Poll(net_query_callback_t cb, void *data) { (void)cb;(void)data; return 0; }
net_addr_t *NET_Query_ResolveAddress(net_context_t *ctx, const char *s) { (void)ctx;(void)s; return NULL; }
int NET_Query_QueueQuery(net_addr_t *addr) { (void)addr; return 0; }
void NET_QueryAddress(const char *addr) { (void)addr; }
void NET_LANQuery(void) {}
void NET_MasterQuery(void) {}
net_addr_t *NET_FindLANServer(void) { return NULL; }
void NET_RequestHolePunch(net_context_t *ctx, net_addr_t *addr) { (void)ctx;(void)addr; }

/* ---- Joystick variables (from i_joystick.c) ---- */
int joywait = 0;
int use_analog = 0;
int joystick_move_sensitivity = 0;
int joystick_turn_sensitivity = 0;

/* ---- Video variables ---- */
int screensaver_mode = 0;
int screenvisible = 1;

/* ---- OPL stubs ---- */
void I_OPL_DevMessages(char *buf, size_t len) { (void)buf;(void)len; if (len > 0) buf[0] = '\0'; }
void I_SetOPLDriverVer(opl_driver_ver_t ver) { (void)ver; }

/* ---- File globbing (i_glob.c) — multi-pattern variant ---- */
void *I_StartMultiGlob(const char *dir, int flags, const char *glob, ...) {
    (void)dir; (void)flags; (void)glob;
    return NULL;
}

/* ---- mkdir stub ---- */
#include <sys/stat.h>
#include <errno.h>
/* newlib's x86_64-elf target may lack mkdir — provide a stub */
int mkdir(const char *path, mode_t mode) __attribute__((weak));
int mkdir(const char *path, mode_t mode) {
    (void)path; (void)mode;
    errno = ENOSYS;
    return -1;
}
