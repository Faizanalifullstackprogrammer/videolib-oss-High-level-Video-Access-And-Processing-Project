/*****************************************************************************
 *
 * stream_resize_base.cpp
 *   Common elements of node objects performing color conversion and resize operations.
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

#include "stream_resize_base.hpp"

#include "streamprv.h"
#include "videolibUtils.h"


//-----------------------------------------------------------------------------
void resize_base_init(resize_base_obj* res)
{
    res->dimSetting.width = 0;
    res->dimSetting.height = 0;
    res->dimSetting.resizeFactor = 0;
    res->dimSetting.keepAspectRatio = 1;
    res->dimActual = res->dimSetting;
    res->allowUpsize = 0;
    res->updatePending = 0;
    res->minHeight = 0;
    res->pixfmt = pfmtUndefined;
    res->colorSpace = -1; // BT601
    res->colorRange = -1;   // restricted
    res->inputHeight = 0;
    res->inputWidth = 0;
    res->inputPixFmt = pfmtUndefined;
    res->retainSourceFrameInterval = 0;
    res->prevFramePts = INVALID_PTS;
}

//-----------------------------------------------------------------------------
void resize_base_flag_for_reopen(resize_base_obj* rszfilter)
{
    rszfilter->logCb(logInfo, _FMT("Resize filter to be reopened on the next read"));
    // we want filter to be closed and reopened on the next read attempt
    rszfilter->updatePending = 1;
    // but for this attempt to actually occur, passthrough flag needs to be
    // removed, even if temporarily
    rszfilter->passthrough = 0;
}

//-----------------------------------------------------------------------------
int  resize_base_set_param      (resize_base_obj* rszfilter,
                                const CHAR_T* name,
                                const void* value)
{
    name = stream_param_name_apply_scope((stream_obj*)rszfilter, name);
    SET_PARAM_IF(rszfilter, name, "width", int, rszfilter->dimSetting.width);
    SET_PARAM_IF(rszfilter, name, "height", int, rszfilter->dimSetting.height);
    SET_PARAM_IF(rszfilter, name, "pixfmt", int, rszfilter->pixfmt);
    SET_PARAM_IF(rszfilter, name, "colorSpace", int, rszfilter->colorSpace);
    SET_PARAM_IF(rszfilter, name, "colorRange", int, rszfilter->colorRange);
    SET_PARAM_IF(rszfilter, name, "resizeFactor", float, rszfilter->dimSetting.resizeFactor);
    SET_PARAM_IF(rszfilter, name, "keepAspectRatio", int, rszfilter->dimSetting.keepAspectRatio);
    SET_PARAM_IF(rszfilter, name, "retainSourceFrameInterval", int, rszfilter->retainSourceFrameInterval);
    SET_PARAM_IF(rszfilter, name, "minHeight", int, rszfilter->minHeight);
    SET_PARAM_IF(rszfilter, name, "allowUpsize", int, rszfilter->allowUpsize);
    if ( !_stricmp(name, "updateSize") ) {
        int* arr = (int*)value;
        int width = arr[0], height = arr[1];
        rszfilter->dimSetting.width = width;
        rszfilter->dimSetting.height = height;
        resize_base_flag_for_reopen(rszfilter);
        return 0;
    }
    if ( !_stricmp(name, "updatePixfmt") ) {
        rszfilter->pixfmt = *(int*)value;
        resize_base_flag_for_reopen(rszfilter);
        return 0;
    }
    return -1;
}

//-----------------------------------------------------------------------------
void        resize_base_proxy_params   (const resize_base_obj* r,
                                        resize_base_obj* other)
{
    other->dimSetting = r->dimSetting;
    other->pixfmt = r->pixfmt;
    other->colorRange = r->colorSpace;
    other->colorSpace = r->colorSpace;
    other->retainSourceFrameInterval = r->retainSourceFrameInterval;
    other->minHeight = r->minHeight;
    other->allowUpsize = r->allowUpsize;

    other->source = r->source;
    other->sourceApi = r->sourceApi;
    other->logCb = r->logCb;
    other->sourceInitialized = 1;
}


//-----------------------------------------------------------------------------
int  resize_base_get_param      (resize_base_obj* rszfilter,
                                const CHAR_T* name,
                                void* value,
                                size_t* size)
{
    name = stream_param_name_apply_scope((stream_obj*)rszfilter, name);
    COPY_PARAM_IF(rszfilter, name, "resizeFactor", float,   rszfilter->dimActual.resizeFactor);
    COPY_PARAM_IF(rszfilter, name, "width", int,   rszfilter->dimActual.width);
    COPY_PARAM_IF(rszfilter, name, "height", int,   rszfilter->dimActual.height);
    COPY_PARAM_IF(rszfilter, name, "passthrough", int,   rszfilter->passthrough);
    return -1;
}


//-----------------------------------------------------------------------------
int  resize_base_open_in       (resize_base_obj* rszfilter)
{
    stream_obj* stream = (stream_obj*)rszfilter;

    int res = default_open_in(stream);
    if (res < 0) {
        return -1;
    }


    rszfilter->inputWidth = default_get_width(stream);
    rszfilter->inputHeight = default_get_height(stream);
    rszfilter->inputPixFmt = default_get_pixel_format(stream);

    // Ideally, we shouldn't have a successful open after which stream parameters
    // aren't established, but better safe than sorry.
    if ( rszfilter->inputWidth <=0 || rszfilter->inputHeight <= 0 ||
         rszfilter->inputPixFmt == pfmtUndefined ) {
        rszfilter->logCb(logError, _FMT("Failed to determine input stream parameters:" <<
                        " w=" << rszfilter->inputWidth <<
                        " h=" << rszfilter->inputHeight <<
                        " pixfmt=" << rszfilter->inputPixFmt ));
        return -1;
    }

    if (!rszfilter->allowUpsize) {
        if ( rszfilter->dimSetting.width > rszfilter->inputWidth ) {
            rszfilter->logCb(logTrace, _FMT("Constraining to input width:" << rszfilter->dimSetting.width << ">" << rszfilter->inputWidth));
            rszfilter->dimSetting.width = rszfilter->inputWidth;
        }
        if ( rszfilter->dimSetting.height > rszfilter->inputHeight ) {
            rszfilter->logCb(logTrace, _FMT("Constraining to input height:" << rszfilter->dimSetting.height << ">" << rszfilter->inputHeight));
            rszfilter->dimSetting.height = rszfilter->inputHeight;
        }
    }

    // default to input values
    if (rszfilter->pixfmt == pfmtUndefined) {
        rszfilter->pixfmt = rszfilter->inputPixFmt;
    }

    rszfilter->dimActual.keepAspectRatio = 1;
    if (rszfilter->dimSetting.width != 0 && rszfilter->dimSetting.height != 0 ) {
        // both height and width explicitly set -- this may change the aspect ratio
        rszfilter->dimActual.resizeFactor = -1;
        rszfilter->dimActual.keepAspectRatio = 0;
        rszfilter->dimActual.height = rszfilter->dimSetting.height;
        rszfilter->dimActual.width = rszfilter->dimSetting.width;
    } else if ( rszfilter->dimSetting.width != 0 ) {
        rszfilter->dimActual.resizeFactor = rszfilter->inputWidth / (float) rszfilter->dimSetting.width;
        rszfilter->dimActual.keepAspectRatio = rszfilter->dimSetting.keepAspectRatio;
        rszfilter->dimActual.height = rszfilter->inputHeight / rszfilter->dimActual.resizeFactor;
        rszfilter->dimActual.width = rszfilter->dimSetting.width;
    } else if ( rszfilter->dimSetting.height != 0 ) {
        rszfilter->dimActual.resizeFactor = rszfilter->inputHeight / (float) rszfilter->dimSetting.height;
        rszfilter->dimActual.keepAspectRatio = rszfilter->dimSetting.keepAspectRatio;
        rszfilter->dimActual.height = rszfilter->dimSetting.height;
        rszfilter->dimActual.width = rszfilter->inputWidth / rszfilter->dimActual.resizeFactor;
    } else if ( rszfilter->dimSetting.resizeFactor != 0 ) {
        if ( rszfilter->minHeight > 0 &&
             rszfilter->inputHeight / rszfilter->dimSetting.resizeFactor < rszfilter->minHeight ) {
            rszfilter->dimActual.resizeFactor = rszfilter->inputHeight / (float) rszfilter->minHeight;
        } else {
            rszfilter->dimActual.resizeFactor = rszfilter->dimSetting.resizeFactor;
        }
        rszfilter->dimActual.width = rszfilter->inputWidth / rszfilter->dimActual.resizeFactor;
        rszfilter->dimActual.height = rszfilter->inputHeight / rszfilter->dimActual.resizeFactor;
        rszfilter->dimActual.keepAspectRatio = 1;
    } else {
        rszfilter->dimActual.width = rszfilter->inputWidth;
        rszfilter->dimActual.height = rszfilter->inputHeight;
        rszfilter->dimActual.resizeFactor = 1;
        rszfilter->dimActual.keepAspectRatio = 1;
    }

    if (rszfilter->inputWidth == rszfilter->dimActual.width &&
        rszfilter->inputHeight == rszfilter->dimActual.height &&
        rszfilter->inputPixFmt == rszfilter->pixfmt) {
        rszfilter->logCb(logDebug, _FMT("Resize filter '" <<
                                        rszfilter->name <<
                                        "' will be operating as a passthrough"));
        rszfilter->passthrough = 1;
        return 0;
    }

    if ( rszfilter->dimActual.keepAspectRatio ) {
        int outW = rszfilter->dimActual.width;
        int outH = rszfilter->dimActual.height;
        videolibapi_preserve_aspect_ratio   ( rszfilter->inputWidth, rszfilter->inputHeight, &outW, &outH, 1 );
        rszfilter->dimActual.width = outW;
        rszfilter->dimActual.height = outH;
    }

    // some encoders (nvenc) will error out if width is an odd number
    if ( (rszfilter->dimActual.width % 2) != 0 ) {
        rszfilter->dimActual.width -= 1;
    }

    rszfilter->logCb(logDebug, _FMT("Resize filter '" << rszfilter->name << "': " <<
                                    " srcSize=" << rszfilter->inputWidth << "x" << rszfilter->inputHeight <<
                                    " srcPixFmt=" << rszfilter->inputPixFmt <<
                                    " settings=[" << rszfilter->dimSetting.width << "x" << rszfilter->dimSetting.height << " rf=" << rszfilter->dimSetting.resizeFactor << " ar=" << rszfilter->dimSetting.keepAspectRatio << "]" <<
                                    " actual=[" << rszfilter->dimActual.width << "x" << rszfilter->dimActual.height << " rf=" << rszfilter->dimActual.resizeFactor << " ar=" << rszfilter->dimActual.keepAspectRatio << "]" <<
                                    " dstPixFmt=" << rszfilter->pixfmt <<
                                    " dstColorRange=" << rszfilter->colorRange <<
                                    " dstColorSpace=" << rszfilter->colorSpace ));
    rszfilter->passthrough = 0;
    return 0;
}


//-----------------------------------------------------------------------------
size_t      resize_base_get_width          (resize_base_obj* rszfilter)
{
    if (!rszfilter->source) {
        return -1;
    }
    return rszfilter->dimActual.width;
}

//-----------------------------------------------------------------------------
size_t      resize_base_get_height         (resize_base_obj* rszfilter)
{
    if (!rszfilter->source) {
        return -1;
    }
    return rszfilter->dimActual.height;
}

//-----------------------------------------------------------------------------
int      resize_base_get_pixel_format      (resize_base_obj* rszfilter)
{
    if (!rszfilter->source) {
        return -1;
    }
    return rszfilter->pixfmt;
}

//-----------------------------------------------------------------------------
frame_obj*  resize_base_pre_process               (resize_base_obj* rszfilter, frame_obj** frame, int* res)
{
    *frame = NULL;


    if ( rszfilter->updatePending ) {
        rszfilter->updatePending = 0;
        *res = rszfilter->api->open_in((stream_obj*)rszfilter);
        if (*res < 0) {
            rszfilter->logCb(logError, _FMT( "Failed to re-init " << default_get_name((stream_obj*)rszfilter) ));
            return NULL;
        }
    }

    frame_obj* tmp = NULL;
    frame_api_t* tmpFrameAPI;
    *res = default_read_frame((stream_obj*)rszfilter, &tmp);
    if ( *res < 0 ||
         tmp == NULL ||
         rszfilter->passthrough ||
         (tmpFrameAPI = frame_get_api(tmp)) == NULL ||
         tmpFrameAPI->get_media_type(tmp) != mediaVideo ) {
        *frame = tmp;
        return NULL;
    }

    // The caller should go on processing the frame
    return tmp;
}

