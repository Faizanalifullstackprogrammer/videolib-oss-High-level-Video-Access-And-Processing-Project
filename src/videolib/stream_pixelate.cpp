/*****************************************************************************
 *
 * stream_pixelate.hpp
 *   Node masking or marking specified areas of the frame.
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
#define SV_MODULE_VAR fffilter
#define SV_MODULE_ID "PIXELATE"
#include "sv_module_def.hpp"

#include "streamprv.h"

#include "frame_basic.h"
#include "sv_pixfmt.h"

#include "videolibUtils.h"

#include <algorithm>
#include <list>

#define FF_FILTER_MAGIC 0x1250
static bool _gInitialized = false;

using namespace std;

#include "bounding_boxes.hpp"

extern "C" stream_api_t*     get_resize_filter_api                    ();

//-----------------------------------------------------------------------------
typedef struct fs_filter ff_filter_obj;

typedef void       (*replace_pixel_proc)   (ff_filter_obj* fffilter,
                                                uint8_t* src,
                                                uint8_t* dst,
                                                int x,
                                                int y);

//-----------------------------------------------------------------------------
typedef struct fs_filter  : public ff_filter_base_obj  {
    int                 width;
    int                 height;
    int                 pixfmt;

    int                 pixsize;
    int                 radius;
    int                 thickness;
    replace_pixel_proc  replace_proc;
    const char*         filterType;
    box_t*              currentBox;
    int                 modifyInPlace;
    int                 markCenter;

    frame_allocator*    fa;
} ff_filter_obj;

//-----------------------------------------------------------------------------
// Stream API
//-----------------------------------------------------------------------------

//-----------------------------------------------------------------------------
//  Forward declarations
//-----------------------------------------------------------------------------
static stream_obj* ff_filter_create             (const char* name);
static int         ff_filter_set_param          (stream_obj* stream,
                                                const CHAR_T* name,
                                                const void* value);
static int         ff_filter_open_in            (stream_obj* stream);
static int         ff_filter_seek               (stream_obj* stream,
                                                       INT64_T offset,
                                                        int flags);
static int         ff_filter_read_frame         (stream_obj* stream, frame_obj** frame);
static int         ff_filter_close              (stream_obj* stream);
static void        ff_filter_destroy            (stream_obj* stream);

extern "C" stream_api_t*     get_ff_filter_api                    ();

static void       _sv_pixelate_replace_pixel_box  (ff_filter_obj* s,
                        uint8_t* src, uint8_t* dst, int x, int y);


//-----------------------------------------------------------------------------
stream_api_t _g_pixelate_filter_provider = {
    ff_filter_create,
    get_default_stream_api()->set_source,
    get_default_stream_api()->set_log_cb,
    get_default_stream_api()->get_name,
    get_default_stream_api()->find_element,
    get_default_stream_api()->remove_element,
    get_default_stream_api()->insert_element,
    ff_filter_set_param,
    get_default_stream_api()->get_param,
    ff_filter_open_in,
    ff_filter_seek,
    get_default_stream_api()->get_width,
    get_default_stream_api()->get_height,
    get_default_stream_api()->get_pixel_format,
    ff_filter_read_frame,
    get_default_stream_api()->print_pipeline,
    ff_filter_close,
    _set_module_trace_level
};


//-----------------------------------------------------------------------------
#define DECLARE_FF_FILTER(stream, name) \
    DECLARE_OBJ(ff_filter_obj, name,  stream, FF_FILTER_MAGIC, -1)

#define DECLARE_FF_FILTER_V(stream, name) \
    DECLARE_OBJ_V(ff_filter_obj, name,  stream, FF_FILTER_MAGIC)

static stream_obj*   ff_filter_create                (const char* name)
{
    ff_filter_obj* res = (ff_filter_obj*)stream_init(sizeof(ff_filter_obj),
                FF_FILTER_MAGIC,
                &_g_pixelate_filter_provider,
                name,
                ff_filter_destroy );

    _ff_filter_init(res);

    res->pixsize = 3;
    res->radius = 30;
    res->thickness = 1;
    res->replace_proc = _sv_pixelate_replace_pixel_box;
    res->filterType = "boundingbox";
    res->currentBox = NULL;
    res->modifyInPlace = 0;
    res->markCenter = 0;

    res->fa = create_frame_allocator(_STR("fffilter_"<<name));

    return (stream_obj*)res;
}


//-----------------------------------------------------------------------------
static uint8_t*   _sv_pixelate_get_pixel(ff_filter_obj* s,
                                                uint8_t* buf, int x, int y)
{
    return &buf[(y*s->width+x)*s->pixsize];
}

//-----------------------------------------------------------------------------
static void       _sv_pixelate_replace_pixel_blur  (ff_filter_obj* s,
                        uint8_t* src, uint8_t* dst, int x, int y)
{
    using std::min;
    using std::max;

    int      radius = s->radius;

    uint8_t* dstPix = _sv_pixelate_get_pixel(s,dst,x,y);
    int      avg[4] = { 0, 0, 0, 0 };
    int      total = 0;
    int leftLimit = max(x-radius, 0),
        rightLimit = min(x+radius, s->width-1),
        topLimit = max(y-radius, 0),
        bottomLimit = min(y+radius, s->height-1);

#if 0
    // average pixels over area -- takes a fairly long time
    for (int otherX=leftLimit; otherX<=rightLimit; otherX++) {
        for (int otherY=topLimit; otherY<=bottomLimit; otherY++) {
            uint8_t* otherPixel = _sv_pixelate_get_pixel(s, src,otherX,otherY);
            for (int nI=0; nI<pixsize; nI++) avg[nI] += otherPixel[nI];
            total++;
        }
    }
#else
    // average pixels over "crosshair" with the current pixel in the middle
    for (int otherX=leftLimit; otherX<=rightLimit; otherX++) {
        uint8_t* otherPixel = _sv_pixelate_get_pixel(s,src,otherX,y);
        for (int nI=0; nI<s->pixsize; nI++) avg[nI] += otherPixel[nI];
        total++;
    }
    for (int otherY=topLimit; otherY<=bottomLimit; otherY++) {
        uint8_t* otherPixel = _sv_pixelate_get_pixel(s,src,x,otherY);
        for (int nI=0; nI<s->pixsize; nI++) avg[nI] += otherPixel[nI];
        total++;
    }
#endif
    dstPix[0] = avg[0]/total;
    dstPix[1] = avg[1]/total;
    dstPix[2] = avg[2]/total;
    if ( s->pixsize > 3 )
        dstPix[3] = avg[3]/total;
}

//-----------------------------------------------------------------------------
static void       _sv_pixelate_replace_pixel_color (ff_filter_obj* s,
                        uint8_t* src, uint8_t* dst, int x, int y, uint8_t* color)
{
    uint8_t* dstPix = _sv_pixelate_get_pixel(s,dst,x,y);

    dstPix[0] = color[0];
    dstPix[1] = color[1];
    dstPix[2] = color[2];
    if ( s->pixsize > 3 )
        dstPix[3] = 0;
}

//-----------------------------------------------------------------------------
static void     _sv_draw_line(ff_filter_obj* fffilter,
                        uint8_t* src, uint8_t* dst, uint8_t* color,
                        int x0, int y0, int x1, int y1) {

    int dx = abs(x1-x0), sx = x0<x1 ? 1 : -1;
    int dy = abs(y1-y0), sy = y0<y1 ? 1 : -1;
    int err = (dx>dy ? dx : -dy)/2, e2;


    for( ;; ) {
        _sv_pixelate_replace_pixel_color(fffilter, src, dst, x0, y0, color);
        if (x0==x1 && y0==y1)
            break;
        e2 = err;
        if (e2 >-dx) {
            err -= dy;
            x0 += sx;
        }
        if (e2 < dy) {
            err += dx;
            y0 += sy;
        }
    }
}

//-----------------------------------------------------------------------------
static void       _sv_pixelate_replace_pixel_fill  (ff_filter_obj* s,
                        uint8_t* src, uint8_t* dst, int x, int y)
{
    return _sv_pixelate_replace_pixel_color(s, src, dst, x, y, s->currentBox->color);
}

//-----------------------------------------------------------------------------
static void       _sv_pixelate_replace_pixel_box  (ff_filter_obj* s,
                        uint8_t* src, uint8_t* dst, int x, int y)
{
    box_t* box = s->currentBox;
    rect_t* r = &box->r;

    int        thick = box->thickness;
    if ( x < r->x+thick ||
         x >= r->x+r->w-thick ||
         y < r->y+thick ||
         y >= r->y+r->h-thick ) {
        _sv_pixelate_replace_pixel_fill(s, src, dst, x, y);
    } else if ( s->markCenter ) {
        int centerX = r->x + r->w/2;
        int centerY = r->y + r->h/2;
        static const int centerMarkSize = 4;
        static uint8_t white[] = { 255, 255, 255 };
        if ( ( x == centerX && abs(y-centerY) < centerMarkSize ) ||
             ( y == centerY && abs(x-centerX) < centerMarkSize ) ) {
            _sv_pixelate_replace_pixel_color(s, src, dst, x, y, white);
        }
    }
}



//-----------------------------------------------------------------------------
static void       _sv_pixelate_replace_pixel_pixelate  (ff_filter_obj* s,
                        uint8_t* src, uint8_t* dst, int x, int y)
{
    using std::min;
    using std::max;

    int      radius = s->radius;

    uint8_t* dstPix = _sv_pixelate_get_pixel(s,dst,x,y);
    int    left = max(x-x%radius,0),
            right = min(left+radius, s->width-1),
            top = max(y-y%radius, 0),
            bottom = min(top+radius, s->height-1);
    memcpy( dstPix,
            _sv_pixelate_get_pixel(s, src,(left+right)/2,(top+bottom)/2),
            s->pixsize );
}

//-----------------------------------------------------------------------------
static int         ff_filter_set_param             (stream_obj* stream,
                                            const CHAR_T* name,
                                            const void* value)
{
    DECLARE_FF_FILTER(stream, fffilter);
    name = stream_param_name_apply_scope(stream, name);
    if (!_stricmp(name, "radius")) {
        fffilter->radius = *(int*)value;
        return 0;
    }
    if (!_stricmp(name, "useBlur")) {
        fffilter->replace_proc = _sv_pixelate_replace_pixel_blur;
        fffilter->filterType = "blur";
        // can't modify in place when depeneding on the neighbors
        fffilter->modifyInPlace = 0;
        return 0;
    }
    if (!_stricmp(name, "useFill")) {
        fffilter->replace_proc = _sv_pixelate_replace_pixel_fill;
        fffilter->filterType = "fill";
        return 0;
    }
    if (!_stricmp(name, "useBox")) {
        fffilter->replace_proc = _sv_pixelate_replace_pixel_box;
        fffilter->filterType = "boundingbox";
        return 0;
    }
    if (!_stricmp(name, "useLine")) {
        fffilter->replace_proc = NULL; // not used
        fffilter->filterType = "drawline";
        return 0;
    }
    if (!_stricmp(name, "usePixelate")) {
        fffilter->replace_proc = _sv_pixelate_replace_pixel_pixelate;
        fffilter->filterType = "pixelate";
        // can't modify in place when depeneding on the neighbors
        fffilter->modifyInPlace = 0;
        return 0;
    }
    if (!_stricmp(name, "modifyInPlace")) {
        // can only modify frame in place for some filters
        if ( _stricmp(fffilter->filterType, "pixelate") &&
             _stricmp(fffilter->filterType, "blur")) {
            fffilter->modifyInPlace = *(int*)value;
        }
        return 0;
    } else
    if (!_stricmp(name, "markCenter")) {
        fffilter->markCenter = *(int*)value;
        return 0;
    } else
    if (!_stricmp(name, "metadata")) {
        TRACE(_FMT("Parsing metadata: " << (const char*)value));
        _ff_filter_parse_metadata( fffilter,
                        fffilter->filterType,
                        (const char*)value,
                        INVALID_PTS,
                        fffilter->currentMetadata);
        fffilter->staticMetadata = true;
        TRACE(_FMT("Parsing current meta: got " << fffilter->currentMetadata.boxes->size() ));
        return 0;
    } else
    if (!_stricmp(name, "filterType")) {
        // ignore (for now, but maybe key off this for compatibility with fffilter)
        return 0;
    }

    return default_set_param(stream, name, value);
}

//-----------------------------------------------------------------------------
static int         ff_filter_open_in                (stream_obj* stream)
{
    DECLARE_FF_FILTER(stream, fffilter);
    int res = 0;
    int retryCount = 0;


Retry:
    res = default_open_in(stream);
    if ( res >= 0 ) {
        fffilter->pixfmt = default_get_pixel_format(stream);
        fffilter->width = default_get_width(stream);
        fffilter->height = default_get_height(stream);
        if ( fffilter->pixfmt != pfmtRGB24 && fffilter->pixfmt != pfmtBGR24 && retryCount == 0 )  {
            int pixfmt = pfmtRGB24;
            retryCount ++;
            if ( fffilter->sourceApi->insert_element(&fffilter->source,
                                            &fffilter->sourceApi,
                                            default_get_name(stream),
                                            get_resize_filter_api()->create(_STR(fffilter->name<<".pixfmt")),
                                            svFlagStreamInitialized) != 0 ||
                default_set_param(stream, _STR(fffilter->name<<".pixfmt.pixfmt"), &pixfmt) != 0 ) {
                    fffilter->logCb(logError, _FMT("Failed to auto-insert pixfmt converter"));
                    res = -1;
            } else {
                TRACE( _FMT("Attempting to re-initialize with the auto-inserted format converter"));
                fffilter->sourceInitialized = 0;
                goto Retry;
            }
        }
        if (fffilter->radius < 0) {
            // radius is percentage of the image height
            fffilter->radius = (fffilter->height*abs(fffilter->radius))/100;
        }
    }
    return res;
}

//-----------------------------------------------------------------------------
static int         ff_filter_seek               (stream_obj* stream,
                                                       INT64_T offset,
                                                        int flags)
{
    DECLARE_FF_FILTER(stream, fffilter);
    int res = default_seek(stream, offset, flags);
    if (!fffilter->staticMetadata) {
        _ff_filter_clear(fffilter);
    }
    return res;
}

//-----------------------------------------------------------------------------
static int         ff_filter_read_frame        (stream_obj* stream,
                                                    frame_obj** frame)
{
    DECLARE_FF_FILTER(stream, fffilter);


    frame_obj* tmp = NULL;
    frame_obj* retFrame = NULL;
    frame_api_t* tmpFrameAPI;
    int        res = -1;
    BoxList*   boxes = NULL;
    uint8_t*   dst;
    uint8_t*   src;

    res = _ff_filter_read_frame( fffilter, fffilter->filterType, &tmp, &boxes );
    if ( res <= 0 ) {
        *frame = tmp;
        return res;
    }

    tmpFrameAPI = frame_get_api(tmp);
    INT64_T ts = tmpFrameAPI->get_pts(tmp);
    size_t  srcSize = tmpFrameAPI->get_data_size(tmp);

    if ( boxes == NULL || boxes->empty() ) {
        TRACE(_FMT("No filter is defined for ts=" << ts));
        *frame = tmp;
        return res;
    }


    if ( tmpFrameAPI->get_width(tmp) != fffilter->width ||
         tmpFrameAPI->get_height(tmp) != fffilter->height ) {
        TRACE(_FMT("Output size had changed!"));
        fffilter->width = tmpFrameAPI->get_width(tmp);
        fffilter->height = tmpFrameAPI->get_height(tmp);
    }


    src = (uint8_t*)tmpFrameAPI->get_data(tmp);
    if ( !fffilter->modifyInPlace ) {
        basic_frame_obj* newFrame = alloc_basic_frame2(FF_FILTER_MAGIC,
                                    srcSize,
                                    fffilter->logCb,
                                    fffilter->fa );
        newFrame->pts = newFrame->dts = ts;
        newFrame->keyframe = 1;
        newFrame->width = fffilter->width;
        newFrame->height = fffilter->height;
        newFrame->pixelFormat = fffilter->pixfmt;
        newFrame->mediaType = mediaVideo;
        newFrame->dataSize = srcSize;

        retFrame = (frame_obj*)newFrame;
        dst = newFrame->data;
        memcpy(dst, src, srcSize);
    } else {
        retFrame = tmp;
        dst = src;
    }

    int         w = fffilter->width;
    int         h = fffilter->height;
    int         left, right, top, bottom;
    int         boxesAreLines = !_stricmp(fffilter->filterType, "drawline");


    for (BoxListIter it = boxes->begin();
                  it != boxes->end();
                  it++) {
        box_t   box;

        _ff_rescale_box( *it, box, w, h );

        left = max(box.r.x,0);
        top = max(box.r.y,0);
        if ( boxesAreLines ) {
            right = max(box.r.w,0);
            bottom = max(box.r.h,0);
            if ( left > right ) std::swap(left, right);
            if ( top > bottom ) std::swap(top, bottom);
        } else {
            right = left+max(box.r.w,0);
            bottom = top+max(box.r.h,0);
        }

        if (right>=w) {
            fffilter->logCb(logWarning, _FMT("Bounding box: left="<<left<<" right="<<right<<" picw="<<w));
            right=w-1;
        }
        if (bottom>=h) {
            fffilter->logCb(logWarning, _FMT("Bounding box: top="<<top<<" bottom="<<bottom<<" pich="<<h));
            bottom=h-1;
        }

        fffilter->currentBox = &box;

        if ( boxesAreLines ) {
            rect_t& r = box.r;
            _sv_draw_line( fffilter, src, dst, box.color, r.x, r.y, r.w, r.h );
        } else {
            for (int x=left; x<right; x++) {
                for (int y=top; y<bottom; y++ ) {
                    fffilter->replace_proc(fffilter, src, dst, x, y);
                }
            }
        }
    }

    TRACE(_FMT("Generated frame for pts=" << ts << " boxesApplied=" << boxes->size()));

    res = 0;
Error:
    // retFrame should be NULL in case of failure
    assert( res == 0 || retFrame == NULL );

    *frame = retFrame;
    if ( retFrame != tmp ) {
        // this covers both success
        frame_unref(&tmp);
        if ( res < 0 ) {
            frame_unref(&retFrame);
        }
    }
    return res;
}

//-----------------------------------------------------------------------------
static int         ff_filter_close             (stream_obj* stream)
{
    DECLARE_FF_FILTER(stream, fffilter);
    _ff_filter_clear(fffilter);
    return 0;
}


//-----------------------------------------------------------------------------
static void ff_filter_destroy         (stream_obj* stream)
{
    DECLARE_FF_FILTER_V(stream, fffilter);
    fffilter->logCb(logTrace, _FMT("Destroying stream object " << (void*)stream));
    _ff_filter_destroy(fffilter);
    destroy_frame_allocator( &fffilter->fa, fffilter->logCb );
    stream_destroy( stream );
}

//-----------------------------------------------------------------------------
SVVIDEOLIB_API stream_api_t*     get_pixelate_filter_api                    ()
{
    if (!_gInitialized) {
        _gInitialized=true;
    }
    return &_g_pixelate_filter_provider;
}

