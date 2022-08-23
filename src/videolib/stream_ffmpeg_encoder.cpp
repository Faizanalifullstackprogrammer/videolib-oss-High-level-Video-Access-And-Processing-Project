#undef SV_MODULE_VAR
#define SV_MODULE_VAR encoder
#define SV_MODULE_ID "FFENCODER"
#include "sv_module_def.hpp"

#include "streamprv.h"
#include "sv_ffmpeg.h"

#include "videolibUtils.h"
#include "frame_basic.h"

#define FFENC_STREAM_MAGIC 0x1713
#define MAX_PARAM 10
#define FORCE_INTERLEAVED_AUDIO 1

extern "C" stream_api_t*     get_resize_factory_api                    ();


//-----------------------------------------------------------------------------
typedef struct ffenc_stream  : public stream_base {
    AVCodecContext*     codecContext;
    AVFrame*            encFrame;
    int                 mediaType;      // audio or video encoder
    int                 srcCodecId;     // we get this from the upstream filter
    int                 dstCodecId;     // set by the consumer
    int                 hls;            // when set, different set of defaults is used
    int                 encoderDelay;   // calculated on the first output frame


    float               bitrate_multiplier;
    int                 max_bitrate;
    int                 gop_size;
    int                 keyint_min;
    int                 videoQualityPreset;
    char*               preset;
    int                 canUpdatePixfmt;

    int                 hlsHibernating; // only encode using i-frames
    INT64_T             lastInputPts;   // pts of the last input frame we've processed

    uint8_t*            sps;
    size_t              spsSize;
    uint8_t*            pps;
    size_t              ppsSize;

    int                 h264profile;
    int                 h264level;

    frame_allocator*    fa;
    frame_obj*          nextFrame;
} ffenc_stream_obj;


//-----------------------------------------------------------------------------
// Stream API
//-----------------------------------------------------------------------------

//-----------------------------------------------------------------------------
//  Forward declarations
//-----------------------------------------------------------------------------
static stream_obj* ffenc_stream_create             (const char* name);
static int         ffenc_stream_set_param          (stream_obj* stream,
                                                    const CHAR_T* name,
                                                    const void* value);
static int         ffenc_stream_get_param          (stream_obj* stream,
                                                    const CHAR_T* name,
                                                    void* value,
                                                    size_t* size);
static int         ffenc_stream_open_in            (stream_obj* stream);
static int         ffenc_stream_read_frame         (stream_obj* stream, frame_obj** frame);
static int         ffenc_stream_close              (stream_obj* stream);
static void        ffenc_stream_destroy            (stream_obj* stream);

//-----------------------------------------------------------------------------
stream_api_t _g_ffenc_stream_provider = {
    ffenc_stream_create,
    get_default_stream_api()->set_source,
    get_default_stream_api()->set_log_cb,
    get_default_stream_api()->get_name,
    get_default_stream_api()->find_element,
    get_default_stream_api()->remove_element,
    get_default_stream_api()->insert_element,
    ffenc_stream_set_param,
    ffenc_stream_get_param,
    ffenc_stream_open_in,
    get_default_stream_api()->seek,
    get_default_stream_api()->get_width,
    get_default_stream_api()->get_height,
    get_default_stream_api()->get_pixel_format,
    ffenc_stream_read_frame,
    get_default_stream_api()->print_pipeline,
    ffenc_stream_close,
    _set_module_trace_level
} ;


//-----------------------------------------------------------------------------
#define DECLARE_STREAM_FF(stream, name) \
    DECLARE_OBJ(ffenc_stream, name,  stream, FFENC_STREAM_MAGIC, -1)
#define DECLARE_STREAM_FF_V(stream, name) \
    DECLARE_OBJ_V(ffenc_stream, name,  stream, FFENC_STREAM_MAGIC)


static stream_obj*   ffenc_stream_create                (const char* name)
{
    ffenc_stream* res = (ffenc_stream*)stream_init(sizeof(ffenc_stream),
                                        FFENC_STREAM_MAGIC,
                                        &_g_ffenc_stream_provider,
                                        name,
                                        ffenc_stream_destroy );

    res->codecContext = NULL;
    res->encFrame = NULL;
    res->mediaType  = mediaVideo;
    res->srcCodecId = streamUnknown;
    res->dstCodecId = streamUnknown;
    res->hls =0;
    res->encoderDelay = -1;
    res->bitrate_multiplier = 0;
    res->max_bitrate = 0;
    res->gop_size = 0;
    res->keyint_min = 0;
    res->videoQualityPreset = svvpNotSpecified;
    res->preset = NULL;
    res->canUpdatePixfmt = 0;
    res->hlsHibernating = 0;
    res->pps = res->sps = NULL;
    res->spsSize = res->ppsSize = 0;
    res->h264profile = h264Baseline;
    res->h264level = 31;
    res->fa = create_frame_allocator(_STR("encoder_"<<name));
    res->nextFrame = NULL;

    return (stream_obj*)res;
}

//-----------------------------------------------------------------------------
static int         ffenc_stream_set_param             (stream_obj* stream,
                                            const CHAR_T* name,
                                            const void* value)
{
    DECLARE_STREAM_FF(stream, encoder);

    name = stream_param_name_apply_scope(stream, name);

    SET_PARAM_IF(stream, name, "encoderType", int, encoder->mediaType);
    SET_PARAM_IF(stream, name, "dstCodecId", int, encoder->dstCodecId);
    SET_PARAM_IF(stream, name, "hls", int, encoder->hls);
    SET_PARAM_IF(stream, name, "bitrate_multiplier", float, encoder->bitrate_multiplier);
    SET_PARAM_IF(stream, name, "max_bitrate", int, encoder->max_bitrate);
    SET_PARAM_IF(stream, name, "gop_size", int, encoder->gop_size);
    SET_PARAM_IF(stream, name, "keyint_min", int, encoder->keyint_min);
    SET_PARAM_IF(stream, name, "canUpdatePixfmt", int, encoder->canUpdatePixfmt);
    SET_PARAM_IF(stream, name, "videoQualityPreset", int, encoder->videoQualityPreset);
    SET_PARAM_IF(stream, name, "hlsHibernating", int, encoder->hlsHibernating);
    SET_PARAM_IF(encoder, name, "h264profile", int, encoder->h264profile);
    SET_PARAM_IF(encoder, name, "h264level", int, encoder->h264level);
    SET_STR_PARAM_IF(stream, name, "preset", encoder->preset);


    return default_set_param(stream, name, value);
}

//-----------------------------------------------------------------------------
static int         ffenc_stream_get_param             (stream_obj* stream,
                                            const CHAR_T* name,
                                            void* value,
                                            size_t* size)
{
    DECLARE_STREAM_FF(stream, encoder);

    name = stream_param_name_apply_scope(stream, name);

    if ( encoder->mediaType == mediaVideo ) {
        COPY_PARAM_IF(encoder, name, "videoCodecId", int, encoder->dstCodecId);
        COPY_PARAM_IF(encoder, name, "encoderDelay", int, encoder->encoderDelay);
        if ( !encoder->passthrough ) {
            COPY_PARAM_IF(encoder, name, "sps", void*, encoder->sps);
            COPY_PARAM_IF(encoder, name, "spsSize", int, encoder->spsSize);
            COPY_PARAM_IF(encoder, name, "pps", void*, encoder->pps);
            COPY_PARAM_IF(encoder, name, "ppsSize", int, encoder->ppsSize);
            COPY_PARAM_IF(encoder, name, "h264profile", int, encoder->h264profile);
            COPY_PARAM_IF(encoder, name, "h264level", int, encoder->h264level);
        }
    }
    if ( encoder->mediaType == mediaAudio ) {
        COPY_PARAM_IF(encoder, name, "audioCodecId", int, encoder->dstCodecId);
    }

    return default_get_param(stream, name, value, size);
}

//-----------------------------------------------------------------------------
static bool      _ffenc_is_compatible_pixfmt(ffenc_stream_obj* mux, int pixfmt)
{
    if ( mux->dstCodecId == streamH264 )
        return pixfmt == pfmtYUV420P
                || pixfmt == pfmtYUVJ420P
                || pixfmt == pfmtNV12
                || pixfmt == pfmtNV21
                ;
    if ( mux->dstCodecId == streamMJPEG || mux->dstCodecId == streamJPG )
         return pixfmt == pfmtYUVJ420P
                || pixfmt == pfmtYUVJ422P
                || pixfmt == pfmtYUVJ444P
                ;
    if ( mux->dstCodecId == streamGIF )
        return pixfmt == pfmtRGB8;

    return false;
}

//-----------------------------------------------------------------------------
static int        _ffenc_get_compatible_pixfmt      (ffenc_stream_obj* mux)
{
    if ( mux->dstCodecId == streamH264 )
        return pfmtYUV420P;
    if ( mux->dstCodecId == streamMJPEG || mux->dstCodecId == streamJPG )
        return pfmtYUVJ420P;
    if ( mux->dstCodecId == streamGIF )
        return pfmtRGB8;
    return pfmtYUV420P;
}

//-----------------------------------------------------------------------------
static int        _ffsink_configure_h264_encoder   (ffenc_stream_obj* encoder,
                                                    AVCodecContext* codecContext,
                                                    AVDictionary*& dict );

static int       _ffenc_prepare_video_encoder (ffenc_stream_obj* encoder)
{
    stream_obj* stream = (stream_obj*)encoder;

    if ( encoder->srcCodecId != streamBitmap ) {
        encoder->logCb(logError, _FMT("Can't proceed with recording: this filter will encode raw frames"));
        return -1;
    }

    int pix_fmt = default_get_pixel_format(stream);
    if ( !_ffenc_is_compatible_pixfmt(encoder, pix_fmt) ) {
        int pixfmtSet = _ffenc_get_compatible_pixfmt(encoder);
        if ( encoder->canUpdatePixfmt ) {
            if ( default_set_param(stream, "updatePixfmt", &pixfmtSet) < 0 ) {
                encoder->logCb(logError, _FMT("Failed to update a pixel format converter"));
                return -1;
            }
        } else {
            TRACE(_FMT("Auto-inserting pixfmt converter from " << pix_fmt << " to " << pfmtYUV420P ));
            std::string elname(_STR(encoder->name<<".pixfmtconv"));
            default_insert_element(&encoder->source, &encoder->sourceApi, NULL, get_resize_factory_api()->create(elname.c_str()), svFlagStreamInitialized);
            if ( default_set_param(stream, _STR(elname<<".pixfmt"), &pixfmtSet) < 0 ||
                 // initialize new element directly
                 encoder->sourceApi->open_in(encoder->source) < 0 ) {
                encoder->logCb(logError, _FMT("Failed to auto-insert a pixel format converter"));
                return -1;
            }
        }
        pix_fmt = pixfmtSet;
    }


    AVCodec*        codec;
    enum AVCodecID  codec_id;
    AVDictionary*   dict = NULL;
    int             res = -1;

    switch ( encoder->dstCodecId ) {
    case streamGIF:     codec_id = AV_CODEC_ID_GIF; break;
    case streamJPG:
    case streamMJPEG:   codec_id = AV_CODEC_ID_MJPEG; break;
    case streamH264:
    default:            codec_id = AV_CODEC_ID_H264; break;
    }

    if ( codec_id == AV_CODEC_ID_H264 ) {
        const char* encoderName = "libx264";
        if ( pix_fmt == pfmtRGB24 || pix_fmt == pfmtBGR24 )
            encoderName = "libx264rgb";
        codec = avcodec_find_encoder_by_name(encoderName);
    } else {
        codec = avcodec_find_encoder(codec_id);
    }
    if ( !codec ) {
        encoder->logCb(logError, _FMT("Can't proceed with recording: codec was not found"));
        return -1;
    }


    AVCodecContext* codecContext = avcodec_alloc_context3(codec);
    if (!codecContext) {
        encoder->logCb(logError, _FMT("Failed to alocate codec context"));
        return -1;
    }


    // These parameters are provided from the higher layer
    codecContext->codec_type = AVMEDIA_TYPE_VIDEO;
    codecContext->codec_id = codec_id;
    codecContext->width = default_get_width(stream);
    codecContext->height = default_get_height(stream);
    codecContext->pix_fmt = svpfmt_to_ffpfmt_ext(pix_fmt, &codecContext->color_range, encoder->dstCodecId);
    codecContext->codec_tag = 0;
    codecContext->max_b_frames = 0;
    codecContext->time_base.num = 1;
    codecContext->time_base.den = 1000;

    encoder->logCb(logDebug, _FMT("Opening codec " << codec->long_name <<
                            "(" << codec->id << ") hls=" << encoder->hls <<
                            " vqp=" << encoder->videoQualityPreset <<
                            " max_bitrate=" << encoder->max_bitrate ));

    if ( encoder->dstCodecId == streamH264 ) {
        _ffsink_configure_h264_encoder( encoder, codecContext, dict );
    } else if ( encoder->dstCodecId == streamGIF ) {
        av_opt_set(codecContext, "gifflags", "-offsetting-transdiff", AV_OPT_SEARCH_CHILDREN);
    }

    codecContext->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;


    av_frame_free( &encoder->encFrame );
    encoder->encFrame = av_frame_alloc();
    if ( encoder->encFrame == NULL ) {
        encoder->logCb(logError, _FMT("Failed to allocate AVFrame object"));
        goto Error;
    }
    encoder->encFrame->width = codecContext->width;
    encoder->encFrame->height = codecContext->height;
    encoder->encFrame->format = codecContext->pix_fmt;

    res = avcodec_open2(codecContext, codec, &dict);
    av_dict_free(&dict);

    if ( res < 0 ) {
        encoder->logCb(logError, _FMT("Failed to open H264 encoder:" << av_err2str(res)));
        goto Error;
    }

    if ( codecContext->extradata_size != 0 ) {
        videolibapi_extradata_to_spspps( codecContext->extradata,
                                        codecContext->extradata_size,
                                        &encoder->sps, &encoder->spsSize,
                                        &encoder->pps, &encoder->ppsSize );
    }

    res = 0;
Error:
    if ( res != 0 ) {
        avcodec_free_context(&codecContext);
    } else {
        avcodec_free_context(&encoder->codecContext);
        encoder->codecContext = codecContext;
    }
    return res;
}

//-----------------------------------------------------------------------------
static int        _ffsink_configure_h264_encoder   (ffenc_stream_obj* encoder,
                                                    AVCodecContext* codecContext,
                                                    AVDictionary*& dict )
{
    int             bitrate = 0;
    if ( encoder->hls ) {
        const char* sProfile;
        char        sLevel[16];

        switch (encoder->h264profile) {
        case h264Baseline:    sProfile = "baseline"; break;
        case h264Main:        sProfile = "main"; break;
        case h264High:        sProfile = "high"; break;
        case h264Extended:    sProfile = "extended"; break;
        default:              sProfile = "baseline"; break;
        };

        if ( (encoder->h264level < 30 || encoder->h264level > 32) &&
             (encoder->h264level < 40 || encoder->h264level > 42) ) {
            // TODO: Should determine the bitrate
            encoder->h264level = 31;
        }
        sprintf(sLevel, "%d.%d", encoder->h264level/10, encoder->h264level%10 );

        bitrate = encoder->max_bitrate > 0 ? encoder->max_bitrate : 256000;
        codecContext->gop_size = 10;
        codecContext->keyint_min = 1;
        av_dict_set(&dict, "preset",  "ultrafast", 0);
        av_dict_set(&dict, "profile", sProfile, 0);
        av_dict_set(&dict, "level", sLevel, 0);
        av_dict_set(&dict, "forced-idr", "1", 0);
    } else {
        size_t size = sizeof (int);
        if ( default_get_param((stream_obj*)encoder, "bitrate", &bitrate, &size) < 0 ) {
            bitrate = 0;
        }

        if ( encoder->bitrate_multiplier > 0 ) {
            bitrate = codecContext->width * codecContext->height * encoder->bitrate_multiplier;
        }

        if ( encoder->max_bitrate > 0 ) {
            bitrate = encoder->max_bitrate;
        }
        codecContext->gop_size = encoder->gop_size ? encoder->gop_size : 42;
        codecContext->keyint_min = encoder->keyint_min ? encoder->keyint_min : 10;

        if ( encoder->preset ) {
            av_dict_set(&dict, "preset", encoder->preset, 0);
        }
    }

    int vqp = encoder->videoQualityPreset;
    if ( vqp != svvpNotSpecified ) {
        int crf;
        if ( vqp <= svvpHighest )
            crf = 18;
        else if ( vqp <= svvpVeryHigh )
            crf = 23;
        else if ( vqp <= svvpHigh )
            crf = 32;
        else if ( vqp <= svvpMedium )
            crf = 37;
        else
            crf = 42;
        av_opt_set(codecContext->priv_data, "crf", _STR(crf), AV_OPT_SEARCH_CHILDREN);
    } else if ( bitrate != 0 ) {
        codecContext->bit_rate = bitrate;
        codecContext->bit_rate_tolerance = bitrate/2;
        codecContext->rc_max_rate = bitrate + codecContext->bit_rate_tolerance;
        codecContext->rc_min_rate = bitrate - codecContext->bit_rate_tolerance;
        codecContext->rc_buffer_size = bitrate;
    }

    return 0;
}

//-----------------------------------------------------------------------------
static int       _ffenc_prepare_audio_encoder (ffenc_stream_obj* encoder)
{
    int res = -1;

    size_t size;
    AVCodec* codec = NULL;

    if ( encoder->srcCodecId != streamLinear ) {
        encoder->logCb(logError, _FMT("Can't proceed with recording: this filter will encode raw frames"));
        return -1;
    }

    switch (encoder->dstCodecId) {
    case streamAAC:
        codec = avcodec_find_encoder(AV_CODEC_ID_AAC);
        break;
    case streamPCMU:
        codec = avcodec_find_encoder(AV_CODEC_ID_PCM_MULAW);
        break;
    case streamPCMA:
        codec = avcodec_find_encoder(AV_CODEC_ID_PCM_ALAW);
        break;
    }

    if (!codec) {
        encoder->logCb(logError, _FMT("Failed to find codec for the stream: codecId=" << encoder->srcCodecId));
        goto Error;
    }

    encoder->codecContext = avcodec_alloc_context3(codec);
    if (!encoder->codecContext) {
        encoder->logCb(logError, _FMT("Failed to alocate codec context"));
        goto Error;
    }

    size = sizeof(int);
    if (default_get_param((stream_obj*)encoder,
                            "audioChannels",
                            &encoder->codecContext->channels,
                            &size) < 0 ) {
        encoder->logCb(logError, _FMT("Failed to determine channel count."));
        goto Error;
    }

    size = sizeof(int);
    if (default_get_param((stream_obj*)encoder,
                            "audioSampleRate",
                            &encoder->codecContext->sample_rate,
                            &size) < 0 ) {
        encoder->logCb(logError, _FMT("Failed to determine sample rate."));
        goto Error;
    }

    // we prefer linear16, but it's not guaranteed
    encoder->codecContext->sample_fmt = AV_SAMPLE_FMT_NONE;

    res = 0;
Error:
    return res;
}

//-----------------------------------------------------------------------------
static int         ffenc_stream_open_in                (stream_obj* stream)
{
    DECLARE_STREAM_FF(stream, encoder);
    int             res = -1;
    size_t          size;
    bool            isVideo = (encoder->mediaType == mediaVideo);
    const char*     sCodecIdParam = (isVideo ? "videoCodecId" : "audioCodecId");

    if (encoder->dstCodecId == streamUnknown) {
        encoder->logCb(logError, _FMT("Need to set 'dstCodecId' parameter before encoder object can be used"));
        return -1;
    }
    if (encoder->mediaType == mediaUnknown) {
        encoder->logCb(logError, _FMT("Need to set 'encoderType' parameter before encoder object can be used"));
        return -1;
    }


    res = default_open_in(stream);
    if (res < 0) {
        goto Error;
    }

    size = sizeof(encoder->srcCodecId);
    if ( default_get_param(stream, sCodecIdParam, &encoder->srcCodecId, &size) < 0 ||
            encoder->srcCodecId == streamUnknown ) {
        // Only log debug message here ... video encoder will fail to initialize later on; and this scenario is normal for audio
        encoder->logCb(logDebug, _FMT("Failed to determine source " << sCodecIdParam << ". encoder operates as passthrough."));
        encoder->passthrough = 1;
        encoder->encoderDelay = 0;
        return 0;
    }

    if ( encoder->srcCodecId == encoder->dstCodecId ) {
        encoder->logCb(logDebug,_FMT("Encoder will operate in passthrough mode."));
        encoder->passthrough = 1;
        encoder->encoderDelay = 0;
        return 0;
    }

    encoder->passthrough = 0;
    res =  isVideo ? _ffenc_prepare_video_encoder(encoder)
                   : _ffenc_prepare_audio_encoder(encoder);
    if ( res < 0 ) {
        goto Error;
    }


    encoder->logCb(logTrace, _FMT("Opened encoder object " << (void*)stream));
    res = 0;

Error:
    if ( res != 0 ) {
        ffenc_stream_close(stream);
    }
    return res;
}


//-----------------------------------------------------------------------------
extern "C" frame_api_t*    get_ffpacket_frame_api();

//-----------------------------------------------------------------------------
static int        _ffenc_receive_frame              (ffenc_stream_obj* encoder,
                                                    frame_obj** frame)
{
    AVPacket*       packet = NULL;
    frame_api_t*    fapi = get_ffpacket_frame_api();
    int             ret;

    *frame = NULL;

    if (encoder->nextFrame == NULL) {
        encoder->nextFrame = fapi->create();
        frame_ref(encoder->nextFrame);
    }

    packet = (AVPacket*)fapi->get_backing_obj(encoder->nextFrame, "avpacket");
    av_init_packet(packet);
    packet->data = NULL;
    packet->size = 0;

    ret = avcodec_receive_packet(encoder->codecContext, packet);
    if (ret == 0) {
        fapi->set_media_type(encoder->nextFrame, encoder->mediaType);
        *frame = encoder->nextFrame;
        encoder->nextFrame = NULL;
        return 0;
    } else
    if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF) {
        *frame = NULL;
        return 0;
    }

    encoder->logCb(logError, _FMT("Failed to encode a packet: " << av_err2str(ret)));
    return ret;
}

//-----------------------------------------------------------------------------
static int        _ffenc_encode_frame              (ffenc_stream_obj* encoder,
                                                    frame_api_t* srcApi,
                                                    frame_obj* srcFrame)
{
    AVFrame*        src = NULL;

    if ( srcFrame ) {
        if ( encoder->mediaType == mediaVideo ) {
            av_image_fill_arrays(encoder->encFrame->data,
                            encoder->encFrame->linesize,
                            (const uint8_t*)srcApi->get_data(srcFrame),
                            encoder->codecContext->pix_fmt,
                            encoder->codecContext->width,
                            encoder->codecContext->height,
                            _kDefAlign);
            if ( encoder->hls && encoder->hlsHibernating ) {
                encoder->encFrame->pict_type = AV_PICTURE_TYPE_I;
                encoder->encFrame->key_frame = 1;
            } else {
                encoder->encFrame->pict_type = AV_PICTURE_TYPE_NONE;
                encoder->encFrame->key_frame = 0;
            }
        } else {
            avcodec_fill_audio_frame(encoder->encFrame,
                                    encoder->codecContext->channels,
                                    encoder->codecContext->sample_fmt,
                                    (const uint8_t*)srcApi->get_data(srcFrame),
                                    srcApi->get_data_size(srcFrame),
                                    _kDefAlign);
        }
        src = encoder->encFrame;
        encoder->lastInputPts = encoder->encFrame->pts = srcApi->get_pts(srcFrame);
    }

    return avcodec_send_frame(encoder->codecContext, src);
}

//-----------------------------------------------------------------------------
static int         ffenc_stream_read_frame        (stream_obj* stream, frame_obj** frame)
{
    DECLARE_STREAM_FF(stream, encoder);
    frame_obj*      f = NULL;
    frame_api_t*    fapiSrc = NULL;
    int             res;
    int             delay;

    // see if there is a packet from the previous frame
    if ( !encoder->passthrough &&
         _ffenc_receive_frame(encoder, frame) >=0 &&
         *frame != NULL ) {
        return 0;
    }

Retry:

    res = default_read_frame(stream, &f);
    if ( encoder->passthrough ||
         res < 0 ||
         f == NULL ||
         (fapiSrc = frame_get_api(f)) == NULL ||
         fapiSrc->get_media_type(f) != encoder->mediaType ) {
        *frame = f;
        return res;
    }

    res = _ffenc_encode_frame(encoder, fapiSrc, f);
    frame_unref(&f);

    if ( res >= 0 ) {
        res = _ffenc_receive_frame(encoder, frame);
    } else {
        encoder->logCb(logError, _FMT("Failed to encode a packet: " << av_err2str(res)));
        res = -1;
        goto Error;
    }

    if ( *frame == NULL ) {
        TRACE(_FMT("No packet received from encoder for frame with pts=" <<
                    encoder->encFrame->pts ));
        goto Retry;
    }

    res = 0;

    delay = encoder->lastInputPts - frame_get_api(*frame)->get_pts(*frame);
    if ( delay > encoder->encoderDelay ) {
        encoder->logCb(logDebug, _FMT("Encoder delay is " << delay));
        encoder->encoderDelay = delay;
    }
Error:
    return res;
 }

//-----------------------------------------------------------------------------
static int         ffenc_stream_close             (stream_obj* stream)
{
    DECLARE_STREAM_FF(stream, encoder);
    avcodec_free_context(&encoder->codecContext);
    av_frame_free( &encoder->encFrame );
    return 0;
}

//-----------------------------------------------------------------------------
static void ffenc_stream_destroy         (stream_obj* stream)
{
    DECLARE_STREAM_FF_V(stream, encoder);
    encoder->logCb(logTrace, _FMT("Destroying stream object " << (void*)stream));
    ffenc_stream_close(stream); // make sure all the internals had been freed
    frame_unref(&encoder->nextFrame);
    destroy_frame_allocator( &encoder->fa, encoder->logCb );
    sv_freep(&encoder->preset);
    sv_freep(&encoder->sps);
    sv_freep(&encoder->pps);
    stream_destroy( stream );
}

//-----------------------------------------------------------------------------
SVVIDEOLIB_API stream_api_t*     get_ffenc_stream_api             ()
{
    ffmpeg_init();
    return &_g_ffenc_stream_provider;
}

