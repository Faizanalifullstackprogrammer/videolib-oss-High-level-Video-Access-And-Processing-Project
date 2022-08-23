/*****************************************************************************
 *
 * stream_recorder_sync.cpp
 *   Graph element responsible for synchronizing an output frame with
 *   the filename it was recorded to.
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
#define SV_MODULE_VAR rs
#define SV_MODULE_ID "RECSYNC"
#include "sv_module_def.hpp"

#include "streamprv.h"

#include <list>
#include <string>

#include "videolibUtils.h"

#define RS_RECSYNC_MAGIC 0x1371
#define MAX_PARAM 10

using std::list;
using std::string;

//-----------------------------------------------------------------------------
typedef struct file_context {
    uint64_t        start;
    uint64_t        end;
    std::string     name;
} file_context;

//-----------------------------------------------------------------------------
typedef struct recorder_sync_stream  : public stream_base  {
    char*                   recFilterName;
    int                     encoderDelay;
    INT64_T                 lastPtsInQueue;
    list<file_context>*     fileContexts;
    FrameList*              frames;
    sv_mutex*               mutex;
} recorder_sync_stream_obj;

//-----------------------------------------------------------------------------
// Stream API
//-----------------------------------------------------------------------------

//-----------------------------------------------------------------------------
//  Forward declarations
//-----------------------------------------------------------------------------
static stream_obj* rs_stream_create             (const char* name);
static int         rs_stream_set_param          (stream_obj* stream,
                                                const CHAR_T* name,
                                                const void* value);
static int         rs_stream_get_param             (stream_obj* stream,
                                                const CHAR_T* name,
                                                void* value,
                                                size_t* size);
static int         rs_stream_open_in            (stream_obj* stream);
static int         rs_stream_seek               (stream_obj* stream,
                                                    INT64_T offset,
                                                    int flags);
static int         rs_stream_read_frame         (stream_obj* stream, frame_obj** frame);
static int         rs_stream_close              (stream_obj* stream);
static void        rs_stream_destroy            (stream_obj* stream);

static void        _rs_file_placeholder         (recorder_sync_stream_obj* rs);
static void        _rs_new_file                 (recorder_sync_stream_obj* rs, stream_ev_obj* ev);
static void        _rs_end_file                 (recorder_sync_stream_obj* rs, stream_ev_obj* ev);

//-----------------------------------------------------------------------------
stream_api_t _g_rs_stream_provider = {
    rs_stream_create,
    get_default_stream_api()->set_source,
    get_default_stream_api()->set_log_cb,
    get_default_stream_api()->get_name,
    get_default_stream_api()->find_element,
    get_default_stream_api()->remove_element,
    get_default_stream_api()->insert_element,
    rs_stream_set_param,
    rs_stream_get_param,
    rs_stream_open_in,
    get_default_stream_api()->seek,
    get_default_stream_api()->get_width,
    get_default_stream_api()->get_height,
    get_default_stream_api()->get_pixel_format,
    rs_stream_read_frame,
    get_default_stream_api()->print_pipeline,
    rs_stream_close,
    _set_module_trace_level
} ;


//-----------------------------------------------------------------------------
static void  stream_event_cb         (stream_obj* stream, stream_ev_obj* ev)
{
    stream_ev_ref(ev);

    stream_ev_api_t* api = stream_ev_get_api(ev);
    recorder_sync_stream_obj* rs = (recorder_sync_stream_obj*)api->get_context(ev);
    const char* name = api->get_name(ev);

    sv_mutex_enter(rs->mutex);
    if ( !_stricmp(name, "recorder.newFile")) {
        _rs_new_file(rs, ev);
    } else
    if ( !_stricmp(name, "recorder.closeFile")) {
        _rs_end_file(rs, ev);
    }
    sv_mutex_exit(rs->mutex);

    stream_ev_unref(&ev);
}


//-----------------------------------------------------------------------------
static void        _rs_new_file                 (recorder_sync_stream_obj* rs, stream_ev_obj* ev)
{
    file_context fc;
    stream_ev_api_t* api = stream_ev_get_api(ev);

    char buffer[1024];
    size_t bufferSize = 1024;

    if (api->get_property(ev, "filename", buffer, &bufferSize) >= 0) {
        if (!rs->fileContexts->empty() && rs->fileContexts->back().name == buffer) {
            rs->logCb(logWarning, _FMT("Multiple new file notifications for " << buffer));
        } else {
            fc.start = api->get_ts(ev);
            fc.end = (uint64_t)-1;
            fc.name = buffer;
            rs->fileContexts->push_back(fc);
            TRACE(_FMT( "Set next file to " << buffer << " starting from " << fc.start));
        }
    } else {
        rs->logCb(logError, _FMT("Failed to retrieve the filename from new file event!"));
    }
}

//-----------------------------------------------------------------------------
static void        _rs_end_file                 (recorder_sync_stream_obj* rs, stream_ev_obj* ev)
{
    stream_ev_api_t* api = stream_ev_get_api(ev);
    for (list<file_context>::iterator it=rs->fileContexts->begin(); it!=rs->fileContexts->end(); it++) {
        file_context& fc = *it;
        if ( fc.end == (uint64_t)-1 ) {
            fc.end = api->get_ts(ev);
            TRACE(_FMT( "Closed current file range: [" << fc.start << "," << fc.end << "]"));
            return;
        }
    }
    rs->logCb(logError, _FMT("Mismatched file end event!"));
}

//-----------------------------------------------------------------------------
#define DECLARE_STREAM_RS(stream, name) \
    DECLARE_OBJ(recorder_sync_stream_obj, name,  stream, RS_RECSYNC_MAGIC, -1)

#define DECLARE_STREAM_RS_V(stream, name) \
    DECLARE_OBJ_V(recorder_sync_stream_obj, name,  stream, RS_RECSYNC_MAGIC)

static stream_obj*   rs_stream_create                (const char* name)
{
    recorder_sync_stream_obj* res = (recorder_sync_stream_obj*)stream_init(sizeof(recorder_sync_stream_obj),
                RS_RECSYNC_MAGIC,
                &_g_rs_stream_provider,
                name,
                rs_stream_destroy );
    res->recFilterName = NULL;
    res->encoderDelay = -1;
    res->lastPtsInQueue = INVALID_PTS;
    res->fileContexts = new list<file_context>;
    res->frames = frame_list_create();
    res->mutex = sv_mutex_create();

    return (stream_obj*)res;
}

//-----------------------------------------------------------------------------
static int         rs_stream_set_param             (stream_obj* stream,
                                            const CHAR_T* name,
                                            const void* value)
{
    DECLARE_STREAM_RS(stream, rs);

    name = stream_param_name_apply_scope(stream, name);
    SET_STR_PARAM_IF(stream, name, "recorderName", rs->recFilterName);
    return default_set_param(stream, name, value);
}

//-----------------------------------------------------------------------------
static int         rs_stream_get_param             (stream_obj* stream,
                                            const CHAR_T* name,
                                            void* value,
                                            size_t* size)
{
    DECLARE_STREAM_RS(stream, rs);
    name = stream_param_name_apply_scope(stream, name);
    COPY_PARAM_IF_SAFE(rs, name, "filename", const char*, rs->fileContexts->empty()?NULL:rs->fileContexts->front().name.c_str(), rs->mutex);
    return default_get_param(stream, name, value, size);
}

//-----------------------------------------------------------------------------
static int         rs_stream_open_in                (stream_obj* stream)
{
    DECLARE_STREAM_RS(stream, rs);
    char cb[512], ctx[512];

    sprintf(cb, "%s.eventCallback", rs->recFilterName);
    sprintf(ctx, "%s.eventCallbackContext", rs->recFilterName);
    if ( default_set_param(stream, ctx, rs) < 0 ||
         default_set_param(stream, cb, (const void*)stream_event_cb) < 0 ) {
        rs->logCb(logError, _FMT("Failed to set event callback for the recorder: " <<
                                cb << " " << ctx));
    }

    return default_open_in(stream);
}

//-----------------------------------------------------------------------------
static int       _rs_stream_get_frame_from_queue(recorder_sync_stream_obj* rs,
                                                 frame_obj** frame)
{
    frame_obj*      f = NULL;
    file_context*   currentFileContext;
    INT64_T         pts;

    if ( rs->encoderDelay < 0 ) {
        char delayParamName[512];
        int delayVal;
        size_t size = sizeof(int);
        sprintf(delayParamName, "%s.encoderDelay", rs->recFilterName);
        if ( default_get_param((stream_obj*)rs, delayParamName, &delayVal, &size) >= 0 && delayVal >= 0 ) {
            rs->encoderDelay = delayVal;
        } else {
            // do not attempt to grab a frame until encoder delay had been established
            return -1;
        }
    }

    while ( !rs->frames->empty() &&
            !rs->fileContexts->empty() ) {
        // some frames obtained before file opening notification occurred are still here
        // use those, before asking for more -- IF we have an open file context
        f = rs->frames->front();
        pts = frame_get_api(f)->get_pts(f);
        currentFileContext = &rs->fileContexts->front();

        if ( pts < currentFileContext->start ) {
            // somehow this frame preceeds our current range ...
            // should not happen, but protect against this anyway
            rs->logCb(logDebug, _FMT("Dropping a frame: pts=" << pts << " currentFilePts=" << currentFileContext->start ));
            rs->frames->pop_front();
            frame_unref(&f);
        } else
        if ( pts > currentFileContext->end && currentFileContext->end != (uint64_t)-1 ) {
            // first frame in the queue is outside of our current range ... time to move to the next range
            // (and retry with the same frame)
            TRACE(_FMT("Frame pts=" << pts << " is outside of the range of [" << currentFileContext->start << "," << currentFileContext->end << "]"));
            rs->fileContexts->pop_front();
            f = NULL;
        } else if ( pts + rs->encoderDelay < rs->lastPtsInQueue ) {
            TRACE(_FMT("Returning frame pts=" << pts << " from frame queue" << " q=" << rs->frames->size() ));
            rs->frames->pop_front();
            break;
        } else {
            TRACE(_FMT("Retaining frame in the queue due to encoder delay: pts=" << pts <<
                    " lastPtsInQueue=" << rs->lastPtsInQueue << " delay=" << rs->encoderDelay ));
            f = NULL;
            break;
        }
    }

    *frame = f;
    return (*frame != NULL) ? 0 : -1;
}

//-----------------------------------------------------------------------------
static int         rs_stream_read_frame        (stream_obj* stream, frame_obj** frame)
{
    DECLARE_STREAM_RS(stream, rs);

    frame_obj*      f = NULL;
    int             res;

    while (f == NULL) {
        sv_mutex_enter(rs->mutex);
        _rs_stream_get_frame_from_queue(rs, &f);
        sv_mutex_exit(rs->mutex);
        if (f != NULL)
            break;

        res = get_default_stream_api()->read_frame(stream, &f);
        if ( res < 0 || f == NULL ) {
            return res;
        }

        frame_api_t* fapi = frame_get_api(f);
        int nType = fapi->get_media_type(f);
        if ( nType != mediaVideo && nType != mediaVideoTime ) {
            // for now we can safely disregard non-video frames coming our way
            // a day will come, though, and we'll need to maintain a separate queue for those
            frame_unref(&f);
        } else {
            // can do this without mutex -- we only modify contexts list in the other thread
            rs->lastPtsInQueue = fapi->get_pts(f);
            rs->frames->push_back(f);
            f = NULL;
        }
    }

    *frame = f;
    return 0;
}

//-----------------------------------------------------------------------------
static int         rs_stream_close             (stream_obj* stream)
{
    DECLARE_STREAM_RS(stream, rs);
    rs->sourceApi->close(rs->source);
    stream_unref(&rs->source);
    return 0;
}


//-----------------------------------------------------------------------------
static void rs_stream_destroy         (stream_obj* stream)
{
    DECLARE_STREAM_RS_V(stream, rs);
    rs->logCb(logTrace, _FMT("Destroying stream object " << (void*)stream));
    rs_stream_close(stream); // make sure all the internals had been freed
    sv_freep(&rs->recFilterName);

    sv_mutex_enter(rs->mutex);
    delete rs->fileContexts;
    frame_list_destroy(&rs->frames);
    sv_mutex_exit(rs->mutex);

    sv_mutex_destroy(&rs->mutex);
    stream_destroy( stream );
}

//-----------------------------------------------------------------------------
SVVIDEOLIB_API stream_api_t*     get_recorder_sync_api                    ()
{
    return &_g_rs_stream_provider;
}

