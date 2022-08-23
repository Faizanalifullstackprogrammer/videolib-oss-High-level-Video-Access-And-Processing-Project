/*****************************************************************************
 *
 * stream_thread_connector.cpp
 *   A node in the graph acting as a threaded frame queue.
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
#define SV_MODULE_VAR tc
#define SV_MODULE_ID "TC"
#include "sv_module_def.hpp"

#include "streamprv.h"
#include "frame_basic.h"

#include "videolibUtils.h"

#include <list>


#define TC_DEMUX_MAGIC 0x1925

enum TCState {
    tcsIdle         = 0x00,
    tcsRunning      = 0x10,
    tcsEOF          = 0x11,
    tcsClosing      = 0x01,
    tcsError        = 0x02
};


//-----------------------------------------------------------------------------
typedef struct stats_snapshot_tc: public stats_snapshot_base {
    stats_item_int  queueDepth;
    stats_item_int  readInterval;
    stats_item_int  writeInterval;
    stats_item_int  ptsSpread;

    void reset() {
        time = sv_time_get_current_epoch_time();
        stats_int_init(&queueDepth);
        stats_int_init(&readInterval);
        stats_int_init(&writeInterval);
        stats_int_init(&ptsSpread);
    }
    void combine(struct stats_snapshot_tc* other) {
        stats_int_combine(&queueDepth, &other->queueDepth);
        stats_int_combine(&readInterval, &other->readInterval);
        stats_int_combine(&writeInterval, &other->writeInterval);
        stats_int_combine(&ptsSpread, &other->ptsSpread);
    }
} stats_snapshot_tc_t;

//-----------------------------------------------------------------------------
typedef struct channel_state_tc {
    stats_snapshot_tc_t     intervalStats;
    stats_snapshot_tc_t     lifetimeStats;
    int64_t                 lastFrameWriteTime;
    int64_t                 lastFrameReadTime;
    int64_t                 lastPtsInQueue;
    int64_t                 lastPtsRead;
    int                     framesDropped;
    int                     framesInQueue;

    fps_limiter*            readLimiter;
    fps_limiter*            writeLimiter;

    channel_state_tc()
        : readLimiter( NULL )
        , writeLimiter ( NULL )
    {
    }

    void init(int fpsLimit)
    {
        fps_limiter_destroy(&readLimiter);
        fps_limiter_destroy(&writeLimiter);
        readLimiter = fps_limiter_create(75, 0);
        writeLimiter = fps_limiter_create(75, fpsLimit);
        fps_limiter_use_wall_clock(writeLimiter, 0);
        fps_limiter_use_ts_as_diff(readLimiter, 1);
        intervalStats.reset();
        lifetimeStats.reset();
        lastFrameWriteTime = lastFrameReadTime = sv_time_get_current_epoch_time();
        lastPtsInQueue = INVALID_PTS;
        lastPtsRead = INVALID_PTS;
        framesDropped = 0;
        framesInQueue = 0;
    }

    void reset() {
        lifetimeStats.combine(&intervalStats);
        intervalStats.reset();
    }

    void close()
    {
        fps_limiter_destroy(&readLimiter);
        fps_limiter_destroy(&writeLimiter);
    }
} channel_state_tc_t;

//-----------------------------------------------------------------------------
typedef struct tc_stream  : public stream_base  {
    sv_thread*      thread;
    // event is reset when queue consumer encounters an empty queue,
    // and set when queue producer deposits a frame into an empty queue
    // the consumer is also released when the queue encounters EOF
    sv_event*       event;
    // queueEvent is waited on by producer when at EOF, or when the queue is full
    // it may be released by a seek event, or by a frame being consumed by read
    // operation, or by a close operation
    sv_event*       queueEvent;
    // dataMutex protects the frame queue access
    sv_mutex*       dataMutex;
    // streamLock assures mutual exclusion of seek/read/close operations
    sv_rwlock*      streamLock;
    FrameList*      queue;
    int             maxQueueSize;
    int             lossy;
    int             lastQueueDepthWarningVal;
    int64_t         timeoutMs;
    int64_t         lastStatsTime;
    // time we last exited read_frame
    int64_t         lastFrameReadTime;
    // amount of time between read_frame calls since last video frame was returned
    int             elapsedAccumulator;
    int64_t         statsIntervalMsec;
    int             state;
    int             fpsLimit;
    // if set to false, mediaVideo frames being dropped will convert to
    // mediaVideoTime frames to notify the consumer about timestamps saved
    int             silentFpsLimiter;
    channel_state_tc_t*  videoState;
    frame_allocator*     fa;
} tc_stream_obj;


//-----------------------------------------------------------------------------
// Stream API
//-----------------------------------------------------------------------------

//-----------------------------------------------------------------------------
//  Forward declarations
//-----------------------------------------------------------------------------
static stream_obj* tc_stream_create             (const char* name);
int                tc_set_source                (stream_obj* stream,
                                                 stream_obj* source,
                                                 INT64_T flags);
stream_obj*        tc_find_element              (stream_obj* stream,
                                                 const char* name);
int                tc_remove_element            (stream_obj** pStream,
                                                 struct stream_api** pAPI,
                                                 const char* name,
                                                 stream_obj** removedStream);
int                tc_insert_element            (stream_obj** pStream,
                                                 struct stream_api** pAPI,
                                                 const char* insertBefore,
                                                 stream_obj* newElement,
                                                 INT64_T flags);
static int         tc_stream_set_param          (stream_obj* stream,
                                                const CHAR_T* name,
                                                const void* value);
static int         tc_stream_get_param             (stream_obj* stream,
                                            const CHAR_T* name,
                                            void* value,
                                            size_t* size);
static int         tc_stream_open_in            (stream_obj* stream);
static int         tc_stream_seek               (stream_obj* stream,
                                                    INT64_T offset,
                                                    int flags);
static int         tc_stream_read_frame         (stream_obj* stream, frame_obj** frame);
static int         tc_stream_close              (stream_obj* stream);
static void        tc_stream_destroy            (stream_obj* stream);

static void       _tc_format_stats              (tc_stream_obj* tc,
                                                std::ostringstream& str,
                                                channel_state_tc_t* cs);

//-----------------------------------------------------------------------------
stream_api_t _g_tc_stream_provider = {
    tc_stream_create,
    tc_set_source,
    get_default_stream_api()->set_log_cb,
    get_default_stream_api()->get_name,
    tc_find_element,
    tc_remove_element,
    tc_insert_element,
    tc_stream_set_param,
    tc_stream_get_param,
    tc_stream_open_in,
    tc_stream_seek,
    get_default_stream_api()->get_width,
    get_default_stream_api()->get_height,
    get_default_stream_api()->get_pixel_format,
    tc_stream_read_frame,
    get_default_stream_api()->print_pipeline,
    tc_stream_close,
    _set_module_trace_level
} ;


//-----------------------------------------------------------------------------
#define DECLARE_STREAM_tc(stream, name) \
    DECLARE_OBJ(tc_stream_obj, name,  stream, TC_DEMUX_MAGIC, -1)

#define DECLARE_STREAM_tc_V(stream, name) \
    DECLARE_OBJ_V(tc_stream_obj, name,  stream, TC_DEMUX_MAGIC)

static stream_obj*   tc_stream_create                (const char* name)
{
    tc_stream_obj* res = (tc_stream_obj*)stream_init(sizeof(tc_stream_obj),
                TC_DEMUX_MAGIC,
                &_g_tc_stream_provider,
                name,
                tc_stream_destroy );

    res->thread = NULL;
    res->event = sv_event_create(0,0);
    res->queueEvent = sv_event_create(0,0);
    res->dataMutex = sv_mutex_create();
    res->streamLock = sv_rwlock_create();
    res->queue = frame_list_create();
    res->timeoutMs = 0;
    res->maxQueueSize = 0;
    res->lossy = 0;
    res->lastQueueDepthWarningVal = 0;
    res->state = tcsIdle;
    res->lastStatsTime = sv_time_get_current_epoch_time();
    res->lastFrameReadTime = INVALID_PTS;
    res->elapsedAccumulator = 0;
    res->fpsLimit = 0;
    res->silentFpsLimiter = 0;
    res->videoState = new channel_state_tc_t();

    if (_gTraceLevel>15) res->statsIntervalMsec = 1;
    else if (_gTraceLevel>10) res->statsIntervalMsec = 5;
    else if (_gTraceLevel>5) res->statsIntervalMsec = 10;
    else if (_gTraceLevel>2) res->statsIntervalMsec = 30;
    else if (_gTraceLevel>0) res->statsIntervalMsec = 60;
    else res->statsIntervalMsec = 0;
    res->statsIntervalMsec *= 1000;

    res->fa = create_frame_allocator(_STR("tc_"<<name));
    return (stream_obj*)res;
}

//-----------------------------------------------------------------------------
static void       _tc_log_stats(tc_stream_obj* tc)
{
    std::ostringstream stats;
    sv_mutex_enter(tc->dataMutex);
    _tc_format_stats(tc, stats, tc->videoState);
    sv_mutex_exit(tc->dataMutex);
    tc->logCb(logDebug, _FMT(stats.str()));
}

//-----------------------------------------------------------------------------
int                tc_set_source                (stream_obj* stream,
                                                 stream_obj* source,
                                                 INT64_T flags)
{
    DECLARE_STREAM_tc(stream, tc);
    int res;
    sv_rwlock_lock_write(tc->streamLock);
    res = default_set_source(stream, source, flags);
    sv_rwlock_unlock_write(tc->streamLock);
    return res;
}

//-----------------------------------------------------------------------------
stream_obj*        tc_find_element              (stream_obj* stream,
                                                 const char* name)
{
    DECLARE_OBJ(tc_stream_obj, tc,  stream, TC_DEMUX_MAGIC, NULL);
    stream_obj* res;
    sv_rwlock_lock_read(tc->streamLock);
    res = default_find_element(stream, name);
    sv_rwlock_unlock_read(tc->streamLock);
    return res;
}

//-----------------------------------------------------------------------------
int                tc_remove_element            (stream_obj** pStream,
                                                 struct stream_api** pAPI,
                                                 const char* name,
                                                 stream_obj** removedStream)
{
    DECLARE_STREAM_tc(*pStream, tc);
    int res;
    sv_rwlock_lock_write(tc->streamLock);
    res = default_remove_element(pStream, pAPI, name, removedStream);
    sv_rwlock_unlock_write(tc->streamLock);
    return res;
}

//-----------------------------------------------------------------------------
int                tc_insert_element            (stream_obj** pStream,
                                                 struct stream_api** pAPI,
                                                 const char* insertBefore,
                                                 stream_obj* newElement,
                                                 INT64_T flags)
{
    DECLARE_STREAM_tc(*pStream, tc);
    int res;
    sv_rwlock_lock_write(tc->streamLock);
    res = default_insert_element(pStream, pAPI, insertBefore, newElement, flags);
    sv_rwlock_unlock_write(tc->streamLock);
    return res;

}
//-----------------------------------------------------------------------------
static int         tc_stream_set_param             (stream_obj* stream,
                                            const CHAR_T* name,
                                            const void* value)
{
    DECLARE_STREAM_tc(stream, tc);

    name = stream_param_name_apply_scope(stream, name);
    if ( !_stricmp(name, "statsIntervalSec") ) {
        int64_t msec = 1000 * (*(int*)value);
        if ( msec < tc->statsIntervalMsec || tc->statsIntervalMsec == 0 ) {
            // only override the default if it results in more frequent stats
            tc->statsIntervalMsec = msec;
        }
        default_set_param(stream, name, value);   // we have to pass this one on
        return 0;
    }
    if ( !_stricmp(name, "flushStats") ) {
        _tc_log_stats(tc);
        return 0;
    }

    SET_PARAM_IF(stream, name, "lossy", int, tc->lossy);
    SET_PARAM_IF(stream, name, "timeout", int, tc->timeoutMs);
    SET_PARAM_IF(stream, name, "maxQueueSize", int, tc->maxQueueSize);
    SET_PARAM_IF(stream, name, "fpsLimit", int, tc->fpsLimit);
    SET_PARAM_IF(stream, name, "silentFpsLimiter", int, tc->silentFpsLimiter);

    int res;
    sv_rwlock_lock_write(tc->streamLock);
    res = default_set_param(stream, name, value);
    sv_rwlock_unlock_write(tc->streamLock);
    return res;
}

//-----------------------------------------------------------------------------
static int         tc_stream_get_param             (stream_obj* stream,
                                            const CHAR_T* name,
                                            void* value,
                                            size_t* size)
{
    DECLARE_STREAM_tc(stream, tc);

    name = stream_param_name_apply_scope(stream, name);

    COPY_PARAM_IF_SAFE(tc, name, "requestFps", float, fps_limiter_get_fps(tc->videoState->readLimiter), tc->dataMutex);
    COPY_PARAM_IF_SAFE(tc, name, "captureFps", float, fps_limiter_get_fps(tc->videoState->writeLimiter), tc->dataMutex);
    COPY_PARAM_IF_SAFE(tc, name, "eof", int, (tc->queue->empty()&&tc->state==tcsEOF)?1:0, tc->dataMutex);

    return default_get_param(stream, name, value, size);
}

//-----------------------------------------------------------------------------
static void       _tc_flush_queue               (tc_stream_obj* tc,
                                                 INT64_T pts = INVALID_PTS,
                                                 int mediaType = mediaUnknown)
{
    while (!tc->queue->empty()) {
        frame_obj* retFrame = tc->queue->front();
        if ( pts != INVALID_PTS ) {
            frame_api_t* api = frame_get_api(retFrame);
            if ( api->get_media_type(retFrame) &&
                 api->get_pts(retFrame) >= pts ) {
                break;
            }
        }
        frame_unref(&retFrame);
        tc->queue->pop_front();
        tc->videoState->framesInQueue--;
    }
}

//-----------------------------------------------------------------------------
static void       _tc_format_stats              (tc_stream_obj* tc,
                                                std::ostringstream& str,
                                                channel_state_tc_t* cs)
{
    cs->lifetimeStats.combine(&cs->intervalStats);
    str <<  tc->name << ": " <<
            " FPSRead=" << fps_limiter_get_fps(cs->readLimiter) <<
            " FPSWrite=" << fps_limiter_get_fps(cs->writeLimiter) <<
            " FPSAccepted=" << fps_limiter_get_frames_accepted(cs->writeLimiter) <<
            " FPSRejected=" << fps_limiter_get_frames_rejected(cs->writeLimiter) <<
            " inQueue=" << cs->framesInQueue <<
            " Period stats: " <<
            " maxQueue=" << cs->intervalStats.queueDepth.max <<
            " avgQueue=" << stats_int_average(&cs->intervalStats.queueDepth) <<
            " maxReadInterval=" << cs->intervalStats.readInterval.max <<
            " avgReadInterval=" << stats_int_average(&cs->intervalStats.readInterval) <<
            " maxWriteInterval=" << cs->intervalStats.writeInterval.max <<
            " avgWriteInterval=" << stats_int_average(&cs->intervalStats.writeInterval) <<
            " maxPtsSpread=" << cs->intervalStats.ptsSpread.max <<
            " avgPtsSpread=" << stats_int_average(&cs->intervalStats.ptsSpread) <<
            " framesDropped=" << cs->framesDropped <<
            "; Lifetime stats: " <<
            " maxQueue=" << cs->lifetimeStats.queueDepth.max <<
            " avgQueue=" << stats_int_average(&cs->lifetimeStats.queueDepth) <<
            " maxReadInterval=" << cs->lifetimeStats.readInterval.max <<
            " avgReadInterval=" << stats_int_average(&cs->lifetimeStats.readInterval) <<
            " maxWriteInterval=" << cs->lifetimeStats.writeInterval.max <<
            " avgWriteInterval=" << stats_int_average(&cs->lifetimeStats.writeInterval) <<
            " maxPtsSpread=" << cs->lifetimeStats.ptsSpread.max <<
            " avgPtsSpread=" << stats_int_average(&cs->lifetimeStats.ptsSpread) <<
            "; ";
    cs->intervalStats.reset();
}

//-----------------------------------------------------------------------------
static bool _tc_check_queue_size(tc_stream_obj* tc)
{
    if ( tc->videoState->framesInQueue <= tc->maxQueueSize ||
         tc->maxQueueSize == 0 ) {
        return true;
    }

    if ( !tc->lossy ) {
        return false;
    }

    FrameIterator           it = tc->queue->begin();
    FrameIterator           remove;
    frame_obj*              toRemove = NULL;

    int64_t              prevFramePts = tc->videoState->lastPtsRead;
    int64_t              distance = 1000000;
    int64_t              framePts;

    while (it != tc->queue->end()) {
        frame_obj*   f = *it;
        frame_api_t* api = frame_get_api(f);
        if (api->get_media_type(f) == mediaVideo) {
            framePts = api->get_pts(f);
            int64_t      d = framePts - prevFramePts;
            if ( d < distance ) {
                remove = it;
                toRemove = *remove;
                distance = d;
            }
            prevFramePts = framePts;
        }
        it++;
    }

    if ( toRemove != NULL ) {
        frame_unref(&toRemove);
        tc->queue->erase(remove);
        tc->videoState->framesDropped++;
        tc->videoState->framesInQueue--;
        TRACE(_FMT("Dropping frame with pts=" << framePts << " totalDropped=" << tc->videoState->framesDropped));
    }

    return true;
}

#define IS_RUNNING(tc) (tc->state&tcsRunning)!=0

//-----------------------------------------------------------------------------
static frame_obj* _tc_convert_video_frame(tc_stream_obj* tc, frame_obj* frame)
{
    frame_api_t* api = frame_get_api(frame);
    basic_frame_obj* newFrame = alloc_basic_frame2 (TC_DEMUX_MAGIC,
                                                    0,
                                                    tc->logCb,
                                                    tc->fa );
    newFrame->pts = api->get_pts(frame);
    newFrame->dts = api->get_dts(frame);
    newFrame->width = api->get_width(frame);
    newFrame->height = api->get_height(frame);
    newFrame->pixelFormat = api->get_pixel_format(frame);
    newFrame->mediaType = mediaVideoTime;
    newFrame->dataSize = 0;
    newFrame->data = NULL;

    frame_unref(&frame);
    return (frame_obj*)newFrame;
}

//-----------------------------------------------------------------------------
// Deposits the frame into the queue and recalculates statistics
static void      _tc_deposit_frame              (tc_stream_obj* tc,
                                                 frame_obj* frame)
{
    sv_mutex_enter(tc->dataMutex);

    size_t sizeBeforeDeposit = tc->queue->size();

    frame_api_t* api = frame_get_api(frame);
    channel_state_tc_t* cs = NULL;
    if (api->get_media_type(frame) == mediaVideo) {
        cs = tc->videoState;
    }
    int64_t pts = api->get_pts(frame);
    size_t  queueDepth = 0;

    if ( cs != NULL ) {
        if ( fps_limiter_report_frame(cs->writeLimiter, NULL, pts) == 0 ) {
            // limiter instructs us to discard this frame
            if ( tc->silentFpsLimiter ) {
                sv_mutex_exit(tc->dataMutex);
                frame_unref(&frame);
                return;
            }
            frame = _tc_convert_video_frame(tc, frame);
        } else {
            int64_t now = sv_time_get_current_epoch_time();
            int64_t dur = sv_time_get_time_diff(cs->lastFrameWriteTime, now);
            cs->framesInQueue++;
            cs->lastFrameWriteTime = now;
            if (pts > cs->lastPtsInQueue ||
                cs->lastPtsInQueue == INVALID_PTS)
                cs->lastPtsInQueue = pts;
            queueDepth = sizeBeforeDeposit + 1;
            stats_int_update(&cs->intervalStats.queueDepth, queueDepth);
            stats_int_update(&cs->intervalStats.writeInterval, dur);
            if ( cs->lastPtsRead != INVALID_PTS ) {
                int64_t diff = cs->lastPtsInQueue - cs->lastPtsRead;
                stats_int_update(&cs->intervalStats.ptsSpread, diff);
            }
            int64_t diff = sv_time_get_time_diff(tc->lastStatsTime, now);
            if ( tc->statsIntervalMsec && diff > tc->statsIntervalMsec ) {
                std::ostringstream stats;
                _tc_format_stats(tc, stats, cs);
                tc->lastStatsTime = now;
                tc->logCb(logInfo, _FMT(stats.str()));
            }
        }
    }

    if ( queueDepth>5 && (queueDepth%5)==0 && tc->lastQueueDepthWarningVal != queueDepth) {
        TRACE(_FMT("Queue depth is currently at " << queueDepth << " with " <<
                    tc->videoState->framesInQueue << " video frames"));
        tc->lastQueueDepthWarningVal = queueDepth;
    }

    tc->queue->push_back(frame);

    // The frame had been deposited into an empty queue,
    // release the potentially waiting thread
    if (sizeBeforeDeposit == 0) {
        sv_event_set(tc->event);
    }

    sv_mutex_exit(tc->dataMutex);
}

//-----------------------------------------------------------------------------
// Blocks for as long as queue depth is more than maximum configured
static bool   _tc_wait_for_space_in_queue    (tc_stream_obj* tc)
{
    bool  waited = false;
    bool  running = true;
    bool  sizeOk;
    int   sizeOfQueue;
    do {
        // make sure we're still running
        sv_rwlock_lock_read(tc->streamLock);
        running = IS_RUNNING(tc);
        sv_rwlock_unlock_read(tc->streamLock);
        if ( !running )
            break;

        sv_mutex_enter(tc->dataMutex);
        sizeOk = _tc_check_queue_size(tc);
        if (!sizeOk) {
            sv_event_reset(tc->queueEvent);
            sizeOfQueue = tc->videoState->framesInQueue;
        }
        sv_mutex_exit(tc->dataMutex);

        if (!sizeOk) {
            // in a non-lossy queue we block here until more space is available in the queue
            TRACE(_FMT("Waiting for queue to recede ... " << (void*)tc <<
                        " " << " - " << sizeOfQueue));
            sv_event_wait(tc->queueEvent, 0);
            waited = true;
        }
    } while (!sizeOk);

    if ( waited ) {
        TRACE(_FMT("Done waiting for queue to recede ... " << (void*)tc));
    }

    return running;
}

//-----------------------------------------------------------------------------
static void*      _tc_thread_func               (void* param)
{
    tc_stream_obj* tc = (tc_stream_obj*)param;
    TRACE(_FMT("Starting thread " << (void*)tc));

    bool   running = true;
    int    eof = 0;
    int    wasEOF;

    while (running) {
        frame_obj* frame = NULL;
        int    readRes = -1;
        size_t sz = sizeof(int);

        sv_rwlock_lock_read(tc->streamLock);

        wasEOF = eof;
        eof = (tc->state == tcsEOF) ? 1 : 0;

        // try to read, but only if we are still in data-consuming mode
        if (tc->state == tcsRunning) {
            readRes = tc->sourceApi->read_frame(tc->source, &frame);
            if ( readRes < 0 || frame == NULL ) {
                tc->sourceApi->get_param(tc->source, "eof", &eof, &sz);
                if (!eof) {
                    tc->logCb(logError, _FMT("Error while reading frame from the source. Exiting thread " << (void*)tc ));
                    tc->state = tcsError;
                }
            }
        }

        if ( eof ) {
            if ( IS_RUNNING(tc) ) {
                // in case of EOF, prepare for hibernation
                sv_mutex_enter(tc->dataMutex);
                tc->state = tcsEOF;
                sv_event_reset(tc->queueEvent);
                sv_mutex_exit(tc->dataMutex);
            } else {
                // thread is being shutdown, don't treat as EOF
                eof = 0;
            }
        } else if ( frame != NULL ) {
            _tc_deposit_frame(tc, frame);
        }

        // update the flag for local consumption outside of mutex
        running = IS_RUNNING(tc);
        sv_rwlock_unlock_read(tc->streamLock);

        if ( eof ) {
            // EOF doesn't automatically mean we're done ... seek may cause us to restart ...
            // wait for either seek or closure event to occur
            if (!wasEOF) {
                // we only want to log on transition to EOF state (we are woken up and reenter the loop
                // every time read occurs)
                tc->logCb(logDebug, _FMT("Thread connector reached EOF. Waiting for seek or close event"));
            }
            sv_event_set(tc->event);
            sv_event_wait(tc->queueEvent, 0);
            continue;
        } else if ( !running ) {
            tc->logCb(logDebug, _FMT("Thread connector exiting") );
            break;
        }

        running = _tc_wait_for_space_in_queue(tc);
    }

    // make sure to signal event, in case someone is waiting on it
    sv_event_set(tc->event);
    TRACE(_FMT("Exiting thread " << (void*)tc));
    return NULL;
}

//-----------------------------------------------------------------------------
static int         tc_stream_open_in            (stream_obj* stream)
{
    DECLARE_STREAM_tc(stream, tc);

    tc->videoState->init(tc->fpsLimit);

    if (default_open_in(stream) < 0) {
        return -1;
    }

    tc->state = tcsRunning;
    tc->thread = sv_thread_create(_tc_thread_func, tc);
    if (!tc->thread) {
        tc->logCb(logError, _FMT("Failed to start thread"));
        tc->state = tcsError;
        return -1;
    }

    return 0;
}

//-----------------------------------------------------------------------------
static int         tc_stream_seek               (stream_obj* stream,
                                                    INT64_T offset,
                                                    int flags)
{
    int res = -1;
    DECLARE_STREAM_tc(stream, tc);
    sv_rwlock_lock_write(tc->streamLock);
    sv_mutex_enter(tc->dataMutex);
    if ( IS_RUNNING(tc) ) {
        TRACE(_FMT("Seek to pts=" << offset << "; flushing the queue"));
        if ( (offset > tc->videoState->lastPtsRead ||
             tc->videoState->lastPtsRead == INVALID_PTS) &&
             offset <= tc->videoState->lastPtsInQueue &&
             tc->videoState->lastPtsInQueue != INVALID_PTS ) {
            // the frame we're seeking to is in queue - no need to flush
            res = 0;
            _tc_flush_queue(tc, offset, mediaVideo);
        } else {
            res = default_seek(stream, offset, flags);
            _tc_flush_queue(tc);
            tc->videoState->init(tc->fpsLimit);
            tc->state = tcsRunning;
        }
    }
    sv_event_set(tc->queueEvent);
    sv_mutex_exit(tc->dataMutex);
    sv_rwlock_unlock_write(tc->streamLock);
    return res;
}


//-----------------------------------------------------------------------------
static int         tc_stream_read_frame        (stream_obj* stream, frame_obj** frame)
{
    DECLARE_STREAM_tc(stream, tc);
    bool gotFrame = false;
    bool timeout = false;
    bool keepTrying = true;
    bool running, eof;
    int size = 0;
    int64_t elapsedSinceLastRead;
    int64_t pts;

    if ( tc->lastFrameReadTime != INVALID_PTS ) {
        elapsedSinceLastRead = sv_time_get_elapsed_time(tc->lastFrameReadTime) + tc->elapsedAccumulator;
    } else {
        // prime with either the expected interval, or 30fps, if we don't impose a limit
        elapsedSinceLastRead = tc->fpsLimit ? 1000/tc->fpsLimit : 33;
    }


    while (!gotFrame && !timeout && keepTrying) {
        sv_mutex_enter(tc->dataMutex);
        size = tc->queue->size();
        if (tc->queue->empty()) {
            TRACE(_FMT("Queue is empty"));
            gotFrame = false;
            sv_event_reset( tc->event );
        } else {
            frame_obj* retFrame = tc->queue->front();
            tc->queue->pop_front();
            sv_event_set(tc->queueEvent);
            // set the return value
            *frame = retFrame;

            gotFrame = true;

            frame_api_t* api = frame_get_api(*frame);
            channel_state_tc_t* cs = NULL;
            if (api->get_media_type(*frame) == mediaVideo) {
                cs = tc->videoState;
            }

            if ( cs != NULL ) {
                int64_t now = sv_time_get_current_epoch_time();
                int64_t dur = sv_time_get_time_diff(cs->lastFrameReadTime, now);

                pts = api->get_pts(*frame);
                cs->lastFrameReadTime = now;
                if (pts > cs->lastPtsRead || cs->lastPtsRead == INVALID_PTS) {
                    cs->lastPtsRead = pts;
                }
                stats_int_update(&cs->intervalStats.queueDepth, tc->queue->size());
                stats_int_update(&cs->intervalStats.readInterval, dur);
                stats_int_update(&cs->intervalStats.ptsSpread, cs->lastPtsInQueue - cs->lastPtsRead);
                cs->framesInQueue--;

                fps_limiter_report_frame(cs->readLimiter, NULL, elapsedSinceLastRead);
                tc->elapsedAccumulator = 0;
            } else {
                tc->elapsedAccumulator = elapsedSinceLastRead;
            }
        }
        sv_mutex_exit(tc->dataMutex);

        if ( !gotFrame ) {
            sv_rwlock_lock_read(tc->streamLock);
            running = (tc->state == tcsRunning);
            eof = (tc->state == tcsEOF);
            sv_rwlock_unlock_read(tc->streamLock);

            if ( running ) {
                int res = 0;
                TRACE(_FMT("Waiting for a frame"));
                res = sv_event_wait(tc->event, tc->timeoutMs);
                TRACE(_FMT("Done waiting for a frame, res=" << res));
                if ( res > 0 ) {
                    timeout = true;
                } else if ( res < 0 ) {
                    keepTrying = false; // error
                }
            } else if ( eof ) {
                // queue may have been refilled since our last attempt to access it
                sv_mutex_enter(tc->dataMutex);
                keepTrying = !tc->queue->empty();
                sv_mutex_exit(tc->dataMutex);
            } else {
                keepTrying = false;
            }
        }
    }

    int retval = 0;
    if ( gotFrame ) {
        TRACE(_FMT("Got frame " << (void*)tc << " pts=" << pts ));
    } else
    if ( !running && !eof ) {
        tc->logCb(logError, _FMT("Failed to read a frame: thread isn't running"));
        retval = -1;
    } else
    if ( eof ) {
        tc->logCb(logDebug, _FMT("Failed to read a frame: stream reached end of file"));
        retval = -1;
    } else
    if ( timeout ) {
        tc->logCb(logError, _FMT("Failed to read a frame: timeout - " << (void*)tc));
        retval = -1;
    } else {
        tc->logCb(logError, _FMT("Failed to read a frame: unknown error"));
        retval = -1;
    }

    tc->lastFrameReadTime = sv_time_get_current_epoch_time();

    return retval;
}

//-----------------------------------------------------------------------------
static int         tc_stream_close             (stream_obj* stream)
{
    DECLARE_STREAM_tc(stream, tc);

    sv_rwlock_lock_write(tc->streamLock);
    if ( tc->state == tcsClosing ) {
        sv_rwlock_unlock_write(tc->streamLock);
        return 0;
    }
    TRACE( _FMT("Closing stream object " << tc->name << " " << (void*)tc));
    tc->state = tcsClosing;
    sv_rwlock_unlock_write(tc->streamLock);

    // release the producer
    sv_mutex_enter(tc->dataMutex);
    sv_event_set(tc->queueEvent);
    sv_mutex_exit(tc->dataMutex);

    int err = sv_thread_destroy(&tc->thread);
    if ( err != 0 ) {
        tc->logCb(logError, _FMT("Error " << err << " terminating thread"));
    }

    _tc_log_stats(tc);
    _tc_flush_queue(tc);
    tc->videoState->close();
    return 0;
}


//-----------------------------------------------------------------------------
static void tc_stream_destroy         (stream_obj* stream)
{
    DECLARE_STREAM_tc_V(stream, tc);
    TRACE( _FMT("Destroying stream object " <<
                tc->name <<
                (void*)stream));

    tc_stream_close(stream); // make sure all the internals had been freed

    sv_event_destroy(&tc->event);
    sv_event_destroy(&tc->queueEvent);
    sv_mutex_destroy(&tc->dataMutex);
    sv_rwlock_destroy(&tc->streamLock);
    frame_list_destroy (&tc->queue);

    destroy_frame_allocator( &tc->fa, tc->logCb );
    delete tc->videoState;
    stream_destroy( stream );
}

//-----------------------------------------------------------------------------
SVVIDEOLIB_API stream_api_t*     get_tc_api                    ()
{
    return &_g_tc_stream_provider;
}

