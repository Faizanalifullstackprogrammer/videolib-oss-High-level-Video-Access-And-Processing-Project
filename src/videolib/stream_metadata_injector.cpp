/*****************************************************************************
 *
 * stream_metadata_injector.cpp
 *   Node injecting metadata (such as bounding boxes) before video frames.
 *   Not used in a real-time context at this point, and only utilized in clip
 *   playback and generation context.
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

/*
*
* This filter delays the frames it reads, until feedback for a frame is received via set_param call.
*
*/


#undef SV_MODULE_VAR
#define SV_MODULE_VAR sf
#define SV_MODULE_ID "METAINJECT"
#include "sv_module_def.hpp"

#include "streamprv.h"
#include "frame_basic.h"
#include "videolibUtils.h"

#include <list>
#include <algorithm>

#define METAINJECT_FILTER_MAGIC 0x1256

//-----------------------------------------------------------------------------
typedef struct metainject_filter  : public stream_base  {
    int                 preloaded;
    FrameList*          metadataFramesPreloaded;
    FrameList*          metadataFramesAvailable;
    FrameList*          dataFramesAvailable;
    frame_allocator*    fa;
    sv_mutex*           mutex;
    sv_event*           event;
    int                 blocking;
    INT64_T             lastMetaPts;
    INT64_T             lastVideoPts;
    int                 metaWritten;
    int                 metaIgnored;
    int                 maxDelayFrames;
    int                 minJitterBuffer;
    int                 videoFramesCount;
    int                 eof;
    bool                isInitialized;
} metainject_filter_obj;

//-----------------------------------------------------------------------------
// Stream API
//-----------------------------------------------------------------------------

//-----------------------------------------------------------------------------
//  Forward declarations
//-----------------------------------------------------------------------------
static stream_obj* metainject_filter_create             (const char* name);
static int         metainject_filter_set_param          (stream_obj* stream,
                                                const CHAR_T* name,
                                                const void* value);
static int         metainject_filter_get_param          (stream_obj* stream,
                                                const CHAR_T* name,
                                                void* value,
                                                size_t* size);
static int         metainject_filter_open_in            (stream_obj* stream);
static int         metainject_filter_seek               (stream_obj* stream,
                                                       INT64_T offset,
                                                        int flags);
static int         metainject_filter_read_frame         (stream_obj* stream, frame_obj** frame);
static void        metainject_filter_destroy            (stream_obj* stream);

//-----------------------------------------------------------------------------
stream_api_t _g_metainject_filter_provider = {
    metainject_filter_create,
    get_default_stream_api()->set_source,
    get_default_stream_api()->set_log_cb,
    get_default_stream_api()->get_name,
    get_default_stream_api()->find_element,
    get_default_stream_api()->remove_element,
    get_default_stream_api()->insert_element,
    metainject_filter_set_param,
    metainject_filter_get_param,
    metainject_filter_open_in,
    metainject_filter_seek,
    get_default_stream_api()->get_width,
    get_default_stream_api()->get_height,
    get_default_stream_api()->get_pixel_format,
    metainject_filter_read_frame,
    get_default_stream_api()->print_pipeline,
    get_default_stream_api()->close,
    _set_module_trace_level
};


//-----------------------------------------------------------------------------
#define DECLARE_METAINJECT_FILTER(stream, name) \
    DECLARE_OBJ(metainject_filter_obj, name,  stream, METAINJECT_FILTER_MAGIC, -1)

#define DECLARE_METAINJECT_FILTER_V(stream, name) \
    DECLARE_OBJ_V(metainject_filter_obj, name,  stream, METAINJECT_FILTER_MAGIC)

static stream_obj*   metainject_filter_create                (const char* name)
{
    metainject_filter_obj* res = (metainject_filter_obj*)stream_init(sizeof(metainject_filter_obj),
                METAINJECT_FILTER_MAGIC,
                &_g_metainject_filter_provider,
                name,
                metainject_filter_destroy );
    res->isInitialized = false;
    res->dataFramesAvailable = frame_list_create();
    res->metadataFramesAvailable = frame_list_create();
    res->metadataFramesPreloaded = frame_list_create();
    res->fa = create_frame_allocator(_STR("metainj_"<<name));
    res->mutex = sv_mutex_create();
    res->event = NULL;
    res->blocking = 0;
    res->lastVideoPts = 0;
    res->lastMetaPts = 0;
    res->metaWritten = 0;
    res->metaIgnored = 0;
    res->maxDelayFrames = 0;
    res->minJitterBuffer = 1;
    res->eof = 0;
    res->preloaded = 1;
    res->videoFramesCount = 0;

    return (stream_obj*)res;
}

//-----------------------------------------------------------------------------
static int         metainject_filter_set_param             (stream_obj* stream,
                                            const CHAR_T* name,
                                            const void* value)
{
    DECLARE_METAINJECT_FILTER(stream, sf);
    name = stream_param_name_apply_scope(stream, name);
    if (!_strnicmp(name, "metadata.", 9)) {
        if ( sf->preloaded && sf->isInitialized) {
            return -1;
        }

        sv_mutex_enter(sf->mutex);
        const char* sTime = &name[9];
        const char* sValue = (const char*)value;
        static const size_t stdalloc = 1024;
        size_t datalen = strlen(sValue)+1;
        size_t alloclen = std::max(datalen, stdalloc);
        INT64_T     ts;
        sscanf(sTime, I64FMT, &ts);

        TRACE(_FMT("Adding meta: " << name << "; pts=" << ts ));
        if (ts < sf->lastVideoPts) {
            sf->metaIgnored++;
            sf->logCb(logWarning, _FMT("Ignoring subtitle for " << ts << ": last video frame served " << sf->lastVideoPts <<
                " metaWritten=" << sf->metaWritten << " metaIgnored=" << sf->metaIgnored));
            sv_mutex_exit(sf->mutex);
            return 0;
        }

        basic_frame_obj* bf = alloc_basic_frame2(METAINJECT_FILTER_MAGIC, alloclen, sf->logCb, sf->fa );
        bf->pts = bf->dts = ts;
        bf->width = 0;
        bf->height = 0;
        bf->mediaType = mediaMetadata;
        bf->dataSize = datalen;
        bf->pixelFormat = pfmtUndefined;
        strcpy((char*)bf->data, sValue);
        if (sf->preloaded) {
            sf->metadataFramesPreloaded->push_back((frame_obj*)bf);
        } else {
            sf->metadataFramesAvailable->push_back((frame_obj*)bf);
        }
        sv_mutex_exit(sf->mutex);
        if ( sf->event ) {
            sv_event_set(sf->event);
        }
        return 0;
    }
    if (!_stricmp(name, "blocking")) {
        sv_mutex_enter(sf->mutex);
        int blocking = *(int*)value;
        if (!blocking) {
            sv_event_destroy(&sf->event);
        } else if (!sf->event) {
            sf->event = sv_event_create(0,0);
        }
        sv_mutex_exit(sf->mutex);
        return 0;
    }
    if (!_stricmp(name, "maxDelayFrames")) {
        int maxDelayFrames = *(int*)value;
        if ( maxDelayFrames < sf->minJitterBuffer ) {
            sf->logCb(logError, _FMT("Attempting to set max queue size to " <<
                                    maxDelayFrames << " while jitter buffer is at " <<
                                    sf->minJitterBuffer ));
            return -1;
        }
        sf->maxDelayFrames = maxDelayFrames;
        return 0;
    }
    if (!_stricmp(name, "minJitterBuffer")) {
        int minJitterBuffer = *(int*)value;
        if ( minJitterBuffer > sf->maxDelayFrames && sf->maxDelayFrames != 0 ) {
            sf->logCb(logError, _FMT("Attempting to set min jitter buffer to " <<
                                    minJitterBuffer << " while max queue size is at " <<
                                    sf->maxDelayFrames ));
            return -1;
        }
        sf->minJitterBuffer = minJitterBuffer;
        return 0;
    }

    return default_set_param(stream, name, value);
}

//-----------------------------------------------------------------------------
static int        metainject_filter_get_param(stream_obj* stream,
                                                const CHAR_T* name,
                                                void* value,
                                                size_t* size)
{
    DECLARE_METAINJECT_FILTER(stream, sf);
    name = stream_param_name_apply_scope(stream, name);
    COPY_PARAM_IF(sf, name, "nextMetadata", frame_obj*,
                                ( !sf->preloaded || sf->metadataFramesAvailable->empty() )
                                            ? NULL
                                            : sf->metadataFramesAvailable->front() );
    return default_get_param(stream, name, value, size);

}

//-----------------------------------------------------------------------------
static void       _metainject_copy_preloaded            (metainject_filter_obj* sf,
                                                        UINT64_T firstTs)
{
    frame_list_clear(sf->metadataFramesAvailable);

    for (FrameList::iterator it=sf->metadataFramesPreloaded->begin();
         it != sf->metadataFramesPreloaded->end();
         it++ ) {
        frame_obj* f = *it;
        INT64_T pts = frame_get_api(f)->get_pts(f);

        // we may need an earlier metadata to apply to the data frame we return after seek
        // (this especially applies to reverse frame-by-frame playback)
        // make sure we include a reasonable amount of meta frames preceeding the relevant timestamp
        static const int _kMaxApplicableMetadataDistance = 50;

        if ( pts + _kMaxApplicableMetadataDistance >= firstTs ) {
            frame_ref(f);
            sf->metadataFramesAvailable->push_back(f);
        }
    }
}

//-----------------------------------------------------------------------------
static int         metainject_filter_open_in            (stream_obj* stream)
{
    DECLARE_METAINJECT_FILTER(stream, sf);
    int res;

    res = default_open_in(stream);

    if ( res == 0 ) {
        sf->isInitialized = true;
        _metainject_copy_preloaded(sf, 0);
    }

    return res;
}

//-----------------------------------------------------------------------------
static int         metainject_filter_seek               (stream_obj* stream,
                                                       INT64_T offset,
                                                       int flags)
{
    DECLARE_METAINJECT_FILTER(stream, sf);

    TRACE(_FMT("Seeking to " << offset));
    int res = default_seek(stream, offset, flags);
    if ( res == 0 ) {
        frame_list_clear(sf->dataFramesAvailable);
        _metainject_copy_preloaded(sf, offset);
        sf->lastVideoPts = 0;
        sf->lastMetaPts = 0;
        sf->eof = 0;
        sf->videoFramesCount = 0;
    }

    return res;
}


//-----------------------------------------------------------------------------
static frame_obj*  _metainject_get_frame    (metainject_filter_obj* sf)
{
    frame_obj*  out = NULL;
    int         metaSize = -1, dataSize = -1, counter;
    INT64_T     metaPts = -1, dataPts = -1, secondDataPts = -1;
    frame_obj   *fData, *fMeta, *fSecondData;
    const char  *condition = "unknown";
    FrameList::iterator it;

    dataSize = sf->dataFramesAvailable->size();
    if ( dataSize == 0 ) {
        // no data, no questions
        TRACE_C(100, _FMT("No data frames are currently available"));
        return NULL;
    }

    fData = sf->dataFramesAvailable->front();
    dataPts = frame_get_api(fData)->get_pts(fData);

    if ( frame_get_api(fData)->get_media_type(fData) != mediaVideo ) {
        // no need to align non-video frames with metadata, return as they come
        condition = "non-video frame";
        goto ReturnData;
    }

    metaSize = sf->metadataFramesAvailable->size();
    if ( metaSize == 0 ) {
        if ( sf->preloaded ) {
            // no metadata left, and no more expected
            condition = "no more metadata";
            goto ReturnVideo;
        }
        if ( dataPts <= sf->lastMetaPts ) {
            // the data frame corresponds to previously served meta frame
            condition = "data frame in order";
            goto ReturnVideo;
        }

        // wait for more meta to be injected
        TRACE_C(10, _FMT("Can't return frame right now, need more metadata, dataPts=" << dataPts));
        return NULL;
    }
    fMeta = sf->metadataFramesAvailable->front();
    metaPts = frame_get_api(fMeta)->get_pts(fMeta);


    if ( metaPts <= dataPts ) {
        condition = "meta frame in order";
        goto ReturnMeta;
    }


    // at this point we have a metadata frame with a timestamp later than that of first data frame
    // we need to determine if it's closer to first or second data frame
    if ( sf->videoFramesCount < 2 ) {
        // don't have 2 data frames
        if ( sf->eof ) {
            // and won't have it ... just return the data frame
            condition = "eof";
            goto ReturnVideo;
        }

        TRACE_C(10, _FMT("Can't return frame right now, need more data, dataPts=" << dataPts << " metaPts=" << metaPts));
        return NULL;
    }
    it = sf->dataFramesAvailable->begin();
    counter = 0;
    while (it != sf->dataFramesAvailable->end()) {
        fSecondData = *it;
        if ( frame_get_api(fSecondData)->get_media_type(fSecondData) == mediaVideo ) {
            counter ++;
            if ( counter > 1 ) {
                secondDataPts = frame_get_api(fSecondData)->get_pts(fSecondData);
                break;
            }
        }
        it++;
    }
    if ( counter != 2 ) {
        sf->logCb(logError, _FMT("Inconsistent state: expecting at least 2 video frames in queue, got " << counter));
        return NULL;
    }

    if ( metaPts >= secondDataPts ) {
        condition = "no meta update";
        goto ReturnVideo;
    }

    if ( metaPts - dataPts > secondDataPts - metaPts ) {
        condition = "data in order";
        goto ReturnVideo;
    }

    condition = "meta adjusting ts";
    frame_get_api(fMeta)->set_pts(fMeta, dataPts);
    goto ReturnMeta;


ReturnVideo:
    sf->videoFramesCount--;

ReturnData:
    TRACE_C(10, _FMT("Returning data: dataPts=" << dataPts << " metaPts=" << metaPts << " dataPts2=" << secondDataPts <<
                    " dataSize=" << dataSize << " metaSize=" << metaSize << ", " << condition ));
    out = fData;
    sf->dataFramesAvailable->pop_front();
    sf->lastVideoPts = dataPts;
    return out;

ReturnMeta:
    TRACE_C(10, _FMT("Returning meta: dataPts=" << dataPts << " metaPts=" << metaPts << " dataPts2=" << secondDataPts <<
                    " dataSize=" << dataSize << " metaSize=" << metaSize << ", " << condition ));
    out = fMeta;
    sf->metadataFramesAvailable->pop_front();
    sf->lastMetaPts = metaPts;
    return out;
}

//-----------------------------------------------------------------------------
static int         metainject_filter_read_frame        (stream_obj* stream,
                                                    frame_obj** frame)
{
    DECLARE_METAINJECT_FILTER(stream, sf);
    int         res = -1;
    frame_obj*  tmp = NULL;

    *frame = NULL;

Retry:
    tmp = NULL;
    res = 0;

    sv_mutex_enter(sf->mutex);
    *frame = _metainject_get_frame(sf);
    sv_mutex_exit(sf->mutex);

    // read a fresh frame ...
    if ( !*frame ) {
        res = (sf->eof ? -1 : sf->sourceApi->read_frame(sf->source, &tmp));

        if ( res == 0 && tmp ) {
            sv_mutex_enter(sf->mutex);
            TRACE(_FMT("Adding data frame: " << frame_get_api(tmp)->get_pts(tmp) << " count=" << sf->dataFramesAvailable->size()));
            sf->dataFramesAvailable->push_back(tmp);
            if (frame_get_api(tmp)->get_media_type(tmp)==mediaVideo) {
                sf->videoFramesCount++;
            }
            sv_mutex_exit(sf->mutex);
            goto Retry;
        } else if ( !sf->eof ) {
            size_t size = sizeof(int);
            if ( sf->sourceApi->get_param(sf->source, "eof", &sf->eof, &size) >= 0 &&
                 sf->eof ) {
                TRACE(_FMT("End of file detected. Retrying ..."));
                goto Retry;
            }
        }

        TRACE(_FMT("Frame queues: metadata=" << sf->metadataFramesAvailable->size() <<
            " data=" << sf->dataFramesAvailable->size() <<
            " video=" << sf->videoFramesCount ));

        if ( sf->event ) {
            TRACE(_FMT("Waiting for more metadata!"));
            // block until some metadata comes in
            sv_event_wait( sf->event, 0 );
            if ( sf->event ) {
                goto Retry;
            } else {
                TRACE(_FMT("Shutdown!"));
            }
        }
    } else {
        res = 0;
    }

    return res;
}


//-----------------------------------------------------------------------------
static void metainject_filter_destroy         (stream_obj* stream)
{
    DECLARE_METAINJECT_FILTER_V(stream, sf);
    sf->logCb(logTrace, _FMT("Destroying stream object " << (void*)stream));
    default_close(stream); // make sure all the internals had been freed
    frame_list_destroy( &sf->dataFramesAvailable );
    frame_list_destroy( &sf->metadataFramesAvailable );
    frame_list_destroy( &sf->metadataFramesPreloaded );
    destroy_frame_allocator( &sf->fa, sf->logCb );
    sv_event_destroy(&sf->event);
    sv_mutex_destroy(&sf->mutex);
    stream_destroy( stream );
}

//-----------------------------------------------------------------------------
SVVIDEOLIB_API stream_api_t*     get_metainject_filter_api                    ()
{
    return &_g_metainject_filter_provider;
}

