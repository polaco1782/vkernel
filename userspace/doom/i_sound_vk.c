/*
 * i_sound_vk.c - Chocolate Doom sound for vkernel
 *
 * Replaces i_sound.c + i_sdlsound.c.  Implements the sound_module_t
 * interface using the vkernel sound API (SB16 emulation in QEMU).
 *
 * Music is stubbed out (no OPL/MIDI support yet).
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "config.h"
#include "doomtype.h"
#include "i_sound.h"
#include "i_system.h"
#include "m_argv.h"
#include "m_config.h"
#include "m_misc.h"
#include "w_wad.h"
#include "z_zone.h"
#include "d_mode.h"

#include "../include/vk.h"

/* ---- per-channel playback end-time (in kernel ticks) ---- */
static vk_u64 channel_end_tick[8];

/* ---- config variables (referenced from m_config.c) ---- */
int snd_samplerate = 44100;
int snd_cachesize = 64 * 1024 * 1024;
int snd_maxslicetime_ms = 28;
char *snd_musiccmd = "";
int snd_pitchshift = -1;
int snd_musicdevice = SNDDEVICE_NONE;
int snd_sfxdevice = SNDDEVICE_SB;

/* DOS compat stubs */
static int snd_sbport = 0;
static int snd_sbirq = 0;
static int snd_sbdma = 0;
static int snd_mport = 0;

/* ============================================================
 * vkernel sound module — simple single-channel SFX playback
 *
 * Doom SFX lumps have an 8-byte header:
 *   u16 format (3 = PCM)
 *   u16 sample_rate
 *   u32 num_samples
 *   ... unsigned 8-bit PCM data
 * ============================================================ */

#define MAX_CHANNELS 8

typedef struct {
    const uint8_t *data;
    uint32_t       length;
    uint32_t       pos;
    int            vol;
    int            sep;
    int            active;
} snd_channel_t;

static snd_channel_t channels[MAX_CHANNELS];

static boolean vk_snd_init(GameMission_t mission)
{
    (void)mission;

    /* Program the AC97 codec to the configured output rate. */
    VK_CALL(snd_set_sample_rate, (uint32_t)snd_samplerate);
    VK_CALL(snd_set_volume, 127, 127);

    memset(channels, 0, sizeof(channels));
    return true;
}

static void vk_snd_shutdown(void)
{
    VK_CALL(snd_stop);
}

static int vk_snd_get_sfx_lump_num(sfxinfo_t *sfx)
{
    char namebuf[16];
    memset(namebuf, 0, sizeof(namebuf));

    M_snprintf(namebuf, sizeof(namebuf), "ds%s", sfx->name);
    return W_CheckNumForName(namebuf);
}

static void vk_snd_update(void)
{
    /* Mix all active channels into a temporary buffer and submit to kernel */
    /* For simplicity, we submit one channel at a time via vk_snd_play.
     * A more sophisticated implementation would mix channels together. */
}

static void vk_snd_update_params(int channel, int vol, int sep)
{
    if (channel < 0 || channel >= MAX_CHANNELS) return;
    channels[channel].vol = vol;
    channels[channel].sep = sep;
}

static int vk_snd_start(sfxinfo_t *sfx, int channel, int vol, int sep, int pitch)
{
    (void)pitch;

    if (channel < 0 || channel >= MAX_CHANNELS)
        return -1;

    int lumpnum = sfx->lumpnum;
    if (lumpnum < 0) return -1;

    int lumplen = W_LumpLength(lumpnum);
    if (lumplen < 8) return -1;

    const uint8_t *lumpdata = (const uint8_t *)W_CacheLumpNum(lumpnum, PU_STATIC);

    /* Parse doom sound header:  format(2) rate(2) samples(4) data... */
    uint32_t sample_rate = (uint32_t)lumpdata[2] | ((uint32_t)lumpdata[3] << 8);
    uint32_t num_samples = (uint32_t)lumpdata[4] | ((uint32_t)lumpdata[5] << 8)
                         | ((uint32_t)lumpdata[6] << 16) | ((uint32_t)lumpdata[7] << 24);

    if (num_samples + 8 > (uint32_t)lumplen)
        num_samples = (uint32_t)lumplen - 8;

    /* Stop previous sound on this channel. */
    channels[channel].active = 0;

    /* Program the codec to the lump's native rate then play.
     * The AC97 Variable Rate Audio (VRA) handles any rate the lump uses.
     * The driver expands 8-bit mono to stereo 16-bit internally. */
    VK_CALL(snd_set_sample_rate, sample_rate);
    VK_CALL(snd_play, lumpdata + 8, num_samples, VK_SND_FORMAT_UNSIGNED_8);

    /* Record when this sound will finish (for vk_snd_playing). */
    vk_u64 tps = VK_CALL(ticks_per_sec);
    vk_u64 duration_ticks = ((vk_u64)num_samples * tps) / (vk_u64)sample_rate + tps / 20;
    channel_end_tick[channel] = VK_CALL(tick_count) + duration_ticks;

    channels[channel].data   = lumpdata + 8;
    channels[channel].length = num_samples;
    channels[channel].pos    = 0;
    channels[channel].vol    = vol;
    channels[channel].sep    = sep;
    channels[channel].active = 1;

    return channel;
}

static void vk_snd_stop_ch(int channel)
{
    if (channel < 0 || channel >= MAX_CHANNELS) return;
    if (channels[channel].active) {
        channels[channel].active = 0;
        VK_CALL(snd_stop);
    }
}

static boolean vk_snd_playing(int channel)
{
    if (channel < 0 || channel >= MAX_CHANNELS) return false;
    if (!channels[channel].active) return false;
    /* Use elapsed time rather than querying the DSP — the kernel never
     * auto-resets s_playing, so vk_snd_is_playing() would always return
     * true and Doom would think all channels are permanently busy. */
    if (VK_CALL(tick_count) >= channel_end_tick[channel]) {
        channels[channel].active = 0;
        return false;
    }
    return true;
}

static void vk_snd_precache(sfxinfo_t *sounds, int num_sounds)
{
    (void)sounds; (void)num_sounds;
}

static const snddevice_t vk_sound_devices[] = {
    SNDDEVICE_SB,
    SNDDEVICE_ADLIB,
    SNDDEVICE_PAS,
    SNDDEVICE_GUS,
    SNDDEVICE_WAVEBLASTER,
    SNDDEVICE_AWE32,
};

const sound_module_t sound_vk_module = {
    vk_sound_devices,
    sizeof(vk_sound_devices) / sizeof(*vk_sound_devices),
    vk_snd_init,
    vk_snd_shutdown,
    vk_snd_get_sfx_lump_num,
    vk_snd_update,
    vk_snd_update_params,
    vk_snd_start,
    vk_snd_stop_ch,
    vk_snd_playing,
    vk_snd_precache,
};

/* ============================================================
 * Music module — stub (no music support yet)
 * ============================================================ */

static boolean vk_mus_init(void) { return false; }
static void vk_mus_shutdown(void) {}
static void vk_mus_set_volume(int vol) { (void)vol; }
static void vk_mus_pause(void) {}
static void vk_mus_resume(void) {}
static void *vk_mus_register(void *data, int len) { (void)data;(void)len; return NULL; }
static void vk_mus_unregister(void *handle) { (void)handle; }
static void vk_mus_play(void *handle, boolean looping) { (void)handle;(void)looping; }
static void vk_mus_stop(void) {}
static boolean vk_mus_playing(void) { return false; }

static const snddevice_t vk_music_devices[] = { SNDDEVICE_SB };

const music_module_t music_vk_module = {
    vk_music_devices,
    sizeof(vk_music_devices) / sizeof(*vk_music_devices),
    vk_mus_init,
    vk_mus_shutdown,
    vk_mus_set_volume,
    vk_mus_pause,
    vk_mus_resume,
    vk_mus_register,
    vk_mus_unregister,
    vk_mus_play,
    vk_mus_stop,
    vk_mus_playing,
    NULL,  /* Poll */
};

/* ============================================================
 * Public sound interface (from i_sound.h)
 *
 * We bypass the module search in the original i_sound.c and wire
 * directly to our vkernel implementation.
 * ============================================================ */

static const sound_module_t *active_sound  = NULL;
static const music_module_t *active_music  = NULL;

void I_InitSound(GameMission_t mission)
{
    if (snd_sfxdevice != SNDDEVICE_NONE) {
        if (vk_snd_init(mission)) {
            active_sound = &sound_vk_module;
        }
    }
    /* Music disabled for now */
    active_music = NULL;
}

void I_ShutdownSound(void)
{
    if (active_sound) active_sound->Shutdown();
}

int I_GetSfxLumpNum(sfxinfo_t *sfx)
{
    if (active_sound) return active_sound->GetSfxLumpNum(sfx);
    return 0;
}

void I_UpdateSound(void)
{
    if (active_sound) active_sound->Update();
}

void I_UpdateSoundParams(int channel, int vol, int sep)
{
    if (active_sound) active_sound->UpdateSoundParams(channel, vol, sep);
}

int I_StartSound(sfxinfo_t *sfx, int channel, int vol, int sep, int pitch)
{
    if (active_sound) return active_sound->StartSound(sfx, channel, vol, sep, pitch);
    return -1;
}

void I_StopSound(int channel)
{
    if (active_sound) active_sound->StopSound(channel);
}

boolean I_SoundIsPlaying(int channel)
{
    if (active_sound) return active_sound->SoundIsPlaying(channel);
    return false;
}

void I_PrecacheSounds(sfxinfo_t *sounds, int num_sounds)
{
    if (active_sound) active_sound->CacheSounds(sounds, num_sounds);
}

/* Music interface */
void I_InitMusic(void) {}
void I_ShutdownMusic(void) {}
void I_SetMusicVolume(int vol) { (void)vol; }
void I_PauseSong(void) {}
void I_ResumeSong(void) {}
void *I_RegisterSong(void *data, int len) { (void)data;(void)len; return NULL; }
void I_UnRegisterSong(void *handle) { (void)handle; }
void I_PlaySong(void *handle, boolean looping) { (void)handle;(void)looping; }
void I_StopSong(void) {}
boolean I_MusicIsPlaying(void) { return false; }

void I_BindSoundVariables(void)
{
    M_BindIntVariable("snd_musicdevice",      &snd_musicdevice);
    M_BindIntVariable("snd_sfxdevice",        &snd_sfxdevice);
    M_BindIntVariable("snd_samplerate",       &snd_samplerate);
    M_BindIntVariable("snd_cachesize",        &snd_cachesize);
    M_BindIntVariable("snd_maxslicetime_ms",  &snd_maxslicetime_ms);
    M_BindStringVariable("snd_musiccmd",      &snd_musiccmd);
    M_BindIntVariable("snd_pitchshift",       &snd_pitchshift);
    M_BindIntVariable("snd_sbport",           &snd_sbport);
    M_BindIntVariable("snd_sbirq",            &snd_sbirq);
    M_BindIntVariable("snd_sbdma",            &snd_sbdma);
    M_BindIntVariable("snd_mport",            &snd_mport);
}
