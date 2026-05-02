/*
 * snd_codec_vk.c - Sound codec init for vkernel (WAV only)
 * Replaces src/sound/src/snd_codec.c
 *
 * We only support WAV — no FLAC, OGG/Vorbis, or MP3.
 */

#include "snd_codec.h"
#include "console.h"
#include "snd_wave.h"
#include "zone.h"
#include <stdio.h>


static snd_codec_t *codecs;


void S_CodecRegister(snd_codec_t *codec)
{
    codec->next = codecs;
    codecs = codec;
}

void S_CodecInit(void)
{
    snd_codec_t *codec;
    codecs = NULL;

    /* Only WAV — skip FLAC, Vorbis, MP3 */
    S_CodecRegister(&wav_codec);

    codec = codecs;
    while (codec) {
        codec->initialize();
        codec = codec->next;
    }
}

void S_CodecShutdown(void)
{
    snd_codec_t *codec = codecs;
    while (codec) {
        codec->shutdown();
        codec = codec->next;
    }
    codecs = NULL;
}

snd_stream_t *S_CodecOpenStreamType(const char *filename, u32 type,
                                    qboolean loop)
{
    if (type == CODECTYPE_NONE) {
        Con_Printf("Bad type for %s\n", filename);
        return NULL;
    }

    snd_codec_t *codec = codecs;
    while (codec) {
        if (type == codec->type)
            break;
        codec = codec->next;
    }
    if (!codec) {
        Con_Printf("Unknown type for %s\n", filename);
        return NULL;
    }

    snd_stream_t *stream = S_CodecUtilOpen(filename, codec, loop);
    if (stream) {
        if (codec->codec_open(stream))
            stream->status = STREAM_PLAY;
        else
            S_CodecUtilClose(&stream);
    }
    return stream;
}

void S_CodecCloseStream(snd_stream_t *stream)
{
    stream->status = STREAM_NONE;
    stream->codec->codec_close(stream);
}

i32 S_CodecRewindStream(snd_stream_t *stream)
{
    return stream->codec->codec_rewind(stream);
}

i32 S_CodecJumpToOrder(snd_stream_t *stream, i32 to)
{
    if (stream->codec->codec_jump)
        return stream->codec->codec_jump(stream, to);
    return -1;
}

i32 S_CodecReadStream(snd_stream_t *stream, i32 bytes, void *buffer)
{
    return stream->codec->codec_read(stream, bytes, buffer);
}

snd_stream_t *S_CodecUtilOpen(const char *filename, snd_codec_t *codec,
                              qboolean loop)
{
    snd_stream_t *stream;
    FILE *handle;

    long length = COM_FindMusicTrack(filename, &handle);
    if (length == -1) {
        Con_DPrintf("Couldn't open %s\n", filename);
        return NULL;
    }

    stream = (snd_stream_t *)Z_Malloc(sizeof(snd_stream_t));
    stream->codec = codec;
    stream->loop = loop;
    stream->fh.file = handle;
    stream->fh.start = ftell(handle);
    stream->fh.pos = 0;
    stream->fh.length = length;
    Q_strncpy(stream->name, filename, MAX_QPATH);
    return stream;
}

void S_CodecUtilClose(snd_stream_t **stream)
{
    fclose((*stream)->fh.file);
    Z_Free(*stream);
    *stream = NULL;
}

i32 S_CodecIsAvailable(u32 type)
{
    snd_codec_t *codec = codecs;
    while (codec) {
        if (type == codec->type)
            return codec->initialized;
        codec = codec->next;
    }
    return -1;
}
