/*****************************************************************************
 *
 * stream_ipp_colorconv.cpp
 *   Node implementing color conversion with IPP primitives.
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
#define SV_MODULE_VAR ccfilt
#define SV_MODULE_ID "RESIZE"
#include "sv_module_def.hpp"

#include "streamprv.h"
#include "sv_ffmpeg.h"


#include "frame_basic.h"

#include "videolibUtils.h"

#include "stream_resize_base.hpp"

#define IPPCC_FILTER_MAGIC 0x1302

#include <ippcore.h>
#include <ippvm.h>
#include <ipps.h>


#include <ippcc.h>


typedef IppStatus (*ConvType_3Plane)(const Ipp8u *pSrc[3], int srcStep[3], Ipp8u *pDst, int dstStep, IppiSize roiSize);
typedef IppStatus (*ConvType_2Plane)(const Ipp8u* pSrcY, int srcYStep, const Ipp8u* pSrcCbCr, int srcCbCrStep, Ipp8u* pDst, int dstStep, IppiSize roiSize);

//-----------------------------------------------------------------------------
typedef struct ipp_cc_filter  : public resize_base_obj  {
    ConvType_3Plane     conv3;
    ConvType_2Plane     conv2;
    int                 swapChannels;
    frame_allocator*    fa;
} ipp_cc_filter_obj;

//-----------------------------------------------------------------------------
// Stream API
//-----------------------------------------------------------------------------

//-----------------------------------------------------------------------------
//  Forward declarations
//-----------------------------------------------------------------------------
static stream_obj* ipp_cc_filter_create             (const char* name);
static int         ipp_cc_filter_set_param          (stream_obj* stream,
                                                const CHAR_T* name,
                                                const void* value);
static int         ipp_cc_filter_get_param          (stream_obj* stream,
                                                const CHAR_T* name,
                                                void* value,
                                                size_t* size);
static int         ipp_cc_filter_open_in            (stream_obj* stream);
static size_t      ipp_cc_filter_get_width          (stream_obj* stream);
static size_t      ipp_cc_filter_get_height         (stream_obj* stream);
static int         ipp_cc_filter_get_pixel_format   (stream_obj* stream);
static int         ipp_cc_filter_read_frame         (stream_obj* stream, frame_obj** frame);
static int         ipp_cc_filter_close              (stream_obj* stream);
static void        ipp_cc_filter_destroy            (stream_obj* stream);

//-----------------------------------------------------------------------------
stream_api_t _g_ipp_cc_filter_provider = {
    ipp_cc_filter_create,
    get_default_stream_api()->set_source,
    get_default_stream_api()->set_log_cb,
    get_default_stream_api()->get_name,
    get_default_stream_api()->find_element,
    get_default_stream_api()->remove_element,
    get_default_stream_api()->insert_element,
    ipp_cc_filter_set_param,
    ipp_cc_filter_get_param,
    ipp_cc_filter_open_in,
    get_default_stream_api()->seek,
    ipp_cc_filter_get_width,
    ipp_cc_filter_get_height,
    ipp_cc_filter_get_pixel_format,
    ipp_cc_filter_read_frame,
    get_default_stream_api()->print_pipeline,
    ipp_cc_filter_close,
    _set_module_trace_level
};

//-----------------------------------------------------------------------------
#define DECLARE_CC_FILTER(stream, name) \
    DECLARE_OBJ(ipp_cc_filter_obj, name,  stream, IPPCC_FILTER_MAGIC, -1)

#define DECLARE_CC_FILTER_V(stream, name) \
    DECLARE_OBJ_V(ipp_cc_filter_obj, name,  stream, IPPCC_FILTER_MAGIC)

static stream_obj*   ipp_cc_filter_create                (const char* name)
{
    ipp_cc_filter_obj* res = (ipp_cc_filter_obj*)stream_init(sizeof(ipp_cc_filter_obj),
                IPPCC_FILTER_MAGIC,
                &_g_ipp_cc_filter_provider,
                name,
                ipp_cc_filter_destroy );

    resize_base_init(res);
    res->conv3 = NULL;
    res->conv2 = NULL;
    res->swapChannels = 0;
    res->fa = create_frame_allocator(_STR("ippcc_"<<name));
    return (stream_obj*)res;
}

//-----------------------------------------------------------------------------
static int         ipp_cc_filter_set_param             (stream_obj* stream,
                                            const CHAR_T* name,
                                            const void* value)
{
    DECLARE_CC_FILTER(stream, ccfilt);
    if (resize_base_set_param(ccfilt, name, value) >= 0 ) {
        return 0;
    }
    return default_set_param(stream, name, value);
}

//-----------------------------------------------------------------------------
static int         ipp_cc_filter_get_param          (stream_obj* stream,
                                                const CHAR_T* name,
                                                void* value,
                                                size_t* size)
{
    DECLARE_CC_FILTER(stream, ccfilt);
    if (resize_base_get_param(ccfilt, name, value, size) >= 0 ) {
        return 0;
    }
    return default_get_param(stream, name, value, size);
}

//-----------------------------------------------------------------------------
static int         ipp_cc_filter_open_in                (stream_obj* stream)
{
    DECLARE_CC_FILTER(stream, ccfilt);

    // This filter is for color conversion only; catch pipeline builder errors here
    if ( ccfilt->dimSetting.width != ccfilt->inputWidth ||
         ccfilt->dimSetting.height != ccfilt->inputHeight ) {
        ccfilt->logCb(logError, _FMT("IPP CC filter cannot change size " <<
                        ccfilt->inputWidth << "x" << ccfilt->inputHeight << " -> " <<
                        ccfilt->dimSetting.width << "x" << ccfilt->dimSetting.height));
        return -1;
    }

    if ( resize_base_open_in(ccfilt) < 0 ) {
        return -1;
    }

    int res = -1;

    if ( ccfilt->pixfmt == pfmtRGB24 || ccfilt->pixfmt == pfmtBGR24 ) {
        switch (ccfilt->inputPixFmt) {
        case pfmtYUVJ420P:
        case pfmtYUV420P: ccfilt->conv3 = &ippiYUV420ToRGB_8u_P3C3R; break;
        case pfmtYUVJ422P:
        case pfmtYUV422P: ccfilt->conv3 = &ippiYUV422ToRGB_8u_P3C3R; break;
        case pfmtYUYV422: ccfilt->conv3 = &ippiYCbCr422ToRGB_8u_P3C3R; break;
        case pfmtNV12:    ccfilt->conv2 = &ippiYCbCr420ToRGB_8u_P2C3R; break;
        default:          ccfilt->logCb(logError, _FMT("Unsupported source pixfmt " << ccfilt->inputPixFmt));
                          goto Exit;
        }
        if ( ccfilt->pixfmt == pfmtBGR24 ) {
            ccfilt->swapChannels = 1;
        }
        res = 0;
    } else {
        ccfilt->logCb(logError, _FMT("Unsupported dest pixfmt " << ccfilt->pixfmt));
    }

Exit:
    if (res < 0) {
        ipp_cc_filter_close(stream);
    }
    return res;
}

//-----------------------------------------------------------------------------
static size_t      ipp_cc_filter_get_width          (stream_obj* stream)
{
    DECLARE_CC_FILTER(stream, ccfilt);
    return resize_base_get_width(ccfilt);
}

//-----------------------------------------------------------------------------
static size_t      ipp_cc_filter_get_height         (stream_obj* stream)
{
    DECLARE_CC_FILTER(stream, ccfilt);
    return resize_base_get_height(ccfilt);
}

//-----------------------------------------------------------------------------
static int      ipp_cc_filter_get_pixel_format   (stream_obj* stream)
{
    DECLARE_CC_FILTER(stream, ccfilt);
    return resize_base_get_pixel_format(ccfilt);
}

//-----------------------------------------------------------------------------
static int         ipp_cc_filter_read_frame        (stream_obj* stream,
                                                    frame_obj** frame)
{
    DECLARE_CC_FILTER(stream, ccfilt);
    int res = -1;

    *frame = NULL;

    frame_obj* tmp = resize_base_pre_process(ccfilt, frame, &res);
    if ( tmp == NULL ) {
        return res;
    }

    frame_api_t*    tmpFrameAPI = frame_get_api(tmp);
    int             dataSize = ccfilt->dimActual.width*ccfilt->dimActual.height*3;
    basic_frame_obj* newFrame = alloc_basic_frame2(IPPCC_FILTER_MAGIC,
                                dataSize,
                                ccfilt->logCb,
                                ccfilt->fa );
    newFrame->pts = tmpFrameAPI->get_pts(tmp);
    newFrame->dts = tmpFrameAPI->get_dts(tmp);
    newFrame->keyframe = 1;
    newFrame->width = ccfilt->dimActual.width;
    newFrame->height = ccfilt->dimActual.height;
    newFrame->pixelFormat = ccfilt->pixfmt;
    newFrame->mediaType = mediaVideo;
    newFrame->dataSize = dataSize;

    AVFrame* srcFrame = (AVFrame*)tmpFrameAPI->get_backing_obj(tmp, "avframe");
    int* srcLinesize = NULL;
    const uint8_t * planes[] = { NULL, NULL, NULL };
    const uint8_t ** srcData = NULL;
    if ( srcFrame != NULL ) {
        srcLinesize = srcFrame->linesize;
        planes[0] = (const uint8_t *)srcFrame->data[0];
        planes[1] = (const uint8_t *)srcFrame->data[1];
        planes[2] = (const uint8_t *)srcFrame->data[2];
        srcData = (const uint8_t **)srcFrame->data;
    } else {
        // TODO: at this point, we don't know how to come up with linesize and plane pointers for various formats
        //       should we ever receive anything other than ffmpeg frames, this will have to be revisited
        ccfilt->logCb(logError, _FMT("Can't process color conversion input, filter=" << default_get_name(ccfilt->source) <<
                                    " parent=" << default_get_name(stream) << " conversion " << ccfilt->inputPixFmt << "->" << ccfilt->pixfmt));
        frame_unref(&tmp);
        return -1;
    }


    int dstStep = ccfilt->dimActual.width * 3; // TODO: do we want/need to keep it aligned?
    IppiSize roiSize = {(int)newFrame->width, (int)newFrame->height};
    IppStatus status;
    if ( ccfilt->conv2 ) {
        status = ccfilt->conv2(planes[0], srcLinesize[0], planes[1], srcLinesize[1], newFrame->data, dstStep, roiSize);
    } else if ( ccfilt->conv3 ) {
        status = ccfilt->conv3(srcData, srcLinesize, newFrame->data, dstStep, roiSize);
    } else {
        status = ippStsNullPtrErr;
    }

    frame_unref(&tmp);
    if (status < 0) {
        ccfilt->logCb(logError, _FMT("Failed to convert image color format: " << ippGetStatusString(status)));
        frame_unref((frame_obj**)&newFrame);
        return -1;
    }

    *frame = (frame_obj*)newFrame;
    return 0;
}

//-----------------------------------------------------------------------------
static int         ipp_cc_filter_close             (stream_obj* stream)
{
    DECLARE_CC_FILTER(stream, ccfilt);
    ccfilt->conv2 = NULL;
    ccfilt->conv3 = NULL;
    return 0;
}

//-----------------------------------------------------------------------------
static void ipp_cc_filter_destroy         (stream_obj* stream)
{
    DECLARE_CC_FILTER_V(stream, ccfilt);
    ccfilt->logCb(logTrace, _FMT("Destroying stream object " << (void*)stream));
    ipp_cc_filter_close(stream); // make sure all the internals had been freed
    destroy_frame_allocator(&ccfilt->fa, ccfilt->logCb);
    stream_destroy( stream );
}

//-----------------------------------------------------------------------------
SVVIDEOLIB_API stream_api_t*     get_ipp_cc_filter_api                    ()
{
    return &_g_ipp_cc_filter_provider;
}



