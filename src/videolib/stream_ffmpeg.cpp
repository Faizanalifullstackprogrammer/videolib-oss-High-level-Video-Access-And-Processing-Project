/*****************************************************************************
 *
 * stream_ffmpeg.cpp
 *   Combination of demux and decoder based on ffmpeg library.
 *   Not used in the context of SV.
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


#include "videolibUtils.h"
#include "sv_ffmpeg.h"
#include "frame_basic.h"

#define FFMPEG_STREAM_MAGIC 0x1212
#define MAX_PARAM 10

#define SAFE_URL(obj,buf) sv_sanitize_uri(obj->descriptor, buf, sizeof(buf)-1)

#undef av_err2str
#define av_err2str(errnum) av_make_error_string((char*)__builtin_alloca(AV_ERROR_MAX_STRING_SIZE), AV_ERROR_MAX_STRING_SIZE, errnum)


//-----------------------------------------------------------------------------
typedef struct ffmpeg_stream : public stream_base {
    char*               descriptor;
    int                 forceTCP;
    int                 forceMJPEG;
    int                 rawIO;          // do not decode or encode packets we read
    int                 mpjpegAttempts;

    AVFormatContext*    format;
    AVStream*           stream;
    ssize_t             streamId;
    int                 rotation;

    AVCodecContext*     codec;          // eventually, should be decoupled into
                                        // a separate decoder ...
    AVPacket            packet;         // packets may contain multiple frames,
                                        // in which case a packet's scope may
                                        // span multiple reads
    int                 packetOffset;   // as we read from a packet, it's data ptr
                                        // is moved; we need to know by how much
                                        // to properly free the packet
    int                 liveStream;     // by default the stream is considered live
    bool                streamHasTimingInfo;
    AVFrame*            currentFrame;
    char*               frameBuffer;

    int                 abortStreamOnChange;
    UINT64_T            prevCaptureTimeMs;
    int                 captureHasStabilized;

    ssize_t             packetsProcessed;// number of packets read or written
    ssize_t             framesProcessed; // number of frames read or written

    int                 width;
    int                 height;
    int                 pixelFormat;
    ssize_t             frameSize;

    UINT64_T            firstFrameTimeMs;// time first frame was captured;
} ffmpeg_stream_obj;

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


static int64_t    _ff_translate_timebase_to_ms (ffmpeg_stream_obj* demux,
                                                int64_t pts);
static int64_t    _ff_translate_ms_to_timebase (ffmpeg_stream_obj* demux,
                                                int64_t pts);

//-----------------------------------------------------------------------------
stream_api_t _g_ffmpeg_stream_provider = {
    ff_stream_create,
    NULL, // set_source,
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
    DECLARE_OBJ(ffmpeg_stream, name,  stream, FFMPEG_STREAM_MAGIC, -1)
#define DECLARE_STREAM_FF_V(stream, name) \
    DECLARE_OBJ_V(ffmpeg_stream, name,  stream, FFMPEG_STREAM_MAGIC)

#define DECLARE_DEMUX_FF(stream,name) \
    DECLARE_STREAM_FF(stream,name)

static stream_obj*   ff_stream_create                (const char* name)
{
    ffmpeg_stream* res = (ffmpeg_stream*)stream_init(sizeof(ffmpeg_stream_obj),
                                        FFMPEG_STREAM_MAGIC,
                                        &_g_ffmpeg_stream_provider,
                                        name,
                                        ff_stream_destroy );
    res->descriptor = NULL;
    res->format = NULL;
    res->stream = NULL;
    res->codec = NULL;
    res->forceTCP = 0;
    res->forceMJPEG = 0;
    res->rawIO = 0;
    res->rotation = 0;
    res->streamId = -1;
    res->currentFrame = NULL;
    res->frameBuffer = NULL;
    res->packet.data = NULL;
    res->packet.size = 0;
    res->packetOffset = 0;
    res->abortStreamOnChange = 1; // TODO: make configurable
    res->prevCaptureTimeMs = 0;
    res->captureHasStabilized = 0;
    res->streamHasTimingInfo = false;
    res->liveStream = 1;
    av_init_packet(&res->packet);
    res->packet.data = NULL;
    res->packet.size = 0;
    res->mpjpegAttempts = 0;
    res->width = -1;
    res->height = -1;
    res->frameSize = 0;
    res->pixelFormat = pfmtUndefined;


    // res->logCb(logTrace, _FMT("Created stream object " << (void*)res));
    res->packetsProcessed = 0;
    res->framesProcessed = 0;
    res->firstFrameTimeMs = INVALID_PTS;

    return (stream_obj*)res;
}

//-----------------------------------------------------------------------------
static int         ff_stream_set_param             (stream_obj* stream,
                                            const CHAR_T* name,
                                            const void* value)
{
    DECLARE_STREAM_FF(stream, demux);
    name = stream_param_name_apply_scope(stream, name);
    if ( !_stricmp(name, "url") ) {
        demux->descriptor = strdup ( (const char*) value );
        return 0;
    } else
    if ( !_stricmp(name, "forceTCP") ) {
        demux->forceTCP = *(int*)value;
        return 0;
    } else
    if ( !_stricmp(name, "forceMJPEG") ) {
        demux->forceMJPEG = *(int*)value;
        return 0;
    } else
    if ( !_stricmp(name, "packetIO") ) {
        demux->rawIO = *(int*)value;
        return 0;
    }
    if ( !_stricmp(name, "liveStream") ) {
        demux->liveStream = *(int*)value;
        return 0;
    }
    return -1;
}

//-----------------------------------------------------------------------------
#define AVR2D(n) (n.den==0?0:(n.num/(double)n.den))

//-----------------------------------------------------------------------------
static double     _ff_stream_get_fps               ( ffmpeg_stream_obj* demux )
{
    double fps = 0.0;

    AVFormatContext* f = demux->format;
    AVStream* s = ( f && demux->streamId != -1 ) ? f->streams[demux->streamId] : NULL;

    if (f != NULL && s != NULL) {

        fps = AVR2D(s->r_frame_rate);

        if (fps <= 0) {
            fps = AVR2D(s->avg_frame_rate);
        }

        if (fps <= 0) {
            fps = 1.0 / AVR2D(demux->codec->time_base);
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

    COPY_PARAM_IF(demux, name, "fps",            double,  _ff_stream_get_fps(demux));
    COPY_PARAM_IF(demux, name, "timebase",       rational_t,  timebase);
    COPY_PARAM_IF(demux, name, "rotation",       int,   demux->rotation);
    COPY_PARAM_IF(demux, name, "audioCodecId",   int,   streamUnknown);
    COPY_PARAM_IF(demux, name, "videoCodecId",   int,   streamBitmap);
    if ( demux->codec != NULL ) {
        COPY_PARAM_IF(demux, name, "sampleAspectRatio", rational_t, *(rational_t*)&demux->codec->sample_aspect_ratio);
        COPY_PARAM_IF(demux, name, "bitrate", rational_t, *(rational_t*)&demux->codec->bit_rate);
    }
    if (demux->format != NULL && demux->format->streams[demux->streamId] != NULL) {
        COPY_PARAM_IF(demux, name, "timebase", rational_t, *(rational_t*)&demux->format->streams[demux->streamId]->time_base);
    }
    return -1;
}

//-----------------------------------------------------------------------------
// Check if filename is of a format that might be missed by libavcodec
static AVInputFormat* _ff_try_get_input_format(ffmpeg_stream_obj* demux)
{
    AVInputFormat* fmt = NULL;
    AVIOContext *io = NULL;
    char* buffer = NULL;



    const char* sJPEGFormat = demux->mpjpegAttempts ? "mjpeg" : "mpjpeg";
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
        demux->logCb(logInfo, _FMT("Attempting opening MJPEG as '" << sJPEGFormat << "', framesProcessed=" << demux->framesProcessed << ", demux=" << (void*)demux));
    }

    if ( buffer != NULL ) free(buffer);
    if ( io != NULL) avio_close(io);

    return fmt;
}

//-----------------------------------------------------------------------------
static bool        _ff_codec_has_timing_info        (enum AVCodecID id)
{
    switch (id) {
    case AV_CODEC_ID_H264         :
    case AV_CODEC_ID_MPEG4SYSTEMS :
    case AV_CODEC_ID_MPEG4     :
    case AV_CODEC_ID_RAWVIDEO  :
    case AV_CODEC_ID_MSMPEG4V1 :
    case AV_CODEC_ID_MSMPEG4V2 :
    case AV_CODEC_ID_MSMPEG4V3 :
        return true;
    case AV_CODEC_ID_MJPEG     :
    case AV_CODEC_ID_MJPEGB    :
    case AV_CODEC_ID_LJPEG     :
    default                    :
        return false;   // by default, we assume codec doesn't carry the timing info
    }
}

//-----------------------------------------------------------------------------
static int         ff_stream_open_in                (stream_obj* stream)
{
    DECLARE_STREAM_FF(stream, demux);
    int             res;
    int             size;
    unsigned int    index;
    AVDictionary*   dict = NULL;
    AVInputFormat*  fmt = NULL;
    char            buf[256];

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

    // Open the stream
    res = avformat_open_input(&demux->format,
                            demux->descriptor,
                            fmt,
                            &dict);
    av_dict_free(&dict);
    if ( res != 0 ) {
        demux->logCb(logError, _FMT("Failed to open " << SAFE_URL(demux, buf) << " err=" << res ));
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
    res = avformat_find_stream_info(demux->format, NULL);
    if (res < 0) {
        demux->logCb(logError, _FMT("Failed to retrieve stream information for "
                                    << SAFE_URL(demux, buf) << " err=" << res ));
        res = -5;
        goto Error;
    }

    // Locate the video stream and allocate a decoder
    demux->streamId = -1;
    for ( index=0; index < demux->format->nb_streams; index++ ) {
        AVCodecParameters* par = demux->format->streams[index]->codecpar;
        if (par->codec_type == AVMEDIA_TYPE_VIDEO) {
            AVCodec* codec = avcodec_find_decoder(par->codec_id);
            res = -1;
            if ( codec ) {
                demux->codec = avcodec_alloc_context3(codec);
                if (demux->codec) {
                    res = avcodec_parameters_to_context(demux->codec, par);
                }
                if ( res >= 0 ) {
                    res = avcodec_open2(demux->codec, codec, NULL);
                }
            }

            if ( res < 0 ) {
                demux->logCb(logError, _FMT("Failed to open the input codec for "
                                        << SAFE_URL(demux, buf) << " err=" << res ));
                avcodec_free_context(&demux->codec);
                res = -6;
                goto Error;
            }

            demux->streamId = index;
            break;
        }
    }
    if (demux->streamId == -1) {
        demux->logCb(logError, _FMT("Couldn't find video stream in " << SAFE_URL(demux, buf) ));
        res = -7;
        goto Error;
    }
    demux->rotation = videolibapi_get_ffmpeg_rotation(demux->format, demux->streamId);

    demux->currentFrame = av_frame_alloc();
    if (!demux->currentFrame) {
        demux->logCb(logError, _FMT("Couldn't allocate frame object for " << SAFE_URL(demux, buf) ));
        res = -8;
        goto Error;
    }


    size = av_image_get_buffer_size(demux->codec->pix_fmt,
                               demux->codec->width,
                               demux->codec->height+1,
                               _kDefAlign);  // see scaleBug
    demux->frameBuffer = (char*)av_malloc(size);
    if (!demux->frameBuffer) {
        demux->logCb(logError, _FMT("Couldn't allocate frame buffer for " << SAFE_URL(demux, buf) ));
        res = -9;
        goto Error;
    }

    av_image_fill_arrays(demux->currentFrame->data,
                   demux->currentFrame->linesize,
                   (const uint8_t *)demux->frameBuffer,
                   demux->codec->pix_fmt,
                   demux->codec->width,
                   demux->codec->height,
                   _kDefAlign);


    demux->logCb(logInfo,
                _FMT("Opened input codec: name=" << demux->codec->codec->long_name <<
                                        " id=" << demux->codec->codec_id <<
                                        " bitrate=" << demux->codec->bit_rate <<
                                        " w=" << demux->codec->width <<
                                        " h=" << demux->codec->height <<
                                        " w_coded=" << demux->codec->coded_width <<
                                        " h_coded=" << demux->codec->coded_height <<
                                        " pix_fmt=" << demux->codec->pix_fmt <<
                                        " threads=" << demux->codec->thread_count ) );


    demux->streamHasTimingInfo = _ff_codec_has_timing_info(demux->codec->codec_id);

    // Disable indexing to save memory
    demux->format->max_index_size = 0;

    demux->width = demux->codec->width;
    demux->height = demux->codec->height;
    demux->pixelFormat = ffpfmt_to_svpfmt(demux->codec->pix_fmt, demux->codec->color_range);
    demux->frameSize = size;

    demux->logCb(logTrace, _FMT("Opened stream object " << (void*)stream));
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
    int64_t pts = _ff_translate_ms_to_timebase(demux, offset);
    TRACE(_FMT("Seeking: id="<<demux->streamId<<" offset="<<offset<<" pts="<<pts<<" flags="<<ff_flags));
    if ( av_seek_frame(demux->format,
                demux->streamId,
                pts,
                ff_flags) < 0 ) {
        demux->logCb(logError, _FMT("Failed to seek to " << offset << "ms or " << pts << " in " <<
                demux->format->streams[demux->streamId]->time_base.num << "/" <<
                demux->format->streams[demux->streamId]->time_base.den << " timebase"));
        return -1;
    }
    avcodec_flush_buffers(demux->codec);
    return 0;
}

//-----------------------------------------------------------------------------
static size_t      ff_stream_get_width          (stream_obj* stream)
{
    DECLARE_STREAM_FF(stream, demux);
    return demux->width;
}

//-----------------------------------------------------------------------------
static size_t      ff_stream_get_height         (stream_obj* stream)
{
    DECLARE_STREAM_FF(stream, demux);
    return demux->height;
}

//-----------------------------------------------------------------------------
static int         ff_stream_get_pixel_format   (stream_obj* stream)
{
    DECLARE_STREAM_FF(stream, demux);
    return demux->pixelFormat;
}

//-----------------------------------------------------------------------------
static void _ff_free_packet(ffmpeg_stream_obj* demux)
{
    demux->packet.data -= demux->packetOffset;
    demux->packet.size += demux->packetOffset;
    av_packet_unref(&demux->packet);
    demux->packet.size = 0;
    demux->packet.data = NULL;
    demux->packetOffset = 0;
}

//-----------------------------------------------------------------------------
static int         _ff_should_ignore_frame     (ffmpeg_stream_obj* demux)
{
    int             retVal = 0;

    if ( demux->liveStream &&
        !demux->captureHasStabilized ) {
        UINT64_T now = sv_time_get_current_epoch_time();

        // Due to FFMPEG (or maybe IP cam) weirdnesses, we get flooded
        // with a whole bunch of frames right when we first open the
        // stream.  Since the frames don't have a timestamp, we would
        // have to assume that they all came from right now, which is
        // wrong.  Since they're really not _that_ important, it's better
        // to drop them than give them the wrong time.  This also
        // manages to avoid ever capturing the first frame, which is
        // nearly always wrong on my TrendNet camera...
        UINT64_T msDelta = sv_time_get_time_diff( demux->prevCaptureTimeMs, now );

        // We're not stabilized until input is less than 100FPS
        if ((demux->prevCaptureTimeMs == 0) || (msDelta <= 10)) {
            demux->prevCaptureTimeMs = now;
            retVal = 1;
        } else {
            demux->captureHasStabilized = 1;
        }
    }

    return retVal;
}

//-----------------------------------------------------------------------------
static int64_t    _ff_translate_timebase_to_ms (ffmpeg_stream_obj* demux,
                                                int64_t pts)
{
    return av_rescale_q(pts,
                            demux->format->streams[demux->streamId]->time_base,
                            AVRATIONAL_MS);
}

//-----------------------------------------------------------------------------
static int64_t    _ff_translate_ms_to_timebase (ffmpeg_stream_obj* demux,
                                                int64_t pts)
{
    return av_rescale_q(pts,
                            AVRATIONAL_MS,
                            demux->format->streams[demux->streamId]->time_base );
}

//-----------------------------------------------------------------------------
static int        _ff_packet_to_frame          (ffmpeg_stream_obj* demux,
                                                AVPacket* packet,
                                                basic_frame_obj* frame)
{
    if ( (size_t)packet->size > frame->allocSize ) {
        // sanity check ... encoded packet should be smaller than
        // decoded one we allocate for
        if ( grow_basic_frame(frame, packet->size, 0) < 0 ) {
            return -1;
        }
    }
    frame->dataSize = packet->size;
    memcpy(frame->data, packet->data, packet->size);
    frame->pts = _ff_translate_timebase_to_ms(demux, packet->pts);
    frame->dts = _ff_translate_timebase_to_ms(demux, packet->dts);
    frame->keyframe = (packet->flags&AV_PKT_FLAG_KEY)?1:0;
    TRACE(_FMT("Read packet: ms=" << frame->pts << " pts=" << packet->pts << " keyframe=" << frame->keyframe));
    return 0;
}

//-----------------------------------------------------------------------------
static basic_frame_obj* _ff_alloc_frame       (ffmpeg_stream_obj* demux)
{
    basic_frame_obj* newFrame = alloc_basic_frame (FFMPEG_STREAM_MAGIC,
                                                    demux->frameSize,
                                                    demux->logCb);
    newFrame->keyframe = 1;
    newFrame->width = demux->width;
    newFrame->height = demux->height;
    newFrame->pixelFormat = demux->pixelFormat;
    newFrame->mediaType = mediaVideo;
    return newFrame;
}

//-----------------------------------------------------------------------------
static int         ff_stream_read_frame        (stream_obj* stream, frame_obj** frame)
{
    DECLARE_STREAM_FF(stream, demux);
    int             finished = 0;
    int             endOfInput = 0;
    int             size;
    int             returnVal = -1;
    basic_frame_obj* exportFrame = NULL;

    if (demux->format == NULL) {
        demux->logCb(logError, _FMT( "Failed to read from demux - it isn't opened") );
        return -1;
    }

    if (frame == NULL) {
        return -1;
    }

TryAgain:
    *frame = NULL;
    returnVal = -1;
    finished = 0;

    while (!endOfInput && !finished) {
        if (demux->packet.size <= 0) {
            _ff_free_packet(demux);
            // no previously saved undecoded data ... read some
            if ( av_read_frame(demux->format, &demux->packet) ) {
                // No more packets, prepare for a cache flush.
                endOfInput = 1;
                av_init_packet(&demux->packet);
                demux->packet.data = NULL;
                demux->packet.size = 0;
            } else if (demux->packet.stream_index != demux->streamId) {
                // ignore the packet, since it doesn't relate to the right stream
                _ff_free_packet(demux);
            } else {
                demux->packetsProcessed++;
                if (demux->rawIO) {
                    // the client is interested in receiving packets, rather than frames
                    basic_frame_obj* f = _ff_alloc_frame(demux);
                    returnVal = _ff_packet_to_frame(demux,
                                                &demux->packet,
                                                f);
                    if ( returnVal>=0 ) {
                        *frame = (frame_obj*)f;
                    } else {
                        frame_unref((frame_obj**)&f);
                    }
                    _ff_free_packet(demux);
                    return returnVal;
                }

                TRACE( _FMT("Read a packet. pts="<< demux->packet.pts <<
                                            " ptsMs=" << _ff_translate_timebase_to_ms(demux, demux->packet.pts) <<
                                            " dts="<< demux->packet.dts <<
                                            " dtsMs=" << _ff_translate_timebase_to_ms(demux, demux->packet.dts) <<
                                            " flags=" << demux->packet.flags <<
                                            " duration=" << demux->packet.duration <<
                                            " pos=" << demux->packet.pos <<
                                            " side_data_elems=" << demux->packet.side_data_elems
                                            ));
            }
        } else {
            TRACE( _FMT("Have input for decoding. inputSize=" << demux->packet.size ));
            // some data available for decoding
            size = avcodec_decode_video2(demux->codec,
                                        demux->currentFrame,
                                        &finished,
                                        &demux->packet);
            // advance the data pointers for the next decode
            if ( size >= 0 ) {
                TRACE( _FMT("Decoded a packet. pts="<<demux->packet.pts <<
                                " frame_pts=" << _ff_translate_timebase_to_ms(demux, FF_FRAME_PTS(demux->currentFrame)) <<
                                " inputSize=" << demux->packet.size <<
                                " usedSize=" << size <<
                                " finished="<<finished));
                demux->packet.size -= size;
                demux->packet.data += size;
                demux->packetOffset += size;
            } else {
                TRACE( _FMT("Failed to decode a packet. inputSize=" << demux->packet.size <<
                                                " err=" << size <<
                                                " " << av_err2str(size)));
            }

            // free the packet, if needed
            if (demux->packet.size <= 0 || size < 0) {
                // we're done with the previous packet, or error had occurred
                _ff_free_packet(demux);
            }
        }
    }

    UINT64_T pts = INVALID_PTS;

    // we may be finished already
    if (finished ||
    // or the decoder may need one last flush to get us to finish
        (endOfInput &&
         avcodec_decode_video2(demux->codec,
                            demux->currentFrame,
                            &finished,
                            &demux->packet) >= 0 &&
         finished)) {
        if ( demux->firstFrameTimeMs == INVALID_PTS ) {
            if ( demux->liveStream ) {
                demux->firstFrameTimeMs = sv_time_get_current_epoch_time();
            } else {
                demux->firstFrameTimeMs = 0;
            }
        }

        if ( demux->streamHasTimingInfo &&
             FF_FRAME_PTS(demux->currentFrame) != AV_NOPTS_VALUE ) {
            pts = _ff_translate_timebase_to_ms(demux, FF_FRAME_PTS(demux->currentFrame)) +
                    demux->firstFrameTimeMs;
        } else {
            pts = sv_time_get_current_epoch_time();
        }

        demux->currentFrame->pts = pts;

        // it's an extra copy operation, compared to the old code -- without it, we'd need
        // to leak abstraction, and expose an AVFrame* object to the higher layer ...
        // which may be an eventual way to go, especially if the higher layer will be an
        // ffmpeg filter
        exportFrame = _ff_alloc_frame(demux);
        if ( ( size = ffframe_to_svframe ( demux->currentFrame, exportFrame ) ) < 0 ) {
            demux->logCb(logError, _FMT( "Failed to export the frame data") );
            frame_unref((frame_obj**)&exportFrame);
        } else {
            exportFrame->dataSize = size;
            exportFrame->pts =
            exportFrame->dts = pts;
            demux->framesProcessed++;
            *frame = (frame_obj*)exportFrame;
            returnVal = 0;
        }
    }

    if ( returnVal == 0 && _ff_should_ignore_frame(demux) ) {
        TRACE(_FMT("Ignoring a frame. pts=" << exportFrame->pts));
        frame_unref((frame_obj**)&exportFrame);
        goto TryAgain;
    }

    if ( returnVal == 0 ) {
        TRACE( _FMT("Read a frame. pts=" << exportFrame->pts << " size=" << exportFrame->dataSize ));
    }

    return returnVal;
}

//-----------------------------------------------------------------------------
static int         ff_stream_close             (stream_obj* stream)
{
    DECLARE_STREAM_FF(stream, demux);
    avcodec_free_context(&demux->codec);
    if (demux->packet.data!=NULL) {
        TRACE(_FMT("Closing stream object " << (void*)stream << ": packet object " << (void*)demux->packet.data));
        _ff_free_packet(demux);
    }
    if (demux->format) {
        TRACE(_FMT("Closing stream object " << (void*)stream << ": format object " << (void*)demux->format));
        avformat_close_input(&demux->format);
    }
    if (demux->frameBuffer) {
        TRACE(_FMT("Closing stream object " << (void*)stream << ": buffer object " << (void*)demux->frameBuffer));
        av_freep(&demux->frameBuffer);
    }
    if (demux->currentFrame) {
        TRACE(_FMT("Closing stream object " << (void*)stream << ": frame object " << (void*)demux->currentFrame));
        av_freep(&demux->currentFrame);
    }
    if (demux->descriptor) {
        TRACE(_FMT("Closing stream object " << (void*)stream << ": descriptor object " << (void*)demux->descriptor));
        free(demux->descriptor);
        demux->descriptor = NULL;
    }
    demux->streamId = -1;
    demux->prevCaptureTimeMs = 0;
    demux->captureHasStabilized = 0;
    return 0;
}

//-----------------------------------------------------------------------------
static void ff_stream_destroy         (stream_obj* stream)
{
    DECLARE_STREAM_FF_V(stream, demux);
    TRACE(_FMT("Destroying stream object " << (void*)stream));
    ff_stream_close(stream); // make sure all the internals had been freed
    stream_destroy( stream );
}

//-----------------------------------------------------------------------------
SVVIDEOLIB_API stream_api_t*     get_ffmpeg_stream_api             ()
{
    ffmpeg_init();
    return &_g_ffmpeg_stream_provider;
}

