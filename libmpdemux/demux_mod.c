/*
 * MOD/XM/S3M/IT demuxer for MPlayer using libmodplug
 * AmigaOS4 port - Sitzung 13
 */

#include "config.h"
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include "mp_msg.h"
#include "stream/stream.h"
#include "demuxer.h"
#include "stheader.h"
#include <libmodplug/modplug.h>

#define MOD_SAMPLERATE  44100
#define MOD_CHANNELS    2
#define MOD_BITS        16
#define MOD_BLOCK_SIZE  4096
#include "libaf/af_format.h"

typedef struct {
    ModPlugFile *mpf;
    int length_ms;
} mod_priv_t;

static int demux_mod_check(demuxer_t *demuxer) {
    const char *filename = demuxer->stream->url;
    if (!filename) return 0;
    const char *ext = strrchr(filename, '.');
    if (!ext) return 0;
    ext++;
    if (strcasecmp(ext, "mod") == 0 ||
        strcasecmp(ext, "xm")  == 0 ||
        strcasecmp(ext, "s3m") == 0 ||
        strcasecmp(ext, "it")  == 0 ||
        strcasecmp(ext, "669") == 0 ||
        strcasecmp(ext, "med") == 0 ||
        strcasecmp(ext, "stm") == 0 ||
        strcasecmp(ext, "mtm") == 0 ||
        strcasecmp(ext, "ult") == 0 ||
        strcasecmp(ext, "far") == 0 ||
        strcasecmp(ext, "dbm") == 0 ||
        strcasecmp(ext, "umx") == 0 ||
        strcasecmp(ext, "okt") == 0 ||
        strcasecmp(ext, "mdl") == 0)
        return DEMUXER_TYPE_MOD;
    return 0;
}

static demuxer_t *demux_mod_open(demuxer_t *demuxer) {
    mod_priv_t *priv;
    ModPlug_Settings settings;
    sh_audio_t *sh;
    int filesize;
    void *filedata;

    // Ganze Datei in Speicher laden
    stream_seek(demuxer->stream, 0);
    filesize = demuxer->stream->end_pos;
    if (filesize <= 0) {
        mp_msg(MSGT_DEMUX, MSGL_ERR, "[MOD] Cannot get file size\n");
        return NULL;
    }

    filedata = malloc(filesize);
    if (!filedata) {
        mp_msg(MSGT_DEMUX, MSGL_ERR, "[MOD] Out of memory\n");
        return NULL;
    }

    if (stream_read(demuxer->stream, filedata, filesize) != filesize) {
        free(filedata);
        mp_msg(MSGT_DEMUX, MSGL_ERR, "[MOD] Read error\n");
        return NULL;
    }

    // libmodplug Settings
    ModPlug_GetSettings(&settings);
    settings.mChannels       = MOD_CHANNELS;
    settings.mBits           = MOD_BITS;
    settings.mFrequency      = MOD_SAMPLERATE;
    settings.mResamplingMode = MODPLUG_RESAMPLE_LINEAR;
    settings.mFlags          = MODPLUG_ENABLE_OVERSAMPLING | MODPLUG_ENABLE_NOISE_REDUCTION;
    settings.mLoopCount      = 0;
    settings.mMaxMixChannels = 64;
    ModPlug_SetSettings(&settings);

    priv = calloc(1, sizeof(mod_priv_t));
    priv->mpf = ModPlug_Load(filedata, filesize);
    free(filedata);

    if (!priv->mpf) {
        free(priv);
        mp_msg(MSGT_DEMUX, MSGL_ERR, "[MOD] Cannot load module\n");
        return NULL;
    }

    priv->length_ms = ModPlug_GetLength(priv->mpf);
    const char *name = ModPlug_GetName(priv->mpf);

    mp_msg(MSGT_DEMUX, MSGL_INFO, "[MOD] Module: %s  Length: %d ms\n",
           name ? name : "unknown", priv->length_ms);

    // Audio Stream erstellen
    sh = new_sh_audio(demuxer, 0, 0);
    sh->format        = 0x0001; // WAVE PCM
    sh->sample_format = AF_FORMAT_S16_BE;
    sh->channels      = MOD_CHANNELS;
    sh->samplerate    = MOD_SAMPLERATE;
    sh->samplesize    = MOD_BITS / 8;
    sh->i_bps         = MOD_SAMPLERATE * MOD_CHANNELS * (MOD_BITS / 8);
    sh->wf            = calloc(1, sizeof(WAVEFORMATEX));
    sh->wf->wFormatTag      = 0x0001;
    sh->wf->nChannels       = MOD_CHANNELS;
    sh->wf->nSamplesPerSec  = MOD_SAMPLERATE;
    sh->wf->wBitsPerSample  = MOD_BITS;
    sh->wf->nBlockAlign     = MOD_CHANNELS * (MOD_BITS / 8);
    sh->wf->nAvgBytesPerSec = sh->i_bps;

    demuxer->audio->sh = sh;
    sh->ds = demuxer->audio;
    demuxer->audio->id = 0;
    demuxer->priv = priv;

    // Länge setzen
    demuxer->movi_start = 0;
    demuxer->movi_end   = priv->length_ms;

    return demuxer;
}

static int demux_mod_fill_buffer(demuxer_t *demuxer, demux_stream_t *ds) {
    mod_priv_t *priv = demuxer->priv;
    demux_packet_t *dp;
    int len;

    dp = new_demux_packet(MOD_BLOCK_SIZE);
    if (!dp) return 0;

    len = ModPlug_Read(priv->mpf, dp->buffer, MOD_BLOCK_SIZE);
    if (len <= 0) {
        free_demux_packet(dp);
        return 0;
    }

    /* Byte-swap s16le -> s16be für AmigaOS4 (Big-Endian) */
    {
        int i;
        unsigned char *p = dp->buffer;
        for (i = 0; i < len - 1; i += 2) {
            unsigned char tmp = p[i];
            p[i] = p[i+1];
            p[i+1] = tmp;
        }
    }

    resize_demux_packet(dp, len);
    dp->pts = 0;
    dp->flags = 0;
    ds_add_packet(demuxer->audio, dp);
    return 1;
}

static void demux_mod_seek(demuxer_t *demuxer, float rel_seek_secs,
                            float audio_delay, int flags) {
    mod_priv_t *priv = demuxer->priv;
    int pos_ms;
    if (flags & SEEK_ABSOLUTE)
        pos_ms = (int)(rel_seek_secs * 1000);
    else
        pos_ms = (int)(rel_seek_secs * 1000); // relativ - vereinfacht
    if (pos_ms < 0) pos_ms = 0;
    ModPlug_Seek(priv->mpf, pos_ms);
}

static void demux_mod_close(demuxer_t *demuxer) {
    mod_priv_t *priv = demuxer->priv;
    if (priv) {
        if (priv->mpf) ModPlug_Unload(priv->mpf);
        free(priv);
    }
}

static int demux_mod_control(demuxer_t *demuxer, int cmd, void *arg) {
    mod_priv_t *priv = demuxer->priv;
    switch (cmd) {
    case DEMUXER_CTRL_GET_TIME_LENGTH:
        *(double *)arg = priv->length_ms / 1000.0;
        return DEMUXER_CTRL_OK;
    case DEMUXER_CTRL_GET_PERCENT_POS:
        *(int *)arg = 0;
        return DEMUXER_CTRL_OK;
    }
    return DEMUXER_CTRL_NOTIMPL;
}

const demuxer_desc_t demuxer_desc_mod = {
    "MOD/XM/S3M/IT module demuxer (libmodplug)",
    "mod",
    "MOD",
    "AmigaOS4 port",
    "Supports MOD XM S3M IT 669 MED STM MTM ULT FAR",
    DEMUXER_TYPE_MOD,
    1,
    demux_mod_check,
    demux_mod_fill_buffer,
    demux_mod_open,
    demux_mod_close,
    demux_mod_seek,
    demux_mod_control
};
