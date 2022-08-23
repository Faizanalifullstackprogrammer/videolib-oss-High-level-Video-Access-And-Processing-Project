/*****************************************************************************
 *
 * stream_ipp_resize.cpp
 *   Node implementing resizing with IPP primitives.
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

#define IPPRESIZE_FILTER_MAGIC 0x1301

#include <ippcore.h>
#include <ippvm.h>
#include <ipps.h>

#include <ippi.h>



typedef IppStatus (*ResizeFunc)(const Ipp8u* pSrc, Ipp32s srcStep, Ipp8u* pDst, Ipp32s dstStep, IppiPoint dstOffset, IppiSize dstSize, const IppiResizeSpec_32f* pSpec, Ipp8u* pBuffer);
typedef IppStatus (*InitFunc)(IppiSize srcSize, IppiSize dstSize, IppiResizeSpec_32f* pSpec);

//-----------------------------------------------------------------------------
typedef struct ipp_resize_filter  : public resize_base_obj  {
    IppiResizeSpec_32f* pSpec;
    Ipp8u*              pWorkBuffer;
    InitFunc            initFunc;
    ResizeFunc          resizeFunc;
    frame_allocator*    fa;
} ipp_resize_filter_obj;

//-----------------------------------------------------------------------------
// Stream API
//-----------------------------------------------------------------------------

//-----------------------------------------------------------------------------
//  Forward declarations
//-----------------------------------------------------------------------------
static stream_obj* ipp_resize_filter_create             (const char* name);
static int         ipp_resize_filter_set_param          (stream_obj* stream,
                                                const CHAR_T* name,
                                                const void* value);
static int         ipp_resize_filter_get_param          (stream_obj* stream,
                                                const CHAR_T* name,
                                                void* value,
                                                size_t* size);
static int         ipp_resize_filter_open_in            (stream_obj* stream);
static size_t      ipp_resize_filter_get_width          (stream_obj* stream);
static size_t      ipp_resize_filter_get_height         (stream_obj* stream);
static int         ipp_resize_filter_get_pixel_format   (stream_obj* stream);
static int         ipp_resize_filter_read_frame         (stream_obj* stream, frame_obj** frame);
static int         ipp_resize_filter_close              (stream_obj* stream);
static void        ipp_resize_filter_destroy            (stream_obj* stream);

//-----------------------------------------------------------------------------
stream_api_t _g_ipp_resize_filter_provider = {
    ipp_resize_filter_create,
    get_default_stream_api()->set_source,
    get_default_stream_api()->set_log_cb,
    get_default_stream_api()->get_name,
    get_default_stream_api()->find_element,
    get_default_stream_api()->remove_element,
    get_default_stream_api()->insert_element,
    ipp_resize_filter_set_param,
    ipp_resize_filter_get_param,
    ipp_resize_filter_open_in,
    get_default_stream_api()->seek,
    ipp_resize_filter_get_width,
    ipp_resize_filter_get_height,
    ipp_resize_filter_get_pixel_format,
    ipp_resize_filter_read_frame,
    get_default_stream_api()->print_pipeline,
    ipp_resize_filter_close,
    _set_module_trace_level
};

//-----------------------------------------------------------------------------
static IppStatus ippLinearWrapper(const Ipp8u* pSrc, Ipp32s srcStep, Ipp8u* pDst, Ipp32s dstStep, IppiPoint dstOffset, IppiSize dstSize, const IppiResizeSpec_32f* pSpec, Ipp8u* pBuffer)
{
    return ippiResizeLinear_8u_C3R(pSrc, srcStep, pDst, dstStep, dstOffset, dstSize, ippBorderRepl, NULL, pSpec, pBuffer);
}

//-----------------------------------------------------------------------------
#define DECLARE_RESIZE_FILTER(stream, name) \
    DECLARE_OBJ(ipp_resize_filter_obj, name,  stream, IPPRESIZE_FILTER_MAGIC, -1)

#define DECLARE_RESIZE_FILTER_V(stream, name) \
    DECLARE_OBJ_V(ipp_resize_filter_obj, name,  stream, IPPRESIZE_FILTER_MAGIC)

static stream_obj*   ipp_resize_filter_create                (const char* name)
{
    ipp_resize_filter_obj* res = (ipp_resize_filter_obj*)stream_init(sizeof(ipp_resize_filter_obj),
                IPPRESIZE_FILTER_MAGIC,
                &_g_ipp_resize_filter_provider,
                name,
                ipp_resize_filter_destroy );

    resize_base_init(res);
    res->pSpec = NULL;
    res->pWorkBuffer = NULL;
    res->fa = create_frame_allocator(_STR("ippresize_"<<name));
    return (stream_obj*)res;
}

//-----------------------------------------------------------------------------
static int         ipp_resize_filter_set_param             (stream_obj* stream,
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
static int         ipp_resize_filter_get_param          (stream_obj* stream,
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
static int         ipp_resize_filter_open_in                (stream_obj* stream)
{
    DECLARE_RESIZE_FILTER(stream, rszfilter);

    if ( resize_base_open_in(rszfilter) < 0 ) {
        return -1;
    }

    if ( rszfilter->inputPixFmt != pfmtRGB24 ) {
        rszfilter->logCb(logError, _FMT("input stream pfmt=" << rszfilter->inputPixFmt << ", ipp resize filter cannot continue"));
        return -1;
    }

    // IppiInterpolationType interpolationType = ippLinear;
    // rszfilter->initFunc = ippiResizeLinearInit_8u;
    // rszfilter->resizeFunc = ippLinearWrapper;
    IppiInterpolationType interpolationType = ippNearest;
    rszfilter->initFunc = ippiResizeNearestInit_8u;
    rszfilter->resizeFunc = ippiResizeNearest_8u_C3R;

    int specSize = 0, initSize = 0, res = -1, bufSize = 0;
    Ipp32u numChannels = 3;
    IppStatus status = ippStsNoErr;
    IppiBorderType border = ippBorderRepl;
    IppiSize srcSize = { (int)rszfilter->inputWidth, (int)rszfilter->inputHeight };
    IppiSize dstSize = { (int)rszfilter->dimActual.width, (int)rszfilter->dimActual.height };

    /* Spec and init buffer sizes */
    status = ippiResizeGetSize_8u(srcSize, dstSize, interpolationType, 0, &specSize, &initSize);
    if (status < 0) {
        rszfilter->logCb(logError, _FMT("Error in ippiResizeGetSize_8u:" << ippGetStatusString(status)));
        goto Exit;
    }

    /* Memory allocation */
    rszfilter->pSpec    = (IppiResizeSpec_32f*)ippsMalloc_8u(specSize);
    if (rszfilter->pSpec == NULL) {
        rszfilter->logCb(logError, _FMT("Failed to allocate buffers: " << specSize));
        goto Exit;
    }

    /* Filter initialization */
    status = rszfilter->initFunc(srcSize, dstSize, rszfilter->pSpec);
    if (status < 0) {
        rszfilter->logCb(logError, _FMT("Error in ippiResizeLinearInit_8u:" << ippGetStatusString(status)));
        goto Exit;
    }

    /* work buffer size */
    status = ippiResizeGetBufferSize_8u(rszfilter->pSpec, dstSize, numChannels, &bufSize);
    if (status < 0) {
        rszfilter->logCb(logError, _FMT("Error in ippiResizeGetBufferSize_8u:" << ippGetStatusString(status)));
        goto Exit;
    }

    rszfilter->pWorkBuffer = ippsMalloc_8u(bufSize);
    if (rszfilter->pWorkBuffer == NULL) {
        rszfilter->logCb(logError, _FMT("Failed to allocate buffers: " << specSize));
        goto Exit;
    }

    res = 0;
Exit:
    if (res < 0) {
        ipp_resize_filter_close(stream);
    }
    return res;
}

//-----------------------------------------------------------------------------
static size_t      ipp_resize_filter_get_width          (stream_obj* stream)
{
    DECLARE_RESIZE_FILTER(stream, rszfilter);
    return resize_base_get_width(rszfilter);
}

//-----------------------------------------------------------------------------
static size_t      ipp_resize_filter_get_height         (stream_obj* stream)
{
    DECLARE_RESIZE_FILTER(stream, rszfilter);
    return resize_base_get_height(rszfilter);
}

//-----------------------------------------------------------------------------
static int      ipp_resize_filter_get_pixel_format   (stream_obj* stream)
{
    DECLARE_RESIZE_FILTER(stream, rszfilter);
    return resize_base_get_pixel_format(rszfilter);
}

//-----------------------------------------------------------------------------
static int         ipp_resize_filter_read_frame        (stream_obj* stream,
                                                    frame_obj** frame)
{
    DECLARE_RESIZE_FILTER(stream, rszfilter);
    int res = -1;

    frame_obj* tmp = resize_base_pre_process(rszfilter, frame, &res);
    if ( tmp == NULL ) {
        return res;
    }

    frame_api_t*    tmpFrameAPI = frame_get_api(tmp);
    int srcPixfmt = tmpFrameAPI->get_pixel_format(tmp);
    if ( srcPixfmt != pfmtRGB24 ) {
        rszfilter->logCb(logError, _FMT("input frame pfmt=" << srcPixfmt << ", ipp resize filter cannot continue"));
        return -1;
    }


    int             dataSize = rszfilter->dimActual.width*rszfilter->dimActual.height*3;
    basic_frame_obj* newFrame = alloc_basic_frame2(IPPRESIZE_FILTER_MAGIC,
                                dataSize,
                                rszfilter->logCb,
                                rszfilter->fa );
    newFrame->pts = tmpFrameAPI->get_pts(tmp);
    newFrame->dts = tmpFrameAPI->get_dts(tmp);
    newFrame->keyframe = 1;
    newFrame->width = rszfilter->dimActual.width;
    newFrame->height = rszfilter->dimActual.height;
    newFrame->pixelFormat = rszfilter->pixfmt;
    newFrame->mediaType = mediaVideo;
    newFrame->dataSize = dataSize;




    IppStatus status = rszfilter->resizeFunc((const Ipp8u*)tmpFrameAPI->get_data(tmp),
                            3*rszfilter->inputWidth,
                            newFrame->data,
                            3*rszfilter->dimActual.width,
                            {0,0},
                            {(int)rszfilter->dimActual.width, (int)rszfilter->dimActual.height},
                            rszfilter->pSpec,
                            rszfilter->pWorkBuffer);

    if (status < 0) {
        rszfilter->logCb(logError, _FMT("Failed to resize image: " << ippGetStatusString(status)));
        frame_unref((frame_obj**)&newFrame);
        frame_unref(&tmp);
        return -1;
    }

    if ( rszfilter->retainSourceFrameInterval > 0 &&
         ( rszfilter->prevFramePts == INVALID_PTS ||
         newFrame->pts >= rszfilter->prevFramePts + rszfilter->retainSourceFrameInterval) ) {
        if ( newFrame->api->set_backing_obj((frame_obj*)newFrame, "srcFrame", tmp) < 0 ) {
            rszfilter->logCb(logError, _FMT("Failed to set source frame object!"));
        }
        // only need this variable when original frames are being retained
        rszfilter->prevFramePts = newFrame->pts;
    }

    frame_unref(&tmp);
    *frame = (frame_obj*)newFrame;
    return 0;
}

//-----------------------------------------------------------------------------
static int         ipp_resize_filter_close             (stream_obj* stream)
{
    DECLARE_RESIZE_FILTER(stream, rszfilter);
    ippsFree(rszfilter->pSpec);
    rszfilter->pSpec = NULL;
    ippsFree(rszfilter->pWorkBuffer);
    rszfilter->pWorkBuffer = NULL;
    return 0;
}

//-----------------------------------------------------------------------------
static void ipp_resize_filter_destroy         (stream_obj* stream)
{
    DECLARE_RESIZE_FILTER_V(stream, rszfilter);
    rszfilter->logCb(logTrace, _FMT("Destroying stream object " << (void*)stream));
    ipp_resize_filter_close(stream); // make sure all the internals had been freed
    destroy_frame_allocator(&rszfilter->fa, rszfilter->logCb);
    stream_destroy( stream );
}

//-----------------------------------------------------------------------------
SVVIDEOLIB_API stream_api_t*     get_ipp_resize_filter_api                    ()
{
    return &_g_ipp_resize_filter_provider;
}



