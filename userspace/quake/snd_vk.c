/*
 * snd_vk.c - Sound DMA driver for Chocolate Quake on vkernel
 * Replaces src/sound/src/snd_sdl.c
 *
 * The Quake sound engine fills a ring buffer (shm->buffer) via S_PaintChannels.
 * We play chunks from that ring buffer through the VK mixer.
 *
 * Strategy: use a single VK mix channel as a "stream" channel.
 * Periodically check if playing; if not, submit the next ~100ms chunk.
 */

#include "sound.h"
#include "sys.h"

#include <stdlib.h>
#include <string.h>

#include "../include/vk.h"

/* The DMA structure filled by the engine - global defined in snd_dma.c */
/* extern volatile dma_t* shm; -- declared in sound.h */

/* VK mix channel we use as the audio stream */
#define STREAM_CHANNEL (VK_SND_MIX_CHANNELS - 1)

/* Saved state for DMA position tracking */
static vk_u64 last_submit_tick = 0;
static vk_u32 last_submit_tps  = 1;
static int    last_submit_frames = 0;
static int    last_submit_samplepos = 0;

/* ---------------------------------------------------------------
 * Helpers
 * --------------------------------------------------------------- */

static int round_up_pow2(int n)
{
    if (n <= 0) return 1;
    n--;
    n |= n >> 1; n |= n >> 2; n |= n >> 4;
    n |= n >> 8; n |= n >> 16;
    return n + 1;
}

/* ---------------------------------------------------------------
 * SNDDMA interface
 * --------------------------------------------------------------- */

qboolean SNDDMA_Init(dma_t *dma)
{
    shm = dma;
    memset(shm, 0, sizeof(*shm));

    shm->samplebits = 16;
    shm->signed8    = 0;
    shm->speed      = 44100;
    shm->channels   = 2;   /* stereo */
    shm->samplepos  = 0;
    shm->submission_chunk = 1;

    /* Ring buffer: must be a power-of-2 number of stereo 16-bit frames.
     * ~1s of audio at 44100 Hz stereo 16-bit = 44100 * 2 * 2 = 176400 bytes.
     * Round up to next power-of-2. */
    int frames   = round_up_pow2(shm->speed);
    int nbytes   = frames * shm->channels * (shm->samplebits / 8);

    shm->samples = frames * shm->channels; /* total samples (L+R interleaved) */
    shm->buffer  = (byte *)calloc(1, (size_t)nbytes);
    if (!shm->buffer) {
        shm = NULL;
        return false;
    }

    VK_CALL(snd_set_volume, 255, 255);

    last_submit_tick     = 0;
    last_submit_tps      = 1;
    last_submit_frames   = 0;
    last_submit_samplepos = 0;

    return true;
}

int SNDDMA_GetDMAPos(void)
{
    if (!shm) return 0;
    if (!last_submit_frames)
        return shm->samplepos;

    vk_u64 now = VK_CALL(tick_count);
    vk_u32 tps = VK_CALL(ticks_per_sec);

    /* How many frames have been consumed since last submit */
    vk_u64 elapsed = now - last_submit_tick;
    int played_frames = (int)((elapsed * (vk_u64)last_submit_tps) / tps);
    if (played_frames > last_submit_frames)
        played_frames = last_submit_frames;

    int pos = (last_submit_samplepos
               + played_frames * shm->channels) % shm->samples;
    return pos;
}

void SNDDMA_Shutdown(void)
{
    if (!shm) return;
    VK_CALL(snd_mix_stop, STREAM_CHANNEL);
    VK_CALL(snd_stop);
    if (shm->buffer) {
        free(shm->buffer);
        shm->buffer = NULL;
    }
    shm = NULL;
}

void SNDDMA_LockBuffer(void)  {}
void SNDDMA_UnblockSound(void) {}
void SNDDMA_BlockSound(void)   {}

void SNDDMA_Submit(void)
{
    if (!shm || !shm->buffer) return;

    /* Already playing? skip this frame */
    if (VK_CALL(snd_mix_is_playing, STREAM_CHANNEL))
        return;

    /* Submit ~100ms worth of frames per call */
    int chunk_frames = shm->speed / 10; /* 4410 frames */
    if (chunk_frames < 512)  chunk_frames = 512;
    if (chunk_frames > shm->samples / shm->channels)
        chunk_frames = shm->samples / shm->channels / 2;

    /* Current byte position in ring buffer */
    int byte_pos = (shm->samplepos / shm->channels)
                   * shm->channels * (shm->samplebits / 8);
    int ring_bytes = shm->samples * (shm->samplebits / 8);
    byte_pos = byte_pos % ring_bytes;

    /* Make sure we don't submit data that wraps around in a single call */
    int avail_bytes = ring_bytes - byte_pos;
    int want_bytes  = chunk_frames * shm->channels * (shm->samplebits / 8);
    if (want_bytes > avail_bytes)
        want_bytes = avail_bytes;
    chunk_frames = want_bytes / (shm->channels * (shm->samplebits / 8));
    if (chunk_frames == 0)
        chunk_frames = 1;

    last_submit_tick      = VK_CALL(tick_count);
    last_submit_tps       = VK_CALL(ticks_per_sec);
    last_submit_frames    = chunk_frames;
    last_submit_samplepos = shm->samplepos;

    VK_CALL(snd_mix_play,
            STREAM_CHANNEL,
            shm->buffer + byte_pos,
            chunk_frames,
            VK_SND_FORMAT_SIGNED_16_STEREO,
            (vk_u32)shm->speed,
            255, 255);

    shm->samplepos = (shm->samplepos
                      + chunk_frames * shm->channels) % shm->samples;
}
