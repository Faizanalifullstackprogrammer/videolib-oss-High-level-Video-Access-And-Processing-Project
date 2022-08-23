/*****************************************************************************
 *
 * stream_resize_factory.cpp
 *   Node responsible for instantiation a specific implementation of pixel format
 *   converter/resize component. Chooses optimal implementation depending on
 *   available libraries and specified pixel formats.
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
#define SV_MODULE_VAR rszfactory
#define SV_MODULE_ID "RESIZEFACTORY"
#include "sv_module_def.hpp"

#include "streamprv.h"

#include "frame_basic.h"

#include "videolibUtils.h"

#define RESIZEFACTORY_FILTER_MAGIC 0x1225

#include "stream_resize_base.hpp"

//-----------------------------------------------------------------------------
typedef struct resize_factory  : public resize_base_obj  {
    stream_obj*     impl;
    stream_api*     implApi;
    // This object is used when we have an ffmpeg NV12 <-> RGB conversion, or IPP split pixfmt/resize
    // It never performs resize; only pixfmt conversion.
    // The only reason we need to keep a reference to it (rather than let impl manage it),
    // is whenever source is replaced, it needs to be replaced directly on impl2
    stream_obj*     impl2;
} resize_factory_obj;

//-----------------------------------------------------------------------------
// Stream API
//-----------------------------------------------------------------------------

//-----------------------------------------------------------------------------
//  Forward declarations
//-----------------------------------------------------------------------------
static stream_obj* resize_factory_create             (const char* name);
static int         resize_factory_set_source         (stream_obj* stream,
                                                stream_obj* source,
                                                INT64_T flags);
static int         resize_factory_insert_element     (stream_obj** pStream,
                                                struct stream_api** pAPI,
                                                const char* insertBefore,
                                                stream_obj* newElement,
                                                INT64_T flags);
static int         resize_factory_remove_element     (stream_obj** pStream,
                                                struct stream_api** pAPI,
                                                const char* name,
                                                stream_obj** removedStream);
static int         resize_factory_set_param          (stream_obj* stream,
                                                const CHAR_T* name,
                                                const void* value);
static int         resize_factory_get_param          (stream_obj* stream,
                                                const CHAR_T* name,
                                                void* value,
                                                size_t* size);
static int         resize_factory_open_in            (stream_obj* stream);
static size_t      resize_factory_get_width          (stream_obj* stream);
static size_t      resize_factory_get_height         (stream_obj* stream);
static int         resize_factory_get_pixel_format   (stream_obj* stream);
static int         resize_factory_read_frame         (stream_obj* stream, frame_obj** frame);
static int         resize_factory_close              (stream_obj* stream);
static void        resize_factory_destroy            (stream_obj* stream);
static int         resize_set_color_options         (stream_obj* stream, struct SwsContext *ctx, int colorspace, int range );


//-----------------------------------------------------------------------------
stream_api_t _g_resize_factory_provider = {
    resize_factory_create,
    resize_factory_set_source,
    get_default_stream_api()->set_log_cb,
    get_default_stream_api()->get_name,
    get_default_stream_api()->find_element,
    resize_factory_remove_element,
    resize_factory_insert_element,
    resize_factory_set_param,
    resize_factory_get_param,
    resize_factory_open_in,
    get_default_stream_api()->seek,
    resize_factory_get_width,
    resize_factory_get_height,
    resize_factory_get_pixel_format,
    resize_factory_read_frame,
    get_default_stream_api()->print_pipeline,
    resize_factory_close,
    _set_module_trace_level
};


//-----------------------------------------------------------------------------
#define DECLARE_RESIZEFACTORY_FILTER(stream, name) \
    DECLARE_OBJ(resize_factory_obj, name,  stream, RESIZEFACTORY_FILTER_MAGIC, -1)

#define DECLARE_RESIZEFACTORY_FILTER_V(stream, name) \
    DECLARE_OBJ_V(resize_factory_obj, name,  stream, RESIZEFACTORY_FILTER_MAGIC)

static stream_obj*   resize_factory_create                (const char* name)
{
    resize_factory_obj* res = (resize_factory_obj*)stream_init(sizeof(resize_factory_obj),
                RESIZEFACTORY_FILTER_MAGIC,
                &_g_resize_factory_provider,
                name,
                resize_factory_destroy );

    resize_base_init(res);
    res->impl = NULL;
    res->impl2 = NULL;
    return (stream_obj*)res;
}

//-----------------------------------------------------------------------------
static void         _replace_impl_source(resize_factory_obj* r, stream_obj* src)
{
    if (!r->impl && !r->impl2) {
        return;
    }
    stream_base* impl = r->impl2 ? (stream_base*)r->impl2 : (stream_base*)r->impl;
    impl->source = src;
    if ( src ) {
        impl->sourceApi = stream_get_api(src);
    }
}

//-----------------------------------------------------------------------------
static int         resize_factory_set_source       (stream_obj* stream,
                                      stream_obj* source,
                                      INT64_T flags)
{
    DECLARE_RESIZEFACTORY_FILTER(stream, rszfactory);
    get_default_stream_api()->set_source(stream, source, flags);
    _replace_impl_source(rszfactory, rszfactory->source);
    return 0;
}


//-----------------------------------------------------------------------------
static int         resize_factory_set_param             (stream_obj* stream,
                                            const CHAR_T* name,
                                            const void* value)
{
    DECLARE_RESIZEFACTORY_FILTER(stream, rszfactory);
    if (resize_base_set_param(rszfactory, name, value) >= 0 ) {
        return 0;
    }
    return default_set_param(stream, name, value);
}

//-----------------------------------------------------------------------------
static int                resize_factory_insert_element    (stream_obj** pStream,
                                                struct stream_api** pAPI,
                                                const char* insertBefore,
                                                stream_obj* newElement,
                                                INT64_T flags)
{
    DECLARE_RESIZEFACTORY_FILTER(*pStream, rszfactory);
    stream_obj* src = rszfactory->source;
    int res = get_default_stream_api()->insert_element(pStream, pAPI, insertBefore, newElement, flags);
    if ( rszfactory->source != src ) {
        _replace_impl_source(rszfactory, rszfactory->source);
    }
    return res;
}

//-----------------------------------------------------------------------------
static int         resize_factory_remove_element     (stream_obj** pStream,
                                                struct stream_api** pAPI,
                                                const char* name,
                                                stream_obj** removedStream)
{
    DECLARE_RESIZEFACTORY_FILTER(*pStream, rszfactory);
    stream_obj* src = rszfactory->source;
    int res = get_default_stream_api()->remove_element(pStream, pAPI, name, removedStream);
    if ( rszfactory->source != src ) {
        _replace_impl_source(rszfactory, rszfactory->source);
    }
    return res;
}

//-----------------------------------------------------------------------------
static int         resize_factory_get_param          (stream_obj* stream,
                                                const CHAR_T* name,
                                                void* value,
                                                size_t* size)
{
    DECLARE_RESIZEFACTORY_FILTER(stream, rszfactory);
    if (resize_base_get_param(rszfactory, name, value, size) >= 0 ) {
        return 0;
    }
    return default_get_param(stream, name, value, size);
}

//-----------------------------------------------------------------------------
static bool        _ipp_supported_cc(int srcCC, int dstCC)
{
    if ( srcCC == dstCC ) {
        return true;
    }
    if ( dstCC == pfmtBGR24 || dstCC == pfmtRGB24 ) {
        switch (srcCC)
        {
        case pfmtYUV420P:
        case pfmtYUVJ420P:
        case pfmtYUV422P:
        case pfmtYUVJ422P:
        case pfmtYUYV422:
        case pfmtNV12:
            return true;
        }
    }
    return false;
}


//-----------------------------------------------------------------------------
static int         resize_factory_open_in                (stream_obj* stream)
{
    DECLARE_RESIZEFACTORY_FILTER(stream, rszfactory);
    int res = 0;

    // make sure we have cleaned up
    resize_factory_close(stream);


    if ( resize_base_open_in(rszfactory) < 0 ) {
        return -1;
    }

    if ( rszfactory->passthrough ) {
        return 0;
    }

    stream_api_t*  pass1api = NULL;
    const char* pass1name = "undefined";
    stream_api_t*  pass2api = NULL;
    const char* pass2name = "undefined";
    int intermediatePifxmt = pfmtUndefined;
    int intermediateResize = 0;
    const char* configuration = "undefined";
#ifdef WITH_IPP
    int localSource = 1;
    size_t szLocalSource = sizeof(int);
    if ( default_get_param(stream, "isLocalSource", &localSource, &szLocalSource) < 0 ) {
        // by default we'll allow IPP, but we do not want to use it with localVideoLib, which implements 'isLocalSource'
        localSource = 0;
    }
    if (!localSource && _ipp_supported_cc(rszfactory->inputPixFmt, rszfactory->pixfmt)) {
        if (rszfactory->dimSetting.width > 0 ||
            rszfactory->dimSetting.height > 0 ||
            rszfactory->dimSetting.resizeFactor > 0 ) {

            if ( rszfactory->inputPixFmt == rszfactory->pixfmt && rszfactory->pixfmt != pfmtRGB24 ) {
                // The client didn't specify a pixfmt to convert to, but in order to succeed with resize
                // we'd have to go to RGB24
                rszfactory->logCb(logDebug, _FMT("Forcing conversion to RGB24, as resize alone cannot handle pixfmt=" << rszfactory->inputPixFmt ));
                rszfactory->pixfmt = pfmtRGB24;
            }

            pass2api = get_ipp_resize_filter_api();
            pass2name = "ippResize";
            configuration = "ipp resize";
        }
        if (rszfactory->inputPixFmt != rszfactory->pixfmt) {
            pass1api = get_ipp_cc_filter_api();
            pass1name = "ippcc";
            intermediatePifxmt = rszfactory->pixfmt;
            configuration = "ipp cc";
        }
        if ( pass1api && pass2api ) {
            configuration = "ipp cc+resize";
        } else if ( pass1api && !pass2api ) {
            pass2api = pass1api;
            pass2name = pass1name;
            pass1api = NULL;
            pass1name = NULL;
        }
    }
#endif
    if ( pass1api == NULL && pass2api == NULL ) {
        // FFmpeg's NV12 <-> YUV420P <-> RGB is faster than NV12 <-> RGB direct conversion
        if ( (rszfactory->inputPixFmt == pfmtNV12 && rszfactory->pixfmt != pfmtYUV420P && rszfactory->pixfmt != pfmtNV12 ) ||
             (rszfactory->pixfmt == pfmtNV12 && rszfactory->inputPixFmt != pfmtYUV420P && rszfactory->inputPixFmt != pfmtNV12 ) ) {
            pass1api = get_resize_filter_api();
            pass1name = "ffmpegNV12";
            intermediatePifxmt = pfmtYUV420P;
            configuration = "ffmpeg 2-step cc";
        } else {
            configuration = "ffmpeg cc+resize";
        }
        pass2api = get_resize_filter_api();
        pass2name = "ffmpeg";
    }
    rszfactory->impl = pass2api->create(_STR(rszfactory->name<<"."<<pass2name));
    rszfactory->implApi = pass2api;
    // apply all the relevant params
    resize_base_obj* r1 = (resize_base_obj*)rszfactory->impl;
    resize_base_proxy_params(rszfactory, r1);
    if ( pass1api ) {
        rszfactory->impl2 = pass1api->create(_STR(rszfactory->name<<"."<<pass1name));

        // Apply all the relevant params, pixfmt and resize will be changed manually
        resize_base_obj* r2 = (resize_base_obj*)rszfactory->impl2;
        resize_base_proxy_params(rszfactory, r2);
        // The first pass only does pixfmt conversion
        r2->pixfmt = intermediatePifxmt;
        memset((void*)&r2->dimSetting, 0, sizeof(r2->dimSetting));

        // finally, we need to change the source on the front pass
        r1->source = rszfactory->impl2;
        r1->sourceApi = stream_get_api(r1->source);
        r1->sourceInitialized = 0;
    }



    if ( rszfactory->implApi->open_in(rszfactory->impl) < 0 ) {
        rszfactory->logCb(logError, _FMT("Failed to initialize resize implementation, config="<<configuration));
        return -1;
    }
    rszfactory->logCb(logDebug, _FMT("Initialized configuration "<<configuration));
    return 0;
}

//-----------------------------------------------------------------------------
static size_t      resize_factory_get_width          (stream_obj* stream)
{
    DECLARE_RESIZEFACTORY_FILTER(stream, rszfactory);
    return resize_base_get_width(rszfactory);
}

//-----------------------------------------------------------------------------
static size_t      resize_factory_get_height         (stream_obj* stream)
{
    DECLARE_RESIZEFACTORY_FILTER(stream, rszfactory);
    return resize_base_get_height(rszfactory);
}

//-----------------------------------------------------------------------------
static int      resize_factory_get_pixel_format   (stream_obj* stream)
{
    DECLARE_RESIZEFACTORY_FILTER(stream, rszfactory);
    return resize_base_get_pixel_format(rszfactory);
}

//-----------------------------------------------------------------------------
static int         resize_factory_read_frame        (stream_obj* stream,
                                                    frame_obj** frame)
{
    DECLARE_RESIZEFACTORY_FILTER(stream, rszfactory);

    if ( rszfactory->updatePending ) {
        rszfactory->updatePending = 0;
        int res = resize_factory_open_in(stream);
        if (res < 0) {
            rszfactory->logCb(logError, _FMT( "Failed to re-init " << default_get_name(stream)));
            return -1;
        }
    }

    if ( rszfactory->passthrough ) {
        return default_read_frame(stream, frame);
    }

    return rszfactory->implApi->read_frame(rszfactory->impl, frame);
}

//-----------------------------------------------------------------------------
static int         resize_factory_close             (stream_obj* stream)
{
    DECLARE_RESIZEFACTORY_FILTER(stream, rszfactory);
    if (rszfactory->impl) {
        // close should not close the actual source
        _replace_impl_source(rszfactory, NULL);
        rszfactory->implApi->close(rszfactory->impl);
        stream_unref(&rszfactory->impl);
        rszfactory->implApi = NULL;
    }
    return 0;
}

//-----------------------------------------------------------------------------
static void resize_factory_destroy         (stream_obj* stream)
{
    DECLARE_RESIZEFACTORY_FILTER_V(stream, rszfactory);
    rszfactory->logCb(logTrace, _FMT("Destroying stream object " << (void*)stream));
    stream_destroy( stream );
}

//-----------------------------------------------------------------------------
SVVIDEOLIB_API stream_api_t*     get_resize_factory_api                    ()
{
    return &_g_resize_factory_provider;
}

