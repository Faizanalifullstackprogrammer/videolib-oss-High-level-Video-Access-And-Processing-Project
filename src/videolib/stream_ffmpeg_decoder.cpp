/*****************************************************************************
 *
 * stream_ffmpeg_decoder.cpp
 *   Decoder node based on ffmpeg library.
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
#define SV_MODULE_VAR decoder
#define SV_MODULE_ID "FFDECODER"
#include "sv_module_def.hpp"

#include "streamprv.h"
#include "sv_ffmpeg.h"

#include "videolibUtils.h"
#include "frame_basic.h"

#include <algorithm>
#include <map>
#include <mutex>
#include <vector>
#include <memory>

#define FFDEC_STREAM_MAGIC 0x1213
#define MAX_PARAM 10
#define FORCE_INTERLEAVED_AUDIO 1
static const int kMaxPacketsWithNoFrames = 120;

typedef struct ffdec_stream ffdec_stream_obj;
typedef int
(*export_frame)                          (ffdec_stream_obj* decoder, AVFrame* f, frame_obj* svf);




//-----------------------------------------------------------------------------
static enum AVPixelFormat _get_preferred_pix_fmt(const enum AVPixelFormat *pix_fmts)
{
    // pixfmpt priorities ... highly experimental, and probably wrong
    static std::map<enum AVPixelFormat, int> pixfmtPriorities = {
        { AV_PIX_FMT_RGB24, 0 },        // jackpot! we want this in most cases
        { AV_PIX_FMT_BGR24, 1 },        // not exactly, but easier to rearrange than the rest
        { AV_PIX_FMT_RGBA,  2 },        // more work than just swapping channels ?
        { AV_PIX_FMT_ARGB,  2 },        // more work than just swapping channels ?
        { AV_PIX_FMT_BGRA,  3 },        // more work than just swapping channels ?
        { AV_PIX_FMT_ABGR,  3 },        // more work than just swapping channels ?
        { AV_PIX_FMT_YUV420P,  4 },     // something we're used to, but not necessarily easy to conver
        { AV_PIX_FMT_YUVJ420P,  4 },    // something we're used to, but not necessarily easy to convert
    };

    const enum AVPixelFormat *p;
    enum AVPixelFormat retval = AV_PIX_FMT_NONE;
    enum AVPixelFormat preferred = AV_PIX_FMT_NONE;


    int envPixFmt = sv_get_int_env_var("SIO_HW_XFER_PIX_FMT", -1);
    if ( envPixFmt >= 0 ) {
        preferred = (enum AVPixelFormat)envPixFmt;
    }
    int currentPriority = 100;

    for (p = pix_fmts; *p != AV_PIX_FMT_NONE; p++) {
        if ( *p == preferred ) {
            retval = preferred;
            break;
        }

        std::map<enum AVPixelFormat, int>::const_iterator it = pixfmtPriorities.find(*p);
        bool bFound = (it != pixfmtPriorities.end());

        if ( (bFound && it->second < currentPriority) || retval == AV_PIX_FMT_NONE ) {
            retval = *p;
            if ( bFound ) {
                currentPriority = it->second;
            }
        }
    }

    return retval;
}


//-----------------------------------------------------------------------------
typedef struct DeviceEntry {
    std::string name;
    int         priority;
} DeviceEntry;

//-----------------------------------------------------------------------------
typedef struct DeviceEntryComparator
{
    bool operator() (const DeviceEntry& d1, const DeviceEntry& d2) const
    {
        return (d1.priority < d2.priority || d1.name < d2.name);
    }
} DeviceEntryComparator;

//-----------------------------------------------------------------------------
typedef std::map<DeviceEntry, std::shared_ptr<AVBufferRef>, DeviceEntryComparator> DevicesDict;
std::once_flag kStaticInitFlag;
static DevicesDict g_hwDevices;
static std::string g_preferredDevice;
static void ffmpeg_init_hw();


extern "C" int videolib_get_hw_devices(const char** ptrStr, int nSize)
{
    std::call_once( kStaticInitFlag, ffmpeg_init_hw );

    int nCount = 0;
    DevicesDict::iterator it = g_hwDevices.begin();
    for (int nI=0; nI<nSize-1 && it!=g_hwDevices.end(); nI++ ) {
        ptrStr[nCount] = it->first.name.c_str();
        nCount++;
        it++;
    }
    ptrStr[nCount] = NULL;
    return nCount;
}

extern "C" int videolib_set_hw_device(const char* dev)
{
    g_preferredDevice = "";
    if ( dev ) {
        int found = 0;
        if ( !_stricmp(dev, "none") ) {
            g_preferredDevice = dev;
            return 0;
        }
        for (auto& entry: g_hwDevices) {
            if ( entry.first.name == dev ) {
                g_preferredDevice = dev;
                return 0;
            }
        }
        return -1;
    }
    return 0;
}


static void ffmpeg_init_hw()
{
    static std::vector<DeviceEntry> allDevices =  {
#if defined __linux__ && defined __x86_64__
                                            { "cuda", 1 },
#endif
//                                            "drm",
#ifdef _WIN32
                                           { "cuda", 1 },
                                           { "dxva2", 3 },
                                           { "d3d11va", 2 },
                                           { "d3d11va_vld", 4 },
#endif
//                                            "opencl",
                                           { "qsv", 5 },
#if defined __linux__ && defined __x86_64__
                                           { "vaapi", 2 },
                                           { "vdpau", 3 },
#endif
#ifdef __APPLE__
#ifndef SIGHTHOUND_VIDEO
                                           { "videotoolbox", 1 },
#endif
#endif
#ifdef __ANDROID__
                                           { "mediacodec", 1 },
#endif
//                                            "vulkan"
                                            };
    ffmpeg_init();

    for ( const auto& dev: allDevices ) {
        AVBufferRef *hw_device_ctx = nullptr;
        enum AVHWDeviceType type = av_hwdevice_find_type_by_name(dev.name.c_str());
        if ( av_hwdevice_ctx_create(&hw_device_ctx, type, NULL, NULL, 0) == 0 && hw_device_ctx != nullptr ) {
            std::shared_ptr<AVBufferRef> ptr(hw_device_ctx, [](AVBufferRef* p){av_buffer_unref(&p);});
            g_hwDevices[dev] = ptr;
        }
    }
}


//-----------------------------------------------------------------------------
typedef struct ffdec_stream  : public stream_base {
    AVCodecContext*     codecContext;
    AVFrame*            ffFrame;
    int                 enableNVMPI;

    int                 srcCodecId;
    int                 mediaType;
    int                 liveStream;

    UINT64_T            prevCaptureTimeMs;
    int                 captureHasStabilized;

    ssize_t             packetsProcessed;// number of packets read or written
    UINT64_T            lastOutputPts;   // last PTS returned
    int                 framesSkipped;   // frames skipped due to PTS inconsistencies
    ssize_t             framesProcessed; // number of frames read or written
    ssize_t             framesIgnored;   // number of frames the decoder had ignored
    int                 noFrameCount;    // consequtive number of packets failing to generate a frame
    int                 lastKeyFrameInterval; // last keyframe interval we've observed
    int                 packetsSinceKeyframe; // number of packets seen since the last keyframe

    frame_obj*          firstFrame;
    int                 firstFrameServed;

    int                 errorOccurred;
    int                 threadCount;
    int                 prevPacketSubmitted;

    uint8_t*            sps;
    uint8_t*            pps;
    int                 spsSize;
    int                 ppsSize;

    int                 height;
    int                 width;
    int                 format;
    int                 interleaved;
    int                 sampleSize;
    int                 eof;

    INT64_T             seekingTo;
    int                 seekFlags;
    frame_obj*          prevSeekFrame;
    frame_obj*          nextFrameToReturn;

    int                 hardwareErrorEncountered;
    DevicesDict::iterator   hardwareDevices;
    enum AVPixelFormat  hardwarePixFmt;
    enum AVPixelFormat  hardwareXferPixFmt;
    AVFrame*            hardwareFrame;
    char*               hardwareDevice;

    frame_allocator*    fa;

    export_frame        _ffdec_export_frame;
} ffdec_stream_obj;

static int
_ffdec_export_video_frame                (ffdec_stream_obj* decoder, AVFrame* f, frame_obj* svf);
static int
_ffdec_export_audio_frame                (ffdec_stream_obj* decoder, AVFrame* f, frame_obj* svf);


//-----------------------------------------------------------------------------
// Stream API
//-----------------------------------------------------------------------------

//-----------------------------------------------------------------------------
//  Forward declarations
//-----------------------------------------------------------------------------
static stream_obj* ffdec_stream_create             (const char* name);
static int         ffdec_stream_set_param          (stream_obj* stream,
                                                    const CHAR_T* name,
                                                    const void* value);
static int         ffdec_stream_get_param          (stream_obj* stream,
                                                    const CHAR_T* name,
                                                    void* value,
                                                    size_t* size);
static int         ffdec_stream_open_in            (stream_obj* stream);
static int         ffdec_stream_seek               (stream_obj* stream,
                                                    INT64_T offset,
                                                    int flags);
static size_t      ffdec_stream_get_width          (stream_obj* stream);
static size_t      ffdec_stream_get_height         (stream_obj* stream);
static int         ffdec_stream_get_pixel_format   (stream_obj* stream);
static int         ffdec_stream_read_frame         (stream_obj* stream, frame_obj** frame);
static int         ffdec_stream_close              (stream_obj* stream);
static void        ffdec_stream_destroy            (stream_obj* stream);

static void        _ffdec_configure_demux          (stream_obj* stream);

extern "C" frame_api_t*     get_ffframe_frame_api   ( );
extern frame_obj*           alloc_avframe_frame     (int ownerTag, frame_allocator* fa,
                                                    fn_stream_log logCb);


//-----------------------------------------------------------------------------
stream_api_t _g_ffdec_stream_provider = {
    ffdec_stream_create,
    get_default_stream_api()->set_source,
    get_default_stream_api()->set_log_cb,
    get_default_stream_api()->get_name,
    get_default_stream_api()->find_element,
    get_default_stream_api()->remove_element,
    get_default_stream_api()->insert_element,
    ffdec_stream_set_param,
    ffdec_stream_get_param,
    ffdec_stream_open_in,
    ffdec_stream_seek,
    ffdec_stream_get_width,
    ffdec_stream_get_height,
    ffdec_stream_get_pixel_format,
    ffdec_stream_read_frame,
    get_default_stream_api()->print_pipeline,
    ffdec_stream_close,
    _set_module_trace_level
} ;


//-----------------------------------------------------------------------------
#define DECLARE_STREAM_FF(stream, name) \
    DECLARE_OBJ(ffdec_stream, name,  stream, FFDEC_STREAM_MAGIC, -1)
#define DECLARE_STREAM_FF_V(stream, name) \
    DECLARE_OBJ_V(ffdec_stream, name,  stream, FFDEC_STREAM_MAGIC)


static stream_obj*   ffdec_stream_create                (const char* name)
{
    ffdec_stream* res = (ffdec_stream*)stream_init(sizeof(ffdec_stream),
                                        FFDEC_STREAM_MAGIC,
                                        &_g_ffdec_stream_provider,
                                        name,
                                        ffdec_stream_destroy );

    res->prevCaptureTimeMs = 0;
    res->captureHasStabilized = 0;

    // by default decoders are processing video, to honor the times when it was the only media they touched
    res->mediaType  = mediaVideo;
    res->srcCodecId = streamUnknown;
    const char* enableNVMPI = getenv("SIO_ENABLE_NVMPI");
    res->enableNVMPI = (enableNVMPI && _stricmp(enableNVMPI,"0") != 0);

    res->liveStream = 1;

    res->packetsProcessed = 0;
    res->lastOutputPts = 0;
    res->framesSkipped = 0;
    res->framesProcessed = 0;
    res->framesIgnored = 0;
    res->noFrameCount = 0;
    res->lastKeyFrameInterval = 0;
    res->packetsSinceKeyframe = 0;

    res->firstFrame = NULL;
    res->firstFrameServed = 0;

    res->codecContext = NULL;
    res->ffFrame = NULL;

    res->errorOccurred = 0;
    res->threadCount = 0;
    res->prevPacketSubmitted = 1;

    res->height = -1;
    res->width = -1;
    res->format = pfmtUndefined;
    res->interleaved = 0;
    res->sampleSize = 1;
    res->eof = 0;

    res->sps = NULL;
    res->pps = NULL;
    res->spsSize = 0;
    res->ppsSize = 0;

    res->seekingTo = INVALID_PTS;
    res->seekFlags = 0;
    res->prevSeekFrame = NULL;
    res->nextFrameToReturn = NULL;

    res->fa = create_frame_allocator(name);

    res->_ffdec_export_frame = NULL;

    res->hardwareErrorEncountered = 0;
    res->hardwareDevices = g_hwDevices.begin();
    res->hardwarePixFmt = AV_PIX_FMT_NONE;
    res->hardwareXferPixFmt = AV_PIX_FMT_NONE;
    res->hardwareFrame = NULL;
    res->hardwareDevice = NULL;

    return (stream_obj*)res;
}

//-----------------------------------------------------------------------------
static int         ffdec_stream_set_param             (stream_obj* stream,
                                            const CHAR_T* name,
                                            const void* value)
{
    DECLARE_STREAM_FF(stream, decoder);

    name = stream_param_name_apply_scope(stream, name);

    SET_PARAM_IF(stream, name, "threadCount", int, decoder->threadCount);
    if ( !_stricmp(name, "liveStream")) {
        decoder->liveStream = *(int*)value;
        // pass it on, if we can
        default_set_param(stream, name, value);
        return 0;
    } else
    if ( !_stricmp(name, "decoderType")) {
        int mediaType = *(int*)value;
        if ( mediaType == mediaVideo || mediaType == mediaAudio ) {
            decoder->mediaType = mediaType;
            return 0;
        }
        decoder->logCb(logError, _FMT("Invalid media type " << mediaType));
        return -1;
    }
    SET_STR_PARAM_IF(stream, name, "hardwareDevice", decoder->hardwareDevice);

    // pass it on, if we can
    return default_set_param(stream, name, value);
}

//-----------------------------------------------------------------------------
static int         ffdec_stream_get_param             (stream_obj* stream,
                                            const CHAR_T* name,
                                            void* value,
                                            size_t* size)
{
    DECLARE_STREAM_FF(stream, decoder);

    name = stream_param_name_apply_scope(stream, name);

    if ( decoder->mediaType == mediaVideo ) {
        COPY_PARAM_IF(decoder, name, "videoCodecId", int, (decoder->passthrough?streamUnknown:streamBitmap));
        COPY_PARAM_IF(decoder, name, "sampleAspectRatio", rational_t,
                        *(rational_t*)&decoder->codecContext->sample_aspect_ratio);
    }
    if ( decoder->mediaType == mediaAudio ) {
        bool pass = decoder->passthrough;
        bool noCodec = pass || decoder->codecContext == NULL;

        COPY_PARAM_IF(decoder, name, "audioCodecId",      int, (pass?streamUnknown:streamLinear));
        COPY_PARAM_IF(decoder, name, "audioSampleFormat", int, (pass?sfmtUndefined:decoder->format));
        COPY_PARAM_IF(decoder, name, "audioSampleRate",   int, (noCodec?-1:decoder->codecContext->sample_rate));
#if FORCE_INTERLEAVED_AUDIO
        COPY_PARAM_IF(decoder, name, "audioInterleaved",  int, 1);
#else
        COPY_PARAM_IF(decoder, name, "audioInterleaved",  int, decoder->interleaved);
#endif
        COPY_PARAM_IF(decoder, name, "audioChannels",     int, (noCodec?-1:decoder->codecContext->channels));
    }
    COPY_PARAM_IF(decoder, name, "framesProcessed", int, decoder->framesProcessed);
    COPY_PARAM_IF(decoder, name, "framesDropped", int, decoder->framesIgnored);


    // pass it on, if we can
    return default_get_param(stream, name, value, size);
}

//-----------------------------------------------------------------------------
static void _ffdec_configure_demux           (stream_obj* stream)
{
    DECLARE_STREAM_FF_V(stream, decoder);
    if ( !decoder->codecContext ) {
        return;
    }

    default_set_param(stream, "width",   &decoder->width);
    default_set_param(stream, "height",  &decoder->height);
    default_set_param(stream, "pixfmt",  &decoder->format);
}

//-----------------------------------------------------------------------------
static enum AVPixelFormat _ffdec_get_hw_format(AVCodecContext *ctx,
                                        const enum AVPixelFormat *pix_fmts)
{
    ffdec_stream* decoder = (ffdec_stream*)ctx->opaque;
    const enum AVPixelFormat *p;

    enum AVPixelFormat retval = AV_PIX_FMT_NONE;
    enum AVPixelFormat preferred = AV_PIX_FMT_NONE;
    int envPixFmt = sv_get_int_env_var("SIO_HW_DECODE_PIX_FMT", -1);
    if ( envPixFmt >= 0 ) {
        preferred = (enum AVPixelFormat)envPixFmt;
    }

    for (p = pix_fmts; *p != AV_PIX_FMT_NONE; p++) {

        if ( *p == preferred ) {
            decoder->logCb(logDebug, _FMT("Preferred pix fmt " << envPixFmt << " is available!"));
            retval = preferred;
            break;
        }

        if ( retval == AV_PIX_FMT_NONE && *p == decoder->hardwarePixFmt ) {
            // Choose first offered HW accelerated format
            retval = *p;
        }
    }

    if ( retval == AV_PIX_FMT_NONE ) {
        decoder->logCb(logError, _FMT("Failed to get HW surface format"));
    } else {
        decoder->logCb(logDebug, _FMT("Using '" << av_get_pix_fmt_name(retval) << "' pix fmt"));
    }
    return retval;
}


//-----------------------------------------------------------------------------
static int       _ffdec_prepare_video_decoder (stream_obj* stream)
{
Retry:
    DECLARE_STREAM_FF(stream, decoder);

    int res = -1;
    size_t size;
    char   buf[128];

    std::string     hwDevName = "";
    AVBufferRef*    hwDev = NULL;

    size = sizeof(buf)-1;
    if ( decoder->hardwareDevice != NULL ) {
        hwDevName = decoder->hardwareDevice;
        decoder->logCb(logInfo, _FMT("Decoder device is set via API to " << hwDevName));
    } if ( sv_get_env_var("SIO_DECODER_DEVICE", buf, &size) == 0 ) {
        hwDevName = buf;
        decoder->logCb(logInfo, _FMT("Decoder device is set via environment to " << hwDevName));
    } else if ( !g_preferredDevice.empty() ) {
        decoder->logCb(logInfo, _FMT("Decoder device is set to " << g_preferredDevice));
        hwDevName = g_preferredDevice;
    }


    if ( !hwDevName.empty() ) {
        if ( hwDevName != "none" && !decoder->hardwareErrorEncountered ) {
            auto it = g_hwDevices.begin();
            while ( it != g_hwDevices.end() ) {
                if ( it->first.name == hwDevName ) {
                    hwDev = it->second.get();
                    break;
                }
                it++;
            }
            if ( hwDev == NULL ) {
                decoder->logCb(logError, _FMT("Device " << hwDevName << " not found"));
                hwDevName = "";
            }
        }
    } else if ( decoder->hardwareDevices != g_hwDevices.end() ) {
        hwDevName = decoder->hardwareDevices->first.name;
        hwDev = decoder->hardwareDevices->second.get();
        decoder->hardwareDevices++;
        // new device, reset this
        decoder->hardwareErrorEncountered = 0;
        decoder->logCb(logDebug, _FMT("Attempting to use decoder device " << hwDevName));
    }


    bool h264PreConfig = (decoder->srcCodecId == streamH264);
    if ( h264PreConfig ) {
        size = sizeof(decoder->sps);
        if ( default_get_param(stream, "sps", &decoder->sps, &size) < 0 ) {
            decoder->logCb(logError, _FMT("Failed to determine sps"));
            h264PreConfig = false;
        }
        size = sizeof(decoder->spsSize);
        if ( h264PreConfig &&
             default_get_param(stream, "spsSize", &decoder->spsSize, &size) < 0 ) {
            decoder->logCb(logError, _FMT("Failed to determine spsSize"));
            h264PreConfig = false;
        }
        size = sizeof(decoder->pps);
        if ( h264PreConfig &&
             default_get_param(stream, "pps", &decoder->pps, &size) < 0 ) {
            decoder->logCb(logError, _FMT("Failed to determine pps"));
            h264PreConfig = false;
        }
        size = sizeof(decoder->ppsSize);
        if ( h264PreConfig &&
             default_get_param(stream, "ppsSize", &decoder->ppsSize, &size) < 0 ) {
            decoder->logCb(logError, _FMT("Failed to determine ppsSize"));
            h264PreConfig = false;
        }
    }


    AVCodec* codec = nullptr;
    switch (decoder->srcCodecId) {
    case streamH264:
#if defined __linux__ && defined __aarch64__
        if ( decoder->enableNVMPI ) {
            codec = avcodec_find_decoder_by_name("h264_nvmpi");
        }
#endif
        if (!codec) {
            decoder->enableNVMPI = 0;
            codec = avcodec_find_decoder(AV_CODEC_ID_H264);
        }
        break;
    case streamMP4:
        codec = avcodec_find_decoder(AV_CODEC_ID_MPEG4);
        break;
    case streamMJPEG:
        codec = avcodec_find_decoder(AV_CODEC_ID_MJPEG);
        break;
    default:
        if ( ( decoder->srcCodecId & streamTransparentBit ) == streamTransparentBit ) {
            enum AVCodecID realCddecId = (enum AVCodecID)( decoder->srcCodecId & ~streamTransparentBit );
            codec = avcodec_find_decoder(realCddecId);
        }
        break;
    }

    if (!codec) {
        decoder->logCb(logError, _FMT("Failed to find codec for the stream: codecId=" << decoder->srcCodecId));
        goto Error;
    }


    avcodec_free_context(&decoder->codecContext);
    decoder->codecContext = avcodec_alloc_context3(codec);
    if (!decoder->codecContext) {
        decoder->logCb(logError, _FMT("Failed to alocate codec context"));
        goto Error;
    }



    if (h264PreConfig) {
        decoder->codecContext->extradata = videolibapi_spspps_to_extradata(
                                  decoder->sps,
                                  decoder->spsSize,
                                  decoder->pps,
                                  decoder->ppsSize,
                                  1,
                                  &decoder->codecContext->extradata_size );
#if defined __linux__ && defined __aarch64__
        if ( decoder->enableNVMPI ) {
            // Most codecs will set width/height fields of codec context. NVMPI does not
            int w=-1, h=-1, p=-1, l=-1;
            videolibapi_parse_sps(decoder->sps, decoder->spsSize, &w, &h, &p, &l);
            if ( w >=0 && h >= 0 ) {
                decoder->codecContext->width = w;
                decoder->codecContext->height = h;
                decoder->logCb(logInfo, _FMT("SPSPPS: w=" << w << " h="<<h << " p=" << p << " l=" << l));
            }
        }
#endif
    }

    decoder->hardwarePixFmt = AV_PIX_FMT_NONE;

    if ( hwDev != NULL ) {
        decoder->codecContext->opaque = (void*)decoder;
        decoder->codecContext->get_format  = _ffdec_get_hw_format;
        decoder->codecContext->hw_device_ctx = av_buffer_ref(hwDev);
        decoder->hardwareFrame = av_frame_alloc();
        if (!decoder->hardwareFrame) {
            decoder->logCb(logError, _FMT("Failed to allocate frame object"));
            return -1;
        }
        enum AVHWDeviceType type = av_hwdevice_find_type_by_name(hwDevName.c_str());

        for (int i = 0;; i++) {
            const AVCodecHWConfig *config = avcodec_get_hw_config(codec, i);
            if (!config) {
                break;
            }
            if (config->methods & AV_CODEC_HW_CONFIG_METHOD_HW_DEVICE_CTX &&
                config->device_type == type) {
                decoder->logCb(logDebug, _FMT("Config " << i << " supports pix_fmt=" << av_get_pix_fmt_name(config->pix_fmt)));
                if ( decoder->hardwarePixFmt == AV_PIX_FMT_NONE ) {
                    decoder->hardwarePixFmt = config->pix_fmt;
                }
            }
        }

        if ( decoder->hardwarePixFmt == AV_PIX_FMT_NONE ) {
            decoder->logCb(logError, _FMT("Decoder '" << codec->name << "' does not support device type " << hwDevName));
            goto Retry;
        }

        AVCodecParameters* codecParams = NULL;
        size = sizeof(codecParams);
        if ( default_get_param(stream, "ffmpegVideoCodecParameters", &codecParams, &size) >= 0 && codecParams != NULL ) {
            avcodec_parameters_to_context(decoder->codecContext, codecParams);
        }

        decoder->logCb(logInfo, _FMT("Using device '" << hwDevName << "' for decoding"));
    } else {
        decoder->logCb(logInfo, _FMT("Using software decoder"));
        if (!decoder->liveStream) {
            decoder->codecContext->thread_count = sv_get_cpu_count();
        }
        if ( decoder->threadCount > 0 ) {
            decoder->codecContext->thread_count = decoder->threadCount;
        }

        // This does NOT work with hardware decoder
        if (codec->capabilities & AV_CODEC_CAP_TRUNCATED) {
            decoder->codecContext->flags |= AV_CODEC_FLAG_TRUNCATED; // we do not send complete frames
        }
        decoder->codecContext->flags2 |= AV_CODEC_FLAG2_CHUNKS;

    }

    decoder->_ffdec_export_frame = _ffdec_export_video_frame;
    res = 0;
Error:
    return res;
}

//-----------------------------------------------------------------------------
static int       _ffdec_prepare_audio_decoder (stream_obj* stream)
{
    DECLARE_STREAM_FF(stream, decoder);

    AVCodecParameters*  sourceAudioCodecpar = NULL;
    int res = -1;
    size_t size;

    AVCodec* codec;
    switch (decoder->srcCodecId) {
    case streamAAC:
        codec = avcodec_find_decoder(AV_CODEC_ID_AAC);
        break;
    case streamPCMU:
        codec = avcodec_find_decoder(AV_CODEC_ID_PCM_MULAW);
        break;
    case streamPCMA:
        codec = avcodec_find_decoder(AV_CODEC_ID_PCM_ALAW);
        break;
    default:
        codec = NULL;
        break;
    }

    if (!codec) {
        decoder->logCb(logError, _FMT("Failed to find codec for the stream: codecId=" << decoder->srcCodecId));
        goto Error;
    }

    decoder->codecContext = avcodec_alloc_context3(codec);
    if (!decoder->codecContext) {
        decoder->logCb(logError, _FMT("Failed to alocate codec context"));
        goto Error;
    }


    size = sizeof(int);
    if (default_get_param(stream, "audioChannels", &decoder->codecContext->channels, &size) < 0 ) {
        decoder->logCb(logError, _FMT("Failed to determine channel count."));
        goto Error;
    }

    size = sizeof(int);
    if (default_get_param(stream, "audioSampleRate",&decoder->codecContext->sample_rate, &size) < 0 ) {
        decoder->logCb(logError, _FMT("Failed to determine sample rate."));
        goto Error;
    }

    size = sizeof(AVCodecParameters*);
    if ( default_get_param(stream, "ffmpegAudioCodecParameters", &sourceAudioCodecpar, &size) >=0 && sourceAudioCodecpar != NULL ) {
        if ( avcodec_parameters_to_context(decoder->codecContext, sourceAudioCodecpar) < 0 ) {
            decoder->logCb(logError, _FMT("Failed to apply codec parameters to audio decoder"));
        }
    }

    // we prefer linear16, but it's not guaranteed
    decoder->codecContext->sample_fmt = AV_SAMPLE_FMT_NONE;

    decoder->_ffdec_export_frame = _ffdec_export_audio_frame;
    res = 0;
Error:
    return res;
}

//-----------------------------------------------------------------------------
static int         _ffdec_prepare_decoder              (stream_obj* stream)
{
    DECLARE_STREAM_FF(stream, decoder);

    decoder->passthrough = 0;
    int res =  (decoder->mediaType == mediaVideo) ? _ffdec_prepare_video_decoder(stream)
                   : _ffdec_prepare_audio_decoder(stream);
    if ( res < 0 ) {
        return res;
    }


    if ( (res=avcodec_open2(decoder->codecContext, decoder->codecContext->codec, NULL)) < 0 ) {
        decoder->logCb(logError, _FMT("Failed to init codec context"));
        return res;
    }

    return 0;
}

//-----------------------------------------------------------------------------
static int         ffdec_stream_open_in                (stream_obj* stream)
{
    DECLARE_STREAM_FF(stream, decoder);
    int             res = -1;
    size_t          size;
    int             frameType;
    bool            isVideo = (decoder->mediaType == mediaVideo);
    const char*     sCodecIdParam = (isVideo ? "videoCodecId" : "audioCodecId");


    res = default_open_in(stream);
    if (res < 0) {
        goto Error;
    }

    size = sizeof(decoder->srcCodecId);
    if ( default_get_param(stream, sCodecIdParam, &decoder->srcCodecId, &size) < 0 ||
            decoder->srcCodecId == streamUnknown ) {
        // Only log debug message here ... video decoder will fail to initialize later on; and this scenario is normal for audio
        decoder->logCb(logDebug, _FMT("Failed to determine source " << sCodecIdParam << ". Decoder operates as passthrough."));
        decoder->passthrough = 1;
        return 0;
    }

    res =  _ffdec_prepare_decoder(stream);
    if ( res < 0 ) {
        goto Error;
    }


ReadAnother:
    if ( ffdec_stream_read_frame(stream, &decoder->firstFrame) < 0 || decoder->firstFrame == NULL ) {
        decoder->logCb(logError, _FMT("Failed to read first frame"));
        res = -1;
        goto Error;
    }
    if ( decoder->mediaType == mediaVideo ) {
        frameType = frame_get_api(decoder->firstFrame)->get_media_type(decoder->firstFrame);
        if ( frameType != decoder->mediaType ) {
            frame_unref(&decoder->firstFrame);
            goto ReadAnother;
        }
        _ffdec_configure_demux(stream);
    } else {
        // for audio frames, we don't have to wait for the first frame,
        // since nothing on the backend depends on the feedback from us
        // TODO: do we still depend on that feedback for video?!
    }

    decoder->logCb(logTrace, _FMT("Opened decoder object " << (void*)stream));
    res = 0;

Error:
    if ( res != 0 ) {
        ffdec_stream_close(stream);
    }
    return res;
}


//-----------------------------------------------------------------------------
static int         ffdec_stream_seek               (stream_obj* stream,
                                                       INT64_T offset,
                                                       int flags)
{
    DECLARE_STREAM_FF(stream, decoder);

    // pass it on ...
    int res = default_seek(stream, offset, flags);
    if ( res >= 0 ) {
        decoder->logCb(logTrace, _FMT("Seeking to " << offset));
        decoder->eof = 0;
        if (decoder->codecContext) {
            avcodec_flush_buffers(decoder->codecContext);
        }
        frame_unref(&decoder->firstFrame);
        frame_unref(&decoder->prevSeekFrame);
        frame_unref(&decoder->nextFrameToReturn);
        decoder->firstFrameServed = 0;
        decoder->framesProcessed = 0;
        decoder->noFrameCount = 0;
        if ( decoder->mediaType == mediaVideo && (flags & sfPrecise)) {
            decoder->seekingTo = offset;
            decoder->seekFlags = flags;
        }
    }
    return res;
}

//-----------------------------------------------------------------------------
static size_t      ffdec_stream_get_width          (stream_obj* stream)
{
    DECLARE_STREAM_FF(stream, decoder);
    return (decoder->mediaType==mediaVideo ? decoder->width : default_get_width(stream));
}

//-----------------------------------------------------------------------------
static size_t      ffdec_stream_get_height         (stream_obj* stream)
{
    DECLARE_STREAM_FF(stream, decoder);
    return (decoder->mediaType==mediaVideo ? decoder->height : default_get_height(stream));
}

//-----------------------------------------------------------------------------
static int         ffdec_stream_get_pixel_format   (stream_obj* stream)
{
    DECLARE_STREAM_FF(stream, decoder);
    return (decoder->mediaType==mediaVideo ? decoder->format : default_get_pixel_format(stream));
}

//-----------------------------------------------------------------------------
static int         _ffdec_should_ignore_frame     (ffdec_stream_obj* decoder)
{
    int             retVal = 0;
    static const int kMaxFramesToIgnore = 10;

    if ( decoder->liveStream &&
        !decoder->captureHasStabilized ) {
        UINT64_T now = sv_time_get_current_epoch_time();

        // Due to FFMPEG (or maybe IP cam) weirdnesses, we get flooded
        // with a whole bunch of frames right when we first open the
        // stream.  Since the frames don't have a timestamp, we would
        // have to assume that they all came from right now, which is
        // wrong.  Since they're really not _that_ important, it's better
        // to drop them than give them the wrong time.  This also
        // manages to avoid ever capturing the first frame, which is
        // nearly always wrong on my TrendNet camera...
        UINT64_T msDelta = sv_time_get_time_diff( decoder->prevCaptureTimeMs, now );

        // We're not stabilized until input is less than 100FPS
        if ( decoder->framesIgnored < kMaxFramesToIgnore &&
             (decoder->prevCaptureTimeMs == 0 || msDelta <= 10)) {
            decoder->prevCaptureTimeMs = now;
            retVal = 1;
        } else {
            decoder->captureHasStabilized = 1;
        }
    }

    return retVal;
}

//-----------------------------------------------------------------------------
#define LOG_ERROR_FRAMES 0
#if LOG_ERROR_FRAMES
static void        _ffdec_pb_cb           ( const char* line, void* ctx )
{
    ((stream_base*)ctx)->logCb(logError, line);
}
#endif

//-----------------------------------------------------------------------------
static int
_ffdec_export_video_frame                (ffdec_stream* decoder, AVFrame* f,
                                        frame_obj* frameOut)
{
    int format = ffpfmt_to_svpfmt((enum AVPixelFormat)f->format, f->color_range);
    if ( f->width != decoder->width && decoder->width != -1 ) {
        decoder->logCb(logError, _FMT("Width had changed: " << decoder->width << "->" << f->width ));
        return -1;
    }
    if ( f->height != decoder->height && decoder->height != -1 ) {
        decoder->logCb(logError, _FMT("Height had changed: " << decoder->height << "->" << f->height ));
        return -1;
    }
    if ( format != decoder->format && decoder->format != pfmtUndefined ) {
        decoder->logCb(logError, _FMT("Pixfmt had changed to : " << av_get_pix_fmt_name((enum AVPixelFormat)decoder->format) << "->" << av_get_pix_fmt_name((enum AVPixelFormat)f->format) ));
        return -1;
    }

    frame_api_t* api = frame_get_api(frameOut);
    api->set_media_type(frameOut, mediaVideo);
    if ( api->set_backing_obj(frameOut, "avframe", (void*)f) < 0 ) {
        decoder->logCb(logError, _FMT("Couldn't initialize output frame"));
        return -1;
    }

    if ( decoder->format == sfmtUndefined ) {
        decoder->logCb(logInfo, _FMT("Setting pixfmt to " << av_get_pix_fmt_name((enum AVPixelFormat)f->format)));
        decoder->format = api->get_pixel_format(frameOut);
        decoder->width = api->get_width(frameOut);
        decoder->height = api->get_height(frameOut);
    }

    TRACE_C(1, _FMT("received video frame #=" << decoder->framesProcessed <<
                                " packet #=" << decoder->packetsProcessed <<
                                " outPts=" << api->get_pts(frameOut) <<
                                " frameRepeat=" << f->repeat_pict <<
                                " size=" << api->get_data_size(frameOut) <<
                                " w=" << f->width <<
                                " h=" << f->height <<
                                " fmt=" << api->get_pixel_format(frameOut)));
    return 0;
}


//-----------------------------------------------------------------------------
static int
_ffdec_export_audio_frame                (ffdec_stream* decoder, AVFrame* f,
                                        frame_obj* frameOut)
{
    frame_api_t* api = frame_get_api(frameOut);
    api->set_media_type(frameOut, mediaAudio);
    if ( api->set_backing_obj(frameOut, "avframe", (void*)f) < 0 ) {
        decoder->logCb(logError, _FMT("Couldn't initialize output frame"));
        return -1;
    }

    TRACE_C(1, _FMT("received frame #=" << decoder->framesProcessed <<
                                " packet #=" << decoder->packetsProcessed <<
                                " outPts=" << api->get_pts(frameOut) <<
                                " frameRepeat=" << f->repeat_pict <<
                                " size=" << api->get_data_size(frameOut)));
    return 0;
}

//-----------------------------------------------------------------------------
static int  _ffdec_receive_frame           (ffdec_stream* decoder,
                                            frame_obj** frame)
{
    static const int    kMaxFramesToSkip = 30;
    frame_api_t*        api = get_ffframe_frame_api();
    frame_obj*          frameOut = NULL;
    int                 res = 0;
    AVFrame*            tmpFrame;

TryAgain:
    // allocate ffmpeg frame object, if needed
    if (decoder->ffFrame == NULL) {
        decoder->ffFrame = av_frame_alloc();
        if (!decoder->ffFrame) {
            decoder->logCb(logError, _FMT("Failed to allocate frame object"));
            return -1;
        }
    }

    tmpFrame = (decoder->codecContext->hw_device_ctx ? decoder->hardwareFrame : decoder->ffFrame);
    res = avcodec_receive_frame(decoder->codecContext, tmpFrame);
    if ( res == AVERROR(EAGAIN) ) {
        return 0;
    }

    if ( res < 0 ) {
        // an error had occurred
        return res;
    }

    if ( decoder->codecContext->hw_device_ctx ) {
        if ( decoder->hardwareXferPixFmt == AV_PIX_FMT_NONE ) {
            enum AVPixelFormat* formats;
            res = av_hwframe_transfer_get_formats(decoder->hardwareFrame->hw_frames_ctx, AV_HWFRAME_TRANSFER_DIRECTION_FROM, &formats, 0);
            if ( res < 0 ) {
                decoder->logCb(logError, "Failed to retrieve supported transfer formats");
                return res;
            }

            decoder->hardwareXferPixFmt = _get_preferred_pix_fmt(formats);
            decoder->logCb(logInfo, _FMT("GPU xfer format: " << av_get_pix_fmt_name((enum AVPixelFormat)decoder->hardwareFrame->format) << "->" << av_get_pix_fmt_name(decoder->hardwareXferPixFmt)));
            av_freep(&formats);
        }
        decoder->ffFrame->format = decoder->hardwareXferPixFmt;
        if ( !decoder->ffFrame->buf[0] ) {
            decoder->ffFrame->width = decoder->hardwareFrame->width;
            decoder->ffFrame->height = decoder->hardwareFrame->height;
            res = av_frame_get_buffer(decoder->ffFrame, 0);
            if ( res < 0 ) {
                decoder->logCb(logError, _FMT("Failed to get frame buffer: " << res));
                return res;
            }
        }
        av_frame_copy_props(decoder->ffFrame, decoder->hardwareFrame);
        av_hwframe_transfer_data(decoder->ffFrame, decoder->hardwareFrame, 0);
    }

    // we have a decoded frame, validate its PTS
    if ( decoder->lastOutputPts > FF_FRAME_PTS(decoder->ffFrame) &&
         decoder->framesProcessed > 0 ) {
        // no frames with PTS in the past should ever leave the decoder ...
        // skip those up to a limit, and force reconnect by returning an error when the limit is hit
        decoder->framesSkipped++;
        decoder->logCb(logError, _FMT("received frame with pts=" << FF_FRAME_PTS(decoder->ffFrame) <<
                                        " after pts=" << decoder->lastOutputPts <<
                                        " was seen; skipped " << decoder->framesSkipped << " frames so far"));
        // error out when too many frames had been skipped
        return decoder->framesSkipped > kMaxFramesToSkip ? -1 : 0;
    }

    // we're going to use this frame
    decoder->lastOutputPts = FF_FRAME_PTS(decoder->ffFrame);
    decoder->framesProcessed ++;

    frameOut = alloc_avframe_frame(FFDEC_STREAM_MAGIC, decoder->fa, decoder->logCb);

    if ( decoder->_ffdec_export_frame(decoder, decoder->ffFrame, frameOut) < 0 ) {
        frame_unref(&frameOut);
        return -1;
    }

    // new ffmpeg frame object will need to be allocated later
    decoder->ffFrame = NULL;

    // if we're decoding as an attempt to seek to a certain position,
    // we may need to get next frame
    // prior to determining which frame shall be returned
    if ( decoder->seekingTo != INVALID_PTS ) {
        if ( decoder->lastOutputPts < decoder->seekingTo ) {
            TRACE(_FMT("Skipping frame with pts=" << decoder->lastOutputPts << ": seeking to pts=" << decoder->seekingTo ));
            // remember the current frame -- we may need to return it
            frame_unref(&decoder->prevSeekFrame);
            decoder->prevSeekFrame = frameOut;
            frameOut = NULL;
            goto TryAgain;
        } else if ( decoder->seekingTo == decoder->lastOutputPts  ||
                    decoder->prevSeekFrame == NULL ||
                    (decoder->seekFlags & sfBackward) == 0 ) {
            // use the current frame as the return value
            TRACE(_FMT("Seek completed at pts=" << decoder->lastOutputPts << ": was seeking to pts=" << decoder->seekingTo ));
            frame_unref(&decoder->prevSeekFrame);
        } else {
            // use the previous frame as the return value
            TRACE(_FMT("Seek completed at pts=" << decoder->lastOutputPts << ": was seeking to pts=" << decoder->seekingTo <<
                        " returning frame at " << frame_get_api(decoder->prevSeekFrame)->get_pts(decoder->prevSeekFrame)));
            // save the current frame; it'll still need to be returned
            frame_unref(&decoder->nextFrameToReturn);
            decoder->nextFrameToReturn = frameOut;
            // return the previous frame
            frameOut = decoder->prevSeekFrame;
            decoder->prevSeekFrame = NULL;
        }
        decoder->seekingTo = INVALID_PTS;
    }

    *frame = frameOut;
    return 0;
}

//-----------------------------------------------------------------------------
static int  _ffdec_submit_packet                (ffdec_stream* decoder,
                                                frame_obj* sourceFrame,
                                                frame_obj** frame)
{
    // we got a new packet
    frame_api_t* fapi = frame_get_api(sourceFrame);

    if ( fapi->get_media_type(sourceFrame) != decoder->mediaType ||
         decoder->passthrough ) {
        // pass frames we don't decode through
        decoder->prevPacketSubmitted = 0;
        *frame = sourceFrame;
        return 0;
    }

    decoder->prevPacketSubmitted = 1;
Retry:
    AVPacket packet;
    av_init_packet(&packet);
    packet.pts  = fapi->get_pts(sourceFrame);
    packet.dts  = packet.pts;
    packet.data = (uint8_t*)fapi->get_data(sourceFrame);
    packet.size = fapi->get_data_size(sourceFrame);
    packet.flags = 0;
    bool key = (fapi->get_keyframe_flag(sourceFrame) > 0);
    if (key) {
        packet.flags |= AV_PKT_FLAG_KEY;
        decoder->lastKeyFrameInterval = decoder->packetsSinceKeyframe;
        decoder->packetsSinceKeyframe = 0;
    } else {
        decoder->packetsSinceKeyframe ++;
    }

    int res = avcodec_send_packet(decoder->codecContext, &packet);
    if ( res < 0 ) {
        if ( decoder->codecContext->hw_device_ctx != NULL && decoder->packetsProcessed == 0 ) {
            decoder->logCb(logInfo, _FMT("Attempting to reinit decoder"));
            avcodec_free_context(&decoder->codecContext);
            decoder->hardwareErrorEncountered = 1;
            res = _ffdec_prepare_decoder((stream_obj*)decoder);
            if ( res >= 0 ) {
                goto Retry;
            }
        } else {
            decoder->logCb(logError, _FMT("Error submitting a packet to decoder (" <<
                            " err=" << res << " - " << av_err2str(res) <<
                            ", size=" << packet.size <<
                            ", flags=" << packet.flags <<
                            ", NALU=" << videolibapi_contained_nalu(packet.data, packet.size, decoder->logCb) <<
                            ", ctx=" << (void*)decoder->codecContext->hw_device_ctx <<
                            ", processed=" << decoder->packetsProcessed << ")"));
            std::string buf;
            for (int nI=0; nI<packet.size; nI++) {
                char foo[16];
                sprintf(foo, "%.02x", packet.data[nI]);
                buf += _STR( " " << foo );
            }
            decoder->logCb(logError, _FMT("Data: " << buf));
        }
        frame_unref(&sourceFrame);
        return -1;
    }

    frame_unref(&sourceFrame);
    decoder->packetsProcessed++;


    // Check for decoder timeout. This will happen, when we aren't reading
    // network bistream fast enough, and it gets corrupted. Theoretically, it should
    // recover on an I-frame, but when I-frames get corrupted, the decoder may
    // end up spinning forever without producing a frame
    if ( key &&
            // wait for at least 2 keyframe intervals
            decoder->noFrameCount > std::max(kMaxPacketsWithNoFrames, decoder->lastKeyFrameInterval*2) &&
            // do not let the decoder time out, if it hasn't processed anything
            decoder->framesProcessed > 0) {
        decoder->logCb(logError, _FMT("Decoder timeout: packets=" <<
                    decoder->noFrameCount << " lastKeyFrameInterval=" <<
                    decoder->lastKeyFrameInterval));
        return -1;
    }

    return 0;
}

//-----------------------------------------------------------------------------
static int  _ffdec_on_read_error                (ffdec_stream* decoder,
                                                frame_obj** frame)
{
    if ( decoder->eof ) {
        // EOF had been previously encountered, do not attempt to flush again
        return -1;
    }

    size_t paramSize = sizeof(int);
    if ( default_get_param((stream_obj*)decoder, "eof", &decoder->eof, &paramSize) >= 0 &&
         decoder->eof ) {
        TRACE(_FMT("Decoder encountered EOF"));
        if ( decoder->passthrough ) {
            return -1;
        }
        // continue -- we'll want to flush the decoder
        int res = avcodec_send_packet(decoder->codecContext, NULL);
        if ( res < 0 ) {
            TRACE(_FMT("Error " << res << " flushing decoder after EOF"));
            return -1;
        }

        return 0;
    }

    decoder->logCb(logError, _FMT("Failed to read a packet"));
    return -1;
}

//-----------------------------------------------------------------------------
static int         ffdec_stream_read_frame        (stream_obj* stream, frame_obj** frame)
{
    DECLARE_STREAM_FF(stream, decoder);

    int                 res = 0;

    *frame = NULL;

    // do not allow continued use of the decoder, if an error previously occurred
    while ( !decoder->errorOccurred && res >= 0 ) {
        // return the frame we've read during initialization, if needed
        if ( !decoder->firstFrameServed && decoder->firstFrame ) {
            *frame = decoder->firstFrame;
            // we've never taken a reference, so no need to release it now
            decoder->firstFrame = NULL;
            decoder->firstFrameServed = 1;
            return 0;
        }

        // a frame may have been previously read, but not returned
        if ( decoder->nextFrameToReturn != NULL ) {
            *frame = decoder->nextFrameToReturn;
            decoder->nextFrameToReturn = NULL;
            return 0;
        }

        // see if the frame can be generated by the decoder
        if  (!decoder->passthrough ) {
            res = _ffdec_receive_frame(decoder, frame);
        }

        // this will be needed later to reset the no-frame counter
        bool frameFromDecoder = (*frame != NULL);

        // attempt to read a new frame, and submit it for decoding (or return it)
        if ( res >= 0 && !frameFromDecoder && !decoder->eof ) {
            // do not increment no-output counter, if we're in seek mode -- frames are being iggnored then
            // only increment this counter, when the previous packet had been submitted
            if ( decoder->seekingTo == INVALID_PTS && decoder->prevPacketSubmitted ) {
                decoder->noFrameCount++;
            }
            // if something is read, we can't use it to reset the no-frame counter - it'd be from a different stream
            frameFromDecoder = false;

            frame_obj* sourceFrame = NULL;

            if ( default_read_frame(stream, &sourceFrame) < 0 || sourceFrame == NULL ) {
                res = _ffdec_on_read_error(decoder, frame);
            } else {
                res = _ffdec_submit_packet(decoder, sourceFrame, frame);
            }
        }

        // return the frame if we got it
        if ( res >= 0 && *frame != NULL ) {
            if (frameFromDecoder) {
                decoder->noFrameCount = 0;
            }
            return 0;
        }
    };

    // all the successful exit conditions occur in the loop; we're only here in case of error
    if (!decoder->eof) {
        // in case of eof, rewind is possible -- we should be ready to continue decoding in that case
        decoder->errorOccurred = 1;
    }
    return res;
}

//-----------------------------------------------------------------------------
static int         ffdec_stream_close             (stream_obj* stream)
{
    DECLARE_STREAM_FF(stream, decoder);

    av_frame_free(&decoder->ffFrame);
    av_frame_free(&decoder->hardwareFrame);
    if (decoder->codecContext) {
        TRACE(_FMT("Closing stream object " << (void*)stream <<
                    ": codec object " << (void*)decoder->codecContext));
        avcodec_free_context(&decoder->codecContext);
    }
    frame_unref(&decoder->firstFrame);
    frame_unref(&decoder->prevSeekFrame);
    frame_unref(&decoder->nextFrameToReturn);
    decoder->sourceApi->close(decoder->source);
    stream_unref(&decoder->source);
    sv_freep(&decoder->hardwareDevice);

    decoder->prevCaptureTimeMs = 0;
    decoder->captureHasStabilized = 0;
    return 0;
}

//-----------------------------------------------------------------------------
static void ffdec_stream_destroy         (stream_obj* stream)
{
    DECLARE_STREAM_FF_V(stream, decoder);
    decoder->logCb(logTrace, _FMT("Destroying stream object " << (void*)stream));
    ffdec_stream_close(stream); // make sure all the internals had been freed
    destroy_frame_allocator(&decoder->fa, decoder->logCb);
    stream_destroy( stream );
}

//-----------------------------------------------------------------------------
SVVIDEOLIB_API stream_api_t*     get_ffdec_stream_api             ()
{
    std::call_once( kStaticInitFlag, ffmpeg_init_hw );
    return &_g_ffdec_stream_provider;
}

