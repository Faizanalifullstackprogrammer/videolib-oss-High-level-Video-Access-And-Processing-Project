/*****************************************************************************
 *
 * frame_cloned.cpp
 *   Wrapper around another frame object, allowing to rely on the same object,
 *   while overriding frames metadata (e.g. timestamp). Used in the context of
 *   jitter buffer.
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

#include "streamprv.h"
#include "frame_allocator.h"
#include "videolibUtils.h"

#define FRAME_CLONE_MAGIC 0x9002

//-----------------------------------------------------------------------------
// Frame object assisting with frame duplication. Keeps reference to the original
// frame, but lets override its PTS.
//-----------------------------------------------------------------------------

typedef struct cloned_frame : public frame_pooled  {
    frame_obj*          source;
    frame_api_t*        sourceApi;
    INT64_T             pts;
    INT64_T             dts;
    AVPacket            packet;
} cloned_frame_obj;




//-----------------------------------------------------------------------------
//  Forward declarations
//-----------------------------------------------------------------------------

static frame_obj*  cl_frame_create              ();
static size_t      cl_frame_get_size            (frame_obj* frame);
static INT64_T     cl_frame_get_pts             (frame_obj* frame);
static INT64_T     cl_frame_get_dts             (frame_obj* frame);
static int         cl_frame_set_pts             (frame_obj* frame, INT64_T pts);
static int         cl_frame_set_dts             (frame_obj* frame, INT64_T dts);
static size_t      cl_frame_get_width           (frame_obj* frame);
static int         cl_frame_set_width           (frame_obj* frame, size_t size);
static size_t      cl_frame_get_height          (frame_obj* frame);
static int         cl_frame_set_height          (frame_obj* frame, size_t height);
static int         cl_frame_get_pixel_format    (frame_obj* frame);
static int         cl_frame_set_pixel_format    (frame_obj* frame, int pixfmt);
static int         cl_frame_get_media_type      (frame_obj* frame);
static int         cl_frame_set_media_type      (frame_obj* frame, int type);
static const void* cl_frame_get_data            (frame_obj* frame);
static void*       cl_frame_get_buffer          (frame_obj* frame, size_t size);
static int         cl_frame_get_keyframe_flag   (frame_obj* frame);
static int         cl_frame_set_keyframe_flag   (frame_obj* frame, int flag);
static void*       cl_frame_get_user_context    (frame_obj* frame);
static int         cl_frame_set_user_context    (frame_obj* frame, void* ctx);
static void*       cl_frame_get_backing_obj     (frame_obj* frame, const char* objType);
static int         cl_frame_set_backing_obj     (frame_obj* frame, const char* objType, void* obj);
static void        cl_frame_destroy             (frame_obj* frame);


//-----------------------------------------------------------------------------
// Frame API. To begin with, we only use it to access the data;
// however can be extended to access/provide metadata about the frame
//-----------------------------------------------------------------------------
#define DECLARE_FRAME(param, name, errval) \
    DECLARE_OBJ(cloned_frame_obj, name,  param, FRAME_CLONE_MAGIC, errval)
#define DECLARE_FRAME_V(param, name) \
    DECLARE_OBJ_V(cloned_frame_obj, name,  param, FRAME_CLONE_MAGIC)

//-----------------------------------------------------------------------------
//  API
//-----------------------------------------------------------------------------

frame_api_t _g_cl_frame_frame_provider = {
    cl_frame_create,
    cl_frame_get_size,
    cl_frame_get_pts,
    cl_frame_set_pts,
    cl_frame_get_dts,
    cl_frame_set_dts,
    cl_frame_get_width,
    cl_frame_set_width,
    cl_frame_get_height,
    cl_frame_set_height,
    cl_frame_get_pixel_format,
    cl_frame_set_pixel_format,
    cl_frame_get_media_type,
    cl_frame_set_media_type,
    cl_frame_get_data,
    cl_frame_get_buffer,
    cl_frame_get_keyframe_flag,
    cl_frame_set_keyframe_flag,
    cl_frame_get_user_context,
    cl_frame_set_user_context,
    cl_frame_get_backing_obj,
    cl_frame_set_backing_obj,
    NULL,
    NULL,
};


//-----------------------------------------------------------------------------
static frame_obj*   cl_frame_create              ()
{
    cloned_frame_obj* res = (cloned_frame_obj*)frame_init(sizeof(cloned_frame_obj),
                    FRAME_CLONE_MAGIC,
                    &_g_cl_frame_frame_provider,
                    cl_frame_destroy );
    res->source = NULL;
    res->sourceApi = NULL;
    res->pts = INVALID_PTS;
    res->dts = INVALID_PTS;
    return (frame_obj*)res;
}

//-----------------------------------------------------------------------------
static size_t      cl_frame_get_size           (frame_obj* frame)
{
    DECLARE_FRAME(frame, cl_frame, -1);
    return cl_frame->sourceApi->get_data_size(cl_frame->source);
}

//-----------------------------------------------------------------------------
static INT64_T     cl_frame_get_pts          (frame_obj* frame)
{
    DECLARE_FRAME(frame, cl_frame, -1);
    if ( cl_frame->pts != INVALID_PTS )
        return cl_frame->pts;
    return cl_frame->sourceApi->get_pts(cl_frame->source);
}

//-----------------------------------------------------------------------------
static INT64_T     cl_frame_get_dts          (frame_obj* frame)
{
    DECLARE_FRAME(frame, cl_frame, -1);
    if ( cl_frame->dts != INVALID_PTS )
        return cl_frame->dts;
    return cl_frame->sourceApi->get_pts(cl_frame->source);
}

//-----------------------------------------------------------------------------
static int         cl_frame_set_pts          (frame_obj* frame, INT64_T pts)
{
    DECLARE_FRAME(frame, cl_frame, -1);
    cl_frame->pts = pts;
    return 0;
}

//-----------------------------------------------------------------------------
static int         cl_frame_set_dts          (frame_obj* frame, INT64_T dts)
{
    DECLARE_FRAME(frame, cl_frame, -1);
    cl_frame->dts = dts;
    return 0;
}

//-----------------------------------------------------------------------------
static size_t      cl_frame_get_width           (frame_obj* frame)
{
    DECLARE_FRAME(frame, cl_frame, -1);
    return cl_frame->sourceApi->get_width(cl_frame->source);
}

//-----------------------------------------------------------------------------
static int         cl_frame_set_width           (frame_obj* frame, size_t size)
{
    return -1;
}

//-----------------------------------------------------------------------------
static size_t      cl_frame_get_height          (frame_obj* frame)
{
    DECLARE_FRAME(frame, cl_frame, -1);
    return cl_frame->sourceApi->get_height(cl_frame->source);
}

//-----------------------------------------------------------------------------
static int         cl_frame_set_height          (frame_obj* frame, size_t height)
{
    return -1;
}

//-----------------------------------------------------------------------------
static int         cl_frame_get_pixel_format    (frame_obj* frame)
{
    DECLARE_FRAME(frame, cl_frame, -1);
    return cl_frame->sourceApi->get_pixel_format(cl_frame->source);
}

//-----------------------------------------------------------------------------
static int         cl_frame_set_pixel_format    (frame_obj* frame, int pixfmt)
{
    return -1;
}

//-----------------------------------------------------------------------------
static int         cl_frame_get_keyframe_flag   (frame_obj* frame)
{
    DECLARE_FRAME(frame, cl_frame, -1);
    return cl_frame->sourceApi->get_keyframe_flag(cl_frame->source);
}

//-----------------------------------------------------------------------------
static int         cl_frame_set_keyframe_flag   (frame_obj* frame, int flag)
{
    return -1;
}


//-----------------------------------------------------------------------------
static int         cl_frame_get_media_type    (frame_obj* frame)
{
    DECLARE_FRAME(frame, cl_frame, -1);
    return cl_frame->sourceApi->get_media_type(cl_frame->source);
}

//-----------------------------------------------------------------------------
static int         cl_frame_set_media_type    (frame_obj* frame, int type)
{
    return -1;
}

//-----------------------------------------------------------------------------
static const void* cl_frame_get_data           (frame_obj* frame)
{
    DECLARE_FRAME(frame, cl_frame, NULL);
    return cl_frame->sourceApi->get_data(cl_frame->source);
}

//-----------------------------------------------------------------------------
static void*       cl_frame_get_buffer          (frame_obj* frame, size_t size)
{
    DECLARE_FRAME(frame, cl_frame, NULL);
    return cl_frame->sourceApi->get_buffer(cl_frame->source, size);
}


//-----------------------------------------------------------------------------
static void*       cl_frame_get_user_context    (frame_obj* frame)
{
    DECLARE_FRAME(frame, cl_frame, NULL);
    return cl_frame->sourceApi->get_user_context(cl_frame->source);
}

//-----------------------------------------------------------------------------
static int         cl_frame_set_user_context    (frame_obj* frame, void* ctx)
{
    return -1;
}

//-----------------------------------------------------------------------------
static void*       cl_frame_get_backing_obj     (frame_obj* frame,
                                                const char* objType)
{
    DECLARE_FRAME(frame, cl_frame, NULL);
    if ( objType ) {
        if ( !_stricmp(objType,"cloneParent")) {
            return cl_frame->source;
        }
    }
    if ( cl_frame ){
        return cl_frame->sourceApi->get_backing_obj(cl_frame->source, objType);
    }
    return NULL;
}

//-----------------------------------------------------------------------------
static int         cl_frame_set_backing_obj     (frame_obj* frame,
                                                const char* objType,
                                                void* obj)
{

    DECLARE_FRAME(frame, cl_frame, -1);
    if ( objType ) {
        if ( !_stricmp(objType,"cloneParent")) {
            frame_unref(&cl_frame->source);
            cl_frame->source = (frame_obj*)obj;
            frame_ref(cl_frame->source);
            cl_frame->sourceApi = frame_get_api(cl_frame->source);
        }
    }
    if ( cl_frame ){
        return cl_frame->sourceApi->set_backing_obj( frame, objType, obj);
    }
    return -1;
}

//-----------------------------------------------------------------------------
static void  cl_frame_destroy                  (frame_obj* frame)
{
    DECLARE_FRAME_V(frame, cl_frame);
    frame_unref(&cl_frame->source);
    sv_freep( &cl_frame );
}

//-----------------------------------------------------------------------------
static void  cl_frame_reset                     (frame_obj* frame)
{
    DECLARE_FRAME_V(frame, cl_frame);
    frame_unref(&cl_frame->source);
    cl_frame->sourceApi = NULL;
    cl_frame->pts = INVALID_PTS;
    cl_frame->dts = INVALID_PTS;
}

//-----------------------------------------------------------------------------
extern "C" frame_api_t*    get_clone_frame_api()
{
    return &_g_cl_frame_frame_provider;
}


//-----------------------------------------------------------------------------
static const int _g_cloneFramesPoolEnabled = 0;

SVCORE_API
frame_obj*  alloc_clone_frame            (int ownerTag,
                                            frame_allocator* fa,
                                            frame_obj* original,
                                            INT64_T ptsOverride )
{
    cloned_frame_obj* res = NULL;
    int              pooled = 0;
    if ( _g_cloneFramesPoolEnabled && fa ) {
        res = (cloned_frame_obj*)frame_allocator_get(fa);
        pooled = 1;
    }

    if (!res) {
        res = (cloned_frame_obj*)cl_frame_create();
        if (res) {
            res->ownerTag = ownerTag;
            res->next = NULL;
            res->resetter = cl_frame_reset;

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

    frame_obj* f = (frame_obj*)res;
    cl_frame_set_pts(f, ptsOverride);
    cl_frame_set_backing_obj(f, "cloneParent", original);
    return f;
}




