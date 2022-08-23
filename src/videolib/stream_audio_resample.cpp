/*****************************************************************************
 *
 * stream_audio_resample.cpp
 *   Audio resampling node based on ffmpeg API.
 *
 *****************************************************************************
 *
 * Copyright 2013-2022 Sighthound, Inc.
 *
 * Licensed under the GNU GPLv3 license found at
 * https://www.gnu.org/licenses/gpl-3.0.txt
 *
 * Alternative licensing available from Sighthound, Inc.
 * by emailing opensource@sighthound.com
 *
 * This file is part of the Sighthound Video project which can be found at
 * https://github.url/thing
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; using version 3 of the License.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02111, USA.
 *
 *****************************************************************************/

#undef SV_MODULE_VAR
#define SV_MODULE_VAR rsm
#define SV_MODULE_ID "RESAMPLE"
#include "sv_module_def.hpp"

#include "streamprv.h"
#include "sv_ffmpeg.h"

#include "frame_basic.h"

#include "videolibUtils.h"

#define RESAMPLE_FILTER_MAGIC 0x1295

//-----------------------------------------------------------------------------
typedef struct resample_filter  : public stream_base  {
    int                 srcChannels;
    int                 srcSampleRate;
    int                 srcSampleFormat;
    enum AVSampleFormat fmtIn; // same as srcSampleFormat, but in ffmpeg speak
    int                 dstSampleRate;
    int                 dstSampleFormat;
    enum AVSampleFormat fmtOut; // same as dstSampleFormat, but in ffmpeg speak
    struct SwrContext*  swr;

    frame_allocator*    fa;
} resample_filter_obj;

//-----------------------------------------------------------------------------
// Stream API
//-----------------------------------------------------------------------------

//-----------------------------------------------------------------------------
//  Forward declarations
//-----------------------------------------------------------------------------
static stream_obj* resample_filter_create             (const char* name);
static int         resample_filter_set_param          (stream_obj* stream,
                                                const CHAR_T* name,
                                                const void* value);
static int         resample_filter_get_param          (stream_obj* stream,
                                                const CHAR_T* name,
                                                void* value,
                                                size_t* size);
static int         resample_filter_open_in            (stream_obj* stream);
static int         resample_filter_read_frame         (stream_obj* stream, frame_obj** frame);
static int         resample_filter_close              (stream_obj* stream);
static void        resample_filter_destroy            (stream_obj* stream);


//-----------------------------------------------------------------------------
stream_api_t _g_resample_filter_provider = {
    resample_filter_create,
    get_default_stream_api()->set_source,
    get_default_stream_api()->set_log_cb,
    get_default_stream_api()->get_name,
    get_default_stream_api()->find_element,
    get_default_stream_api()->remove_element,
    get_default_stream_api()->insert_element,
    resample_filter_set_param,
    resample_filter_get_param,
    resample_filter_open_in,
    get_default_stream_api()->seek,
    get_default_stream_api()->get_width,
    get_default_stream_api()->get_height,
    get_default_stream_api()->get_pixel_format,
    resample_filter_read_frame,
    get_default_stream_api()->print_pipeline,
    resample_filter_close,
    _set_module_trace_level
};


//-----------------------------------------------------------------------------
#define DECLARE_RESAMPLE_FILTER(stream, name) \
    DECLARE_OBJ(resample_filter_obj, name,  stream, RESAMPLE_FILTER_MAGIC, -1)

#define DECLARE_RESAMPLE_FILTER_V(stream, name) \
    DECLARE_OBJ_V(resample_filter_obj, name,  stream, RESAMPLE_FILTER_MAGIC)

static stream_obj*   resample_filter_create                (const char* name)
{
    resample_filter_obj* res = (resample_filter_obj*)stream_init(sizeof(resample_filter_obj),
                RESAMPLE_FILTER_MAGIC,
                &_g_resample_filter_provider,
                name,
                resample_filter_destroy );

    res->srcChannels = -1;
    res->srcSampleRate = -1;
    res->dstSampleRate = -1;
    res->srcSampleFormat = sfmtUndefined;
    res->dstSampleFormat = sfmtUndefined;
    res->swr = NULL;
    res->fa = create_frame_allocator(_STR("resample_"<<name));
    return (stream_obj*)res;
}

//-----------------------------------------------------------------------------
static int         resample_filter_set_param             (stream_obj* stream,
                                            const CHAR_T* name,
                                            const void* value)
{
    DECLARE_RESAMPLE_FILTER(stream, rsm);
    name = stream_param_name_apply_scope(stream, name);
    if ( !rsm->passthrough ) {
        SET_PARAM_IF(rsm, name, "audioSampleRate", int, rsm->dstSampleRate);
        SET_PARAM_IF(rsm, name, "audioSampleFormat", int, rsm->dstSampleFormat);
    }
    return default_set_param(stream, name, value);
}

//-----------------------------------------------------------------------------
static int         resample_filter_get_param          (stream_obj* stream,
                                                const CHAR_T* name,
                                                void* value,
                                                size_t* size)
{
    DECLARE_RESAMPLE_FILTER(stream, rsm);
    name = stream_param_name_apply_scope(stream, name);
    COPY_PARAM_IF(rsm, name, "audioSampleRate", int,   rsm->dstSampleRate);
    return default_get_param(stream, name, value, size);
}

//-----------------------------------------------------------------------------
static enum AVSampleFormat   _resample_sfmt_to_ffmpeg (
                                                resample_filter_obj* rsm,
                                                int sfmt, bool isPlanar)
{
    switch (sfmt) {
    case sfmtInt8:      return isPlanar?AV_SAMPLE_FMT_U8P:AV_SAMPLE_FMT_U8;
    case sfmtInt16:     return isPlanar?AV_SAMPLE_FMT_S16P:AV_SAMPLE_FMT_S16;
    case sfmtInt32:     return isPlanar?AV_SAMPLE_FMT_S32P:AV_SAMPLE_FMT_S32;
    case sfmtFloat:     return isPlanar?AV_SAMPLE_FMT_FLTP:AV_SAMPLE_FMT_FLT;
    default:            rsm->logCb(logError, _FMT("Unexpected value for source sample format: "<<sfmt));
                        return AV_SAMPLE_FMT_NONE ;
    }
}

//-----------------------------------------------------------------------------
static int         resample_filter_open_in                (stream_obj* stream)
{
    DECLARE_RESAMPLE_FILTER(stream, rsm);
    int res = 0, audioCodec, chLayout;

    res = default_open_in(stream);
    if (res < 0) {
        return res;
    }

    size_t size=sizeof(int);
    res = default_get_param(stream, "audioCodecId", &audioCodec, &size);
    if ( res<0 || audioCodec != streamLinear ) {
        TRACE(_FMT("Resampler will operate in passthrough mode: res=" << res <<
                " codec=" << audioCodec ));
        rsm->passthrough = 1;
        return 0;
    }

    res = default_get_param(stream, "audioSampleRate", &rsm->srcSampleRate, &size);
    if ( res >= 0 ) {
        res = default_get_param(stream, "audioSampleFormat", &rsm->srcSampleFormat, &size);
    }
    if ( res >= 0 ) {
        res = default_get_param(stream, "audioChannels", &rsm->srcChannels, &size);
    }
    if ( res >= 0 ) {
        if ( rsm->dstSampleFormat == sfmtUndefined ) {
            TRACE(_FMT("Desired sample format not set"));
            rsm->dstSampleFormat = rsm->srcSampleFormat;
        }
        if ( rsm->dstSampleRate < 0 ) {
            TRACE(_FMT("Desired sample rate not set"));
            rsm->dstSampleRate = rsm->srcSampleRate;
        }
    }

    if ( res < 0 ||
         rsm->srcSampleRate < 0 ||
         (rsm->srcSampleRate == rsm->dstSampleRate &&
         rsm->srcSampleFormat == rsm->dstSampleFormat) ) {
        TRACE(_FMT("Resampler will operate in passthrough mode" ));
        rsm->passthrough = 1;
        return 0;
    }

    rsm->swr = swr_alloc();
    if (!rsm->swr) {
        rsm->logCb(logError, _FMT("Failed to create swr"));
        return -1;
    }

    TRACE(_FMT("Resampler: " << rsm->srcSampleRate << " --> " << rsm->dstSampleRate ));
    // assume stereo or bust ... and don't modify the layout
    chLayout = ( rsm->srcChannels == 2 ) ? AV_CH_LAYOUT_STEREO : AV_CH_LAYOUT_MONO;
    rsm->fmtIn = _resample_sfmt_to_ffmpeg(rsm, rsm->srcSampleFormat, false);
    rsm->fmtOut = _resample_sfmt_to_ffmpeg(rsm, rsm->dstSampleFormat, false);

    av_opt_set_int(rsm->swr, "in_channel_layout",    chLayout, 0);
    av_opt_set_int(rsm->swr, "in_sample_rate",       rsm->srcSampleRate, 0);
    av_opt_set_sample_fmt(rsm->swr, "in_sample_fmt", rsm->fmtIn, 0);

    av_opt_set_int(rsm->swr, "out_channel_layout",    chLayout, 0);
    av_opt_set_int(rsm->swr, "out_sample_rate",       rsm->dstSampleRate, 0);
    av_opt_set_sample_fmt(rsm->swr, "out_sample_fmt", rsm->fmtOut, 0);

    if (swr_init(rsm->swr) < 0) {
        rsm->logCb(logError, _FMT("Failed to inititalize swr"));
        return -1;
    }

    return 0;
}

//-----------------------------------------------------------------------------
static int         resample_filter_read_frame        (stream_obj* stream,
                                                    frame_obj** frame)
{
    DECLARE_RESAMPLE_FILTER(stream, rsm);
    int res = -1;
    *frame = NULL;

    frame_obj* tmp = NULL;
    res = default_read_frame(stream, &tmp);
    if ( res < 0 || tmp == NULL || rsm->passthrough ) {
        *frame = tmp;
        return res;
    }

    frame_api_t* tmpFrameAPI = frame_get_api(tmp);
    if ( tmpFrameAPI->get_media_type(tmp) != mediaAudio ) {
        *frame = tmp;
        return res;
    }


    int inBytes = tmpFrameAPI->get_data_size(tmp);
    int srcBytesPerSample=av_get_bytes_per_sample(rsm->fmtIn);
    int srcSamples = inBytes/srcBytesPerSample;
    int dstSamples = av_rescale_rnd(swr_get_delay(rsm->swr, rsm->srcSampleRate) + srcSamples,
                                    rsm->dstSampleRate,
                                    rsm->srcSampleRate,
                                    AV_ROUND_UP);
    int dstBytes = dstSamples * av_get_bytes_per_sample(rsm->fmtOut) * rsm->srcChannels;
    int dataSize;

    basic_frame_obj* newFrame = alloc_basic_frame2 (RESAMPLE_FILTER_MAGIC,
                                                    dstBytes,
                                                    rsm->logCb,
                                                    rsm->fa );
    newFrame->pts = newFrame->dts = tmpFrameAPI->get_pts(tmp);
    newFrame->keyframe = 1;
    newFrame->mediaType = mediaAudio;


    TRACE(_FMT("Converting " << inBytes << "/" << srcSamples << ", expecting " << dstBytes << "/" << dstSamples ));
    const uint8_t* inPlanes[] = { (const uint8_t*)tmpFrameAPI->get_data(tmp), NULL };
    uint8_t* outPlanes[] = { newFrame->data, NULL };
    res = swr_convert(rsm->swr, outPlanes, dstSamples, inPlanes, srcSamples);
    if ( res >= 0 ) {
        dataSize = av_samples_get_buffer_size(NULL, rsm->srcChannels, res, rsm->fmtOut, 1);
    }
    TRACE(_FMT("Res=" << res << " bufSize=" << dataSize << " pts=" << newFrame->pts << " ptr=" << (void*)newFrame));
    if (res < 0 || dataSize < 0) {
        rsm->logCb(logError, _FMT("Error resampling: " << res));
        res = -1;
        goto Error;
    }
    newFrame->dataSize = dataSize;

Error:
    if ( res < 0 ) {
        frame_unref((frame_obj**)&newFrame);
        *frame = NULL;
    } else {
        *frame = (frame_obj*)newFrame;
    }
    frame_unref(&tmp);
    return res;
}

//-----------------------------------------------------------------------------
static int         resample_filter_close             (stream_obj* stream)
{
    DECLARE_RESAMPLE_FILTER(stream, rsm);
    swr_free(&rsm->swr);
    return 0;
}

//-----------------------------------------------------------------------------
static void resample_filter_destroy         (stream_obj* stream)
{
    DECLARE_RESAMPLE_FILTER_V(stream, rsm);
    rsm->logCb(logTrace, _FMT("Destroying stream object " << (void*)stream));
    resample_filter_close(stream); // make sure all the internals had been freed
    destroy_frame_allocator( &rsm->fa, rsm->logCb );
    stream_destroy( stream );
}

//-----------------------------------------------------------------------------
SVVIDEOLIB_API stream_api_t*     get_resample_filter_api                    ()
{
    ffmpeg_init();
    return &_g_resample_filter_provider;
}

