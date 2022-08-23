/*****************************************************************************
 *
 * stream_limiter.cpp
 *   Node limiting flow of frames to a specified FPS.
 *   Used when no threaded queue implemented by stream_thread_connector is needed.
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
#define SV_MODULE_VAR limiter
#define SV_MODULE_ID "LIMITER"
#include "sv_module_def.hpp"

#include "streamprv.h"

extern "C" {
#include <libavutil/opt.h>
#include <libavcodec/avcodec.h>
#include <libavfilter/avfilter.h>
#include <libavformat/avformat.h>
#include <libavfilter/buffersrc.h>
#include <libavfilter/buffersink.h>
#include <libavutil/mem.h>
#include <libswscale/swscale.h>
}

#include "frame_basic.h"

#include "videolibUtils.h"

#define LIMITER_FILTER_MAGIC 0x1268
static const int kDefaultFps = 10;
static const int kAccumulatorSize = 64;

typedef float AccType;

//-----------------------------------------------------------------------------
typedef struct limiter_filter  : public stream_base  {
    int             desiredFps;
    float           currentFps;
    int             framesAccepted;
    int             framesIgnored;
    INT64_T         prevFrameTime;
    INT64_T         firstFrameTime;

    int64_t         lastLogTime;
    int             lastFramesAccepted;
    int             lastFramesIgnored;
    int             useWallClock;
    int             useSecondIntervals;

    sv_mutex*       mutex;
    fps_limiter*    limit;
    fps_limiter*    measure;
} limiter_filter_obj;

//-----------------------------------------------------------------------------
// Stream API
//-----------------------------------------------------------------------------

//-----------------------------------------------------------------------------
//  Forward declarations
//-----------------------------------------------------------------------------
static stream_obj* limiter_filter_create             (const char* name);
static int         limiter_filter_set_param          (stream_obj* stream,
                                                const CHAR_T* name,
                                                const void* value);
static int         limiter_filter_get_param          (stream_obj* stream,
                                                const CHAR_T* name,
                                                void* value,
                                                size_t* size);
static int         limiter_filter_open_in            (stream_obj* stream);
static int         limiter_filter_read_frame         (stream_obj* stream, frame_obj** frame);
static int         limiter_filter_close              (stream_obj* stream);
static void        limiter_filter_destroy            (stream_obj* stream);


//-----------------------------------------------------------------------------
stream_api_t _g_limiter_filter_provider = {
    limiter_filter_create,
    get_default_stream_api()->set_source,
    get_default_stream_api()->set_log_cb,
    get_default_stream_api()->get_name,
    get_default_stream_api()->find_element,
    get_default_stream_api()->remove_element,
    get_default_stream_api()->insert_element,
    limiter_filter_set_param,
    limiter_filter_get_param,
    limiter_filter_open_in,
    get_default_stream_api()->seek,
    get_default_stream_api()->get_width,
    get_default_stream_api()->get_height,
    get_default_stream_api()->get_pixel_format,
    limiter_filter_read_frame,
    get_default_stream_api()->print_pipeline,
    limiter_filter_close,
    _set_module_trace_level
};


//-----------------------------------------------------------------------------
#define DECLARE_LIMITER_FILTER(stream, name) \
    DECLARE_OBJ(limiter_filter_obj, name,  stream, LIMITER_FILTER_MAGIC, -1)

#define DECLARE_LIMITER_FILTER_V(stream, name) \
    DECLARE_OBJ_V(limiter_filter_obj, name,  stream, LIMITER_FILTER_MAGIC)

static stream_obj*   limiter_filter_create                (const char* name)
{
    limiter_filter_obj* res = (limiter_filter_obj*)stream_init(sizeof(limiter_filter_obj),
                LIMITER_FILTER_MAGIC,
                &_g_limiter_filter_provider,
                name,
                limiter_filter_destroy );
    res->desiredFps = kDefaultFps;
    res->currentFps = kDefaultFps;
    res->framesAccepted = 0;
    res->framesIgnored = 0;
    res->prevFrameTime = 0;
    res->firstFrameTime = 0;
    res->lastFramesIgnored = 0;
    res->lastFramesAccepted = 0;
    res->limit = NULL;
    res->measure = NULL;
    res->useWallClock = 1;
    res->useSecondIntervals = 0;
    res->mutex = NULL;

    return (stream_obj*)res;
}

//-----------------------------------------------------------------------------
static void       _limiter_create(limiter_filter_obj* limiter)
{
    fps_limiter_destroy(&limiter->limit);
    fps_limiter_destroy(&limiter->measure);
    limiter->limit = fps_limiter_create( kAccumulatorSize, limiter->desiredFps );
    fps_limiter_use_wall_clock(limiter->limit, limiter->useWallClock);
    fps_limiter_use_second_intervals(limiter->limit, limiter->useSecondIntervals);
    limiter->measure = fps_limiter_create( kAccumulatorSize, 0 );
}

//-----------------------------------------------------------------------------
static int         limiter_filter_set_param             (stream_obj* stream,
                                            const CHAR_T* name,
                                            const void* value)
{
    DECLARE_LIMITER_FILTER(stream, limiter);
    name = stream_param_name_apply_scope(stream, name);
    if ( !_stricmp(name, "variable") && *(int*)value ) {
        limiter->mutex = sv_mutex_create();
        return 0;
    }
    if ( !_stricmp(name, "fps") ) {
        if ( limiter->mutex ) {
            sv_mutex_enter(limiter->mutex);
            limiter->desiredFps = *(int*)value;
            if ( limiter->limit ) {
                _limiter_create(limiter);
            }
            sv_mutex_exit(limiter->mutex);
        } else {
            limiter->desiredFps = *(int*)value;
        }
        return 0;
    }
    SET_PARAM_IF(stream, name, "useWallClock", int, limiter->useWallClock);
    SET_PARAM_IF(stream, name, "useSecondIntervals", int, limiter->useSecondIntervals);
    return default_set_param(stream, name, value);
}

//-----------------------------------------------------------------------------
static int         limiter_filter_get_param             (stream_obj* stream,
                                            const CHAR_T* name,
                                            void* value,
                                            size_t* size)
{
    DECLARE_LIMITER_FILTER(stream, limiter);
    name = stream_param_name_apply_scope(stream, name);
    COPY_PARAM_IF(limiter, name, "desiredFps", int, limiter->desiredFps);
    COPY_PARAM_IF(limiter, name, "fps", float, limiter->currentFps);
    if ( !_stricmp(name, "requestFps" ) ) {
        // first check if someone upstream can get a more precise metric
        if (default_get_param(stream, name, value, size) < 0 ) {
            COPY_PARAM_IF(limiter, name, "requestFps", float, limiter->currentFps);
        }
        return 0;
    }
    if ( !_stricmp(name, "captureFps" ) ) {
        // first check if someone upstream can get a more precise metric
        if (default_get_param(stream, name, value, size) < 0 ) {
            COPY_PARAM_IF(limiter, name, name, float, fps_limiter_get_fps(limiter->measure));
        }
        return 0;
    }
    return default_get_param(stream, name, value, size);
}

//-----------------------------------------------------------------------------
static int         limiter_filter_open_in            (stream_obj* stream)
{
    DECLARE_LIMITER_FILTER(stream, limiter);
    if ( default_open_in(stream) < 0 ) {
        return -1;
    }

    _limiter_create(limiter);
    return 0;
}

//-----------------------------------------------------------------------------
static int         limiter_filter_read_frame        (stream_obj* stream,
                                                    frame_obj** frame)
{
    DECLARE_LIMITER_FILTER(stream, limiter);
    int res = -1;
    static const int reportInterval = 10000;
    int64_t currentTime, timeDiff;
    frame_api_t* frameApi;

    *frame = NULL;
    if ( limiter->mutex ) {
        sv_mutex_enter(limiter->mutex);
    }

Retry:


    frame_obj* tmp = NULL;
    res = default_read_frame(stream, &tmp);
    if ( res < 0 || tmp == NULL ||
         (frameApi = frame_get_api(tmp)) == NULL ||
         frameApi->get_media_type(tmp) != mediaVideo ) {
        // we're only dealing with video frames
        *frame = tmp;
        goto Exit;
    }


    res = fps_limiter_report_frame(limiter->limit, &limiter->currentFps, frameApi->get_pts(tmp));
    // report frame to the limiter measuring wall-clock fps of frame arrival
    fps_limiter_report_frame(limiter->measure, NULL, 0);

    if ( res == 0 ) {
        limiter->framesIgnored++;
        frame_unref(&tmp);
        TRACE(_FMT("Ignoring a frame. fpsCurrent=" << limiter->currentFps <<
                            " fpsDesired=" << limiter->desiredFps <<
                            " ignored=" << limiter->framesIgnored <<
                            " accepted=" << limiter->framesAccepted <<
                            " elapsed=" << sv_time_get_elapsed_time(limiter->prevFrameTime) ) );
        goto Retry;
    } else {
        *frame = tmp;
        limiter->framesAccepted++;
        TRACE(_FMT("Accepted a frame. fpsCurrent=" << limiter->currentFps <<
                            " fpsDesired=" << limiter->desiredFps <<
                            " ignored=" << limiter->framesIgnored <<
                            " accepted=" << limiter->framesAccepted <<
                            " elapsed=" << sv_time_get_elapsed_time(limiter->prevFrameTime)));
        limiter->prevFrameTime = sv_time_get_current_epoch_time();
        if ( limiter->framesAccepted == 1 ) {
            limiter->firstFrameTime = limiter->prevFrameTime;
        }
    }

    currentTime = sv_time_get_current_epoch_time();
    timeDiff = currentTime - limiter->lastLogTime;

    if ( timeDiff > reportInterval ) {
        TRACE( _FMT("In the last " << sv_time_get_current_epoch_time() - limiter->lastLogTime << "ms" <<
                                    " accepted=" << limiter->framesAccepted - limiter->lastFramesAccepted <<
                                    " ignored=" << limiter->framesIgnored - limiter->lastFramesIgnored <<
                                    " fps=" << ((limiter->framesAccepted - limiter->lastFramesAccepted)/(float)timeDiff)*1000 <<
                                    " allTimeFps=" << (limiter->framesAccepted/(float)sv_time_get_elapsed_time(limiter->firstFrameTime))*1000 <<
                                    " reportedFps=" << limiter->currentFps <<
                                    " desiredFps=" << limiter->desiredFps ));
        limiter->lastFramesAccepted = limiter->framesAccepted;
        limiter->lastFramesIgnored = limiter->framesIgnored;
        limiter->lastLogTime = currentTime;
    }

    res = 0;

Exit:
    if ( limiter->mutex ) {
        sv_mutex_exit(limiter->mutex);
    }

    return res;
}

//-----------------------------------------------------------------------------
static int         limiter_filter_close             (stream_obj* stream)
{
    DECLARE_LIMITER_FILTER(stream, limiter);
    fps_limiter_destroy(&limiter->limit);
    fps_limiter_destroy(&limiter->measure);
    return 0;
}


//-----------------------------------------------------------------------------
static void limiter_filter_destroy         (stream_obj* stream)
{
    DECLARE_LIMITER_FILTER_V(stream, limiter);
    limiter->logCb(logTrace, _FMT("Destroying stream object " << (void*)stream));
    limiter_filter_close(stream); // make sure all the internals had been freed
    stream_destroy( stream );
}

//-----------------------------------------------------------------------------
SVVIDEOLIB_API stream_api_t*     get_limiter_filter_api                    ()
{
    return &_g_limiter_filter_provider;
}

