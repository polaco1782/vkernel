/*
 * i_sound_vk.c - Chocolate Doom sound for vkernel
 *
 * Replaces i_sound.c + i_sdlsound.c.  Implements the sound_module_t
 * interface using the vkernel sound API (SB16 emulation in QEMU).
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

#define MUSIC_CHANNEL (VK_SND_MIX_CHANNELS - 1)
#define MUSIC_SLICE_MAX_FRAMES 4096

void OPL_VK_Render(int16_t *buffer, unsigned int nsamples);

/* ---- config variables (referenced from m_config.c) ---- */
int snd_samplerate = 44100;
int snd_cachesize = 64 * 1024 * 1024;
int snd_maxslicetime_ms = 28;
char *snd_musiccmd = "";
int snd_pitchshift = -1;
int snd_musicdevice = SNDDEVICE_SB;
int snd_sfxdevice = SNDDEVICE_SB;

/* DOS compat stubs */
static int snd_sbport = 0;
static int snd_sbirq = 0;
static int snd_sbdma = 0;
static int snd_mport = 0;

static const sound_module_t *active_sound  = NULL;
static const music_module_t *active_music  = NULL;
static int music_initialized = 0;
static int16_t music_buffer[MUSIC_SLICE_MAX_FRAMES * 2];

/* ============================================================
 * vkernel sound module — simple single-channel SFX playback
 *
 * Doom SFX lumps have an 8-byte header:
 *   u16 format (3 = PCM)
 *   u16 sample_rate
 *   u32 num_samples
 *   ... unsigned 8-bit PCM data
 * ============================================================ */

#define MAX_CHANNELS MUSIC_CHANNEL

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

    /* Set master hardware volume to maximum; per-channel volume is
     * controlled via the software mixer. */
    VK_CALL(snd_set_volume, 255, 255);

    memset(channels, 0, sizeof(channels));
    return true;
}

static void vk_snd_shutdown(void)
{
    /* Stop all mixer channels then the hardware. */
    for (int i = 0; i < MAX_CHANNELS; ++i)
        VK_CALL(snd_mix_stop, i);
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
    /* Re-submit the mixed buffer if the hardware window has expired
     * but mixer channels still have remaining data. */
    VK_CALL(snd_mix_update);
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

    /* Compute per-channel L/R volumes from Doom's vol (0-127) and
     * sep (0-255, 128 = centre) parameters. */
    uint32_t vol_l_u = ((uint32_t)vol * (255u - (uint32_t)sep)) >> 7;
    uint32_t vol_r_u = ((uint32_t)vol * (uint32_t)sep) >> 7;
    uint8_t vol_l = (vol_l_u > 255u) ? 255u : (uint8_t)vol_l_u;
    uint8_t vol_r = (vol_r_u > 255u) ? 255u : (uint8_t)vol_r_u;

    /* Submit to the software mixer — it handles resampling to 48 kHz
     * and blending with any other active channels. */
    VK_CALL(snd_mix_play, channel, lumpdata + 8, num_samples,
            VK_SND_FORMAT_UNSIGNED_8, sample_rate, vol_l, vol_r);

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
        VK_CALL(snd_mix_stop, channel);
    }
}

static boolean vk_snd_playing(int channel)
{
    if (channel < 0 || channel >= MAX_CHANNELS) return false;
    if (!channels[channel].active) return false;
    /* Delegate to the kernel mixer which tracks per-channel position. */
    if (!VK_CALL(snd_mix_is_playing, channel)) {
        channels[channel].active = 0;
        return false;
    }
    return true;
}

static void vk_snd_precache(sfxinfo_t *sounds, int num_sounds)
{
    (void)sounds; (void)num_sounds;
}

static unsigned int vk_music_slice_frames(void)
{
    unsigned int frames;

    frames = (unsigned int) (((uint64_t) snd_samplerate * (uint64_t) snd_maxslicetime_ms) / 1000u);

    if (frames < 256)
    {
        frames = 256;
    }
    else if (frames > MUSIC_SLICE_MAX_FRAMES)
    {
        frames = MUSIC_SLICE_MAX_FRAMES;
    }

    return frames;
}

static void vk_music_poll(void)
{
    unsigned int frames;

    if (!music_initialized || active_music == NULL || active_music != &music_opl_module)
    {
        return;
    }

    if (!active_music->MusicIsPlaying())
    {
        VK_CALL(snd_mix_stop, MUSIC_CHANNEL);
        return;
    }

    if (VK_CALL(snd_mix_is_playing, MUSIC_CHANNEL))
    {
        return;
    }

    frames = vk_music_slice_frames();
    OPL_VK_Render(music_buffer, frames);
    VK_CALL(snd_mix_play, MUSIC_CHANNEL, music_buffer, frames,
            VK_SND_FORMAT_SIGNED_16_STEREO, snd_samplerate, 255, 255);
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
 * Public sound interface (from i_sound.h)
 *
 * We bypass the module search in the original i_sound.c and wire
 * directly to our vkernel implementation.
 * ============================================================ */

void I_InitSound(GameMission_t mission)
{
    boolean nosound;
    boolean nosfx;
    boolean nomusic;

    nosound = M_CheckParm("-nosound") > 0;
    nosfx = M_CheckParm("-nosfx") > 0;
    nomusic = M_CheckParm("-nomusic") > 0;

    active_sound = NULL;
    active_music = NULL;
    music_initialized = 0;

    if (!nosound && !nosfx && snd_sfxdevice != SNDDEVICE_NONE) {
        if (vk_snd_init(mission)) {
            active_sound = &sound_vk_module;
        }
    }

    if (!nosound && !nomusic && snd_musicdevice != SNDDEVICE_NONE) {
        if (music_opl_module.Init()) {
            active_music = &music_opl_module;
            music_initialized = 1;
        }
    }
}

void I_ShutdownSound(void)
{
    if (active_sound) active_sound->Shutdown();
    if (active_music) active_music->Shutdown();
    active_sound = NULL;
    active_music = NULL;
    music_initialized = 0;
}

int I_GetSfxLumpNum(sfxinfo_t *sfx)
{
    if (active_sound) return active_sound->GetSfxLumpNum(sfx);
    return 0;
}

void I_UpdateSound(void)
{
    if (active_sound) active_sound->Update();
    vk_music_poll();
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
void I_ShutdownMusic(void)
{
    if (active_music) {
        active_music->Shutdown();
        active_music = NULL;
        music_initialized = 0;
    }
}
void I_SetMusicVolume(int vol)
{
    if (active_music) active_music->SetMusicVolume(vol);
}
void I_PauseSong(void)
{
    if (active_music) active_music->PauseMusic();
}
void I_ResumeSong(void)
{
    if (active_music) active_music->ResumeMusic();
}
void *I_RegisterSong(void *data, int len)
{
    if (active_music) return active_music->RegisterSong(data, len);
    return NULL;
}
void I_UnRegisterSong(void *handle)
{
    if (active_music) active_music->UnRegisterSong(handle);
}
void I_PlaySong(void *handle, boolean looping)
{
    if (active_music) {
        active_music->PlaySong(handle, looping);
        vk_music_poll();
    }
}
void I_StopSong(void)
{
    if (active_music) active_music->StopSong();
    VK_CALL(snd_mix_stop, MUSIC_CHANNEL);
}
boolean I_MusicIsPlaying(void)
{
    if (active_music) return active_music->MusicIsPlaying();
    return false;
}

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
    M_BindStringVariable("snd_dmxoption",     &snd_dmxoption);
    M_BindIntVariable("opl_io_port",          &opl_io_port);
}
