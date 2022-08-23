/*****************************************************************************
 *
 * stream_ffmpeg_demux.cpp
 *   Demux source node based on ffmpeg API. Used for most things except RTSP.
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
#define SV_MODULE_VAR demux
#define SV_MODULE_ID "FFDEMUX"
#include "sv_module_def.hpp"

#include "streamprv.h"
#include "sv_ffmpeg.h"

#include "videolibUtils.h"

#define FFMPEG_DEMUX_MAGIC 0x1218
#define MAX_PARAM 10

#define SAFE_URL(obj,buf) sv_sanitize_uri(obj->descriptor, buf, sizeof(buf)-1)

static char kAnnexBHeader[] = { 0,0,0,1 };
static const size_t kAnnexBHeaderSize = sizeof(kAnnexBHeader);
static const int kBitsInByte = 8;
static const int kMsecInSec = 1000;


extern "C" frame_api_t*    get_ffpacket_frame_api();

typedef struct stats_snapshot_demux: public stats_snapshot_base {
    size_t          framesProcessed;
    stats_item_int  frameSize;
    stats_item_int  interFrameTime;
    stats_item_int  frameAcquisitionTime;

    void reset() {
        time = sv_time_get_current_epoch_time();
        framesProcessed = 0;
        stats_int_init(&frameSize);
        stats_int_init(&interFrameTime);
        stats_int_init(&frameAcquisitionTime);
    }
    void combine(struct stats_snapshot_demux* other) {
        framesProcessed += other->framesProcessed;
        stats_int_combine(&frameSize, &other->frameSize);
        stats_int_combine(&interFrameTime, &other->interFrameTime);
        stats_int_combine(&frameAcquisitionTime, &other->frameAcquisitionTime);
    }
} stats_snapshot_demux_t;

//-----------------------------------------------------------------------------
typedef struct stream_data {
    ssize_t                     id;
    int                         codec;
    INT64_T                     lastPts;
    INT64_T                     lastFrameTime;
    stats_snapshot_demux_t      lifetimeStats;
    stats_snapshot_demux_t      intervalStats;

    void reset()
    {
        id = -1;
        codec = streamUnknown;
        lastPts = INVALID_PTS;
        lastFrameTime = sv_time_get_current_epoch_time();
        lifetimeStats.reset();
        intervalStats.reset();
    }
} stream_data;

static const char* streamLabels[] = { "video", "audio" };
#define S_VIDEO 0
#define S_AUDIO 1

//-----------------------------------------------------------------------------
typedef struct ffmpeg_stream  : public stream_base {
    char*               descriptor;
    int                 forceTCP;
    int                 forceMJPEG;
    int                 forceFLVFix;
    int                 mpjpegAttempts;

    AVFormatContext*    format;
    AVCodecContext*     videoCodec;
    stream_data         streams[2];
    bool                streamHasTimingInfo;
    INT64_T             firstFrameTimeMs;
    int                 liveStream;
    int                 eof;
    char*               sps;
    char*               pps;
    int                 spsSize;
    int                 ppsSize;
    int                 rotation;

    int                 width;
    int                 height;
    int                 pix_fmt;
    int                 keyframeOnly;

    int                 statsIntervalSec;
    INT64_T             statsLastReportTime;
    INT64_T             startTime;
} ffmpeg_stream_obj;


#define A_STREAM(demux) demux->streams[S_AUDIO]
#define V_STREAM(demux) demux->streams[S_VIDEO]


static int64_t    _ff_translate_timebase_to_ms (ffmpeg_stream_obj* demux,
                                                int index,
                                                int64_t pts);
static int64_t    _ff_translate_ms_to_timebase (ffmpeg_stream_obj* demux,
                                                int index,
                                                int64_t pts);



//-----------------------------------------------------------------------------
// Stream API
//-----------------------------------------------------------------------------

//-----------------------------------------------------------------------------
//  Forward declarations
//-----------------------------------------------------------------------------
static stream_obj* ff_stream_create             (const char* name);
static int         ff_stream_set_param          (stream_obj* stream,
                                                const CHAR_T* name,
                                                const void* value);
static int         ff_stream_get_param          (stream_obj* stream,
                                                const CHAR_T* name,
                                                void* value,
                                                size_t* size);
static int         ff_stream_open_in            (stream_obj* stream);
static int         ff_stream_seek               (stream_obj* stream,
                                                INT64_T offset,
                                                int flags);
static size_t      ff_stream_get_width          (stream_obj* stream);
static size_t      ff_stream_get_height         (stream_obj* stream);
static int         ff_stream_get_pixel_format   (stream_obj* stream);
static int         ff_stream_read_frame         (stream_obj* stream, frame_obj** frame);
static int         ff_stream_close              (stream_obj* stream);
static void        ff_stream_destroy            (stream_obj* stream);

static int         _ff_get_video_frames_processed(ffmpeg_stream_obj* demux);
static int         _ff_get_audio_frames_processed(ffmpeg_stream_obj* demux);
static int         _ff_get_total_frames_processed(ffmpeg_stream_obj* demux);
static void        _ff_log_stats                (ffmpeg_stream_obj* demux,
                                                 bool final=false);


//-----------------------------------------------------------------------------
stream_api_t _g_ffmpeg_demux_provider = {
    ff_stream_create,
    NULL, // set_source
    get_default_stream_api()->set_log_cb,
    get_default_stream_api()->get_name,
    get_default_stream_api()->find_element,
    get_default_stream_api()->remove_element,
    get_default_stream_api()->insert_element,
    ff_stream_set_param,
    ff_stream_get_param,
    ff_stream_open_in,
    ff_stream_seek,
    ff_stream_get_width,
    ff_stream_get_height,
    ff_stream_get_pixel_format,
    ff_stream_read_frame,
    get_default_stream_api()->print_pipeline,
    ff_stream_close,
    _set_module_trace_level
} ;


//-----------------------------------------------------------------------------
#define DECLARE_STREAM_FF(stream, name) \
    DECLARE_OBJ(ffmpeg_stream, name,  stream, FFMPEG_DEMUX_MAGIC, -1)

#define DECLARE_STREAM_FF_V(stream, name) \
    DECLARE_OBJ_V(ffmpeg_stream, name,  stream, FFMPEG_DEMUX_MAGIC)

#define DECLARE_DEMUX_FF(stream,name) \
    DECLARE_STREAM_FF(stream,name)

static stream_obj*   ff_stream_create                (const char* name)
{
    ffmpeg_stream* res = (ffmpeg_stream*)stream_init(sizeof(ffmpeg_stream_obj),
                FFMPEG_DEMUX_MAGIC,
                &_g_ffmpeg_demux_provider,
                name,
                ff_stream_destroy );
    res->descriptor = NULL;
    res->format = NULL;
    res->videoCodec = NULL;
    res->forceTCP = 0;
    res->forceFLVFix = 0;
    res->forceMJPEG = 0;
    res->streamHasTimingInfo = false;
    res->firstFrameTimeMs = 0;
    res->liveStream = 1;
    res->eof = 0;
    res->sps = NULL;
    res->pps = NULL;
    res->spsSize = 0;
    res->ppsSize = 0;
    res->mpjpegAttempts = 0;
    res->rotation = 0;
    res->width = -1;
    res->height = -1;
    res->pix_fmt = pfmtUndefined;
    res->startTime = sv_time_get_current_epoch_time();
    res->keyframeOnly = 0;

    res->streams[S_VIDEO].reset();
    res->streams[S_AUDIO].reset();
    res->statsIntervalSec = 0;
    res->statsLastReportTime = res->startTime;

    return (stream_obj*)res;
}

//-----------------------------------------------------------------------------
static int         ff_stream_set_param             (stream_obj* stream,
                                            const CHAR_T* name,
                                            const void* value)
{
    DECLARE_STREAM_FF(stream, demux);
    name = stream_param_name_apply_scope(stream, name);

    SET_STR_PARAM_IF(stream, name, "url", demux->descriptor);
    SET_PARAM_IF(stream, name, "forceTCP", int, demux->forceTCP);
    SET_PARAM_IF(stream, name, "forceMJPEG", int, demux->forceMJPEG);
    SET_PARAM_IF(stream, name, "forceFLVFix", int, demux->forceFLVFix);
    SET_PARAM_IF(stream, name, "liveStream", int, demux->liveStream);
    SET_PARAM_IF(stream, name, "width", int, demux->width);
    SET_PARAM_IF(stream, name, "height", int, demux->height);
    SET_PARAM_IF(stream, name, "keyframeOnly", int, demux->keyframeOnly);
    SET_PARAM_IF(stream, name, "pixfmt", int, demux->pix_fmt);
    SET_PARAM_IF(stream, name, "statsIntervalSec", int, demux->statsIntervalSec);

    return -1;
}

//-----------------------------------------------------------------------------
int _ff_stream_get_bitrate(ffmpeg_stream* demux, int isVideo)
{
    static const int kMinDuration = 10000;
    static const int kBitsInByte = 8;
    INT64_T elapsed = sv_time_get_elapsed_time(demux->startTime);
    if ( elapsed < kMinDuration ) {
        return 0;
    }
    INT64_T         bytesReceived;
    stream_data&    s = demux->streams[isVideo?S_VIDEO:S_AUDIO];
    bytesReceived = s.intervalStats.frameSize.cumulative +
                    s.lifetimeStats.frameSize.cumulative;
    return kBitsInByte*bytesReceived*kMsecInSec/elapsed;
}

//-----------------------------------------------------------------------------
static int64_t         _ff_stream_get_duration             (ffmpeg_stream* demux)
{
    int s_id = V_STREAM(demux).id;
    if (demux->format == NULL ||
        s_id == -1 ||
        demux->format->streams[s_id] == NULL ) {
        return 0;
    }
    AVStream* s = demux->format->streams[s_id];
    return ((1000*s->time_base.num)/(float)s->time_base.den)*s->duration;

}

//-----------------------------------------------------------------------------
#define AVR2D(n) (n.den==0?0:(n.num/(double)n.den))

//-----------------------------------------------------------------------------
static double     _ff_stream_get_fps                      ( ffmpeg_stream* demux )
{
    double fps = 0.0;

    AVFormatContext* f = demux->format;
    int s_id = V_STREAM(demux).id;
    AVStream* s = ( f && s_id != -1 ) ? f->streams[s_id] : NULL;

    if (f != NULL && s != NULL) {

        fps = AVR2D(s->r_frame_rate);

        if (fps <= 0) {
            fps = AVR2D(s->avg_frame_rate);
        }

        if (fps <= 0) {
            fps = 1.0 / AVR2D(s->time_base);
        }
    }

    return fps;
}


//-----------------------------------------------------------------------------
static int         ff_stream_get_param             (stream_obj* stream,
                                            const CHAR_T* name,
                                            void* value,
                                            size_t* size)
{
    static const rational_t timebase = { 1, 1000 };

    DECLARE_STREAM_FF(stream, demux);

    name = stream_param_name_apply_scope(stream, name);

    COPY_PARAM_IF(demux, name, "fps",          double,   _ff_stream_get_fps(demux));
    COPY_PARAM_IF(demux, name, "duration",     int64_t,  _ff_stream_get_duration(demux));
    COPY_PARAM_IF(demux, name, "timebase",     rational_t,  timebase);
    COPY_PARAM_IF(demux, name, "videoCodecId", int,   V_STREAM(demux).codec);
    COPY_PARAM_IF(demux, name, "audioCodecId", int,   A_STREAM(demux).codec);
    COPY_PARAM_IF(demux, name, "eof",          int,   demux->eof);
    COPY_PARAM_IF(demux, name, "spsSize",      int,   demux->spsSize);
    COPY_PARAM_IF(demux, name, "ppsSize",      int,   demux->ppsSize);
    COPY_PARAM_IF(demux, name, "sps",          char*, demux->sps);
    COPY_PARAM_IF(demux, name, "pps",          char*, demux->pps);
    COPY_PARAM_IF(demux, name, "rotation",     int,   demux->rotation);
    COPY_PARAM_IF(demux, name, "width",        int,   ff_stream_get_width(stream));
    COPY_PARAM_IF(demux, name, "height",       int,   ff_stream_get_height(stream));
    COPY_PARAM_IF(demux, name, "videoBitrate",  int, _ff_stream_get_bitrate(demux, true));
    COPY_PARAM_IF(demux, name, "videoFramesProcessed",  int, _ff_get_video_frames_processed(demux));
    COPY_PARAM_IF(demux, name, "videoFramesDropped",    int, 0);
    COPY_PARAM_IF(demux, name, "audioBitrate",  int, _ff_stream_get_bitrate(demux, false));
    COPY_PARAM_IF(demux, name, "audioFramesProcessed",  int, _ff_get_audio_frames_processed(demux));
    COPY_PARAM_IF(demux, name, "audioFramesDropped",    int, 0);
    if ( A_STREAM(demux).id != -1 ) {
        AVCodecParameters* c = demux->format->streams[A_STREAM(demux).id]->codecpar;
        COPY_PARAM_IF(demux, name, "audioSampleRate", int, c->sample_rate);
        /*
        This is a hack. ffmpeg hardcodes 2 for number of channels into MP4 file
        (see libavf/movenc.c:mov_write_audio_tag).
        The demux, unless used with decoder, blindly reports number from that header.
        */
        COPY_PARAM_IF(demux, name, "audioChannels", int, 1);
        COPY_PARAM_IF(demux, name, "ffmpegAudioCodecParameters", AVCodecParameters*, c );
    }
    if ( V_STREAM(demux).id != -1 ) {
        AVCodecParameters* c = demux->format->streams[V_STREAM(demux).id]->codecpar;
        COPY_PARAM_IF(demux, name, "ffmpegVideoCodecParameters", AVCodecParameters*, c );
    }

    demux->logCb(logDebug, _FMT("Unknown param " << name));
    return -1;
}

//-----------------------------------------------------------------------------
// Check if filename is of a format that might be missed by libavcodec
static AVInputFormat* _ff_try_get_input_format(ffmpeg_stream_obj* demux)
{
    AVInputFormat* fmt = NULL;
    AVIOContext *io = NULL;
    char* buffer = NULL;


    const char* sJPEGFormat = "mjpeg";
    demux->mpjpegAttempts++;

    if (demux->forceMJPEG) {
        // There is at least one camera that acts incredibly badly if you
        // probe it at all before opening, so if we know we're forcing
        // mjpeg we need to skip try_get_input_format. Makes sense anyway,
        // but NEVER let try_get_input_format come before any force options.
        fmt = av_find_input_format(sJPEGFormat);
    } else
    if (avio_open(&io, demux->descriptor, AVIO_FLAG_READ) == 0) {
        buffer = (char*)malloc(8192);
        if ( buffer ) {
            int read = avio_read(io, (unsigned char*)buffer, 8192);
            if (read > 0) {
                // If the received packet contains jpeg information assume this is an
                // mjpeg stream
                if (strstr(buffer, "image/jpeg")) {
                    fmt = av_find_input_format(sJPEGFormat);
                }
            }
        }
    }

    if ( fmt ) {
        demux->logCb(logInfo, _FMT("Attempting opening MJPEG as '" << sJPEGFormat << "', demux=" << (void*)demux));
    }

    if ( buffer != NULL ) free(buffer);
    if ( io != NULL) avio_close(io);

    return fmt;
}

//-----------------------------------------------------------------------------
static AVCodecParameters* _ff_get_video_codecpar(ffmpeg_stream* demux)
{
    int s_id = V_STREAM(demux).id;
    if (demux->format == NULL ||
        s_id == -1 ||
        demux->format->streams[s_id] == NULL ) {
        return NULL;
    }
    return demux->format->streams[s_id]->codecpar;
}

//-----------------------------------------------------------------------------
static void _ff_stream_save_sps_pps_avcc(ffmpeg_stream* demux)
{
    AVCodecParameters* codecpar = _ff_get_video_codecpar(demux);
    if (!codecpar) {
        return;
    }


#   define READ_UINT16(x)                           \
    ((((const uint8_t*)(x))[0] << 8) |          \
      ((const uint8_t*)(x))[1])

    uint8_t* extradata = codecpar->extradata;
    size_t   extradata_size = codecpar->extradata_size;
    if (!extradata || extradata_size == 0 || extradata[0] != 1)
        return;

    uint16_t spsSize = READ_UINT16(&extradata[6]);
    if (11 + spsSize > extradata_size) {
        demux->logCb(logDebug, _FMT("extradata_size="<<extradata_size<<" spsSize="<<spsSize));
        return;
    }
    uint16_t ppsSize = READ_UINT16(&extradata[9+spsSize]);
    if (11 + ppsSize + spsSize > extradata_size) {
        demux->logCb(logDebug, _FMT("extradata_size="<<extradata_size<<" spsSize="<<spsSize<<" ppsSize="<<ppsSize));
        return;
    }
    sv_freep( &demux->sps );
    sv_freep( &demux->pps );

    demux->spsSize = spsSize;
    demux->ppsSize = ppsSize;
    if ( demux->spsSize ) {
        demux->sps = (char*)malloc(demux->spsSize);
        memcpy(&demux->sps[0], &extradata[8], spsSize);
    }
    if ( demux->ppsSize ) {
        demux->pps = (char*)malloc(demux->ppsSize);
        memcpy(&demux->pps[0], &extradata[11+spsSize], ppsSize);
    }
}

//-----------------------------------------------------------------------------
static void _ff_stream_save_sps_pps_annexb(ffmpeg_stream* demux)
{
    AVCodecParameters* codecpar = _ff_get_video_codecpar(demux);
    if (!codecpar) {
        demux->logCb(logDebug, _FMT("No codec parameters"));
        return;
    }

    uint8_t* extradata = codecpar->extradata;
    size_t   extradata_size = codecpar->extradata_size;
    if (!extradata || extradata_size == 0) {
        demux->logCb(logDebug, _FMT("No extradata on the stream"));
        return;
    }

    if (extradata[0] == 1) {
        _ff_stream_save_sps_pps_avcc(demux);
        return;
    }

    const uint8_t* ptr = extradata;

#define FIND_NAL(var)\
        while (var - extradata < extradata_size && memcmp(var, kAnnexBHeader, kAnnexBHeaderSize) != 0)\
            ++var;\
        if (var + kAnnexBHeaderSize - extradata >= extradata_size)\
            var=NULL;\

    while (ptr && ptr - extradata < extradata_size) {
        FIND_NAL(ptr);
        if (!ptr) {
            demux->logCb(logDebug, _FMT("Can't find NAL header in extradata"));
            break;
        }

        const uint8_t* end = ptr+kAnnexBHeaderSize;
        uint8_t nal_type = *end & 0x1f;

        demux->logCb(logDebug, _FMT("Got NALU type="<<nal_type));
        ptr = end;
        if (nal_type != 7 && nal_type != 8) { /* Only output SPS and PPS */
            continue;
        }
        FIND_NAL(end);

        char**  containerVar = (nal_type==7)?&demux->sps:&demux->pps;
        int* sizeVar = (nal_type==7)?&demux->spsSize:&demux->ppsSize;

        sv_freep(containerVar);

        *sizeVar = (end?end-ptr:extradata+extradata_size-ptr);
        *containerVar = (char*) malloc(*sizeVar);
        memcpy(*containerVar, ptr, *sizeVar);
    }
}

//-----------------------------------------------------------------------------
static int         ff_stream_open_in                (stream_obj* stream)
{
    DECLARE_STREAM_FF(stream, demux);
    int             res;
    unsigned int    index;
    AVDictionary*   dict = NULL;
    AVInputFormat*  fmt = NULL;
    AVCodec*        codec;
    char            buf[256];
    int             analyzeduration = 5;

TryAgain:
    if (demux->descriptor == NULL) {
        demux->logCb(logError, "Failed to open demux - descriptor isn't set");
        res = -2;
        goto Error;
    }

    if (demux->format != NULL) {
        demux->logCb(logError, _FMT( "Failed to open demux - already opened stream from " << SAFE_URL(demux, buf) ) );
        res = -3;
        goto Error;
    }

    // if MJPEG is either forced in configuration, or provided by the camera, fmt will be set
    fmt = _ff_try_get_input_format(demux);

    if (demux->forceTCP) {
        demux->logCb(logTrace, _FMT("Attempting to force TCP connection for " << SAFE_URL(demux, buf) ) );
        av_dict_set(&dict, "rtsp_transport", "tcp", 0);
    }

    if (_stristr(demux->descriptor, ".flv") != NULL ||
        demux->forceFLVFix ) {
        demux->logCb(logTrace, _FMT("Expecting broken FLV") );
        av_dict_set(&dict, "broken_sizes", "1", 0);
    }

    // Open the stream
    res = avformat_open_input(&demux->format,
                            demux->descriptor,
                            fmt,
                            &dict);
    av_dict_free(&dict);
    if ( res != 0 ) {
        demux->logCb(logError, _FMT("Failed to open " << SAFE_URL(demux, buf) << " errno=" << res << " err=" << av_err2str(res)));
        res = -4;
        goto Error;
    }


    // If fmt is not NULL, we must have detected MJPEG in
    // try_get_input_format().  FFMPEG will never succeed at 'analyzing'
    // the MJPEG frame, so adjust the analyze duration...
    // Look for max_analyze_duration in libavformat/utils.c for more info.
    //
    // Note, rdc, 10/23/2013 - I think we should remove this, but haven't
    // tested/thought completely through, so leaving for now to preserve
    // legacy behavior.
    if (fmt != NULL) {
        av_opt_set_int(demux->format, "analyzeduration", 1, 0);
    }


    // Read a bit of the input and determine it's format.
    if ( demux->format->nb_streams == 0 ) {
        av_opt_set_int(demux->format, "analyzeduration", analyzeduration*1000*1000, 0);
        res = avformat_find_stream_info(demux->format, NULL);
        if (res < 0) {
            demux->logCb(logError, _FMT("Failed to retrieve stream information for "
                                        << SAFE_URL(demux, buf) << " err=" << res ));
            res = -5;
            goto Error;
        }
    }


    // Locate the video stream
    V_STREAM(demux).id = -1;
    A_STREAM(demux).id = -1;
    for ( index=0; index < demux->format->nb_streams; index++ ) {
        AVStream* stream = demux->format->streams[index];
        AVCodecID codecId = stream->codecpar->codec_id;
        int codecType = stream->codecpar->codec_type;
        if (codecType == AVMEDIA_TYPE_VIDEO) {
            if ( V_STREAM(demux).id != -1 &&
                  ( V_STREAM(demux).codec == streamH264 ||
                    V_STREAM(demux).codec == streamMP4 ) ) {
                // we've already selected the video stream to operate on
                continue;
            }


            if ( codecId == AV_CODEC_ID_H264 ) {
                V_STREAM(demux).codec = streamH264;
                V_STREAM(demux).id = index;
                demux->streamHasTimingInfo = true;
                TRACE(_FMT("Opened video stream:" <<
                        " duration=" << stream->duration <<
                        " timebase=" << stream->time_base.num <<
                        "/" << stream->time_base.den <<
                        " bitrate=" << stream->codecpar->bit_rate <<
                        " url=" << SAFE_URL(demux, buf) ));
            } else if ( codecId == AV_CODEC_ID_MJPEG ) {
                V_STREAM(demux).codec = streamMJPEG;
                V_STREAM(demux).id = index;
                demux->streamHasTimingInfo = false;
            } else if ( codecId == AV_CODEC_ID_MPEG4 ) {
                V_STREAM(demux).codec = streamMP4;
                V_STREAM(demux).id = index;
                demux->streamHasTimingInfo = true;
            } else {
                // attempt to use the codec anyway
                demux->logCb(logDebug, _FMT("Found unsupported video stream " << codecId));
                V_STREAM(demux).codec = ( streamTransparentBit | codecId );
                V_STREAM(demux).id = index;
                // TODO: this is correct for image, but will break other video formats ...
                //       may want explicit conditions above for video formats we plan to support
                demux->streamHasTimingInfo = false;
            }
        } else
        if (codecType == AVMEDIA_TYPE_AUDIO) {
            if ( codecId == AV_CODEC_ID_PCM_ALAW ) {
                A_STREAM(demux).codec = streamPCMA;
                A_STREAM(demux).id = index;
            } else if ( codecId == AV_CODEC_ID_PCM_MULAW ) {
                A_STREAM(demux).codec = streamPCMU;
                A_STREAM(demux).id = index;
            } else if ( codecId == AV_CODEC_ID_AAC ) {
                A_STREAM(demux).codec = streamAAC;
                A_STREAM(demux).id = index;
                demux->streamHasTimingInfo = true;
            } else {
                // do not use unknown codec for audio stream
                demux->logCb(logDebug, _FMT("Found unsupported audio stream " << codecId));
            }
        } else {
            demux->logCb(logDebug, _FMT("Found unsupported stream type " << codecType));
        }
    }
    if (V_STREAM(demux).id == -1) {
        demux->logCb(logError, _FMT("Couldn't find video stream in " << SAFE_URL(demux, buf) ));
        res = -7;
        goto Error;
    }

    /*
        If we attempt to open audio codec here, DiskCleaner is crashing on Mac due to the same libdispatch-related, Audiotoolbox-induced crash,
        that prevents us from using Audiotoolbox's AAC encoder/decoder in camera processes.
        Additionally, not finding the codec (as will be the case with AAC on Windows), will become either a log nuisance, or cause for errors.
    */
    // TODO: This used to be needed, since without a call to avcodec_open2, codec parameters,
    //       such as extradata, weren't populated. Since ffmpeg transitioned to decoupling
    //       AVStream/AVCodecParameters/AVCodecContext, this may be no longer needed.
    {
        AVStream* avstream = demux->format->streams[V_STREAM(demux).id];
        AVCodecID codecId = avstream->codecpar->codec_id;
        res = -1;
        codec = avcodec_find_decoder(codecId);
        demux->videoCodec = avcodec_alloc_context3(codec);
        if (demux->videoCodec) {
            res = avcodec_parameters_to_context(demux->videoCodec, avstream->codecpar);
            if ( res >= 0 ) {
                res = avcodec_open2(demux->videoCodec, codec, NULL);
            } else {
                avcodec_free_context(&demux->videoCodec);
            }
        }

        if ( res < 0 ) {
            demux->logCb(logError, _FMT("Failed to init codec " << codecId ));
            res = -8;
            goto Error;
        }
    }


    demux->rotation = videolibapi_get_ffmpeg_rotation(demux->format, V_STREAM(demux).id);

    // Disable indexing to save memory
    demux->format->max_index_size = 0;

    // attempt to access SPS/PPS on this stream
    _ff_stream_save_sps_pps_annexb(demux);

    demux->logCb(logTrace, _FMT("Opened demux object " << (void*)stream));
    demux->startTime = sv_time_get_current_epoch_time();
    return 0;

Error:
    demux->logCb(logError, _FMT("Failed to open stream object " << (void*)stream));
    ff_stream_close(stream);
    if ( fmt != NULL && demux->mpjpegAttempts<2 ) {
        goto TryAgain;
    }
    return res;
}

//-----------------------------------------------------------------------------
static int         ff_stream_seek               (stream_obj* stream,
                                                       INT64_T offset,
                                                       int flags)
{
    DECLARE_STREAM_FF(stream, demux);
    int ff_flags = 0;
    // TODO: implement the rest of the flags later
    if ((flags&sfBackward)!=0) ff_flags|=AVSEEK_FLAG_BACKWARD;

    int     s_id = V_STREAM(demux).id;
    int64_t pts = _ff_translate_ms_to_timebase(demux, s_id, offset);
    TRACE(_FMT("Seeking: id="<< s_id <<" offset="<<offset<<" pts="<<pts<<" flags="<<ff_flags));
    if ( av_seek_frame(demux->format,
                s_id,
                pts,
                ff_flags) < 0 ) {
        demux->logCb(logError, _FMT("Failed to seek to " << offset << "ms or " << pts << " in " <<
                demux->format->streams[s_id]->time_base.num << "/" <<
                demux->format->streams[s_id]->time_base.den << " timebase"));
        return -1;
    }
    if ( demux->videoCodec ) {
        // TODO: not sure what purpose this was carrying when stream and codec objects
        //       were coupled in ffmpeg land, but seems to be even less useful now.
        avcodec_flush_buffers(demux->videoCodec);
    }
    return 0;
}

//-----------------------------------------------------------------------------
static size_t      ff_stream_get_width          (stream_obj* stream)
{
    DECLARE_STREAM_FF(stream, demux);
    if ( demux->width != -1 )
        return demux->width;
    AVCodecParameters* codecpar = _ff_get_video_codecpar(demux);
    return codecpar?codecpar->width:0;
}

//-----------------------------------------------------------------------------
static size_t      ff_stream_get_height         (stream_obj* stream)
{
    DECLARE_STREAM_FF(stream, demux);
    if ( demux->height != -1 )
        return demux->height;
    AVCodecParameters* codecpar = _ff_get_video_codecpar(demux);
    return codecpar?codecpar->height:0;
}

//-----------------------------------------------------------------------------
static int         ff_stream_get_pixel_format   (stream_obj* stream)
{
    DECLARE_STREAM_FF(stream, demux);
    if ( demux->pix_fmt != pfmtUndefined )
        return demux->pix_fmt;
    AVCodecParameters* codecpar = _ff_get_video_codecpar(demux);
    return codecpar?ffpfmt_to_svpfmt((enum AVPixelFormat)codecpar->format, codecpar->color_range):pfmtUndefined;
}


//-----------------------------------------------------------------------------
static int64_t    _ff_translate_timebase_to_ms (ffmpeg_stream_obj* demux,
                                                int index,
                                                int64_t pts)
{
    return av_rescale_q(pts,
                            demux->format->streams[index]->time_base,
                            AVRATIONAL_MS);
}

//-----------------------------------------------------------------------------
static int64_t    _ff_translate_ms_to_timebase (ffmpeg_stream_obj* demux,
                                                int index,
                                                int64_t pts)
{
    return av_rescale_q(pts,
                        AVRATIONAL_MS,
                            demux->format->streams[index]->time_base);
}

//-----------------------------------------------------------------------------
static int _ff_get_audio_frames_processed(ffmpeg_stream_obj* demux)
{
    return  A_STREAM(demux).lifetimeStats.framesProcessed +
            A_STREAM(demux).intervalStats.framesProcessed;
}

//-----------------------------------------------------------------------------
static int _ff_get_video_frames_processed(ffmpeg_stream_obj* demux)
{
    return  V_STREAM(demux).lifetimeStats.framesProcessed +
            V_STREAM(demux).intervalStats.framesProcessed;
}

//-----------------------------------------------------------------------------
static int _ff_get_total_frames_processed(ffmpeg_stream_obj* demux)
{
    return  _ff_get_audio_frames_processed(demux) +
            _ff_get_video_frames_processed(demux);
}

//-----------------------------------------------------------------------------
static int         ff_stream_read_frame        (stream_obj* stream, frame_obj** frame)
{
    DECLARE_STREAM_FF(stream, demux);
    int             returnVal = 0;
    frame_api_t*    fapi = get_ffpacket_frame_api();
    frame_obj*      newFrame = NULL;
    INT64_T         readStart = sv_time_get_current_epoch_time();

    if (demux->format == NULL) {
        demux->logCb(logError, _FMT( "Failed to read from demux - it isn't opened") );
        return -1;
    }

    if (frame == NULL) {
        return -1;
    }

    newFrame = fapi->create();
    frame_ref(newFrame);

    bool        bContinue = false;
    INT64_T*    lastPts = NULL;
    AVPacket*   packet = (AVPacket*)fapi->get_backing_obj(newFrame, "avpacket");
    int         mediaType = mediaUnknown;

    assert(packet != NULL);

    do {
#define SKIP_PACKET(p)\
            av_packet_unref(p); \
            p->size = 0; \
            p->data = NULL; \
            bContinue = true;

        bContinue = false;
        packet->flags |= AV_PKT_FLAG_KEY;
        returnVal = av_read_frame(demux->format, packet);
        if (returnVal < 0) {
            demux->eof = (returnVal == AVERROR_EOF);
            if (!demux->eof) {
                demux->logCb(logError, _FMT("Error " << returnVal << " reading a packet: " << av_err2str(returnVal) <<
                        " packetsProcessed=" << _ff_get_total_frames_processed(demux) <<
                        " timeToRead=" << sv_time_get_elapsed_time(readStart)));
            } else {
                TRACE(_FMT("Reached EOF"));
            }
        } else if ((packet->flags & AV_PKT_FLAG_DISCARD) != 0) {
            TRACE(_FMT("Ignoring a packet with DISCARD flag set"));
            SKIP_PACKET(packet);
        } else if (packet->stream_index == V_STREAM(demux).id) {
            // all is good, return this frame
            mediaType = mediaVideo;
            if ( demux->keyframeOnly && (packet->flags & AV_PKT_FLAG_KEY) == 0 ) {
                SKIP_PACKET(packet);
            } else {
                lastPts = &V_STREAM(demux).lastPts;
            }
        } else if (packet->stream_index == A_STREAM(demux).id) {
            // all is good, return this frame
            mediaType = mediaAudio;
            lastPts = &A_STREAM(demux).lastPts;
        } else {
            TRACE(_FMT("Unrecognized stream ID: stream=" << packet->stream_index << " videoStreamId=" << V_STREAM(demux).id));
            SKIP_PACKET(packet);
        }
    } while (returnVal==0 && bContinue);

    if ( returnVal == 0 ) {
        // we'll use ffmpeg-provided pts, unless we know it's bogus (as is the case with mjpeg live stream)
        if ( (demux->streamHasTimingInfo || !demux->liveStream) &&
             packet->pts != AV_NOPTS_VALUE ) {
            if ( _ff_get_total_frames_processed(demux) == 0) {
                if ( demux->liveStream ) {
                    demux->firstFrameTimeMs = sv_time_get_current_epoch_time();
                } else {
                    demux->firstFrameTimeMs = 0;
                }
            }
            packet->pts = _ff_translate_timebase_to_ms(demux, packet->stream_index, packet->pts) +
                                        demux->firstFrameTimeMs;
            packet->dts = _ff_translate_timebase_to_ms(demux, packet->stream_index, packet->dts) +
                                        demux->firstFrameTimeMs;
        } else {
            // wall clock timestamps are being used!
            INT64_T pts = sv_time_get_current_epoch_time();

            // beware of duplicate timestamps
            if ( pts <= *lastPts && *lastPts != INVALID_PTS ) {
                // use a made up value in the future
                // TODO: is increment of 1 sufficient?
                pts = *lastPts + 1;
            }

            *lastPts = packet->pts = packet->dts = pts;
        }

        INT64_T acquisition = sv_time_get_elapsed_time( readStart );

        stream_data& s = (mediaType==mediaVideo) ? V_STREAM(demux) : A_STREAM(demux);
        s.intervalStats.framesProcessed++;
        INT64_T interval = sv_time_get_elapsed_time( s.lastFrameTime );
        stats_int_update(&s.intervalStats.frameSize, packet->size);
        stats_int_update(&s.intervalStats.interFrameTime, interval);
        stats_int_update(&s.intervalStats.frameAcquisitionTime, acquisition);
        s.lastFrameTime = sv_time_get_current_epoch_time();

        fapi->set_media_type(newFrame, mediaType);
        TRACE(_FMT("Read frame: keyframe="<< fapi->get_keyframe_flag(newFrame) <<
                               " timeToRead=" << acquisition <<
                               " pts=" << fapi->get_pts(newFrame) <<
                               " size=" << fapi->get_data_size(newFrame) <<
                               " mediaType=" << (mediaType == mediaVideo?"video":"audio") ) );
        _ff_log_stats(demux);
    } else {
        frame_unref((frame_obj**)&newFrame);
    }
    *frame = (frame_obj*)newFrame;
    return returnVal;
}

//-----------------------------------------------------------------------------
static void _ff_format_stats(stats_snapshot_demux_t& ls,
                                const char* channel,
                                int lifetime,
                                std::ostringstream& os)
{
    INT64_T elapsed = sv_time_get_elapsed_time( ls.time );
    os << channel << "-" << (lifetime ? "lifetime" : "interval" ) << ":" <<
            " uptime=" << elapsed <<
            " framesSeen=" << ls.framesProcessed <<
            " maxFrameSize=" << ls.frameSize.max <<
            " avgFrameSize=" << stats_int_average(&ls.frameSize) <<
            " avgBitrate=" << (elapsed?ls.frameSize.cumulative*kBitsInByte*kMsecInSec/elapsed:0) <<
            " maxInterFrame=" << ls.interFrameTime.max <<
            " avgInterFrame=" << stats_int_average(&ls.interFrameTime) <<
            " maxReadMs=" << ls.frameAcquisitionTime.max <<
            " avgReadMs=" << stats_int_average(&ls.frameAcquisitionTime) <<
            "; ";
}

//-----------------------------------------------------------------------------
static void _ff_log_stats(ffmpeg_stream_obj* demux, bool final)
{
    if ( !demux->liveStream ) {
        return;
    }

    if ( !final && (
         demux->statsIntervalSec <= 0 ||
         demux->statsIntervalSec*kMsecInSec > sv_time_get_elapsed_time(demux->statsLastReportTime) ) ) {
        return;
    }

    std::ostringstream str;

    for (int i=0; i<2; i++) {
        stream_data& s = demux->streams[i];
        if ( s.id != -1 ) {
            s.lifetimeStats.combine(&s.intervalStats);
            _ff_format_stats(s.intervalStats, streamLabels[i], false, str);
            _ff_format_stats(s.lifetimeStats, streamLabels[i], true, str);
            s.intervalStats.reset();
        }
    }

    demux->logCb(logInfo, _FMT("Stats: " << str.str().c_str()));
    demux->statsLastReportTime = sv_time_get_current_epoch_time();
}

//-----------------------------------------------------------------------------
static int         ff_stream_close             (stream_obj* stream)
{
    DECLARE_STREAM_FF(stream, demux);
    if (demux->format) {
        TRACE(_FMT("Closing stream object " << (void*)stream << ": format object " << (void*)demux->format));
        _ff_log_stats(demux, true);
        avformat_close_input(&demux->format);
    }
    avcodec_free_context(&demux->videoCodec);
    V_STREAM(demux).id = -1;
    A_STREAM(demux).id = -1;
    demux->eof = 0;
    demux->width = -1;
    demux->height = -1;
    demux->pix_fmt = pfmtUndefined;
    return 0;
}


//-----------------------------------------------------------------------------
static void ff_stream_destroy         (stream_obj* stream)
{
    DECLARE_STREAM_FF_V(stream, demux);
    demux->logCb(logTrace, _FMT("Destroying stream object " << (void*)stream));
    ff_stream_close(stream); // make sure all the internals had been freed
    if (demux->descriptor) {
        TRACE(_FMT("Closing stream object " << (void*)stream << ": descriptor object " << (void*)demux->descriptor));
        sv_freep(&demux->descriptor);
    }
    sv_freep(&demux->sps);
    sv_freep(&demux->pps);
    stream_destroy( stream );
}

//-----------------------------------------------------------------------------
SVVIDEOLIB_API stream_api_t*     get_ffmpeg_demux_api                    ()
{
    ffmpeg_init();
    return &_g_ffmpeg_demux_provider;
}

