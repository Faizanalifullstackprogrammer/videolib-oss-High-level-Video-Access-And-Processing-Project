/*****************************************************************************
 *
 * stream_ffmpeg_recorder.cpp
 *   Muxer component responsible for persisting pre-encoded media on the
 *   file system (both as video files for storage and for HLS stream)
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
#define SV_MODULE_VAR mux
#define SV_MODULE_ID "RECORDER"
#include "sv_module_def.hpp"

#include "streamprv.h"
#include "sv_ffmpeg.h"


#include <list>

#include "videolibUtils.h"
#include "event_basic.h"

#define FFSINK_STREAM_MAGIC 0x1515

static const int kDefaultMaxFileDuration = 2*60*1000; // 2 min

int _mux_packets_total(int *values)
{
    int res = 0;
    for (int i=0; i<mediaTotal; i++)
        res += values[i];
    return res;
}

//-----------------------------------------------------------------------------
typedef struct ffsink_stream  : public stream_base  {
    char*               uri;
    sv_mutex*           mutex;              // guards nextURI/restart
    char*               nextURI;
    char*               outputLocation;
    char*               outputFormat;
    const char*         formatName;
    const char*         fileExtension;
    bool                newFileRequested;
    int                 maxFileDurationMs;
    fn_stream_event_cb  eventCallback;
    const void*         eventCallbackContext;
    bool                criticalError;

    // meta-config
    int                 hls;
    float               bit_rate_multiplier;
    int                 max_bit_rate;
    int                 gop_size;
    int                 keyint_min;
    int                 videoQualityPreset;
    int64_t             hlsStartIndex;
    const char*         preset;
    int                 recordInRAM;

    // video params
    int                 videoCodecId;
    int                 width;
    int                 height;
    int                 src_pix_fmt;
    int                 dst_pix_fmt;
    INT64_T             firstPts;
    INT64_T             lastVideoPts;
    INT64_T             duration;

    // audio params
    int                 audioCodecId;
    int                 audioOn;

    // runtime data
    AVFormatContext*    formatCtx;
    AVStream*           videoStream;
    AVFrame*            videoEncFrame;
    AVStream*           audioStream;
    AVStream*           subtitleStream;
    int                 applyBitstreamFilter;
    AVBSFContext*       h264bsfc;
    int                 videoStreamIndex;
    int                 audioStreamIndex;
    int                 subtitleStreamIndex;
    int                 hasSubtitles;
    int                 subtitleDuration;

    uint8_t*            sps;
    uint8_t*            pps;
    size_t              spsSize;
    size_t              ppsSize;
    bool                ownSPS;
    bool                ownPPS;

    bool                outputInitialized;

    std::list<frame_obj*>*   savedFrames;

    int                 packetsWritten[mediaTotal];
    int                 packetsWrittenKeyframes;
    int                 packetsError[mediaTotal];
    int                 packetsLeadIn;
    int                 packetsRead;
} ffsink_stream_obj;


//-----------------------------------------------------------------------------
// Stream API (no frame API for this module)
//-----------------------------------------------------------------------------

//-----------------------------------------------------------------------------
//  Forward declarations
//-----------------------------------------------------------------------------
static stream_obj* ffsink_stream_create             (const char* name);
static int         ffsink_stream_set_param          (stream_obj* stream,
                                                    const CHAR_T* name,
                                                    const void* value);
static int         ffsink_stream_get_param          (stream_obj* stream,
                                                    const CHAR_T* name,
                                                    void* value,
                                                    size_t* size);
static int         _ffsink_stream_open_out          (ffsink_stream_obj* stream,
                                                    frame_obj* frame);
static int         ffsink_stream_read_frame         (stream_obj* stream, frame_obj** frame);
static int         _ffsink_stream_write_frame       (ffsink_stream_obj* stream,
                                                    frame_obj* frame,
                                                    int& written);
static int         _ffsink_stream_close             (stream_obj* stream, bool bCloseAll);
static int         ffsink_stream_close              (stream_obj* stream);
static void        ffsink_stream_destroy            (stream_obj* stream);
static void        _ffsink_free_saved_frames        (ffsink_stream_obj* mux, bool bWrite);
static int         _ffsink_can_start_new_file       (ffsink_stream_obj* mux,
                                                    frame_obj* frame );
static void        _ffsink_notify_new_file          (ffsink_stream_obj* mux,
                                                    int64_t firstPts);
static void        _ffsink_notify_close_file       (ffsink_stream_obj* mux,
                                                    int64_t lastPts);


//-----------------------------------------------------------------------------
stream_api_t _g_ffsink_stream_provider = {
    ffsink_stream_create,
    get_default_stream_api()->set_source,
    get_default_stream_api()->set_log_cb,
    get_default_stream_api()->get_name,
    get_default_stream_api()->find_element,
    get_default_stream_api()->remove_element,
    get_default_stream_api()->insert_element,
    ffsink_stream_set_param,
    ffsink_stream_get_param,
    get_default_stream_api()->open_in,
    get_default_stream_api()->seek,
    get_default_stream_api()->get_width, // get_width,
    get_default_stream_api()->get_height, // get_height,
    get_default_stream_api()->get_pixel_format, // get_pixel_format,
    ffsink_stream_read_frame,
    get_default_stream_api()->print_pipeline,
    ffsink_stream_close,
    _set_module_trace_level
} ;


//-----------------------------------------------------------------------------
#define DECLARE_MUX_FF(param, name) \
    DECLARE_OBJ(ffsink_stream, name,  param, FFSINK_STREAM_MAGIC, -1)
#define DECLARE_MUX_FF_V(param, name) \
    DECLARE_OBJ_V(ffsink_stream, name,  param, FFSINK_STREAM_MAGIC)

static stream_obj*   ffsink_stream_create                (const char* name)
{
    ffsink_stream* res = (ffsink_stream*)stream_init(sizeof(ffsink_stream_obj),
                            FFSINK_STREAM_MAGIC,
                            &_g_ffsink_stream_provider,
                            name,
                            ffsink_stream_destroy );
    res->uri = NULL;
    res->outputLocation = NULL;
    res->outputFormat = NULL;
    res->formatName = NULL;
    res->fileExtension = NULL;
    res->newFileRequested = false;
    res->maxFileDurationMs = kDefaultMaxFileDuration;
    res->eventCallback = NULL;
    res->eventCallbackContext = NULL;
    res->criticalError = false;


    res->hls = 0;
    res->bit_rate_multiplier = 0;
    res->max_bit_rate = 0;
    res->gop_size = 0;
    res->keyint_min = 0;
    res->videoQualityPreset = svvpNotSpecified;
    res->preset = strdup("ultrafast");
    res->recordInRAM = 0;
    res->hlsStartIndex = 0;

    res->nextURI = NULL;
    res->formatCtx = NULL;
    res->firstPts = AV_NOPTS_VALUE;
    res->videoCodecId = streamUnknown;
    res->audioCodecId = streamUnknown;
    res->audioStream = NULL;
    res->audioOn = 1;
    res->videoStream = NULL;
    res->videoEncFrame = NULL;
    res->subtitleStream = NULL;
    res->videoStreamIndex = -1;
    res->audioStreamIndex = -1;
    res->subtitleStreamIndex = -1;
    res->hasSubtitles = 0;
    res->subtitleDuration = 200;
    res->sps = NULL;
    res->pps = NULL;
    res->spsSize = 0;
    res->ppsSize = 0;
    res->ownSPS = 0;
    res->ownPPS = 0;
    memset( res->packetsWritten, 0, sizeof(int)*mediaTotal );
    res->packetsWrittenKeyframes = 0;

    res->applyBitstreamFilter = 0;
    res->h264bsfc = NULL;

    memset( res->packetsError, 0, sizeof(int)*mediaTotal );
    res->packetsLeadIn = 0;
    res->packetsRead = 0;
    res->width = 0;
    res->height = 0;
    res->src_pix_fmt = pfmtUndefined;
    res->dst_pix_fmt = pfmtUndefined;
    res->outputInitialized = false;
    res->savedFrames = new std::list<frame_obj*>;

    res->mutex = sv_mutex_create();

    return (stream_obj*)res;
}

//-----------------------------------------------------------------------------
static int         ffsink_stream_set_param             (stream_obj* stream,
                                            const CHAR_T* name,
                                            const void* value)
{
    DECLARE_MUX_FF(stream, mux);
    name = stream_param_name_apply_scope(stream, name);

    if ( !_stricmp(name, "eventCallback")) {
        mux->eventCallback = (fn_stream_event_cb)value;
        return 0;
    }
    if ( !_stricmp(name, "eventCallbackContext")) {
        mux->eventCallbackContext = value;
        return 0;
    }
    if ( !_stricmp(name, "newFile") ) {
        mux->newFileRequested = true;
        return 0;
    }
    if ( !_stricmp(name, "restart") ) {
        sv_mutex_enter(mux->mutex);
        // uri is freed when we close the stream, so we'd need to preserve
        // it for restart to be successful
        char* saveURI = mux->uri;
        mux->uri = NULL;
        _ffsink_stream_close(stream, false);
        mux->uri = saveURI;
        sv_mutex_exit(mux->mutex);
        return 0;
    }
    SET_STR_PARAM_IF_SAFE(stream, name, "outputURI", mux->nextURI, mux->mutex);
    SET_STR_PARAM_IF_SAFE(stream, name, "uri", mux->nextURI, mux->mutex);
    SET_STR_PARAM_IF(stream, name, "outputLocation", mux->outputLocation);
    SET_STR_PARAM_IF(stream, name, "outputFormat", mux->outputFormat);
    SET_PARAM_IF(stream, name, "maxFileDurationMs", int, mux->maxFileDurationMs);
    SET_PARAM_IF(stream, name, "videoCodecId", int, mux->videoCodecId);
    SET_PARAM_IF(stream, name, "audioCodecId", int, mux->audioCodecId);
    SET_PARAM_IF(stream, name, "width", int, mux->width);
    SET_PARAM_IF(stream, name, "height", int, mux->height);
    SET_PARAM_IF(stream, name, "pixfmt", int, mux->src_pix_fmt);
    SET_PARAM_IF(stream, name, "hls", int, mux->hls);
    SET_PARAM_IF(stream, name, "hlsStartIndex", int64_t, mux->hlsStartIndex);
    SET_PARAM_IF(stream, name, "bitrate_mutiplier", float, mux->bit_rate_multiplier);
    SET_PARAM_IF(stream, name, "max_bitrate", int, mux->max_bit_rate);
    SET_PARAM_IF(stream, name, "gop_size", int, mux->gop_size);
    SET_PARAM_IF(stream, name, "keyint_min", int, mux->keyint_min);
    SET_PARAM_IF(stream, name, "videoQualityPreset", int, mux->videoQualityPreset);
    SET_STR_PARAM_IF(stream, name, "preset", mux->preset);
    SET_PARAM_IF(stream, name, "hasSubtitles", int, mux->hasSubtitles);
    SET_PARAM_IF(stream, name, "subtitleDuration", int, mux->subtitleDuration);
    SET_PARAM_IF(stream, name, "audioOn", int, mux->audioOn);
    SET_PARAM_IF(stream, name, "recordInRAM", int, mux->recordInRAM);


    // pass it on, if we can
    return default_set_param(stream, name, value);
}

//-----------------------------------------------------------------------------
static int         ffsink_stream_get_param             (stream_obj* stream,
                                            const CHAR_T* name,
                                            void* value,
                                            size_t* size)
{
    DECLARE_MUX_FF(stream, mux);

    name = stream_param_name_apply_scope(stream, name);

    COPY_PARAM_IF(mux, name, "firstMs", INT64_T, mux->firstPts);
    COPY_PARAM_IF(mux, name, "packetsLeadIn", int , mux->packetsLeadIn);
    COPY_PARAM_IF(mux, name, "packetsSkipped", int, 0);
    COPY_PARAM_IF(mux, name, "packetsWritten", int, _mux_packets_total(mux->packetsWritten) );
    COPY_PARAM_IF(mux, name, "packetsError", int, _mux_packets_total(mux->packetsError) );
    COPY_PARAM_IF(mux, name, "packetsRead", int, mux->packetsRead);

    // pass it on, if we can
    return default_get_param(stream, name, value, size);
}

//-----------------------------------------------------------------------------
static int        _ffsink_add_subtitle_stream           (ffsink_stream_obj* mux)
{
    if (mux->subtitleStream != NULL){
        mux->logCb(logWarning, _FMT("Subtitle stream is already established"));
        return -1;
    }

    if ( (mux->subtitleStream = avformat_new_stream(mux->formatCtx, NULL) ) == NULL ) {
        mux->logCb(logError, _FMT("Failed to add output video stream: failed to create new stream"));
        return -1;
    }


    TRACE(_FMT("Adding subtitle stream"));
    mux->subtitleStream->duration = 0;
    mux->subtitleStreamIndex = mux->subtitleStream->index;
    AVCodecParameters* codecpar = mux->subtitleStream->codecpar;
    // These parameters are provided from the higher layer
    codecpar->codec_type = AVMEDIA_TYPE_SUBTITLE;
    codecpar->codec_id = AV_CODEC_ID_WEBVTT; //AV_CODEC_ID_TEXT;
    codecpar->extradata = NULL;
    codecpar->extradata_size = 0;

    return 0;
}

//-----------------------------------------------------------------------------
static int        _ffsink_add_video_stream              (ffsink_stream_obj* mux)
{
    if ( mux->width == 0 ||
         mux->height == 0 ||
         mux->videoCodecId == streamUnknown ) {
        mux->logCb(logError, _FMT("Failed to open output video stream: required params aren't set"));
        return -1;
    }


    if (mux->videoStream != NULL){
        mux->logCb(logWarning, _FMT("Video stream is already established"));
        return -1;
    }

    AVCodec*        codec = NULL;
    enum AVCodecID  codec_id;

    mux->dst_pix_fmt = mux->src_pix_fmt;

    if ( (mux->videoStream = avformat_new_stream(mux->formatCtx, codec) ) == NULL ) {
        mux->logCb(logError, _FMT("Failed to add output video stream: failed to create new stream"));
        return -1;
    }

    switch ( mux->videoCodecId ) {
    case streamGIF:     codec_id = AV_CODEC_ID_GIF; break;
    case streamJPG:
    case streamMJPEG:   codec_id = AV_CODEC_ID_MJPEG; break;
    case streamH264:    codec_id = AV_CODEC_ID_H264; break;
    default:
        mux->logCb(logWarning, _FMT("Can't proceed with recording: this filter will record only H264/JPG/GIF frames"));
        return -1;
    }

    if (mux->videoCodecId != streamH264) {
        // not having an audio stream is perfectly normal
        TRACE(_FMT("audio is not supported for current video stream"));
        mux->audioOn = false;
    }


    TRACE(_FMT("Adding video stream: hls=" << mux->hls <<
                                    " width=" << mux->width <<
                                    " height=" << mux->height <<
                                    " codecId=" << mux->videoCodecId <<
                                    " pixfmt=" << mux->src_pix_fmt <<
                                    " ppsSize=" << mux->ppsSize <<
                                    " spsSize=" << mux->spsSize ));
    mux->videoStream->duration = 0;
    mux->videoStreamIndex = mux->videoStream->index;
    AVCodecParameters* codecpar = mux->videoStream->codecpar;
    // These parameters are provided from the higher layer
    codecpar->codec_type = AVMEDIA_TYPE_VIDEO;
    codecpar->codec_id = codec_id;
    codecpar->width = mux->width;
    codecpar->height = mux->height;
    codecpar->format = svpfmt_to_ffpfmt_ext(mux->dst_pix_fmt, &codecpar->color_range, mux->videoCodecId);



    if ( mux->videoCodecId == streamH264 && mux->sps && mux->pps ) {
        // extradata is based on externally provided sps/pps
        codecpar->extradata = videolibapi_spspps_to_extradata(
                                      mux->sps,
                                      mux->spsSize,
                                      mux->pps,
                                      mux->ppsSize,
                                      1,
                                      &codecpar->extradata_size );
    } else {
        codecpar->extradata = NULL;
        codecpar->extradata_size = 0;
    }

    codecpar->codec_tag = 0;
    mux->formatCtx->oformat->video_codec = codec_id;

    // copy the stream parameters to the muxer
    mux->videoStream->time_base.num = 1;
    mux->videoStream->time_base.den = 1000;
    return 0;
}


//-----------------------------------------------------------------------------
int ff_hex_to_data(uint8_t *data, const char *p)
{
    int c, len, v;

    len = 0;
    v   = 1;
    for (;;) {
        p += strspn(p, " \n\t\r");
        if (*p == '\0')
            break;
        c = toupper((unsigned char) *p++);
        if (c >= '0' && c <= '9')
            c = c - '0';
        else if (c >= 'A' && c <= 'F')
            c = c - 'A' + 10;
        else
            break;
        v = (v << 4) | c;
        if (v & 0x100) {
            if (data)
                data[len] = v;
            len++;
            v = 1;
        }
    }
    return len;
}


//-----------------------------------------------------------------------------
static int        _ffsink_add_audio_stream              (ffsink_stream_obj* mux)
{
    if ( !mux->audioOn ) {
        // not having an audio stream is perfectly normal
        TRACE(_FMT("Audio stream is disabled"));
        return 0;
    }

    if (mux->audioCodecId == streamUnknown) {
        // not having an audio stream is perfectly normal
        TRACE(_FMT("no audio stream detected"));
        return 0;
    }

    if (mux->videoCodecId != streamH264) {
        // not having an audio stream is perfectly normal
        TRACE(_FMT("audio is not supported for current video stream"));
        mux->audioOn = false;
        return 0;
    }

    AVCodecParameters*  sourceAudioCodecpar = NULL;
    enum AVCodecID      codec_id;
    enum AVSampleFormat sformat;
    int                 bitrate = 0;
    int                 frame_size = 0;
    int                 sample_rate = 0;
    int                 profile = 0;
    int                 channels = 1;
    size_t              size;
    const char*         audioCodecConfig = NULL;
    unsigned char       extradata[32] = {0};
    size_t              extradata_size = 0;
    unsigned char*      extradata_ptr = extradata;
    stream_obj*         stream = (stream_obj*)mux;

    switch (mux->audioCodecId) {
    case streamPCMU:
        if ( mux->formatName == NULL || _stricmp(mux->formatName, "matroska") ) {
            mux->logCb(logWarning, "ulaw is only supported with mkv container at this time");
            return 0;
        }
        codec_id = AV_CODEC_ID_PCM_MULAW;
        sformat = AV_SAMPLE_FMT_U8;
        sample_rate = 8000;
        bitrate = 64000;
        frame_size = 1;
        break;
    case streamPCMA:
        if ( mux->formatName == NULL || _stricmp(mux->formatName, "matroska") ) {
            mux->logCb(logWarning, "alaw is only supported with mkv container at this time");
            return 0;
        }
        codec_id = AV_CODEC_ID_PCM_ALAW;
        sformat = AV_SAMPLE_FMT_U8;
        sample_rate = 8000;
        bitrate = 64000;
        frame_size = 1;
        break;
    case streamAAC:
        codec_id = AV_CODEC_ID_AAC;
        sformat = AV_SAMPLE_FMT_FLTP;

        size = sizeof(AVCodecParameters*);
        if ( default_get_param(stream, "ffmpegAudioCodecParameters", &sourceAudioCodecpar, &size) < 0 ) {
            sourceAudioCodecpar = NULL;
        }
        size = sizeof(sample_rate);
        if ( default_get_param(stream, "audioSampleRate", &sample_rate, &size) < 0 ) {
            sample_rate = 16000;
        }
        size = sizeof(profile);
        if ( default_get_param(stream, "audioCodecProfile", &profile, &size) < 0 ) {
            profile = FF_PROFILE_AAC_LOW;
        }

        size = sizeof(audioCodecConfig);
        if ( default_get_param(stream, "audioCodecConfig", &audioCodecConfig, &size) < 0 ||
             audioCodecConfig == NULL ) {
            extradata_size = 0;
        } else {
            extradata_size = ff_hex_to_data((uint8_t*)extradata, audioCodecConfig);
            unsigned char extradata1orig = extradata[1];
            int providedChannels = extradata[1]&0x78; // channel config is bits 10 through 13
            if ( providedChannels!=1 ) {
                // We do not support multichannel muxing yet ... and some single channel cameras
                // send us 1 channel in SDP and 2 in config (I'm looking at you, Grandstream)
                // Make sure we have 1 channel set
                // For that, we only want to preserve bit 9 and set bit 13
                extradata[1] &= 0x80;
                extradata[1] |= 0x08;
            }
            TRACE(_FMT("Extradata: " << audioCodecConfig << " size=" << extradata_size <<
                            " e[0]=" << (int)extradata[0] <<
                            " e[1]=" << (int)extradata[1] <<
                            " e[1]unpatched=" << (int)extradata1orig));
        }

        if ( extradata_size == 0 ) {
            if ( default_get_param(stream, "AudioSpecificConfigSize", &extradata_size, &size) >= 0 &&
                 extradata_size > 0 &&
                 extradata_size <= sizeof(extradata) ) {
                size = extradata_size;
                if ( default_get_param(stream, "AudioSpecificConfig", &extradata, &size) >= 0 ) {
                    TRACE(_FMT("Got extradata via AudioSpecificConfig"));
                } else {
                    extradata_size = 0;
                }
            }

        }


        frame_size = 1024;
        break;
    default:
        mux->logCb(logError, _FMT("Unknown input audio codec: " << mux->audioCodecId));
        return 0;
    }

    if ( (mux->audioStream = avformat_new_stream(mux->formatCtx, NULL ) ) == NULL ) {
        mux->logCb(logError, _FMT("Failed to add output audio stream: failed to create new stream"));
        return -1;
    }


    mux->audioStreamIndex = mux->audioStream->index;
    AVCodecParameters* audioCodecpar = mux->audioStream->codecpar;
    if ( sourceAudioCodecpar ) {
        avcodec_parameters_copy(audioCodecpar, sourceAudioCodecpar);
        extradata_size = sourceAudioCodecpar->extradata_size;
        extradata_ptr = sourceAudioCodecpar->extradata;
    } else {
        audioCodecpar->codec_type = AVMEDIA_TYPE_AUDIO;
        audioCodecpar->codec_id = codec_id;
        audioCodecpar->bit_rate = bitrate;
        audioCodecpar->frame_size = frame_size;
        audioCodecpar->profile = profile;
        audioCodecpar->channels = channels;
        audioCodecpar->sample_rate = sample_rate;
        audioCodecpar->format = sformat;
        audioCodecpar->bit_rate = 24000;
    }


    if ( extradata_size != 0 ) {
        // may be freed in ffmpeg -- use ffmpegs alloc routine
        audioCodecpar->extradata = (uint8_t*)av_mallocz(extradata_size+AV_INPUT_BUFFER_PADDING_SIZE);
        memset(audioCodecpar->extradata, 0, extradata_size+AV_INPUT_BUFFER_PADDING_SIZE);
        memcpy(audioCodecpar->extradata, extradata_ptr, extradata_size);
    } else {
        audioCodecpar->extradata = NULL;
    }
    audioCodecpar->extradata_size = extradata_size;


    audioCodecpar->codec_tag = 0;
    mux->formatCtx->oformat->audio_codec = codec_id;

    // copy the stream parameters to the muxer
    mux->audioStream->time_base.num = 1;
    mux->audioStream->time_base.den = 1000;
    mux->audioStream->codecpar->profile = profile;

    TRACE(_FMT("opened audio stream: index=" << mux->audioStreamIndex <<
                                        " profile=" << profile <<
                                        " extradata[0]=" << (audioCodecpar->extradata_size?(int)audioCodecpar->extradata[0]:0) <<
                                        " extradata[1]=" << (audioCodecpar->extradata_size?(int)audioCodecpar->extradata[1]:0) <<
                                        " extradata_size=" << audioCodecpar->extradata_size <<
                                        " channels=" << audioCodecpar->channels <<
                                        " sample_rate=" << audioCodecpar->sample_rate ));
    return 0;

}

//-----------------------------------------------------------------------------
static int         _ffsink_set_opt                      (ffsink_stream_obj* mux, const char* name, const char* value)
{
    if (av_opt_set(mux->formatCtx, name, value, AV_OPT_SEARCH_CHILDREN) < 0) {
        mux->logCb(logError, _FMT( "Failed to set hls parameter " << name << " to " << value << "; url=" << mux->uri) );
        return -1;
    }
    return 0;
}

 //-----------------------------------------------------------------------------
static const char* _ffsink_get_rec_format_name          (ffsink_stream_obj* mux);
static const char* _ffsink_get_file_ext                  (ffsink_stream_obj* mux)
{
    switch ( mux->videoCodecId )
    {
    case streamJPG: return "jpg";
    case streamMJPEG: return "mjpeg";
    case streamGIF: return "gif";
    case streamH264:
    default: {
        if ( mux->fileExtension == NULL ) {
            const char* format = _ffsink_get_rec_format_name(mux);
            if (!_stricmp(format, "hls") || !_stricmp(format, "mpegts")) {
                mux->fileExtension = "ts";
            } else if (!_stricmp(format, "matroska")) {
                mux->fileExtension = "mkv";
            } else {
                mux->fileExtension = "mp4";
            }
        }
        return mux->fileExtension;
    }
    }
}


//-----------------------------------------------------------------------------
static const char* _ffsink_get_rec_format_name          (ffsink_stream_obj* mux)
{
    if (!mux->formatName) {
        if (mux->hls)
            mux->formatName = "hls";
        else if (mux->outputFormat == NULL)
            mux->formatName = "mp4";
        else if (!_stricmp(mux->outputFormat,"hls")) {
            mux->formatName = "hls";
            mux->hls = 1;
        } else if (!_stricmp(mux->outputFormat,"ts") || !_stricmp(mux->outputFormat,"mpegts"))
            mux->formatName = "mpegts";
        else if (!_stricmp(mux->outputFormat,"mkv") || !_stricmp(mux->outputFormat,"matroska"))
            mux->formatName = "matroska";
        else
            mux->formatName = "mp4";
    }
    return mux->formatName;
}

//-----------------------------------------------------------------------------
static int         _ffsink_create_output_context        (ffsink_stream_obj* mux)
{
    // TODO: hack alert -- forcing mkv regardless of requested file format
    const char* formatName;
    enum AVCodecID  codec_id;
    switch (mux->videoCodecId) {
    case streamGIF:     formatName = "gif"; codec_id = AV_CODEC_ID_GIF; break;
    case streamJPG:
    case streamMJPEG:   formatName = "mjpeg"; codec_id = AV_CODEC_ID_MJPEG; break;
    case streamH264:
    default:            formatName = _ffsink_get_rec_format_name(mux); codec_id = AV_CODEC_ID_H264; break;
    }

    if ( avformat_alloc_output_context2(&mux->formatCtx,
                                        NULL,
                                        formatName,
                                        NULL) < 0 ||
        !mux->formatCtx ) {
        mux->logCb(logError, _FMT( "Failed to allocate the output context for " << mux->uri) );
        return -1;
    }

    mux->formatCtx->oformat->video_codec = codec_id;
    mux->formatCtx->pb = NULL;
    if ( codec_id != AV_CODEC_ID_H264 ) {
        return 0;
    }

    if ( mux->hls ) {
        static const int _kHLSSegmentTime = 2;
        static const int _kHLSSegmentListSize = 4;

        int live = (mux->hls == 1);

        if ( _ffsink_set_opt(mux, "hls_time", live?_STR(_kHLSSegmentTime):"5") < 0
            || _ffsink_set_opt(mux, "hls_list_size", live?_STR(_kHLSSegmentListSize):"5000") < 0
            || _ffsink_set_opt(mux, "start_number", _STR(mux->hlsStartIndex)) < 0
            || _ffsink_set_opt(mux, "hls_flags", _STR((live?"+":"-")<<"delete_segments")) < 0
            ) {
            return -1;
        }
    }

    const char* bsf_name;
    if ( mux->hls || !strcmp(mux->formatName,"mpegts")) {
        mux->applyBitstreamFilter = 1;
        // mpegtsenc.c autoinserts h264_mp4toannexb bitstream filters, but it could be
        // beneficial to dump SPS/PPS along with keyframes ... dump_extra filter does that
        bsf_name = "dump_extra";
    } else {
        // There isn't a scenario or condition in the current code flow, where it is required.
        // However, it's very likely this will need to be used at some point.
        bsf_name = "h264_mp4toannexb";
    }

    if ( mux->applyBitstreamFilter ) {
        const AVBitStreamFilter* bsf = av_bsf_get_by_name(bsf_name);
        if ( bsf ) {
            if ( av_bsf_alloc(bsf, &mux->h264bsfc) < 0 ) {
                mux->logCb(logError, _FMT("Filter " << bsf_name << " could not be created"));
            } else
            if ( av_bsf_init(mux->h264bsfc) < 0 ) {
                mux->logCb(logError, _FMT("Filter " << bsf_name << " could not be initialized"));
                av_bsf_free(&mux->h264bsfc);
            }
        } else {
            mux->logCb(logError, _FMT("Filter " << bsf_name << " not found"));
        }
    }

    return 0;
}

//-----------------------------------------------------------------------------
static int         _ffsink_stream_get_sps_pps            (ffsink_stream_obj* mux,
                                                          frame_obj* frame)
{
    size_t size;
    if (!mux->pps || mux->ppsSize==0) {
        mux->pps =NULL;
        mux->ppsSize = 0;
        size = sizeof(mux->pps);
        if ( mux->sourceApi->get_param(mux->source, "pps", &mux->pps, &size) >= 0 && mux->pps != NULL ) {
            size = sizeof(mux->ppsSize);
            if ( mux->sourceApi->get_param(mux->source, "ppsSize", &mux->ppsSize, &size) < 0 ) {
                mux->logCb(logWarning, _FMT("PPS size isn't available from the source"));
                mux->pps = NULL;
            } else {
                TRACE(_FMT("Got PPS: ptr="<<(void*)mux->pps << " size="<<mux->ppsSize));
            }
        } else {
            mux->logCb(logDebug, _FMT("PPS isn't available from the source"));
        }
    }
    if (!mux->sps || mux->spsSize==0 ) {
        mux->sps =NULL;
        mux->spsSize = 0;
        size = sizeof(mux->sps);
        if ( mux->sourceApi->get_param(mux->source, "sps", &mux->sps, &size) >= 0 && mux->sps != NULL ) {
            size = sizeof(mux->spsSize);
            if ( mux->sourceApi->get_param(mux->source, "spsSize", &mux->spsSize, &size) < 0 ) {
                mux->logCb(logWarning, _FMT("SPS size isn't available from the source"));
                mux->sps = NULL;
            } else {
                TRACE(_FMT("Got SPS: ptr="<<(void*)mux->sps << " size="<<mux->spsSize));
            }
        } else {
            mux->logCb(logDebug, _FMT("SPS isn't available from the source"));
        }
    }
    if (!mux->pps || !mux->sps) {
        frame_api_t* api = frame_get_api(frame);
        uint8_t* data = (uint8_t*)api->get_data(frame);
        size_t   size = api->get_data_size(frame);

        if (mux->sps == NULL &&
            videolibapi_extract_nalu(data, size, kNALSPS, &mux->sps, &mux->spsSize, NULL, mux->logCb) ) {
            TRACE((_FMT("Found SPS in the incoming frame!")));
            mux->ownSPS = true;
        }
        if (mux->pps == NULL &&
            videolibapi_extract_nalu(data, size, kNALPPS, &mux->pps, &mux->ppsSize, NULL, mux->logCb) ) {
            TRACE((_FMT("Found PPS in the incoming frame!")));
            mux->ownPPS = true;
        }
        if (videolibapi_contains_idr_frame(data, size, mux->logCb)) {
            // only save frames going back to the last i-frame
            _ffsink_free_saved_frames(mux, false);
        }
    }

    return 0;
}

//-----------------------------------------------------------------------------
static int64_t     _ffsink_get_next_ts                   (ffsink_stream_obj* mux,
                                                          frame_obj* frame)
{
    frame_obj* firstFrame;
    if ( !mux->savedFrames->empty() ) {
        firstFrame = mux->savedFrames->front();
    } else {
        firstFrame = frame;
    }
    frame_api_t* api = frame_get_api(firstFrame);
    return api->get_pts(firstFrame);
}

//-----------------------------------------------------------------------------
static int         _ffsink_stream_open_out               (ffsink_stream_obj* mux,
                                                          frame_obj* frame)
{
    int res = -1;
    size_t size;

    if ( mux->outputInitialized ) {
        // already initalized -- noop
        TRACE_C(5, _FMT("Stream is already initialized"));
        return 0;
    }

    if ( mux->source == NULL ) {
        mux->logCb(logError, _FMT("Cannot initialize the recorder -- source is invalid"));
        return -1;
    }

    stream_api_t* api = mux->sourceApi;
    stream_obj* src = mux->source;

    if ( mux->uri == NULL && mux->outputLocation != NULL ) {
        mux->uri = (char*)malloc(strlen(mux->outputLocation)+64);
        int64_t nextTs = _ffsink_get_next_ts(mux, frame);
        sprintf(mux->uri, "%s%c" I64FMT "-%.03u.%s",
                        mux->outputLocation,
                        PATH_SEPA,
                        nextTs/1000,
                        (int)(nextTs%1000),
                        _ffsink_get_file_ext(mux));
    }

    // --------------------------------------------------------------------------------
    // Get the relevant parameters (codec ids) from the source
    size = sizeof(mux->videoCodecId);
    if ( api->get_param(src, "videoCodecId", &mux->videoCodecId, &size) < 0 ) {
        mux->logCb(logError, _FMT("Failed to determine video codec of the source"));
        return -1;
    }
    size = sizeof(mux->audioCodecId);
    if ( api->get_param(src, "audioCodecId", &mux->audioCodecId, &size) < 0 ) {
        mux->logCb(logError, _FMT("Failed to determine audio codec of the source"));
        return -1;
    }


    // --------------------------------------------------------------------------------
    // Get needed info
    if ( mux->uri == NULL ||
         mux->width == 0 ||
         mux->height == 0 ) {
        int gotIt = 0;
        if (mux->source && mux->uri != NULL) {
            size_t width;
            size_t height;
            int    pixfmt;

            if (api->get_pixel_format) {
                pixfmt = api->get_pixel_format(src);
            } else {
                pixfmt = pfmtUndefined;
            }

            if ( api->get_width &&
                 (width = api->get_width(src)) > 0 &&
                 api->get_height &&
                 (height =api->get_height(src)) > 0 ) {
                mux->width = width;
                mux->height = height;
                mux->src_pix_fmt = pixfmt;
                TRACE(_FMT("Got params from the source: width=" << width << " height=" << height << " pixfmt=" << pixfmt));
                gotIt = 1;
            }
        }
        // haven't received the parameters from the owner yet
        if (!gotIt) {
            TRACE_C(5, _FMT("One of required parameters isn't set yet: " <<
                            " width=" << mux->width <<
                            " height=" << mux->height <<
                            " uri=" << (void*)mux->uri ));
            frame_ref(frame);
            mux->savedFrames->push_back(frame);
            return 0;
        }
    }

    TRACE_C(5, _FMT("Opening for target file at " << mux->uri));

    // --------------------------------------------------------------------------------
    // Try to obtain pps/sps from the source. This is a best effort operation --
    // if we can't, we'll try to procur those from the stream.
    if ( mux->videoCodecId == streamH264 ) {
        _ffsink_stream_get_sps_pps( mux, frame );
        if (!mux->pps || !mux->sps) {
            frame_ref(frame);
            mux->savedFrames->push_back(frame);
            TRACE(_FMT("Not ready to start saving yet: " <<
                        " sps=" << (void*)mux->sps <<
                        " pps=" << (void*)mux->pps <<
                        " bufferedFrames=" << mux->savedFrames->size()));
            return 0;
        }
    }


    if (mux->videoCodecId == mediaUnknown) {
        mux->logCb(logError, _FMT("Failed to open mux - video codec isn't set"));
    } else
    if (mux->formatCtx != NULL) {
        mux->logCb(logError, _FMT( "Failed to open mux - already opened stream from " << mux->uri) );
    } else
    if ( _ffsink_create_output_context(mux) < 0 ) {
        // error was logged
    } else
    if ( _ffsink_add_video_stream(mux) < 0 ) {
        mux->logCb(logError, _FMT( "Couldn't create a video stream for " << mux->uri) );
    } else if ( _ffsink_add_audio_stream(mux) < 0 ) {
        mux->logCb(logError, _FMT( "Couldn't create an audio stream for " << mux->uri) );
    } else if ( mux->hasSubtitles && _ffsink_add_subtitle_stream(mux) < 0 ) {
        mux->logCb(logError, _FMT( "Couldn't create a subtitle stream for " << mux->uri) );
    } else {
#if LIBAVFORMAT_VERSION_MAJOR < 58 // not needed after n4.0
        snprintf(mux->formatCtx->filename, strlen(mux->uri)+1, "%s", mux->uri);
#else
        size_t len = strlen(mux->uri)+1;
        mux->formatCtx->url = (char*)av_mallocz(len);
        strcpy(mux->formatCtx->url, mux->uri);
#endif
        av_dump_format(mux->formatCtx, 0, mux->uri, 1);
        if ( !(mux->formatCtx->oformat->flags & AVFMT_NOFILE) ) {
            TRACE(_FMT("Opening file at " << mux->uri));
            if ( mux->recordInRAM ) {
                mux->formatCtx->pb = ffmpeg_create_buffered_io(mux->uri);
                res = mux->formatCtx->pb != NULL ? 0 : -1;
            } else {
                res = avio_open(&mux->formatCtx->pb, mux->uri, AVIO_FLAG_WRITE);
            }
        } else {
            TRACE(_FMT("Not opening file for " << mux->uri));
            res = 0;
        }

        if ( res < 0 ) {
            mux->logCb(logError, _FMT( "Couldn't open I/O for " << mux->uri) );
            mux->criticalError = true;
        } else
        if ( (res=avformat_write_header(mux->formatCtx, NULL)) < 0) {
            mux->logCb(logError, _FMT( "Couldn't write header for " << mux->uri << ": " << av_err2str(res)));
        } else {
            mux->logCb(logDebug, _FMT( "Opened output stream for " << mux->uri) );
            res = 0;
        }
    }


    if ( res < 0 ) {
        _ffsink_stream_close((stream_obj*)mux, false);
    } else {
        mux->outputInitialized = true;
        mux->newFileRequested = false;
        _ffsink_free_saved_frames(mux, true);
    }

    return res;
}

//-----------------------------------------------------------------------------
template<class T>
static int64_t    _ff_translate_ms_to_timebase (T* timebaseOwner,
                                                int64_t pts)
{
    if (timebaseOwner == NULL) {
        return (int64_t)-1;
    }
    return av_rescale_q(pts, AVRATIONAL_MS, timebaseOwner->time_base );
}

//-----------------------------------------------------------------------------
template<class T>
static int64_t    _ff_translate_timebase_to_ms (T* timebaseOwner,
                                                int64_t pts)
{
    if (timebaseOwner == NULL) {
        return (int64_t)-1;
    }
    return av_rescale_q(pts, timebaseOwner->time_base, AVRATIONAL_MS );
}

//-----------------------------------------------------------------------------
static int         ffsink_stream_close       (stream_obj* stream)
{
    return _ffsink_stream_close(stream, true);
}

//-----------------------------------------------------------------------------
static void    _ffsink_free_saved_frames    (ffsink_stream_obj* mux, bool bWrite)
{
    while ( mux->savedFrames && !mux->savedFrames->empty() ) {
        frame_obj* frame=mux->savedFrames->front();
        if ( bWrite ) {
            int written;
            _ffsink_stream_write_frame(mux, frame, written);
        }
        frame_unref(&frame);
        mux->savedFrames->pop_front();
    }
}

//-----------------------------------------------------------------------------
static int         _ffsink_stream_close       (stream_obj* stream,
                                               bool bCloseAll)
{
    DECLARE_MUX_FF(stream, mux);
    int res;

    if (mux->formatCtx) {
        TRACE(_FMT("Closing mux object " << (void*)stream <<
                    ": format object " << (void*)mux->formatCtx));
        if ( _mux_packets_total(mux->packetsWritten) > 0 &&
             mux->videoCodecId == streamH264 ) {
            mux->videoStream->duration = mux->duration;
            res=av_write_trailer( mux->formatCtx );
            int logLevel = ( mux->packetsError[mediaAudio] > 0 ||
                             mux->packetsError[mediaVideo] > 0 ) ? logWarning : logDebug;
            mux->logCb(logLevel, _FMT("Wrote trailer: res=" << res <<
                    " file=" << mux->uri <<
                    " duration=" << mux->videoStream->duration <<
                    " timebase=" << mux->videoStream->time_base.num <<
                    "/" << mux->videoStream->time_base.den <<
                    " firstPts=" << mux->firstPts <<
                    " lastPts=" << mux->lastVideoPts <<
                    " bitrate=" << mux->videoStream->codecpar->bit_rate <<
                    " writtenAudio=" << mux->packetsWritten[mediaAudio] <<
                    " writtenVideo=" << mux->packetsWritten[mediaVideo] <<
                    " errorAudio=" << mux->packetsError[mediaAudio] <<
                    " errorVideo=" << mux->packetsError[mediaVideo] ));
            if ( res < 0 ) {
                mux->logCb(logError, _FMT("Failed to write a trailer: err=" << res << "(" <<
                                av_err2str(res) << ")"));
            }
            _ffsink_notify_close_file(mux, mux->lastVideoPts);
        }

        av_bsf_free (&mux->h264bsfc);

        if ( mux->formatCtx->pb &&
             !( mux->formatCtx->oformat->flags & AVFMT_NOFILE ) ) {
            if ( mux->recordInRAM ) {
                ffmpeg_close_buffered_io(mux->formatCtx->pb);
            } else {
                avio_close(mux->formatCtx->pb);
            }
            mux->formatCtx->pb = NULL;
        }

        avformat_free_context(mux->formatCtx);
        mux->audioStream = NULL;
        mux->videoStream = NULL;
        mux->subtitleStream = NULL;

        mux->formatCtx = NULL;
        mux->audioStreamIndex = -1;
        mux->videoStreamIndex = -1;
        mux->subtitleStreamIndex = -1;
    }

    if ( mux->ownPPS ) {
        sv_freep(&mux->pps);
    }
    mux->ppsSize = 0;
    mux->pps = NULL;

    if ( mux->ownSPS ) {
        sv_freep(&mux->sps);
    }
    mux->spsSize = 0;
    mux->sps = NULL;

    sv_freep ( &mux->uri );

    _ffsink_free_saved_frames(mux, false);

    mux->outputInitialized = false;
    mux->firstPts = AV_NOPTS_VALUE;

    if ( bCloseAll ) {
        if ( mux->savedFrames != NULL ) {
            // close method may be called multiple times ... only log stats once
            mux->logCb(logInfo, _FMT("Closing recorder. Packets: " <<
                                " read=" << mux->packetsRead <<
                                " leadIn=" << mux->packetsLeadIn <<
                                " video=" << mux->packetsWritten[mediaVideo] <<
                                " audio=" << mux->packetsWritten[mediaAudio] <<
                                " keyframe=" << mux->packetsWrittenKeyframes <<
                                " videoErr=" << mux->packetsError[mediaVideo] <<
                                " audioErr=" << mux->packetsError[mediaAudio] ));
            delete mux->savedFrames;
            mux->savedFrames = NULL;
        }

        stream_unref(&mux->source);
        mux->sourceApi = NULL;
        sv_freep (&mux->nextURI);
        if ( mux->videoEncFrame ) {
            av_frame_free( &mux->videoEncFrame );
        }
        sv_freep(&mux->outputLocation);
        sv_freep(&mux->outputFormat);
        sv_mutex_destroy(&mux->mutex);
    }

    memset( mux->packetsWritten, 0, sizeof(int)*mediaTotal );
    memset( mux->packetsError, 0, sizeof(int)*mediaTotal );
    return 0;
}



//-----------------------------------------------------------------------------
static int         _ffsink_can_start_new_file     ( ffsink_stream_obj* mux,
                                                    frame_obj* frame )
{
    frame_api_t* api = frame_get_api(frame);
    bool isVideo = (api->get_media_type(frame)==mediaVideo);
    if (!isVideo) {
        return false;
    }
    return  api->get_keyframe_flag(frame)>0 ||
            videolibapi_contains_idr_frame((uint8_t*)api->get_data(frame),
                                  api->get_data_size(frame),
                                  mux->logCb);
}

//-----------------------------------------------------------------------------
static int         ffsink_stream_read_frame       ( stream_obj* stream,
                                                    frame_obj** frame)
{
    int res = -1;

    DECLARE_MUX_FF(stream, mux);

    sv_mutex_enter(mux->mutex);

    if ( mux->nextURI ) {
        // request had been made to stop or restart the recording
        char* nextURIValue;

        nextURIValue = strdup(mux->nextURI);
        sv_freep(&mux->nextURI);

        if ( mux->uri != NULL ) {
            TRACE_C(1, _FMT("Stopping recording to " << mux->uri ));
            _ffsink_stream_close(stream, false);
        }
        if ( *nextURIValue != '\0' ) {
            TRACE_C(1, _FMT("Starting recording to " << nextURIValue ));
            mux->uri = strdup(nextURIValue);
        }
        free((void*)nextURIValue);
    }


    res = default_read_frame(stream, frame);

    if ( res>=0 && *frame != NULL ) {
        mux->packetsRead++;
        if (!mux->outputInitialized) {
            TRACE(_FMT("Attempting to complete initialization of the output sink"));
            _ffsink_stream_open_out(mux, *frame);
            if (!mux->outputInitialized) {
                if ( mux->criticalError ) {
                    frame_unref(frame);
                    res = -1;
                }
                goto Exit;
            }
        } else {
            int64_t msSinceStart = 0;
            // need to reopen when we're past max specified duration of the file
            bool bPeriodicReopen = (mux->outputLocation != NULL &&
                            _mux_packets_total(mux->packetsWritten) > 0);
            if ( bPeriodicReopen ) {
                msSinceStart = _ffsink_get_next_ts(mux, *frame) - mux->firstPts;
                bPeriodicReopen &= (mux->maxFileDurationMs < msSinceStart);
            }

            // need to reopen file at the first keyframe when requested so by the upper layer
            bool bRequestedReopen = (mux->newFileRequested &&
                            _mux_packets_total(mux->packetsWritten) > 0);


            if ( bPeriodicReopen||bRequestedReopen ) {
                if (_ffsink_can_start_new_file(mux, *frame)) {
                    TRACE(_FMT("Closing current file and opening a new one due to " <<
                          (bRequestedReopen?"app request":"app settings") <<
                          "; msSinceStart=" << msSinceStart ));
                    _ffsink_stream_close((stream_obj*)mux, false);
                    TRACE(_FMT("Completing initialization of the output sink"));
                    _ffsink_stream_open_out(mux, *frame);
                    if (!mux->outputInitialized) {
                        goto Exit;
                    }
                } else {
                    TRACE(_FMT("Waiting for a keyframe to reopen file output due to " <<
                          (bRequestedReopen?"app request":"app settings") <<
                          "; msSinceStart=" << msSinceStart ));
                }
            }
        }
        int written;
        /*res = */_ffsink_stream_write_frame(mux, *frame, written);
    }

Exit:
    sv_mutex_exit(mux->mutex);
    return res;
}

//-----------------------------------------------------------------------------
static void        _ffsink_notify_new_file        (ffsink_stream_obj* mux,
                                              int64_t firstPts)
{
    int res;
    TRACE_C(0, _FMT("New file notification: uri=" << mux->uri <<
                                " cb=" << (void*)mux->eventCallback <<
                                " firstPts=" << firstPts));

    if ( mux->eventCallback == NULL ) {
        if ( mux->outputLocation != NULL ) {
            mux->logCb(logError, _FMT("Dropping new file event: " << mux->uri << "!"));
        }
        return;
    }

    stream_ev_api* api = get_basic_event_api();
    stream_ev_obj* ev = api->create("recorder.newFile");
    stream_ev_ref(ev);
    api->set_ts(ev, firstPts);
    api->set_context(ev, mux->eventCallbackContext);
    res = api->set_property(ev, "filename", mux->uri, strlen(mux->uri)+1);
    if ( res < 0 ) {
        mux->logCb(logError, _FMT("Failed to set event property 'filename' to " << mux->uri << "; res=" << res));
    } else {
        mux->eventCallback((stream_obj*)mux, ev );
    }
    stream_ev_unref(&ev);
}

//-----------------------------------------------------------------------------
static void        _ffsink_notify_close_file       (ffsink_stream_obj* mux,
                                              int64_t lastPts)
{
    TRACE_C(0, _FMT("Close file notification: uri=" << mux->uri <<
                                " cb=" << (void*)mux->eventCallback <<
                                " firstPts=" << mux->firstPts <<
                                " lastPts=" << lastPts <<
                                " duration=" << lastPts - mux->firstPts ));

    if ( mux->eventCallback == NULL ) {
        return;
    }

    stream_ev_api* api = get_basic_event_api();
    stream_ev_obj* ev = api->create("recorder.closeFile");
    stream_ev_ref(ev);
    api->set_ts(ev, lastPts);
    api->set_context(ev, mux->eventCallbackContext);
    mux->eventCallback((stream_obj*)mux, ev );
    stream_ev_unref(&ev);
}

//-----------------------------------------------------------------------------
static void        _adjust_time_field(AVStream* activeStream,
                                        int64_t& packetVal,
                                        INT64_T& val,
                                        uint64_t firstPts,
                                        int mediaType,
                                        int isHLS)
{
    if ( val != INVALID_PTS ) {
        packetVal = isHLS ? val : val - firstPts;
        if ( mediaType != mediaText )
            packetVal = _ff_translate_ms_to_timebase(activeStream, packetVal);
    } else {
        packetVal = AV_NOPTS_VALUE;
    }
}

//-----------------------------------------------------------------------------
static int         _ffsink_stream_write_frame     (ffsink_stream_obj* mux,
                                              frame_obj* frame,
                                              int& written)
{
    AVPacket packet;
    av_init_packet(&packet);
    int      res = 0;
    int64_t  start = sv_time_get_current_epoch_time();
    frame_api_t* api = frame_get_api(frame);
    assert( api != NULL );


    INT64_T         pts = api->get_pts(frame),
                    dts = api->get_dts(frame);
    int             mediaType = api->get_media_type(frame);
    uint8_t*        data = (uint8_t*)api->get_data(frame);
    size_t          size = api->get_data_size(frame);
    const char*     frameType;
    bool            isKeyframe = false;
    AVStream*       activeStream;
    int             streamIndex;
    INT64_T         lastPts;


    written = 0;

    if ( data == NULL ) {
        mux->logCb(logError, _FMT("No data"));
    }

    if ( mediaType == mediaVideo ) {
        if ( mux->videoCodecId == streamH264 ) {
            frameType="h264";
            if ( api->get_keyframe_flag && api->get_keyframe_flag(frame) ) {
                isKeyframe = true;
            } else if ( videolibapi_contains_idr_frame( data, size, mux->logCb ) ) {
                isKeyframe = true;
            }
        } else {
            frameType="image";
            isKeyframe = true;
        }

        activeStream = mux->videoStream;
        streamIndex = mux->videoStreamIndex;
    } else if ( mediaType == mediaAudio ) {
        frameType = "audio";
        activeStream = mux->audioStream;
        streamIndex = mux->audioStreamIndex;
        if (!mux->audioOn) {
            TRACE(_FMT("Ignoring audio packet -- audio output is disabled!"));
            return 0;
        }
        if (streamIndex < 0) {
            TRACE(_FMT("Ignoring audio packet -- unsupported codec!"));
            return 0;
        }
    } else if ( mediaType == mediaText ) {
        frameType = "text";
        activeStream = mux->subtitleStream;
        streamIndex = mux->subtitleStreamIndex;
    } else if ( mediaType == mediaMetadata ) {
        static const char* subPrefix = "subtitle.";
        static const int   subPrefixLen = strlen(subPrefix);
        if ( !memcmp(data, subPrefix, subPrefixLen) ) {
            frameType = "text";
            activeStream = mux->subtitleStream;
            streamIndex = mux->subtitleStreamIndex;
        } else {
            TRACE(_FMT("Ignoring unrecognized metadata packet!"));
            // we want the caller to continue consuming packets
            written=1;
            return 0;
        }
    } else {
        mux->logCb(logError, _FMT("Unsupported media type " << mediaType));
        return -1;
    }

    if ( mux->firstPts == AV_NOPTS_VALUE ) {
        if ( !isKeyframe ) {
            TRACE(_FMT("Waiting for a keyframe: pts=" << pts << " currentFrameType=" << frameType));
            mux->packetsLeadIn++;
            return 0;
        }

        if (isKeyframe) {
            TRACE(_FMT("Assigning first PTS. pts=" << pts));
            mux->firstPts = pts;
        }
    }

    packet.pos = -1;
    packet.duration = 0;

    _adjust_time_field( activeStream, packet.pts, pts, mux->firstPts, mediaType, mux->hls );
    if ( dts == INVALID_PTS ) {
        // TODO: This is wrong -- doesn't take into account codec delay, or
        //       reordering caused by B-frames.
        //       However, ffmpeg doesn't seem to take into consideration the possibility
        //       of remuxing (say, RTSP stream). For all the other cases, we should have
        //       both PTS and DTS assigned by the encoder.
        dts = pts;
    }
    _adjust_time_field( activeStream, packet.dts, dts, mux->firstPts, mediaType, mux->hls );

    packet.data = data;
    packet.size = size;
    packet.flags |= (isKeyframe||mediaType!=mediaVideo)?AV_PKT_FLAG_KEY:0;
    packet.stream_index = streamIndex;
    if ( mediaType == mediaVideo ) {
        mux->duration = packet.pts;
        mux->lastVideoPts = pts;
    } else if ( mediaType == mediaText ) {
        packet.duration = mux->subtitleDuration;
    }

    if ( mediaType == mediaVideo &&
         mux->videoCodecId == streamH264 &&
         mux->applyBitstreamFilter &&
         mux->h264bsfc != NULL ) {
        res = av_bsf_send_packet(mux->h264bsfc, &packet);
        while ( res >= 0 ) {
            AVPacket pktToWrite;
            av_init_packet(&pktToWrite);
            res = av_bsf_receive_packet(mux->h264bsfc, &pktToWrite);
            if ( res == AVERROR(EAGAIN) ) {
                // no more output -- not an error
                res = 0;
                break;
            }
            if ( res < 0 ) {
                mux->logCb(logError, _FMT("Error " << res << " applying bitstream filter"));
                break;
            }
            lastPts = pktToWrite.pts;
            res = av_write_frame(mux->formatCtx, &pktToWrite);
            av_packet_unref(&pktToWrite);
        }
    } else {
        lastPts = packet.pts;
        res = av_write_frame(mux->formatCtx, &packet);
    }



    if ( res < 0 ) {
        mux->logCb(logDebug, _FMT("Failed to write " << frameType << " frame: err=" << res << "(" <<
                                av_err2str(res) << "), pts=" << lastPts ));
        mux->packetsError[mediaType]++;
        res = -1;
    } else {
        written = 1;
        if ( mediaType == mediaVideo ) {
            if ( mux->packetsWritten[mediaVideo] == 1 ) {
                // we just generated the first packet, send out the notification
                _ffsink_notify_new_file(mux, pts);
            }
            if (isKeyframe)
                mux->packetsWrittenKeyframes++;
        }
        mux->packetsWritten[mediaType]++;
    }
    TRACE(_FMT( (res==0 ? "success":"failure" ) << " writing " << frameType << " frame:" <<
                        " #=" << _mux_packets_total(mux->packetsWritten) <<
                        " hls=" << mux->hls <<
                        " timeSpent=" << sv_time_get_elapsed_time(start) <<
                        " err=" << mux->packetsError <<
                        " size=" << mux->width << "x" << mux->height <<
                        // pts as supplied by frame, in ms (epoch time)
                        " ptsIn=" << pts <<
                        " dtsIn=" << dts <<
                        // pts relative to the first incoming keyframe (or video frame to encode)
                        " ptsInRelative=" << pts - mux->firstPts <<
                        // pts as produced by the encoder, based on the above
                        " packetPts=" << packet.pts <<
                        " packetDts=" << packet.dts <<
                        " packetDuration=" << packet.duration <<
                        " timebaseStream=" << activeStream->time_base.num << "/" << activeStream->time_base.den <<
                        " data=" << (void*)packet.data <<
                        " inputSize=" << size <<
                        " size=" << packet.size <<
                        " flags=" << packet.flags <<
                        " index=" << packet.stream_index ));

    return res;
}

//-----------------------------------------------------------------------------
static void ffsink_stream_destroy         (stream_obj* stream)
{
    DECLARE_MUX_FF_V(stream, mux);
    mux->logCb(logTrace, _FMT("Destroying stream object " << (void*)stream));
    _ffsink_stream_close(stream, true); // make sure all the internals had been freed
    free((void*)mux->preset);
    stream_destroy( stream );
}


//-----------------------------------------------------------------------------
SVVIDEOLIB_API stream_api_t*     get_ffsink_stream_api             ()
{
    ffmpeg_init();
    return &_g_ffsink_stream_provider;
}

