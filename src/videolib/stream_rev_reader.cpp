/*****************************************************************************
 *
 * stream_rev_reader.cpp
 *   Reads (always finite) input in reverse. Unused in context of Sighthound Video.
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
#define SV_MODULE_ID "REVREADER"
#include "sv_module_def.hpp"

#include "streamprv.h"

#include "frame_basic.h"
#include "sv_pixfmt.h"

#include "videolibUtils.h"

#include <algorithm>
#include <list>

#define FF_FILTER_MAGIC 0x1270
static bool _gInitialized = false;

using namespace std;

//-----------------------------------------------------------------------------
typedef std::list<frame_obj*> FrameList;
typedef FrameList::iterator FrameIter;


//-----------------------------------------------------------------------------
typedef struct fs_filter  : public stream_base  {
    FrameList*     frames;
    INT64_T        firstPtsInList;
    INT64_T        lastPtsProcessed;
    INT64_T        firstPts;
    INT64_T        duration;
    int            eof;
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
static int         ff_stream_get_param          (stream_obj* stream,
                                                const CHAR_T* name,
                                                void* value,
                                                size_t* size);
static int         ff_filter_open_in            (stream_obj* stream);
static int         ff_filter_read_frame         (stream_obj* stream, frame_obj** frame);
static int         ff_filter_close              (stream_obj* stream);
static void        ff_filter_destroy            (stream_obj* stream);

extern "C" stream_api_t*     get_ff_filter_api                    ();


//-----------------------------------------------------------------------------
stream_api_t _g_rev_reader_filter_provider = {
    ff_filter_create,
    get_default_stream_api()->set_source,
    get_default_stream_api()->set_log_cb,
    get_default_stream_api()->get_name,
    get_default_stream_api()->find_element,
    get_default_stream_api()->remove_element,
    get_default_stream_api()->insert_element,
    ff_filter_set_param,
    ff_stream_get_param,
    ff_filter_open_in,
    get_default_stream_api()->seek,
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
                &_g_rev_reader_filter_provider,
                name,
                ff_filter_destroy );

    res->frames = new FrameList;
    res->lastPtsProcessed = 0;
    res->eof = 0;
    res->duration = 0;
    res->firstPtsInList = 0;
    return (stream_obj*)res;
}

//-----------------------------------------------------------------------------
static int         ff_filter_set_param             (stream_obj* stream,
                                            const CHAR_T* name,
                                            const void* value)
{
    DECLARE_FF_FILTER(stream, fffilter);
    name = stream_param_name_apply_scope(stream, name);

    return default_set_param(stream, name, value);
}

//-----------------------------------------------------------------------------
static int         ff_stream_get_param          (stream_obj* stream,
                                                const CHAR_T* name,
                                                void* value,
                                                size_t* size)
{
    DECLARE_FF_FILTER(stream, fffilter);
    name = stream_param_name_apply_scope(stream, name);
    COPY_PARAM_IF(fffilter, name, "eof", int, fffilter->eof);
    return default_get_param(stream, name, value, size);

}

//-----------------------------------------------------------------------------
static int         ff_filter_open_in                (stream_obj* stream)
{
    DECLARE_FF_FILTER(stream, fffilter);
    int res = 0;


    res = default_open_in(stream);
    if ( res >= 0 ) {
        size_t size = sizeof(int64_t);
        bool   gotFirstPts = false;

        while (!gotFirstPts) {
            frame_obj* f = NULL;
            frame_api* api;
            if ( default_read_frame(stream, &f) < 0 || f == NULL) {
                fffilter->logCb(logError, _FMT("Error reading next frame"));
                return -1;
            }
            api = frame_get_api(f);
            if ( api->get_media_type(f) == mediaVideo ) {
                fffilter->firstPts = api->get_pts(f);
                gotFirstPts = true;
            }
            frame_unref(&f);
        }

        if ( default_get_param(stream, "duration", &fffilter->duration, &size) >= 0 ) {
            TRACE(_FMT("duration=" << fffilter->duration));
            fffilter->lastPtsProcessed = fffilter->duration+1;
        } else {
            fffilter->logCb(logError, _FMT("Failed to determine stream duration"));
            return -1;
        }
    }
    return res;
}


//-----------------------------------------------------------------------------
static int         ff_filter_read_frame        (stream_obj* stream,
                                                    frame_obj** frame)
{
    DECLARE_FF_FILTER(stream, fffilter);

    if (fffilter->frames->empty()) {
        if ( fffilter->lastPtsProcessed <= fffilter->firstPts ) {
            fffilter->logCb(logInfo, _FMT("EOF had been reached"));
            fffilter->eof = 1;
            return -1;
        }
        static  const int seekRetryDelta = 5;
        int     delta = 1;
        int64_t nextSeekPts = fffilter->lastPtsProcessed - delta,
                nextPts = nextSeekPts;
        int64_t lastPtsRead = fffilter->firstPts - 1;
        int64_t firstPtsRead = fffilter->firstPts - 1;
        int     eof = 0;
        size_t  size=sizeof(int);
        int     res;
        int     firstRun = (fffilter->lastPtsProcessed > fffilter->duration);

Retry:
        nextSeekPts = fffilter->lastPtsProcessed - delta;
        if ( nextSeekPts < fffilter->firstPts ) {
            TRACE(_FMT("Seeking past beginning of file to pts=" << nextSeekPts));
            fffilter->eof = 1;
            return -1;
        }
        TRACE(_FMT("Seeking to pts=" << nextSeekPts));
        res     = default_seek(stream, nextSeekPts, sfBackward);
        if ( res < 0 ) {
            fffilter->logCb(logError, _FMT("Error seeking to " << nextSeekPts));
            return -1;
        }
        do {
            frame_obj* f = NULL;
            frame_api* api;
            if ( default_read_frame(stream, &f) < 0 || f == NULL) {
                if ( default_get_param(stream, "eof", &eof, &size ) < 0 || !eof ) {
                    fffilter->logCb(logError, _FMT("Error reading next frame after " << lastPtsRead));
                    return -1;
                }
                if ( firstRun && lastPtsRead < 0 && nextSeekPts>seekRetryDelta ) {
                    TRACE(("EOF before first packet. Trying to seek further back"));
                    eof = 0;
                    delta += seekRetryDelta;
                    goto Retry;
                }
                // we can continue rewinding
            } else {
                bool added = false;
                bool retry = false;

                api = frame_get_api(f);
                if ( api->get_media_type(f) == mediaVideo ) {
                    INT64_T pts = api->get_pts(f);
                    if ( pts >= fffilter->lastPtsProcessed && lastPtsRead < fffilter->firstPts ) {
                        TRACE(_FMT("Retrying: pts=" << pts << " lastPtsProcessed=" << fffilter->lastPtsProcessed));
                        retry = true;
                    } else {
                        lastPtsRead = pts;
                        if ( firstPtsRead < fffilter->firstPts )
                            firstPtsRead = pts;
                        if ( pts <= nextPts ) {
                            TRACE(_FMT("Read frame pts="<< pts << " ptr=" << (void*)f));
                            fffilter->frames->push_back(f);
                            added = true;
                        }
                    }
                }

                if (!added) {
                    frame_unref(&f);
                    if (retry) {
                        delta += seekRetryDelta;
                        goto Retry;
                    }
                }
            }
        } while (lastPtsRead <= nextPts &&
                !eof);
        TRACE(_FMT("Finished refilling the buffer: count="<<fffilter->frames->size() <<
                                " firstPts=" << firstPtsRead <<
                                " lastPts=" << lastPtsRead <<
                                " prevPts=" << fffilter->lastPtsProcessed));
        fffilter->lastPtsProcessed = firstPtsRead;
    }

    *frame = fffilter->frames->back();
    fffilter->frames->pop_back();
    TRACE(_FMT("Returning frame with pts=" << frame_get_api(*frame)->get_pts(*frame)));
    return 0;
}

//-----------------------------------------------------------------------------
static int         ff_filter_close             (stream_obj* stream)
{
    DECLARE_FF_FILTER(stream, fffilter);
    return 0;
}


//-----------------------------------------------------------------------------
static void ff_filter_destroy         (stream_obj* stream)
{
    DECLARE_FF_FILTER_V(stream, fffilter);
    fffilter->logCb(logTrace, _FMT("Destroying stream object " << (void*)stream));
    while (fffilter->frames->size()) {
        frame_obj* f = fffilter->frames->front();
        fffilter->frames->pop_front();
        frame_unref(&f);
    }
    delete fffilter->frames;
    ff_filter_close(stream); // make sure all the internals had been freed
    stream_destroy( stream );
}

//-----------------------------------------------------------------------------
SVVIDEOLIB_API stream_api_t*     get_rev_reader_filter_api                    ()
{
    if (!_gInitialized) {
        _gInitialized=true;
    }
    return &_g_rev_reader_filter_provider;
}

