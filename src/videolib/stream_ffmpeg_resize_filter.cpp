/*****************************************************************************
 *
 * stream_ffmpeg_resize_filter.cpp
 *   Resize/color conversion node based on ffmpeg API
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
#define SV_MODULE_VAR rszfilter
#define SV_MODULE_ID "RESIZE"
#include "sv_module_def.hpp"

#include "streamprv.h"
#include "sv_ffmpeg.h"


#include "frame_basic.h"

#include "videolibUtils.h"

#include "stream_resize_base.hpp"

#define RESIZE_FILTER_MAGIC 0x1225

//-----------------------------------------------------------------------------
typedef struct resize_filter  : public resize_base_obj  {
    struct SwsContext*  ctx;
    AVFrame*            srcFrame;
    frame_allocator*    fa;
} resize_filter_obj;

//-----------------------------------------------------------------------------
// Stream API
//-----------------------------------------------------------------------------

//-----------------------------------------------------------------------------
//  Forward declarations
//-----------------------------------------------------------------------------
static stream_obj* resize_filter_create             (const char* name);
static int         resize_filter_set_param          (stream_obj* stream,
                                                const CHAR_T* name,
                                                const void* value);
static int         resize_filter_get_param          (stream_obj* stream,
                                                const CHAR_T* name,
                                                void* value,
                                                size_t* size);
static int         resize_filter_open_in            (stream_obj* stream);
static size_t      resize_filter_get_width          (stream_obj* stream);
static size_t      resize_filter_get_height         (stream_obj* stream);
static int         resize_filter_get_pixel_format   (stream_obj* stream);
static int         resize_filter_read_frame         (stream_obj* stream, frame_obj** frame);
static int         resize_filter_close              (stream_obj* stream);
static void        resize_filter_destroy            (stream_obj* stream);
static int         resize_set_color_options         (stream_obj* stream, struct SwsContext *ctx, int colorspace, int range );

extern "C" frame_api_t*     get_ffframe_frame_api   ( );
extern frame_obj*           alloc_avframe_frame     (int ownerTag, frame_allocator* fa,
                                                    fn_stream_log logCb);

//-----------------------------------------------------------------------------
stream_api_t _g_resize_filter_provider = {
    resize_filter_create,
    get_default_stream_api()->set_source,
    get_default_stream_api()->set_log_cb,
    get_default_stream_api()->get_name,
    get_default_stream_api()->find_element,
    get_default_stream_api()->remove_element,
    get_default_stream_api()->insert_element,
    resize_filter_set_param,
    resize_filter_get_param,
    resize_filter_open_in,
    get_default_stream_api()->seek,
    resize_filter_get_width,
    resize_filter_get_height,
    resize_filter_get_pixel_format,
    resize_filter_read_frame,
    get_default_stream_api()->print_pipeline,
    resize_filter_close,
    _set_module_trace_level
};


//-----------------------------------------------------------------------------
#define DECLARE_RESIZE_FILTER(stream, name) \
    DECLARE_OBJ(resize_filter_obj, name,  stream, RESIZE_FILTER_MAGIC, -1)

#define DECLARE_RESIZE_FILTER_V(stream, name) \
    DECLARE_OBJ_V(resize_filter_obj, name,  stream, RESIZE_FILTER_MAGIC)

static stream_obj*   resize_filter_create                (const char* name)
{
    resize_filter_obj* res = (resize_filter_obj*)stream_init(sizeof(resize_filter_obj),
                RESIZE_FILTER_MAGIC,
                &_g_resize_filter_provider,
                name,
                resize_filter_destroy );

    resize_base_init(res);
    res->ctx = NULL;
    res->srcFrame = NULL;
    res->fa = create_frame_allocator(name);
    return (stream_obj*)res;
}

//-----------------------------------------------------------------------------
static int         resize_filter_set_param             (stream_obj* stream,
                                            const CHAR_T* name,
                                            const void* value)
{
    DECLARE_RESIZE_FILTER(stream, rszfilter);
    if (resize_base_set_param(rszfilter, name, value) >= 0 ) {
        return 0;
    }
    return default_set_param(stream, name, value);
}

//-----------------------------------------------------------------------------
static int         resize_filter_get_param          (stream_obj* stream,
                                                const CHAR_T* name,
                                                void* value,
                                                size_t* size)
{
    DECLARE_RESIZE_FILTER(stream, rszfilter);
    if (resize_base_get_param(rszfilter, name, value, size) >= 0 ) {
        return 0;
    }
    return default_get_param(stream, name, value, size);
}

//-----------------------------------------------------------------------------
static int         resize_filter_open_in                (stream_obj* stream)
{
    DECLARE_RESIZE_FILTER(stream, rszfilter);
    int res = 0;

    // make sure we have cleaned up
    resize_filter_close(stream);

    if (resize_base_open_in(rszfilter) < 0) {
        return -1;
    }

    rszfilter->ctx = sws_getContext(rszfilter->inputWidth,
                                  rszfilter->inputHeight,
                                  svpfmt_to_ffpfmt(rszfilter->inputPixFmt,
                                                  (enum AVColorRange*)&rszfilter->colorRange),
                                  rszfilter->dimActual.width,
                                  rszfilter->dimActual.height,
                                  svpfmt_to_ffpfmt(rszfilter->pixfmt, NULL),
                                  SWS_FAST_BILINEAR,
                                  NULL,
                                  NULL,
                                  NULL);
    if (!rszfilter->ctx) {
        rszfilter->logCb(logError, _FMT("Can't allocate resize filter"));
        return -1;
    }

    if (rszfilter->colorSpace >=0 && rszfilter->colorRange >=0) {
        rszfilter->logCb(logDebug, _FMT("Resize filter '" <<
                                        rszfilter->name <<
                                        "' is setting the color options"));
        // Set the color space options
        if (resize_set_color_options(stream, rszfilter->ctx, rszfilter->colorSpace, rszfilter->colorRange ) == -1 ) {
            rszfilter->logCb(logError, _FMT("Failed to set our colorspace options!"));
            return -1;
        }
    }

    // Allocate storage for converted frames
    rszfilter->srcFrame = av_frame_alloc();
    if (!rszfilter->srcFrame) {
        rszfilter->logCb(logError, _FMT("Can't allocate resize filter's frame object"));
        return -1;
    }

    return 0;
}

//-----------------------------------------------------------------------------
static size_t      resize_filter_get_width          (stream_obj* stream)
{
    DECLARE_RESIZE_FILTER(stream, rszfilter);
    return resize_base_get_width(rszfilter);
}

//-----------------------------------------------------------------------------
static size_t      resize_filter_get_height         (stream_obj* stream)
{
    DECLARE_RESIZE_FILTER(stream, rszfilter);
    return resize_base_get_height(rszfilter);
}

//-----------------------------------------------------------------------------
static int      resize_filter_get_pixel_format   (stream_obj* stream)
{
    DECLARE_RESIZE_FILTER(stream, rszfilter);
    return resize_base_get_pixel_format(rszfilter);
}

//-----------------------------------------------------------------------------
static int         resize_filter_read_frame        (stream_obj* stream,
                                                    frame_obj** frame)
{
    DECLARE_RESIZE_FILTER(stream, rszfilter);
    int res = -1;

    frame_obj* tmp = resize_base_pre_process(rszfilter, frame, &res);
    if ( tmp == NULL ) {
        return res;
    }

    frame_api_t*    fapi = get_ffframe_frame_api();
    frame_api_t*    tmpFrameAPI = frame_get_api(tmp);
    frame_obj*      outputFrame = alloc_avframe_frame(RESIZE_FILTER_MAGIC, rszfilter->fa,
                                                    rszfilter->logCb);

    AVFrame*   dstFrame = (AVFrame*)fapi->get_backing_obj(outputFrame, "avframe");

    INT64_T pts, dts;
    pts = tmpFrameAPI->get_pts(tmp);
    dts = tmpFrameAPI->get_dts(tmp);

    fapi->set_media_type(outputFrame, mediaVideo);
    fapi->set_pts(outputFrame, pts);
    fapi->set_dts(outputFrame, dts);
    fapi->set_keyframe_flag(outputFrame, 1);
    fapi->set_width(outputFrame, rszfilter->dimActual.width);
    fapi->set_height(outputFrame, rszfilter->dimActual.height);
    fapi->set_pixel_format(outputFrame, rszfilter->pixfmt);
    // must call this -- it allocates the actual frame buffer
    fapi->get_buffer(outputFrame, -1);


    AVFrame* srcFrame = (AVFrame*)tmpFrameAPI->get_backing_obj(tmp, "avframe");
    if ( srcFrame == NULL ) {
        srcFrame = rszfilter->srcFrame;
        av_image_fill_arrays(srcFrame->data,
                       srcFrame->linesize,
                       (const uint8_t*)tmpFrameAPI->get_data(tmp),
                       svpfmt_to_ffpfmt(rszfilter->inputPixFmt, &srcFrame->color_range),
                       rszfilter->inputWidth,
                       rszfilter->inputHeight,
                       _kDefAlign );
        srcFrame->width = rszfilter->inputWidth;
        srcFrame->height = rszfilter->inputHeight;
        srcFrame->format = svpfmt_to_ffpfmt( rszfilter->inputPixFmt, &srcFrame->color_range);
    }



    res = sws_scale(rszfilter->ctx,
              (const uint8_t* const*)srcFrame,
              srcFrame->linesize,
              0,
              rszfilter->inputHeight,
              dstFrame->data,
              dstFrame->linesize);
    if ( res < 0 ) {
        rszfilter->logCb(logError, _FMT( "Failed to resize the image: " <<
                            " res=" << res << "(" << av_err2str(res) << ")" <<
                            " dstSize=" << rszfilter->dimActual.width << "x" << rszfilter->dimActual.height <<
                            " dstBuf=" << fapi->get_data_size(outputFrame) <<
                            " dstPixFmt=" << rszfilter->pixfmt <<
                            " srcSize=" << rszfilter->inputWidth << "x" <<
                            rszfilter->inputHeight <<
                            " srcBuf=" << tmpFrameAPI->get_data_size(tmp) <<
                            " srcPixFmt=" << tmpFrameAPI->get_pixel_format(tmp) ) );
        frame_unref(&outputFrame);
    } else {
        *frame = outputFrame;
        res = 0;
        TRACE(_FMT("Generated frame: pts=" << tmpFrameAPI->get_pts(tmp) <<
                " dstSize=" << rszfilter->dimActual.width << "x" << rszfilter->dimActual.height <<
                " dstBuf=" << fapi->get_data_size(outputFrame) <<
                " dstPixFmt=" << fapi->get_pixel_format(outputFrame) <<
                " srcSize=" << rszfilter->inputWidth << "x" <<
                rszfilter->inputHeight <<
                " srcBuf=" << tmpFrameAPI->get_data_size(tmp) <<
                " srcPixFmt=" << tmpFrameAPI->get_pixel_format(tmp) ) );
    }

    if ( res == 0 && outputFrame &&
         rszfilter->retainSourceFrameInterval > 0 &&
         ( rszfilter->prevFramePts == INVALID_PTS ||
         pts >= rszfilter->prevFramePts + rszfilter->retainSourceFrameInterval) ) {
        if ( fapi->set_backing_obj(*frame, "srcFrame", tmp) < 0 ) {
            rszfilter->logCb(logError, _FMT("Failed to set source frame object!"));
        }
        // only need this variable when original frames are being retained
        rszfilter->prevFramePts = pts;
    }

    frame_unref(&tmp);
    return res;
}

//-----------------------------------------------------------------------------
static int         resize_filter_close             (stream_obj* stream)
{
    DECLARE_RESIZE_FILTER(stream, rszfilter);

    av_frame_free(&rszfilter->srcFrame);
    sws_freeContext(rszfilter->ctx);
    rszfilter->ctx = NULL;

    return 0;
}

//-----------------------------------------------------------------------------
static void resize_filter_destroy         (stream_obj* stream)
{
    DECLARE_RESIZE_FILTER_V(stream, rszfilter);
    rszfilter->logCb(logTrace, _FMT("Destroying stream object " << (void*)stream));
    resize_filter_close(stream); // make sure all the internals had been freed
    destroy_frame_allocator(&rszfilter->fa, rszfilter->logCb);
    stream_destroy( stream );
}

//-----------------------------------------------------------------------------
SVVIDEOLIB_API stream_api_t*     get_resize_filter_api                    ()
{
    ffmpeg_init();
    return &_g_resize_filter_provider;
}

//-----------------------------------------------------------------------------
static int resize_set_color_options( stream_obj* stream, struct SwsContext *ctx, int colorspace, int range )
{
    DECLARE_RESIZE_FILTER(stream, rszfilter);

    int *currentColorSpaceType;
    const int *newColorSpaceType;
    int currentRange;
    int currentBrightness;
    int currentContrast;
    int currentSaturation;

    if ( sws_getColorspaceDetails( ctx, &currentColorSpaceType, &currentRange, &currentColorSpaceType, &currentRange,
            &currentBrightness, &currentContrast, &currentSaturation ) != -1 ) {
        if (colorspace  == 601) {
            newColorSpaceType = sws_getCoefficients( SWS_CS_ITU601 );
        } else if (colorspace == 709) {
            newColorSpaceType = sws_getCoefficients( SWS_CS_ITU709 );
        } else {
            newColorSpaceType = currentColorSpaceType;
        }
        if (sws_setColorspaceDetails( ctx, newColorSpaceType, range, newColorSpaceType, range,
            currentBrightness, currentContrast, currentSaturation ) == -1) {
            rszfilter->logCb(logError, _FMT("Unable to call sws_setColorspaceDetails"));
            return -1;
        };
        return 0;
    } else {
        rszfilter->logCb(logError, _FMT("Unable to get current colorspace details"));
        return -1;
    }
}


