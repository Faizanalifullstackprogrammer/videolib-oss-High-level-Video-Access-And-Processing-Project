/*****************************************************************************
 *
 * frame_basic.cpp
 *   Basic frame object around a buffer.
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

#undef SV_MODULE_NAME
#define SV_MODULE_NAME "framebasic"


#include "sv_os.h"
#include "streamprv.h"
#include "frame_basic.h"

#include <set>
#include <cmath>
#include <algorithm>


static int _g_basicFramesAllocated = 0;
static int _g_basicFramesPoolEnabled = 1;
static int _g_basicFramesLeakDetection = 0;

static std::set<frame_obj*> allocatedFrames;



//-----------------------------------------------------------------------------
// Frame API. To begin with, we only use it to access the data;
// however can be extended to access/provide metadata about the frame
//-----------------------------------------------------------------------------

//-----------------------------------------------------------------------------
//  Forward declarations
//-----------------------------------------------------------------------------

static frame_obj*  basic_frame_create              ();
static size_t      basic_frame_get_size            (frame_obj* frame);
static size_t      basic_frame_get_width           (frame_obj* frame);
static int         basic_frame_set_width           (frame_obj* frame, size_t size);
static INT64_T     basic_frame_get_pts             (frame_obj* frame);
static INT64_T     basic_frame_get_dts             (frame_obj* frame);
static int         basic_frame_set_pts             (frame_obj* frame, INT64_T pts);
static int         basic_frame_set_dts             (frame_obj* frame, INT64_T dts);
static size_t      basic_frame_get_height          (frame_obj* frame);
static int         basic_frame_set_height          (frame_obj* frame, size_t height);
static int         basic_frame_get_pixel_format    (frame_obj* frame);
static int         basic_frame_set_pixel_format    (frame_obj* frame, int pixfmt);
static int         basic_frame_get_media_type      (frame_obj* frame);
static int         basic_frame_set_media_type      (frame_obj* frame, int mediaType);
static int         basic_frame_get_keyframe_flag   (frame_obj* frame);
static int         basic_frame_set_keyframe_flag   (frame_obj* frame, int flag);
static const void* basic_frame_get_data            (frame_obj* frame);
static void*       basic_frame_get_buffer          (frame_obj* frame, size_t size);
static void*       basic_frame_get_user_context    (frame_obj* frame);
static int         basic_frame_set_user_context    (frame_obj* frame, void* ctx);
void*              basic_frame_get_backing_obj     (frame_obj* frame, const char* objType);
int                basic_frame_set_backing_obj     (frame_obj* frame, const char* objType, void* obj);
#if ENABLE_SERIALIZATION
static int         basic_frame_serialize           (frame_obj* frame, const char* location);
static int         basic_frame_deserialize         (frame_obj* frame, const char* location);
#endif
static void        basic_frame_destroy             (frame_obj* frame);
static int         _alloc_basic_frame_mem          (basic_frame_obj* frame,
                                                    size_t desiredSize);


//-----------------------------------------------------------------------------
//  API
//-----------------------------------------------------------------------------

frame_api_t _g_basic_frame_provider = {
    basic_frame_create,
    basic_frame_get_size,
    basic_frame_get_pts,
    basic_frame_set_pts,
    basic_frame_get_dts,
    basic_frame_set_dts,
    basic_frame_get_width,
    basic_frame_set_width,
    basic_frame_get_height,
    basic_frame_set_height,
    basic_frame_get_pixel_format,
    basic_frame_set_pixel_format,
    basic_frame_get_media_type,
    basic_frame_set_media_type,
    basic_frame_get_data,
    basic_frame_get_buffer,
    basic_frame_get_keyframe_flag,
    basic_frame_set_keyframe_flag,
    basic_frame_get_user_context,
    basic_frame_set_user_context,
    basic_frame_get_backing_obj,
    basic_frame_set_backing_obj,
#if ENABLE_SERIALIZATION
    basic_frame_serialize,
    basic_frame_deserialize
#else
    NULL,
    NULL
#endif
} ;

//-----------------------------------------------------------------------------
SVCORE_API frame_api_t*      get_basic_frame_api              ()
{
    return &_g_basic_frame_provider;
}

//-----------------------------------------------------------------------------
#if DEBUG_FRAME_ALLOC>0
int frame_allocator_get_free(frame_allocator* fa)
{
    int freect;
    int allocct;
    frame_allocator_get_stats(fa, &freect, &allocct);
    return freect;
}
#endif

//-----------------------------------------------------------------------------
static frame_obj*   basic_frame_create              ()
{
    basic_frame_obj* res = (basic_frame_obj*)frame_init(sizeof(basic_frame_obj),
                                    BASIC_STREAM_MAGIC,
                                    &_g_basic_frame_provider,
                                    basic_frame_destroy);
    res->width = 0;
    res->height = 0;
    res->pts = INVALID_PTS;
    res->dts = INVALID_PTS;
    res->pixelFormat = pfmtUndefined;
    res->mem = NULL;
    res->data = NULL;
    res->dataSize = 0;
    res->allocSize = 0;
    res->keyframe = 0;
    res->mediaType = mediaUnknown;
    res->fa = NULL;
    res->next = NULL;
    res->userContext = NULL;
    res->srcFrame = NULL;
#if ENABLE_SERIALIZATION
    res->serializationLocation = NULL;
#endif
    _g_basicFramesAllocated++;

    return (frame_obj*)res;
}

//-----------------------------------------------------------------------------
static size_t      basic_frame_get_size           (frame_obj* frame)
{
    DECLARE_FRAME(frame, basic_frame, -1);
    return basic_frame->dataSize;
}

//-----------------------------------------------------------------------------
static size_t      basic_frame_get_width          (frame_obj* frame)
{
    DECLARE_FRAME(frame, basic_frame, -1);
    return basic_frame->width;
}

//-----------------------------------------------------------------------------
static int         basic_frame_set_width          (frame_obj* frame, size_t w)
{
    DECLARE_FRAME(frame, basic_frame, -1);
    basic_frame->width = w;
    return 0;
}

//-----------------------------------------------------------------------------
static INT64_T     basic_frame_get_pts          (frame_obj* frame)
{
    DECLARE_FRAME(frame, basic_frame, -1);
    return basic_frame->pts;
}

//-----------------------------------------------------------------------------
static INT64_T     basic_frame_get_dts          (frame_obj* frame)
{
    DECLARE_FRAME(frame, basic_frame, -1);
    return basic_frame->dts;
}

//-----------------------------------------------------------------------------
static int         basic_frame_set_pts          (frame_obj* frame, INT64_T pts)
{
    DECLARE_FRAME(frame, basic_frame, -1);
    basic_frame->pts = pts;
    return 0;
}

//-----------------------------------------------------------------------------
static int         basic_frame_set_dts          (frame_obj* frame, INT64_T dts)
{
    DECLARE_FRAME(frame, basic_frame, -1);
    basic_frame->dts = dts;
    return 0;
}

//-----------------------------------------------------------------------------
static size_t      basic_frame_get_height         (frame_obj* frame)
{
    DECLARE_FRAME(frame, basic_frame, -1);
    return basic_frame->height;
}

//-----------------------------------------------------------------------------
static int         basic_frame_set_height         (frame_obj* frame, size_t h)
{
    DECLARE_FRAME(frame, basic_frame, -1);
    basic_frame->height = h;
    return 0;
}

//-----------------------------------------------------------------------------
static int         basic_frame_get_pixel_format   (frame_obj* frame)
{
    DECLARE_FRAME(frame, basic_frame, pfmtUndefined);
    return basic_frame->pixelFormat;
}

//-----------------------------------------------------------------------------
static int         basic_frame_set_pixel_format   (frame_obj* frame, int pfmt)
{
    DECLARE_FRAME(frame, basic_frame, pfmtUndefined);
    basic_frame->pixelFormat = pfmt;
    return 0;
}

//-----------------------------------------------------------------------------
static int         basic_frame_get_media_type    (frame_obj* frame)
{
    DECLARE_FRAME(frame, basic_frame, -1);
    return basic_frame->mediaType;
    return 0;
}

//-----------------------------------------------------------------------------
static int         basic_frame_set_media_type    (frame_obj* frame, int type)
{
    DECLARE_FRAME(frame, basic_frame, -1);
    basic_frame->mediaType = type;
    return 0;
}


//-----------------------------------------------------------------------------
static int         basic_frame_get_keyframe_flag   (frame_obj* frame)
{
    DECLARE_FRAME(frame, basic_frame, -1);
    return basic_frame->keyframe;
}

//-----------------------------------------------------------------------------
static int         basic_frame_set_keyframe_flag   (frame_obj* frame, int flag)
{
    DECLARE_FRAME(frame, basic_frame, -1);
    basic_frame->keyframe = flag;
    return 0;
}

//-----------------------------------------------------------------------------
static const void* basic_frame_get_data           (frame_obj* frame)
{
    DECLARE_FRAME(frame, basic_frame, NULL);
    return basic_frame->data;
}

//-----------------------------------------------------------------------------
static void*       basic_frame_get_buffer         (frame_obj* frame, size_t size)
{
    // not implemented for writing
    return NULL;
}

//-----------------------------------------------------------------------------
static int         basic_frame_set_user_context    (frame_obj* frame, void* ctx)
{
    DECLARE_FRAME(frame, basic_frame, -1);
    basic_frame->userContext = ctx;
    return 0;
}

//-----------------------------------------------------------------------------
static void*       basic_frame_get_user_context    (frame_obj* frame)
{
    DECLARE_FRAME(frame, basic_frame, NULL);
    return basic_frame->userContext;
}

//-----------------------------------------------------------------------------
void*              basic_frame_get_backing_obj     (frame_obj* frame,
                                                    const char* objType)
{
    DECLARE_FRAME(frame, basic_frame, NULL);
    if ( objType ) {
        if ( !_stricmp(objType,"srcFrame")) {
            return basic_frame->srcFrame;
        }
    }
    return NULL;
}

//-----------------------------------------------------------------------------
int                basic_frame_set_backing_obj     (frame_obj* frame,
                                                    const char* objType,
                                                    void* obj)
{
    DECLARE_FRAME(frame, basic_frame, -1);
    if ( objType ) {
        if ( !_stricmp(objType, "srcFrame") ) {
            frame_unref(&basic_frame->srcFrame);
            basic_frame->srcFrame = (frame_obj*)obj;
            frame_ref(basic_frame->srcFrame);
            return 0;
        }
    }
    return -1;
}

//-----------------------------------------------------------------------------
#if ENABLE_SERIALIZATION
static int         basic_frame_serialize           (frame_obj* frame,
                                                    const char* location)
{
    DECLARE_FRAME(frame, bf, -1);
    int   res = -1;

    FILE* f = fopen(location, "w+b");

    if ( f ) {
        res = fwrite( bf->data, 1, bf->dataSize, f );
        fflush (f);
        fclose(f);

        if ( res == bf->dataSize ) {
            bf->serializationLocation = strdup(location);
            if (bf->serializationLocation) {
                sv_freep(&bf->data);
                res = 0;
            }
        }

        if (res<0) {
            remove(location);
        }
    }

    return res;
}

//-----------------------------------------------------------------------------
static int         basic_frame_deserialize         (frame_obj* frame,
                                                    const char* location)
{
    DECLARE_FRAME(frame, bf, -1);
    int res = 0;
    FILE* f = NULL;

    if (!bf->serializationLocation ||
        bf->data != NULL ||
        bf->dataSize > bf->allocSize ||
        bf->dataSize <= 0 ) {
        res = -1;
    }

    if ( res >= 0 ) {
        bf->data = (uint8_t*)malloc(bf->allocSize);
        if (!bf->data) {
            res = -1;
        }
    }

    if ( res >= 0 ) {
        f = fopen(location, "r+b");
    }

    if ( f ) {
        res = fread( bf->data, 1, bf->dataSize, f );
        fclose(f);
        res = ( res != bf->dataSize ) ? -1 : 0;
    } else {
        res = -1;
    }

    if ( res == 0 ) {
        remove(bf->serializationLocation);
        sv_freep(&bf->serializationLocation);
    }

    return res;
}
#endif

//-----------------------------------------------------------------------------
static void  basic_frame_destroy          (frame_obj* frame)
{
    DECLARE_FRAME_V(frame, basic_frame);
#if DEBUG_FRAME_ALLOC>0
    int             ownerTag = basic_frame->ownerTag;
    fn_stream_log   logCb = basic_frame->logCb;
    INT64_T         pts = basic_frame->pts;
#endif
    frame_unref(&basic_frame->srcFrame);
    if ( basic_frame->fa ) {
#if DEBUG_FRAME_ALLOC<2
        if (_g_basicFramesAllocated>=20 && (_g_basicFramesAllocated%10)==0) {
#endif
#if DEBUG_FRAME_ALLOC>0
            logCb(logInfo, _FMT("Freed basic frame " << basic_frame <<
                            " to alloc: count=" << _g_basicFramesAllocated <<
                            " fa->name=" << frame_allocator_get_name(basic_frame->fa) <<
                            " fa->count=" << frame_allocator_get_free(basic_frame->fa) <<
                            " pts=" << pts << " tag=0x" << std::setbase(16) << ownerTag));
#endif
#if DEBUG_FRAME_ALLOC<2
        }
#endif
        basic_frame->dataSize = 0;
        basic_frame->pts = INVALID_PTS;
        basic_frame->dts = INVALID_PTS;
        frame_allocator_return(basic_frame->fa, basic_frame);
    } else {
#if DEBUG_FRAME_ALLOC>0
        int     refcount=basic_frame->refcount;
        void*   fp = basic_frame;
#endif
#if ENABLE_SERIALIZATION
        sv_freep(&basic_frame->serializationLocation);
#endif
        sv_freep(&basic_frame->mem);
        sv_freep(&basic_frame);
        _g_basicFramesAllocated--;
#if DEBUG_FRAME_ALLOC<2
        if (_g_basicFramesAllocated>=20 && (_g_basicFramesAllocated%10)==0) {
#endif
#if DEBUG_FRAME_ALLOC>0
            logCb(logInfo, _FMT("Freed basic frame " << fp << ": count=" << _g_basicFramesAllocated <<
                            " pts=" << pts << " tag=0x" << std::setbase(16) << ownerTag <<
                            " refcount=" << refcount));
#endif
#if DEBUG_FRAME_ALLOC<2
        }
#endif
        if ( _g_basicFramesLeakDetection ) {
            allocatedFrames.erase(frame);
        }
    }

}


static const int kOverallocateBy = 32;

//-----------------------------------------------------------------------------
static void reset_basic_frame(frame_obj* frame)
{
    basic_frame_obj* bf = (basic_frame_obj*)frame;
    bf->userContext = NULL;
    frame_unref(&bf->srcFrame);
}

//-----------------------------------------------------------------------------
SVCORE_API basic_frame_obj*  clone_basic_frame  (basic_frame_obj* src,
                                                 fn_stream_log logCb,
                                                 frame_allocator* bfa)
{
    basic_frame_obj* res = alloc_basic_frame2(src->ownerTag, src->dataSize, logCb, bfa);
    if ( res ) {
        res->width = src->width;
        res->height = src->height;
        res->pixelFormat = src->pixelFormat;
        memcpy(res->data, src->data, src->dataSize);
        res->dataSize = src->dataSize;
        res->pts = src->pts;
        res->dts = src->dts;
        res->keyframe = src->keyframe;
        res->mediaType = src->mediaType;
        res->userContext = src->userContext;
        res->srcFrame = src->srcFrame;
        frame_ref(res->srcFrame);
    #if ENABLE_SERIALIZATION
        if ( src->serializationLocation ) res->serializationLocation = strdup(src->serializationLocation);
    #endif
    #if DEBUG_FRAME_ALLOC>0
        res->logCb = src->logCb;
    #endif
    }
    return res;
}

//-----------------------------------------------------------------------------
SVCORE_API basic_frame_obj* alloc_basic_frame        (int ownerTag,
                                                      size_t desiredSize,
                                                      fn_stream_log logCb)
{
    basic_frame_obj* ff = (basic_frame_obj*)get_basic_frame_api()->create();
    if ( ff ) {
        _alloc_basic_frame_mem(ff, desiredSize);
        ff->resetter = reset_basic_frame;
#if DEBUG_FRAME_ALLOC>0
        ff->logCb = logCb;
#endif
        if ( ff->mem ) {
            ff->dataSize = 0;
            ff->pts = INVALID_PTS;
            ff->dts = INVALID_PTS;
            ff->ownerTag = ownerTag;
            ff->refcount = 1;
#if DEBUG_FRAME_ALLOC<2
            if (_g_basicFramesAllocated>=20 && (_g_basicFramesAllocated%10)==0) {
#endif
#if DEBUG_FRAME_ALLOC>0
                logCb(logInfo, _FMT("Allocated new basic frame " << ff << ": count=" << _g_basicFramesAllocated << " tag=0x" << std::setbase(16) << ownerTag));
#endif
#if DEBUG_FRAME_ALLOC<2
            }
#endif
            if ( _g_basicFramesLeakDetection ) {
                allocatedFrames.insert((frame_obj*)ff);
            }
        } else {
            logCb(logError, _FMT("Failed to allocate frame; size=" << desiredSize ));
            frame_ref((frame_obj*)ff);
            frame_unref((frame_obj**)&ff);
        }
    }
    return ff;
}

//-----------------------------------------------------------------------------
SVCORE_API basic_frame_obj*  alloc_basic_frame2 (int ownerTag,
                                                 size_t desiredSize,
                                                 fn_stream_log logCb,
                                                 frame_allocator* fa)
{
    basic_frame_obj* res = NULL;
    int              pooled = 0;
    if ( _g_basicFramesPoolEnabled && fa ) {
        res = (basic_frame_obj*)frame_allocator_get(fa);
        pooled = 1;
    }

    if (!res) {
        res = alloc_basic_frame(ownerTag, desiredSize, logCb);
        if (res && pooled) {
            frame_allocator_register_frame(fa, res);
        }
    } else {
#if DEBUG_FRAME_ALLOC>0
        res->logCb = logCb;
#endif

#if DEBUG_FRAME_ALLOC<2
        if (_g_basicFramesAllocated>=20 && (_g_basicFramesAllocated%10)==0) {
#endif
#if DEBUG_FRAME_ALLOC>0
            logCb(logInfo, _FMT("Got basic frame " << res << " from alloc: count=" << _g_basicFramesAllocated <<
                            " fa->name=" << frame_allocator_get_name(fa) <<
                            " fa->count=" << frame_allocator_get_free(fa) <<
                            " tag=0x" << std::setbase(16) << ownerTag));
#endif
#if DEBUG_FRAME_ALLOC<2
        }
#endif

        if ( res->refcount != 0 ) {
            // we have seen this error condition at least once
            logCb(logError, _FMT("Received frame with a refcount=" <<
                    res->refcount <<
                    " fa=" << (fa ? frame_allocator_get_name(fa) : "[none]") <<
                    " tag=0x" << std::setbase(16) << ownerTag <<
                    " size=" << res->allocSize <<
                    " desiredSize=" << desiredSize <<
                    " pooled=" << pooled ) );
            assert( false );
        }

        res->refcount = 1;
        if (res->allocSize < desiredSize || res->mem == NULL ) {
            grow_basic_frame( res, desiredSize, 0 );
        }
    }
    return res;
}

//-----------------------------------------------------------------------------
static int      _alloc_basic_frame_mem          (basic_frame_obj* frame,
                                                 size_t desiredSize)
{
    // we overallocate by FF_INPUT_BUFFER_PADDING_SIZE .. this should come handy, if input needs to be padded for ffmpeg
    // but even if not, there are cases where it'll help with off-by-one errors in ffmpeg
    // (for example, see https://trac.ffmpeg.org/ticket/5886)
    frame->mem = (uint8_t*)malloc(desiredSize+kOverallocateBy+16);
    if (!frame->mem)
        return -1;
    frame->data = (uint8_t*)(((uintptr_t)frame->mem+15) & ~ (uintptr_t)0x0F);
    frame->allocSize = desiredSize;
    memset(&frame->data[desiredSize], 0, kOverallocateBy);
    return 0;
}

//-----------------------------------------------------------------------------
SVCORE_API int               grow_basic_frame   (basic_frame_obj* frame,
                                                 size_t desiredSize,
                                                 int keepData)
{
    if ( desiredSize <= frame->allocSize )
        return 0;

    uint8_t* memBak = frame->mem;
    uint8_t* dataBak = frame->data;
    size_t   dataSizeBak = frame->dataSize;

    if ( _alloc_basic_frame_mem( frame, desiredSize ) < 0 ) {
        frame->mem = memBak;
        return -1;
    }
    if ( keepData ) {
        memmove(frame->data, dataBak, dataSizeBak);
        frame->dataSize = dataSizeBak;
    } else {
        frame->dataSize = 0;
    }
    if ( memBak ) {
        free( memBak );
    };
    return 0;
}

//-----------------------------------------------------------------------------
SVCORE_API size_t    get_basic_frame_free_space    (basic_frame_obj* frame)
{
    return frame->allocSize - frame->dataSize;
}

//-----------------------------------------------------------------------------
SVCORE_API int    ensure_basic_frame_free_space (basic_frame_obj* frame,
                                                 size_t additionalSizeRequired)
{
    static const int kReallocGrowFactor = 5; // grow by 1/5th of the current size

    if ( get_basic_frame_free_space(frame) < additionalSizeRequired ) {
        size_t newSize = frame->allocSize +
                    std::max(frame->allocSize/kReallocGrowFactor, additionalSizeRequired);
        if ( grow_basic_frame(frame, newSize, 1) < 0 ) {
            return -1;
        }
    }
    return 0;
}

//-----------------------------------------------------------------------------
SVCORE_API int    append_basic_frame            (basic_frame_obj* frame,
                                                 uint8_t* data,
                                                 size_t size)
{
    if ( ensure_basic_frame_free_space(frame, size) < 0 ) {
        return -1;
    }
    memcpy(&frame->data[frame->dataSize], data, size);
    frame->dataSize += size;
    return 0;
}

//-----------------------------------------------------------------------------
int               get_basic_frame_count                   ()
{
    return _g_basicFramesAllocated;
}

//-----------------------------------------------------------------------------
SVCORE_API int  enable_basic_frame_alloc_pool           (int enable)
{
    if ( _g_basicFramesAllocated > 0 )
        return -1;
    _g_basicFramesPoolEnabled = enable;
    return 0;
}

//-----------------------------------------------------------------------------
int               enable_frame_leak_detection             (int enable)
{
    if ( _g_basicFramesAllocated > 0 )
        return -1;
    _g_basicFramesLeakDetection = enable;
    return 0;
}

//-----------------------------------------------------------------------------
void              print_allocated_frames                  (fn_stream_log log)
{
    std::set<frame_obj*>::iterator it = allocatedFrames.begin();
    while ( it != allocatedFrames.end() ) {
        basic_frame_obj* f = (basic_frame_obj*)*it;
        log(logInfo, _FMT("Outstanding frame:" <<
                " tag=0x" << std::setbase(16) << f->ownerTag << std::setbase(10) <<
                " pts=" << f->pts <<
                " size=" << f->dataSize <<
                " fa=" << (f->fa?frame_allocator_get_name(f->fa):"NULL")));
        it++;
    }
}
