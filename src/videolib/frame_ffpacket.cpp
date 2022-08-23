/*****************************************************************************
 *
 * frame_ffpacket.cpp
 *   Wrapper around ffmpeg packet object.
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

#define FRAME_FFPACKET_MAGIC 0x9000



typedef struct ffmpeg_frame : public frame_base  {
    int                 mediaType;
    void*               userContext;
    AVPacket            packet;
} ffmpeg_frame_obj;


//-----------------------------------------------------------------------------
//  Forward declarations
//-----------------------------------------------------------------------------

static frame_obj*  ff_frame_create              ();
static size_t      ff_frame_get_size            (frame_obj* frame);
static INT64_T     ff_frame_get_pts             (frame_obj* frame);
static INT64_T     ff_frame_get_dts             (frame_obj* frame);
static int         ff_frame_set_pts             (frame_obj* frame, INT64_T pts);
static int         ff_frame_set_dts             (frame_obj* frame, INT64_T dts);
static int         ff_frame_get_media_type      (frame_obj* frame);
static int         ff_frame_set_media_type      (frame_obj* frame, int type);
static const void* ff_frame_get_data            (frame_obj* frame);
static int         ff_frame_set_pts             (frame_obj* frame, INT64_T pts);
static int         ff_frame_set_dts             (frame_obj* frame, INT64_T dts);
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
    DECLARE_OBJ(ffmpeg_frame_obj, name,  param, FRAME_FFPACKET_MAGIC, errval)
#define DECLARE_FRAME_V(param, name) \
    DECLARE_OBJ_V(ffmpeg_frame_obj, name,  param, FRAME_FFPACKET_MAGIC)

//-----------------------------------------------------------------------------
//  API
//-----------------------------------------------------------------------------

frame_api_t _g_ff_packet_frame_provider = {
    ff_frame_create,
    ff_frame_get_size,
    ff_frame_get_pts,
    ff_frame_set_pts,
    ff_frame_get_dts,
    ff_frame_set_dts,
    NULL, // get_width,
    NULL, // set_width,
    NULL, // get_height,
    NULL, // set_height,
    NULL, // get_pixel_format,
    NULL, // set_pixel_format,
    ff_frame_get_media_type,
    ff_frame_set_media_type,
    ff_frame_get_data,
    NULL, // get_buffer,
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
                    FRAME_FFPACKET_MAGIC,
                    &_g_ff_packet_frame_provider,
                    ff_frame_destroy );
    res->mediaType = mediaUnknown;
    res->userContext = NULL;
    av_init_packet(&res->packet);
    res->packet.data = NULL;
    res->packet.size = 0;
    return (frame_obj*)res;
}

//-----------------------------------------------------------------------------
static size_t      ff_frame_get_size           (frame_obj* frame)
{
    DECLARE_FRAME(frame, ff_frame, -1);
    return ff_frame->packet.size;
}

//-----------------------------------------------------------------------------
static INT64_T     ff_frame_get_pts          (frame_obj* frame)
{
    DECLARE_FRAME(frame, ff_frame, -1);
    return ff_frame->packet.pts;
}

//-----------------------------------------------------------------------------
static INT64_T     ff_frame_get_dts          (frame_obj* frame)
{
    DECLARE_FRAME(frame, ff_frame, -1);
    return ff_frame->packet.dts;
}

//-----------------------------------------------------------------------------
static int         ff_frame_set_pts          (frame_obj* frame, INT64_T pts)
{
    DECLARE_FRAME(frame, ff_frame, -1);
    ff_frame->packet.pts = pts;
    return 0;
}

//-----------------------------------------------------------------------------
static int         ff_frame_set_dts          (frame_obj* frame, INT64_T dts)
{
    DECLARE_FRAME(frame, ff_frame, -1);
    ff_frame->packet.dts = dts;
    return 0;
}

//-----------------------------------------------------------------------------
static int         ff_frame_get_keyframe_flag   (frame_obj* frame)
{
    DECLARE_FRAME(frame, ff_frame, -1);
    return (ff_frame->packet.flags & AV_PKT_FLAG_KEY);
}

//-----------------------------------------------------------------------------
static int         ff_frame_set_keyframe_flag   (frame_obj* frame, int flag)
{
    DECLARE_FRAME(frame, ff_frame, -1);
    if ( flag )
        ff_frame->packet.flags |= AV_PKT_FLAG_KEY;
    else
        ff_frame->packet.flags = 0;
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
static const void* ff_frame_get_data           (frame_obj* frame)
{
    DECLARE_FRAME(frame, ff_frame, NULL);
    return ff_frame->packet.data;
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
    if ( objType && !_stricmp(objType,"avpacket")) {
        return &ff_frame->packet;
    }
    return NULL;
}

//-----------------------------------------------------------------------------
static int         ff_frame_set_backing_obj     (frame_obj* frame,
                                                const char* objType,
                                                void* obj)
{
    // No setter for this object. Remains to be seen if we need a setter at all.
    return -1;
}

//-----------------------------------------------------------------------------
static void  ff_frame_destroy                  (frame_obj* frame)
{
    DECLARE_FRAME_V(frame, ff_frame);
    av_packet_unref( &ff_frame->packet );
    sv_freep( &ff_frame );
}

//-----------------------------------------------------------------------------
SVVIDEOLIB_API frame_api_t*    get_ffpacket_frame_api()
{
    return &_g_ff_packet_frame_provider;
}

