/*****************************************************************************
 *
 * stream_jitter_buffer.cpp
 *   Node synchronizing audio and video frames. Used mostly in context of
 *   generating HLS output, as iOS is somewhat finicky, when A/V timestamps are
 *   too far apart.
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
#define SV_MODULE_VAR impl
#define SV_MODULE_ID "JITTERBUFFER"
#include "sv_module_def.hpp"

#include "streamprv.h"
#include "sv_ffmpeg.h"

#include "videolibUtils.h"
#include "frame_basic.h"

#include <algorithm>

#define JITBUF_STREAM_MAGIC 0x1718


//-----------------------------------------------------------------------------
typedef struct jitbuf_stream  : public stream_base {
    int         bufferTime;             // amount of time we want to buffer by default
    int         bufferTimeWhenPaused;   // amount of data we will retain when paused
    FrameList*  frameQueue;             // list of future frames
    FrameList*  pastFrameQueue;         // list of past frames
    int         jumpstartWithPastFrames;// allow old frames to jumpstart the stream after pausing it
    int         jumpstartFps;           // at which fps we store past frames
    int         targetFps;              // fps which we expect when the filter isn't paused
    int64_t     lastVideoPastFramePts;  // pts of the last video frame deposited into history buffer
    int64_t     lastVideoServedFramePts;// pts of the last video frame returned from the buffer
    int64_t     prebufferEndPts;        // pts of the last frame in buffer when pause state ended
    bool        determinedEncoderDelay;
    int         paused;                 // in paused mode, we retain a larger queue, and never return packets

    int         framesRead;             // frames we've read from backend
} jitbuf_stream_obj;


//-----------------------------------------------------------------------------
// Stream API
//-----------------------------------------------------------------------------

//-----------------------------------------------------------------------------
//  Forward declarations
//-----------------------------------------------------------------------------
static stream_obj* jitbuf_stream_create             (const char* name);
static int         jitbuf_stream_set_param          (stream_obj* stream,
                                                    const CHAR_T* name,
                                                    const void* value);
static int         jitbuf_stream_read_frame         (stream_obj* stream, frame_obj** frame);
static int         jitbuf_stream_close              (stream_obj* stream);
static void        jitbuf_stream_destroy            (stream_obj* stream);

static const int kDefaultBufferDuration = 300;

//-----------------------------------------------------------------------------
stream_api_t _g_jitbuf_stream_provider = {
    jitbuf_stream_create,
    get_default_stream_api()->set_source,
    get_default_stream_api()->set_log_cb,
    get_default_stream_api()->get_name,
    get_default_stream_api()->find_element,
    get_default_stream_api()->remove_element,
    get_default_stream_api()->insert_element,
    jitbuf_stream_set_param,
    get_default_stream_api()->get_param,
    get_default_stream_api()->open_in,
    get_default_stream_api()->seek,
    get_default_stream_api()->get_width,
    get_default_stream_api()->get_height,
    get_default_stream_api()->get_pixel_format,
    jitbuf_stream_read_frame,
    get_default_stream_api()->print_pipeline,
    jitbuf_stream_close,
    _set_module_trace_level
} ;


//-----------------------------------------------------------------------------
#define DECLARE_STREAM_FF(stream, name) \
    DECLARE_OBJ(jitbuf_stream, name,  stream, JITBUF_STREAM_MAGIC, -1)
#define DECLARE_STREAM_FF_V(stream, name) \
    DECLARE_OBJ_V(jitbuf_stream, name,  stream, JITBUF_STREAM_MAGIC)


static stream_obj*   jitbuf_stream_create                (const char* name)
{
    jitbuf_stream* res = (jitbuf_stream*)stream_init(sizeof(jitbuf_stream),
                                        JITBUF_STREAM_MAGIC,
                                        &_g_jitbuf_stream_provider,
                                        name,
                                        jitbuf_stream_destroy );
    res->bufferTime = kDefaultBufferDuration;
    res->bufferTimeWhenPaused = res->bufferTime;
    res->determinedEncoderDelay = false;
    res->paused = 0;
    res->jumpstartWithPastFrames = 0;
    res->jumpstartFps = 2;
    res->targetFps = 0;
    res->frameQueue = frame_list_create();
    res->pastFrameQueue = frame_list_create();
    res->lastVideoPastFramePts = 0;
    res->lastVideoServedFramePts = 0;
    res->prebufferEndPts = 0;
    res->framesRead = 0;
    return (stream_obj*)res;
}

//-----------------------------------------------------------------------------
static int         jitbuf_stream_set_param             (stream_obj* stream,
                                            const CHAR_T* name,
                                            const void* value)
{
    DECLARE_STREAM_FF(stream, impl);
    name = stream_param_name_apply_scope(stream, name);
    SET_PARAM_IF(stream, name, "bufferDuration", int, impl->bufferTime);
    SET_PARAM_IF(stream, name, "bufferDurationWhenPaused", int, impl->bufferTimeWhenPaused);
    SET_PARAM_IF(stream, name, "jumpstartWithPastFrames", int, impl->jumpstartWithPastFrames);
    SET_PARAM_IF(stream, name, "jumpstartFps", int, impl->jumpstartFps);
    SET_PARAM_IF(stream, name, "targetFps", int, impl->targetFps);
    SET_PARAM_IF(stream, name, "paused", int, impl->paused);
    if ( !_stricmp(name, "reset") ) {
        impl->determinedEncoderDelay = false;
        return 0;
    }
    return default_set_param(stream, name, value);
}

//-----------------------------------------------------------------------------
static void         _jitbuf_append          (jitbuf_stream_obj* impl, frame_obj* f)
{
    frame_api*      fapi1 = frame_get_api(f);
    INT64_T         pts = fapi1->get_pts(f);

    FrameIterator it = impl->frameQueue->end();
    while ( it != impl->frameQueue->begin() ) {
        it--;

        frame_obj*      curr = *it;
        frame_api*      currApi = frame_get_api(curr);
        INT64_T         ptsCurr = currApi->get_pts(curr);

        TRACE(_FMT("Checking queue position " << ptsCurr));
        if ( ptsCurr <= pts ) {
            TRACE(_FMT("Inserting " << pts << " after position " << ptsCurr));
            impl->frameQueue->insert(++it, f);
            return;
        }
    }

    TRACE(_FMT("Inserting " << pts << " at the beginning of the queue"));
    impl->frameQueue->push_front(f);
}

//-----------------------------------------------------------------------------
static void         _jitbuf_reduce         (jitbuf_stream_obj* impl, FrameList* q, INT64_T pts_tail)
{
     while ( !q->empty() ) {
        frame_obj*   f_head = q->front();
        frame_api_t* api = frame_get_api(f_head);
        INT64_T      pts_head = api->get_pts(f_head);

        if ( pts_tail - pts_head <= impl->bufferTimeWhenPaused ) {
            break;
        }

        frame_unref(&f_head);
        q->pop_front();
     }
}

//-----------------------------------------------------------------------------
static int          _jitbuf_save_frame_for_jumpstart(jitbuf_stream_obj* impl, frame_obj* f)
{
    frame_api_t* api = frame_get_api(f);

    int64_t pts = api->get_pts(f);
    bool isVideo = (api->get_media_type(f) == mediaVideo);

    bool shouldSave = ( impl->pastFrameQueue->empty() ||
         // keep all audio frames
         !isVideo ||
         // deposit first video frame we see
         impl->lastVideoPastFramePts == 0 ||
         // always start from the first frame in the second to sync across multiple jitbuf instances
         pts / 1000 != impl->lastVideoPastFramePts / 1000 ||
         // and finally, deposit when FPS allows
         pts - impl->lastVideoPastFramePts > 1000 / impl->jumpstartFps );

    if ( shouldSave ) {
        /*
        We are, essentially, creating new frames by re-injecting frames served in the past.
        Therefore, we take an extra reference, which will be released either by downstream pipeline,
        if this frame is ever returned, or by _jitbuf_reduce, like any other unused frame
        */
        frame_ref(f);
        impl->pastFrameQueue->push_back(f);
        if ( isVideo ) {
            impl->lastVideoPastFramePts = pts;
        }
        return 0;
    }
    return -1;
}

//-----------------------------------------------------------------------------
static frame_obj*  _jitbuf_generate_frame  ( jitbuf_stream_obj* impl )
{
    frame_obj*      res = impl->frameQueue->front();
    frame_api_t*    api = frame_get_api(res);
    int64_t         pts = api->get_pts(res);
    int64_t         diff = pts - impl->lastVideoServedFramePts;

    if ( api->get_media_type(res) == mediaVideo ) {
        if ( impl->lastVideoServedFramePts != 0 &&
             pts < impl->prebufferEndPts &&
             impl->targetFps != 0 &&
             diff > 1000/impl->targetFps ) {
            int64_t fakeFramePts = impl->lastVideoServedFramePts + 1000/impl->targetFps;
            res = alloc_clone_frame(JITBUF_STREAM_MAGIC, NULL, res, fakeFramePts);
            impl->lastVideoServedFramePts = fakeFramePts;
        } else {
            impl->lastVideoServedFramePts = pts;
        }
    }

    return res;
}

//-----------------------------------------------------------------------------
static int          _jitbuf_get             (jitbuf_stream_obj* impl, frame_obj** pf)
{
    *pf = NULL;
    if ( impl->frameQueue->empty() ) {
        TRACE(_FMT("The queue is empty"));
        return -1;
    }

    if ( impl->paused ) {
        if ( !impl->pastFrameQueue->empty() ) {
            impl->frameQueue->splice( impl->frameQueue->begin(), *impl->pastFrameQueue);
            impl->pastFrameQueue->clear();
            impl->lastVideoPastFramePts = 0;
            impl->lastVideoServedFramePts = 0;
        }
    }

    frame_obj* f_head = impl->frameQueue->front();
    frame_obj* f_tail = impl->frameQueue->back();
    bool       startingUp = (impl->lastVideoServedFramePts == 0 && !impl->paused);

    INT64_T pts_head = frame_get_api(f_head)->get_pts(f_head);
    INT64_T pts_tail = frame_get_api(f_tail)->get_pts(f_tail);

    if ( impl->framesRead % 100 == 0 || startingUp ) {
        TRACE( _FMT("Queue: head=" << pts_head << " tail=" << pts_tail << " diff=" << pts_tail-pts_head <<
                " len=" << impl->frameQueue->size() << " lenPast=" << impl->pastFrameQueue->size() << " startingUp=" << startingUp));
    }
    if ( startingUp ) {
        impl->prebufferEndPts = pts_tail;
    }


    if ( impl->paused ) {
         // Reduce the queue, but don't return anything
         _jitbuf_reduce( impl, impl->frameQueue, pts_tail );
    } else
    if ( pts_tail - pts_head > impl->bufferTime ) {
        *pf = _jitbuf_generate_frame(impl);
        if ( *pf == f_head ) {
            // only remove the front frame and deposit it to history if it's
            // not a frame we created to compensate for low startup fps
            impl->frameQueue->pop_front();
            if ( impl->jumpstartWithPastFrames ) {
                if ( _jitbuf_save_frame_for_jumpstart(impl, *pf ) >= 0 ) {
                    _jitbuf_reduce( impl, impl->pastFrameQueue, pts_tail );
                }
            }
        }
        return 0;
    }

    return -1;
}

//-----------------------------------------------------------------------------
static int         jitbuf_stream_read_frame        (stream_obj* stream, frame_obj** frame)
{
    DECLARE_STREAM_FF(stream, impl);

    while ( true ) {
        frame_obj* tmp = NULL;

        if ( impl->determinedEncoderDelay &&
             _jitbuf_get(impl, frame) >= 0 ) {
            return 0;
        }

        int res = default_read_frame(stream, &tmp);
        if ( res < 0 || tmp == NULL ) {
            *frame = tmp;
            return res;
        }
        impl->framesRead++;

        if ( !impl->determinedEncoderDelay ) {
            size_t size = sizeof(int);
            int    delay;
            if ( default_get_param(stream, "encoderDelay", &delay, &size) < 0 ) {
                TRACE(_FMT("No known delay"));
                // upstream filters do not know of any encoder delay
                // keep the delay at default, either hardcoded, or set by the graph builder
                impl->determinedEncoderDelay = true;
            } else if ( delay < 0 ) {
                // the delay hasn't been determined yet
                TRACE(_FMT("Delay isn't known yet."));
            } else {
                // delay is the maximum of what encoder introduced and configured/hardcoded default
                impl->bufferTime = std::max(delay, impl->bufferTime);
                impl->determinedEncoderDelay = true;
                TRACE(_FMT("Delay is set at " << impl->bufferTime));
            }
        }

        _jitbuf_append(impl, tmp);
    }

    return -1;
 }

//-----------------------------------------------------------------------------
static int         jitbuf_stream_close             (stream_obj* stream)
{
    DECLARE_STREAM_FF(stream, impl);
    return 0;
}

//-----------------------------------------------------------------------------
static void jitbuf_stream_destroy         (stream_obj* stream)
{
    DECLARE_STREAM_FF_V(stream, impl);
    impl->logCb(logTrace, _FMT("Destroying stream object " << (void*)stream));
    jitbuf_stream_close(stream); // make sure all the internals had been freed
    frame_list_destroy(&impl->frameQueue);
    frame_list_destroy(&impl->pastFrameQueue);
    stream_destroy( stream );
}

//-----------------------------------------------------------------------------
SVVIDEOLIB_API stream_api_t*     get_jitbuf_stream_api             ()
{
    return &_g_jitbuf_stream_provider;
}

