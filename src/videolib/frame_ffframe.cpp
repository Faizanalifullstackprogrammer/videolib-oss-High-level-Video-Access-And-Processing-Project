/*****************************************************************************
 *
 * frame_ffframe.cpp
 *   Wrapper around ffmpeg frame object.
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

#include "sv_ffmpeg.h"
#include "streamprv.h"

#include "sv_pixfmt.h"
#include "frame_allocator.h"
#include "videolibUtils.h"

#define FRAME_AVFRAME_MAGIC 0x9001



typedef struct ffmpeg_frame : public frame_pooled  {
    int                 mediaType;
    void*               userContext;
    AVFrame*            avframe;
    uint8_t*            frameBuffer;
    int                 bufferSize;
    int                 bufferAlloc;
    int                 sampleSize;
    int                 needRecalc; // size needs to be recalculated
    int                 needRefill; // need to re-prepare frame again
    int                 needReexport; // need to re-export data into our buffer
    frame_obj*          srcFrame; // source frame from which this frame was derived
    sv_mutex*           mutex;
    fn_stream_log       logCb;
} ffmpeg_frame_obj;

//# define LOG_ERROR(x, msg)  x->logCb(logError, msg)
#define LOG_ERROR(x,msg)


//-----------------------------------------------------------------------------
//  Forward declarations
//-----------------------------------------------------------------------------

static frame_obj*  ff_frame_create              ();
static size_t      ff_frame_get_size            (frame_obj* frame);
static INT64_T     ff_frame_get_pts             (frame_obj* frame);
static INT64_T     ff_frame_get_dts             (frame_obj* frame);
static int         ff_frame_set_pts             (frame_obj* frame, INT64_T pts);
static int         ff_frame_set_dts             (frame_obj* frame, INT64_T dts);
static size_t      ff_frame_get_width           (frame_obj* frame);
static int         ff_frame_set_width           (frame_obj* frame, size_t size);
static size_t      ff_frame_get_height          (frame_obj* frame);
static int         ff_frame_set_height          (frame_obj* frame, size_t height);
static int         ff_frame_get_pixel_format    (frame_obj* frame);
static int         ff_frame_set_pixel_format    (frame_obj* frame, int pixfmt);
static int         ff_frame_get_media_type      (frame_obj* frame);
static int         ff_frame_set_media_type      (frame_obj* frame, int type);
static const void* ff_frame_get_data            (frame_obj* frame);
static void*       ff_frame_get_buffer          (frame_obj* frame, size_t size);
static int         ff_frame_get_keyframe_flag   (frame_obj* frame);
static int         ff_frame_set_keyframe_flag   (frame_obj* frame, int flag);
static void*       ff_frame_get_user_context    (frame_obj* frame);
static int         ff_frame_set_user_context    (frame_obj* frame, void* ctx);
static void*       ff_frame_get_backing_obj     (frame_obj* frame, const char* objType);
static int         ff_frame_set_backing_obj     (frame_obj* frame, const char* objType, void* obj);
static void        ff_frame_destroy             (frame_obj* frame);


//-----------------------------------------------------------------------------
// Frame API. To begin with, we only use it to access the data;
// however can be extended to access/provide metadata about the frame
//-----------------------------------------------------------------------------
#define DECLARE_FRAME(param, name, errval) \
    DECLARE_OBJ(ffmpeg_frame_obj, name,  param, FRAME_AVFRAME_MAGIC, errval)
#define DECLARE_FRAME_V(param, name) \
    DECLARE_OBJ_V(ffmpeg_frame_obj, name,  param, FRAME_AVFRAME_MAGIC)

//-----------------------------------------------------------------------------
//  API
//-----------------------------------------------------------------------------

frame_api_t _g_ff_frame_frame_provider = {
    ff_frame_create,
    ff_frame_get_size,
    ff_frame_get_pts,
    ff_frame_set_pts,
    ff_frame_get_dts,
    ff_frame_set_dts,
    ff_frame_get_width,
    ff_frame_set_width,
    ff_frame_get_height,
    ff_frame_set_height,
    ff_frame_get_pixel_format,
    ff_frame_set_pixel_format,
    ff_frame_get_media_type,
    ff_frame_set_media_type,
    ff_frame_get_data,
    ff_frame_get_buffer,
    ff_frame_get_keyframe_flag,
    ff_frame_set_keyframe_flag,
    ff_frame_get_user_context,
    ff_frame_set_user_context,
    ff_frame_get_backing_obj,
    ff_frame_set_backing_obj,
    NULL,
    NULL,
};


//-----------------------------------------------------------------------------
static frame_obj*   ff_frame_create              ()
{
    ffmpeg_frame_obj* res = (ffmpeg_frame_obj*)frame_init(sizeof(ffmpeg_frame_obj),
                    FRAME_AVFRAME_MAGIC,
                    &_g_ff_frame_frame_provider,
                    ff_frame_destroy );
    res->mediaType = mediaUnknown;
    res->userContext = NULL;
    res->avframe = av_frame_alloc();
    res->frameBuffer = NULL;
    res->bufferSize = 0;
    res->bufferAlloc = 0;
    res->needRecalc = 1;
    res->needRefill = 1;
    res->needReexport = 1;
    res->logCb = NULL;
    res->srcFrame = NULL;
    res->mutex = sv_mutex_create();
    return (frame_obj*)res;
}

//-----------------------------------------------------------------------------
static void        _ff_frame_get_size         (ffmpeg_frame_obj* ff_frame,
                                                int* alloc,
                                                int* actual)
{
    if ( ff_frame->mediaType == mediaVideo ) {
        *actual = av_image_get_buffer_size((enum AVPixelFormat)ff_frame->avframe->format,
                               ff_frame->avframe->width,
                               ff_frame->avframe->height,
                               _kDefAlign);
        *alloc = av_image_get_buffer_size((enum AVPixelFormat)ff_frame->avframe->format,
                               ff_frame->avframe->width,
                               ff_frame->avframe->height+1,
                               _kDefAlign); //see scaleBug
    } else
    if ( ff_frame->mediaType == mediaAudio ) {
        *alloc = *actual =
                av_samples_get_buffer_size(NULL,
                                ff_frame->avframe->channels,
                                ff_frame->avframe->nb_samples,
                                (enum AVSampleFormat)ff_frame->avframe->format,
                                _kDefAlign);
    } else {
        assert( false );
    }
}

//-----------------------------------------------------------------------------
static size_t      ff_frame_get_size           (frame_obj* frame)
{
    DECLARE_FRAME(frame, ff_frame, -1);

    if ( ff_frame->bufferSize <= 0 || ff_frame->needRecalc ) {
        _ff_frame_get_size(ff_frame, &ff_frame->bufferAlloc, &ff_frame->bufferSize);
        ff_frame->needRecalc = 0;
    }

    return ff_frame->bufferSize;
}

//-----------------------------------------------------------------------------
static INT64_T     ff_frame_get_pts          (frame_obj* frame)
{
    DECLARE_FRAME(frame, ff_frame, -1);
    INT64_T res = FF_FRAME_PTS(ff_frame->avframe);
    if ( res == AV_NOPTS_VALUE )
        res = ff_frame->avframe->pts;
    return res;
}

//-----------------------------------------------------------------------------
static INT64_T     ff_frame_get_dts          (frame_obj* frame)
{
    DECLARE_FRAME(frame, ff_frame, -1);
    return ff_frame->avframe->pkt_dts;
}

//-----------------------------------------------------------------------------
static int         ff_frame_set_pts          (frame_obj* frame, INT64_T pts)
{
    DECLARE_FRAME(frame, ff_frame, -1);
#if LIBAVUTIL_VERSION_MAJOR < 56 // earlier than n4.0
    assert ( ff_frame->avframe->pkt_pts == AV_NOPTS_VALUE );
    ff_frame->avframe->pkt_pts = pts;
#endif
    assert ( ff_frame->avframe->pts == AV_NOPTS_VALUE );
    ff_frame->avframe->pts = pts;
    return 0;
}

//-----------------------------------------------------------------------------
static int         ff_frame_set_dts          (frame_obj* frame, INT64_T dts)
{
    DECLARE_FRAME(frame, ff_frame, -1);
    assert ( ff_frame->avframe->pkt_dts == AV_NOPTS_VALUE );
    ff_frame->avframe->pkt_dts = dts;
    return 0;
}

//-----------------------------------------------------------------------------
static size_t      ff_frame_get_width           (frame_obj* frame)
{
    DECLARE_FRAME(frame, ff_frame, -1);
    return ff_frame->avframe->width;
}

//-----------------------------------------------------------------------------
static int         ff_frame_set_width           (frame_obj* frame, size_t size)
{
    DECLARE_FRAME(frame, ff_frame, -1);
    assert ( ff_frame->avframe->width == 0 || ff_frame->avframe->width == size );
    ff_frame->avframe->width = size;
    ff_frame->needRecalc = 1;
    return 0;
}

//-----------------------------------------------------------------------------
static size_t      ff_frame_get_height          (frame_obj* frame)
{
    DECLARE_FRAME(frame, ff_frame, -1);
    return ff_frame->avframe->height;

}

//-----------------------------------------------------------------------------
static int         ff_frame_set_height          (frame_obj* frame, size_t height)
{
    DECLARE_FRAME(frame, ff_frame, -1);
    assert ( ff_frame->avframe->height == 0 || ff_frame->avframe->height == height );
    ff_frame->avframe->height = height;
    ff_frame->needRecalc = 1;
    return 0;
}

//-----------------------------------------------------------------------------
static int         ff_frame_get_pixel_format    (frame_obj* frame)
{
    DECLARE_FRAME(frame, ff_frame, -1);
    if ( ff_frame->mediaType == mediaVideo)
        return ffpfmt_to_svpfmt((enum AVPixelFormat)ff_frame->avframe->format,
                                ff_frame->avframe->color_range);
    if ( ff_frame->mediaType == mediaAudio) {
        int sampleSize, interleaved;
        return ffsfmt_to_svsfmt((enum AVSampleFormat)ff_frame->avframe->format,
                                    &interleaved,
                                    &sampleSize);
    }
    return -1;
}

//-----------------------------------------------------------------------------
static int         ff_frame_set_pixel_format    (frame_obj* frame, int pixfmt)
{
    DECLARE_FRAME(frame, ff_frame, -1);
    ff_frame->needRecalc = 1;
    assert ( ff_frame->mediaType != mediaUnknown );
    if ( ff_frame->mediaType == mediaVideo ) {
        enum AVColorRange cr;
        int format = svpfmt_to_ffpfmt(pixfmt, &cr);
        assert ( ff_frame->avframe->format == -1 ||
                 ff_frame->avframe->format == format );
        ff_frame->avframe->format = format;
        ff_frame->avframe->color_range = cr;
        return 0;
    }
    if ( ff_frame->mediaType == mediaAudio ) {
        int format = svsfmt_to_ffsfmt(pixfmt, 1);
        assert ( ff_frame->avframe->format == -1 ||
                 ff_frame->avframe->format == format );
        ff_frame->avframe->format = format;
    }
    return -1;
}

//-----------------------------------------------------------------------------
static int         ff_frame_get_keyframe_flag   (frame_obj* frame)
{
    DECLARE_FRAME(frame, ff_frame, -1);
    return ff_frame->avframe->key_frame;
}

//-----------------------------------------------------------------------------
static int         ff_frame_set_keyframe_flag   (frame_obj* frame, int flag)
{
    DECLARE_FRAME(frame, ff_frame, -1);
    ff_frame->avframe->key_frame = flag?1:0;
    return 0;
}


//-----------------------------------------------------------------------------
static int         ff_frame_get_media_type    (frame_obj* frame)
{
    DECLARE_FRAME(frame, ff_frame, -1);
    return ff_frame->mediaType;
}

//-----------------------------------------------------------------------------
static int         ff_frame_set_media_type    (frame_obj* frame, int type)
{
    DECLARE_FRAME(frame, ff_frame, -1);
    ff_frame->mediaType = type;
    return 0;
}

//-----------------------------------------------------------------------------
static void*      _ff_alloc_buffer             (ffmpeg_frame_obj* ff_frame)
{
    int alloc, actual;
    if ( ff_frame->frameBuffer == NULL || ff_frame->needRecalc ) {
        _ff_frame_get_size(ff_frame, &alloc, &actual);
        if ( ff_frame->frameBuffer != NULL &&
             ff_frame->bufferAlloc == alloc &&
             ff_frame->bufferSize == actual ) {
            return ff_frame->frameBuffer;
        }

        av_freep(&ff_frame->frameBuffer);
        ff_frame->frameBuffer = (unsigned char*)av_malloc(alloc);

        if ( !ff_frame->frameBuffer ) {
            ff_frame->bufferSize = 0;
            ff_frame->bufferAlloc = 0;
            if ( ff_frame->logCb != NULL ) {
                ff_frame->logCb( logError, _FMT("Failed to allocate frame buffer of " << alloc));
            }
        } else {
            ff_frame->bufferAlloc = alloc;
            ff_frame->bufferSize = actual;
            ff_frame->needRecalc = 0;
            ff_frame->needRefill = 1;
            ff_frame->needReexport = 1;
            // if ( ff_frame->logCb != NULL ) {
            //     ff_frame->logCb( logTrace, _FMT("Allocated frame buffer of " << alloc));
            // }
        }

    }
    return ff_frame->frameBuffer;
}

//-----------------------------------------------------------------------------
#define FORCE_INTERLEAVED_AUDIO 1
static const void* ff_frame_get_data           (frame_obj* frame)
{
    DECLARE_FRAME(frame, ff_frame, NULL);

    sv_mutex_enter(ff_frame->mutex);

    _ff_alloc_buffer(ff_frame);

    if ( ff_frame->needReexport ) {
        int res = 0;
        if ( ff_frame->mediaType == mediaVideo ) {
            res = av_image_copy_to_buffer ( (unsigned char*)ff_frame->frameBuffer,
                                        ff_frame->bufferSize,
                                        ff_frame->avframe->data,
                                        ff_frame->avframe->linesize,
                                        (enum AVPixelFormat)ff_frame->avframe->format,
                                        ff_frame->avframe->width,
                                        ff_frame->avframe->height,
                                        _kDefAlign);
        } else
        if ( ff_frame->mediaType == mediaAudio ) {
            _ff_frame_get_size(ff_frame, &ff_frame->bufferSize, &ff_frame->bufferSize);

            int channels = ff_frame->avframe->channels;
            int samplesCount = ff_frame->avframe->nb_samples;
            int sampleSize, interleaved;
            ffsfmt_to_svsfmt( (enum AVSampleFormat)ff_frame->avframe->format, &interleaved, &sampleSize );
            assert ( ff_frame->bufferSize/(channels*samplesCount) == sampleSize );

            if (interleaved) {
                memcpy(ff_frame->frameBuffer, ff_frame->avframe->data[0], ff_frame->bufferSize);
            } else {
        #if FORCE_INTERLEAVED_AUDIO
                for (int sample=0; sample<samplesCount; sample++) {
                    int sampleOffset = channels*sampleSize;
                    int sampleOffsetIn = sample*sampleSize;
                    for (int chan=0; chan<channels; chan++) {
                        int channelOffset = sampleOffset + chan*sampleSize;
                        for (int byte=0; byte<sampleSize; byte++) {
                            int byteOffset = channelOffset + byte;
                            int byteOffsetIn = sampleOffsetIn + byte;
                            ff_frame->frameBuffer[byteOffset] = ff_frame->avframe->data[chan][byteOffsetIn];
                        }
                    }
                }
        #else
                for (int chan=0; chan<channels; chan++) {
                    memcpy(&ff_frame->frameBuffer->data[chan*sampleSize*samplesCount],
                            ff_frame->avframe->data[chan],
                            samplesCount);
                }
        #endif
            }
            res = ff_frame->bufferSize;
        }
        if ( res < 0 ) {
            ff_frame->bufferSize = 0;
            ff_frame->bufferAlloc = 0;
            av_freep(&ff_frame->frameBuffer);
        } else {
            ff_frame->needReexport = 0;
            assert ( ff_frame->bufferSize == res );
        }
    }

    sv_mutex_exit(ff_frame->mutex);

    return ff_frame->frameBuffer;
}

//-----------------------------------------------------------------------------
static void*       ff_frame_get_buffer          (frame_obj* frame, size_t size)
{
    DECLARE_FRAME(frame, ff_frame, NULL);

    sv_mutex_enter(ff_frame->mutex);

    assert( ff_frame->avframe->format >= 0 &&
            ff_frame->avframe->width > 0 &&
            ff_frame->avframe->height > 0 );

    _ff_alloc_buffer(ff_frame);

    if ( ff_frame->needRefill ) {
        av_image_fill_arrays(ff_frame->avframe->data,
                       ff_frame->avframe->linesize,
                       ff_frame->frameBuffer,
                       (enum AVPixelFormat)ff_frame->avframe->format,
                       ff_frame->avframe->width,
                       ff_frame->avframe->height,
                       _kDefAlign);
        ff_frame->needRefill = 0;
        // we can just return this buffer, since it's backing the frame object
        ff_frame->needReexport = 0;
    }

    sv_mutex_exit(ff_frame->mutex);

    return ff_frame->frameBuffer;
}


//-----------------------------------------------------------------------------
static void*       ff_frame_get_user_context    (frame_obj* frame)
{
    DECLARE_FRAME(frame, ff_frame, NULL);
    return ff_frame->userContext;
}

//-----------------------------------------------------------------------------
static int         ff_frame_set_user_context    (frame_obj* frame, void* ctx)
{
    DECLARE_FRAME(frame, ff_frame, -1);
    ff_frame->userContext = ctx;
    return 0;
}

//-----------------------------------------------------------------------------
static void*       ff_frame_get_backing_obj     (frame_obj* frame,
                                                const char* objType)
{
    DECLARE_FRAME(frame, ff_frame, NULL);
    if ( objType ) {
        if ( !_stricmp(objType,"avframe")) {
            ff_frame->needReexport = 1;
            return ff_frame->avframe;
        } else if ( !_stricmp(objType,"srcFrame")) {
            return ff_frame->srcFrame;
        }
    }
    return NULL;
}

//-----------------------------------------------------------------------------
static int         ff_frame_set_backing_obj     (frame_obj* frame,
                                                const char* objType,
                                                void* obj)
{
    DECLARE_FRAME(frame, ff_frame, -1);
    if ( objType ) {
        if ( !_stricmp(objType,"avframe")) {
            if ( ff_frame->mediaType != mediaAudio &&
                 ff_frame->mediaType != mediaVideo ) {
                return -1;
            }

            LOG_ERROR(ff_frame, _FMT("Set backing obj=" << obj << " f=" << ff_frame << " ; pts=" << ff_frame_get_pts(frame)));

            av_frame_unref( ff_frame->avframe );
            av_frame_free( &ff_frame->avframe );
            ff_frame->avframe = (AVFrame*)obj;

            return 0;
        } else if ( !_stricmp(objType, "log")) {
            ff_frame->logCb = (fn_stream_log)obj;
        } else if ( !_stricmp(objType, "srcFrame") ) {
            frame_unref(&ff_frame->srcFrame);
            ff_frame->srcFrame = (frame_obj*)obj;
            frame_ref(ff_frame->srcFrame);
            return 0;
        }
    }
    return -1;
}

//-----------------------------------------------------------------------------
static void  ff_frame_destroy                  (frame_obj* frame)
{
    DECLARE_FRAME_V(frame, ff_frame);

    // don't keep source frame even if pooled
    frame_unref(&ff_frame->srcFrame);

    if ( ff_frame->fa ) {
        LOG_ERROR(ff_frame, _FMT("Returning f=" << ff_frame << " to allocator; pts=" << ff_frame_get_pts(frame)));
        frame_allocator_return(ff_frame->fa, ff_frame);
    } else {
        LOG_ERROR(ff_frame, _FMT("Destroying f=" << ff_frame << " ; pts=" << ff_frame_get_pts(frame)));
        av_frame_unref( ff_frame->avframe );
        av_frame_free( &ff_frame->avframe );
        av_freep( &ff_frame->frameBuffer );
        sv_mutex_destroy(&ff_frame->mutex);
        sv_freep( &ff_frame );
    }
}

//-----------------------------------------------------------------------------
static void  ff_frame_reset                     (frame_obj* frame)
{
    DECLARE_FRAME_V(frame, ff_frame);
    LOG_ERROR(ff_frame, _FMT("Resetting f=" << ff_frame << " ; pts=" << ff_frame_get_pts(frame)));

    av_frame_unref( ff_frame->avframe );

    ff_frame->needRecalc = 1;
    ff_frame->needRefill = 1;
    ff_frame->needReexport = 1;

#if LIBAVUTIL_VERSION_MAJOR < 56 // earlier than n4.0
    ff_frame->avframe->pkt_pts = AV_NOPTS_VALUE;
#endif
    ff_frame->avframe->pkt_dts = AV_NOPTS_VALUE;
    ff_frame->avframe->pts = AV_NOPTS_VALUE;
}

//-----------------------------------------------------------------------------
SVVIDEOLIB_API frame_api_t*    get_ffframe_frame_api()
{
    return &_g_ff_frame_frame_provider;
}


//-----------------------------------------------------------------------------
static int _g_avframeFramesPoolEnabled = 1;

frame_obj*  alloc_avframe_frame            (int ownerTag,
                                             frame_allocator* fa,
                                             fn_stream_log logCb)
{
    ffmpeg_frame_obj* res = NULL;
    int              pooled = 0;
    if ( _g_avframeFramesPoolEnabled && fa ) {
        res = (ffmpeg_frame_obj*)frame_allocator_get(fa);
        pooled = 1;
    }

    if (!res) {
        res = (ffmpeg_frame_obj*)ff_frame_create();
        if (res) {
            res->ownerTag = ownerTag;
            res->next = NULL;
            res->resetter = ff_frame_reset;
            res->logCb = logCb;

            if ( pooled ) {
                frame_allocator_register_frame(fa, res);
            } else {
                res->fa = NULL;
            }
        }
    } else {
        assert( res->refcount == 0 );
    }

    assert( res );
    res->refcount = 1;
    return (frame_obj*)res;
}




