/*****************************************************************************
 *
 * stream_api.cpp
 *   Base implementation of videoLib objects (nodes, frames, events, etc)
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

#include "sv_os.h"
#include "streamprv.h"
#include "sv_ffmpeg.h"


//-----------------------------------------------------------------------------
FrameList*          frame_list_create()
{
    return new FrameList;
}

//-----------------------------------------------------------------------------
void                frame_list_clear(FrameList* l)
{
    while (!l->empty()) {
        frame_obj* f = l->front();
        l->pop_front();
        frame_unref(&f);
    }
}

//-----------------------------------------------------------------------------
void                frame_list_destroy(FrameList** pl)
{
    if ( pl && *pl ) {
        FrameList* l = *pl;
        frame_list_clear(l);
        delete l;
        *pl = NULL;
    }
}


//-----------------------------------------------------------------------------
frame_base* frame_init                                     (size_t size,
                                                            int tag,
                                                            frame_api_t* api,
                                                            fn_frame_destroy destructor)
{
    frame_base* base = (frame_base*)malloc(size);
    base->refcount = 0;
    base->magic = tag;
    base->api = api;
    base->destructor = destructor;
    return base;
}

//-----------------------------------------------------------------------------
void        _default_log_cb               (int level, const CHAR_T* s)
{
    printf("%d: %s\n", level, s);
}

//-----------------------------------------------------------------------------
static fn_stream_log    g_DefaultLogCb = _default_log_cb;


//-----------------------------------------------------------------------------
stream_base* stream_init                                    (size_t size,
                                                            int tag,
                                                            stream_api_t* api,
                                                            const char* name,
                                                            fn_stream_destroy destructor)
{
    stream_base* base = (stream_base*)malloc(size);
    base->refcount = 0;
    base->magic = tag;
    base->api = api;
    base->destructor = destructor;
    base->sourceApi = NULL;
    base->source = NULL;
    base->sourceInitialized = false;
    base->name = name?strdup(name):NULL;
    base->passthrough = 0;
    base->logCb = g_DefaultLogCb;
    return base;
}

#define GET_API_TEMPLATE(myclass)\
SVCORE_API myclass##_api_t* myclass##_get_api                           (myclass##_obj* obj)\
{\
    if (obj==NULL) {\
        return NULL;\
    }\
    return ((myclass##_base*)obj)->api;\
}


#define REF_TEMPLATE(myclass)\
SVCORE_API REF_T myclass##_ref                               (myclass##_obj* obj)\
{\
    myclass##_base_t* base = (myclass##_base_t*)obj;\
    if (base==NULL) return 0;\
    REF_T res = ATOMIC_INCREMENT(base->refcount);\
    return res;\
}

#define UNREF_TEMPLATE(myclass) \
SVCORE_API REF_T myclass##_unref                             (myclass##_obj** obj)\
{\
    if (obj==NULL) return 0;\
    myclass##_base_t* base = (myclass##_base_t*)*obj;\
    if (base==NULL) return 0;\
    REF_T res = ATOMIC_DECREMENT(base->refcount);\
    if ( res <= 0 ) {\
        base->destructor( *obj );\
    }\
    *obj = NULL;\
    return res;\
}

#define SET_TEMPLATE(myclass)\
SVCORE_API void myclass##_set                               (myclass##_obj** dst, myclass##_obj* src)\
{\
    myclass##_ref(src);\
    if (*dst) {\
        myclass##_unref(dst);\
    }\
    *dst = src;\
}


GET_API_TEMPLATE(frame);
REF_TEMPLATE(frame);
UNREF_TEMPLATE(frame);
//SET_TEMPLATE(frame);


GET_API_TEMPLATE(stream);
REF_TEMPLATE(stream);
UNREF_TEMPLATE(stream);
SET_TEMPLATE(stream);

GET_API_TEMPLATE(stream_ev);
REF_TEMPLATE(stream_ev);
UNREF_TEMPLATE(stream_ev);
SET_TEMPLATE(stream_ev);



//-----------------------------------------------------------------------------
SVCORE_API
void              stream_set_default_log_cb              (fn_stream_log log)
{
    g_DefaultLogCb = ( log ? log : _default_log_cb );
    if ( log ) {
        ffmpeg_log_open( log, 0 );
    }
}



#define DECLARE_BASE(param, name, rv) \
    stream_base* name = (stream_base*)param;\
    if (name == NULL)\
        return rv;

#define DECLARE_BASE_I(param, name)   DECLARE_BASE(param, name, -1)
#define DECLARE_BASE_P(param, name)   DECLARE_BASE(param, name, NULL)

#define DECLARE_BASE_V(param, name) \
    stream_base* name = (stream_base*)param;\
    if (name == NULL)\
        return;


//-----------------------------------------------------------------------------
SVCORE_API void stream_destroy                          (stream_obj* stream)
{
    DECLARE_BASE_V(stream, def);
    if (def) {
        get_default_stream_api()->close(stream);
        sv_freep(&def->name);
        free(def);
    }
}

//-----------------------------------------------------------------------------
const char*       stream_param_name_apply_scope           (stream_obj* stream, const char* name)
{
    DECLARE_BASE_P(stream, def);
    if ( def->name ) {
        // adjust the scope of the name, if needed
        size_t len = strlen(def->name);
        if ( !_strnicmp(def->name, name, len) && name[len] == '.' ) {
            name = &name[len+1];
        }
    }
    return name;
}

//-----------------------------------------------------------------------------
#define GRAPH_DEBUG 0
int         defaultstream_set_source       (stream_obj* stream,
                                      stream_obj* source,
                                      INT64_T flags)
{
    DECLARE_BASE_I(stream, def);
    if ( def->source ) {
        stream_unref(&def->source);
        def->sourceApi = NULL;
    }
#if GRAPH_DEBUG
    int i = 0;
    char graph[2048] = "";
    strcpy(graph, def->name);
    strcat(graph, "->");
    stream_base* srcBase = (stream_base*)source;
    while (srcBase!=NULL && i++ < 20) {
        strcat(graph, srcBase->name);
        strcat(graph, "->");
        if ( srcBase == (stream_base*)stream ) {
            def->logCb(logError, _FMT("Circular reference detected while trying to set source for " << def->name <<
                        "; graph=" << graph ));
            return -1;
        }
        srcBase = (stream_base*)srcBase->source;
    };
    if ( i>=20 ) {
        def->logCb(logError, _FMT("Circular reference detected while trying to set source for " << def->name <<
                    "; graph=" << graph ));
        return -1;
    }
    def->logCb(logError, _FMT("New pipeline is: " << graph ));
#endif
    stream_set( &def->source, source );
    if ( def->source != NULL ) {
        def->sourceApi = stream_get_api(def->source);
#if GRAPH_DEBUG
        def->logCb(logDebug, _FMT("Setting source of " <<
                                def->name << " to " <<
                                def->sourceApi->get_name(def->source) <<
                                "; " <<
                                (def->sourceInitialized?"already":"not yet") <<
                                " initialized"));
#endif
        def->sourceApi->set_log_cb(def->source, def->logCb);
        def->sourceInitialized = (flags&svFlagStreamInitialized)?1:0;
#if GRAPH_DEBUG
        def->logCb(logDebug, _FMT("Set source of " <<
                                def->name << " to " <<
                                def->sourceApi->get_name(def->source) <<
                                "; " <<
                                (def->sourceInitialized?"already":"not yet") <<
                                " initialized"));
#endif
    }
    return 0;
}


//-----------------------------------------------------------------------------
int         defaultstream_set_log_cb         (stream_obj* stream, fn_stream_log log)
{
    DECLARE_BASE_I(stream, def);
    def->logCb = log?log:g_DefaultLogCb;
    if ( def->source != NULL ) {
        def->sourceApi->set_log_cb(def->source, def->logCb);
    }
    return 0;
}

//-----------------------------------------------------------------------------
const char*  defaultstream_get_name             (stream_obj* stream)
{
    DECLARE_BASE_P(stream, def);
    return def->name;
}

//-----------------------------------------------------------------------------
stream_obj*  defaultstream_find_element          (stream_obj* stream,
                                          const char* name)
{
    DECLARE_BASE_P(stream, def);
    if ( name == NULL ) {
        return def->source;
    }
    if ( def->name != NULL && !strcmp(def->name,name) ) {
        return stream;
    }
    if ( def->source != NULL ) {
        return def->sourceApi->find_element(def->source, name);
    }
    return NULL;
}

//-----------------------------------------------------------------------------
int          defaultstream_remove_element       (stream_obj** pStream,
                                          struct stream_api** pAPI,
                                          const char* name,
                                          stream_obj** removedStream)
{
    if (!pStream) {
        return -1;
    }
    DECLARE_BASE_I(*pStream, def);
    if ( name == NULL ) {
        return 0;
    }
    def->logCb(logDebug, _FMT("Removing pipeline element " << name <<
                            " in pipeline starting with " << def->name ));
    if ( def->name != NULL && !strcmp(def->name,name) ) {
        // keep reference to our source
        stream_obj* oldSource = NULL;
        // make sure there's at least one reference to our source
        stream_set(&oldSource, def->source);
        // zero out the source, so default closure methods won't close it
        stream_unref(&def->source);
        if ( removedStream == NULL ) {
            // make sure the strem is closed -- this should resolve all circular references
            stream_get_api(*pStream)->close(*pStream);
        } else {
            // the caller wants to preserve the removed node for future reuse
            stream_set(removedStream, *pStream);
        }
        // link our source to whoever is upstream from us
        stream_set(pStream, oldSource);
        // now we can free the reference we've taken out
        stream_unref(&oldSource);
        // update the API variable, if needed
        if ( pAPI ) {
            *pAPI = stream_get_api(*pStream);
        }
        return 1;
    }
    if ( def->source != NULL ) {
        return def->sourceApi->remove_element(&def->source, &def->sourceApi, name, removedStream);
    }
    return -1;
}

//-----------------------------------------------------------------------------
int        defaultstream_insert_element   (stream_obj** pStream,
                                    struct stream_api** pAPI,
                                    const char* insertBefore,
                                    stream_obj* newElement,
                                    INT64_T flags)
{
    if (!pStream) {
        return -1;
    }

    if (*pStream == NULL) {
        // This is the first element of pipeline ... init the head, ref the stream
        if ( insertBefore != NULL ) {
            // but the client obviously expected something to be here ...
            assert( false );
        }
        *pStream = newElement;
        *pAPI = stream_get_api(*pStream);
        stream_ref(newElement);
        return 0;
    }

    DECLARE_BASE_I(*pStream, def);
    if ( insertBefore == NULL ) {
        insertBefore = def->name;
    }
    def->logCb(logDebug, _FMT("Inserting pipeline element " <<
                            stream_get_api(newElement)->get_name(newElement) <<
                            " in pipeline starting with " << def->name << " after " << insertBefore));
    if ( def->name != NULL && !strcmp(def->name,insertBefore) ) {
        int open = 0;
        if ( flags & svFlagStreamOpen ) {
            open = 1;
            flags &= ~svFlagStreamOpen;
        }
#if GRAPH_DEBUG
        def->logCb(logDebug, _FMT("Setting source of pipeline element " <<
                            stream_get_api(newElement)->get_name(newElement) <<
                            " before element " << def->name ));
#endif
        stream_api_t* api = stream_get_api(newElement);
        api->set_log_cb(newElement, def->logCb);
        api->set_source(newElement, *pStream, flags);
        stream_set(pStream, newElement);
        if (pAPI) {
            *pAPI = stream_get_api(*pStream);
        }
        if ( open ) {
            stream_api_t* api = stream_get_api(*pStream);
            if ( api->open_in(*pStream) < 0 ) {
                def->logCb(logError, _FMT("Couldn't initialize element " << def->name ));
                return -1;
            }
        }
        return 0;
    }
    if ( def->source != NULL ) {
        return  def->sourceApi->insert_element(&def->source,
                                               &def->sourceApi,
                                               insertBefore,
                                               newElement,
                                               flags);
    }
    // couldn't find element with the specified name
    def->logCb(logError, _FMT("Couldn't insert element " << def->name <<
                            " in front of " << insertBefore));
    return -1;
}

//-----------------------------------------------------------------------------
int         defaultstream_print_pipeline        (stream_obj* stream,
                                            char* buffer,
                                            size_t size)
{
    DECLARE_BASE_I(stream, def);
    static const char* sFilterFormat = "%s[%03d]->";
    static const char* sDemuxFormat = "%s[%03d]";


    int spaceNeeded = strlen(def->name)+
                        (def->source?2:1)+ // arrow or term zero
                        2+  // param brackets and commas
                        3;  // refcount
    if ( spaceNeeded > size) {
        return -1;
    }

    sprintf(buffer, def->source?sFilterFormat:sDemuxFormat, def->name, def->refcount);

    if ( def->source ) {
        return stream_get_api(def->source)->print_pipeline(def->source,
                &buffer[spaceNeeded],
                size-spaceNeeded);
    }

    buffer[spaceNeeded-1] = '\0';
    return 0;
}

//-----------------------------------------------------------------------------
int         defaultstream_set_param             (stream_obj* stream,
                                            const CHAR_T* name,
                                            const void* value)
{
    DECLARE_BASE_I(stream, def);
    if (def->source) {
        return def->sourceApi->set_param(def->source, name, value);
    }
    def->logCb(logDebug, _FMT("Unknown parameter: " << name));
    return -1;
}

//-----------------------------------------------------------------------------
int         defaultstream_get_param             (stream_obj* stream,
                                            const CHAR_T* name,
                                            void* value,
                                            size_t* size)
{
    DECLARE_BASE_I(stream, def);
    if ( def->source ) {
        return def->sourceApi->get_param(def->source,
                                        name,
                                        value,
                                        size);
    }
    def->logCb(logDebug, _FMT("Unknown parameter: " << name));
    return -1;
}

//-----------------------------------------------------------------------------
int         defaultstream_open_in                (stream_obj* stream)
{
    DECLARE_BASE_I(stream, def);

    if (def->source == NULL || def->sourceApi == NULL) {
        def->logCb(logDebug, _FMT("Failed to open " << def->name <<
                                " - source isn't set"));
        return -1;
    }

    if (!def->sourceInitialized &&
        def->sourceApi->open_in(def->source) < 0 ) {
        def->logCb(logDebug, _FMT("Failed to open source of " << def->name <<
                                " - " <<
                                def->sourceApi->get_name(def->source)));
        return -1;
    }
    def->sourceInitialized = 1;
    return 0;
}

//-----------------------------------------------------------------------------
int         defaultstream_seek               (stream_obj* stream,
                                       INT64_T offset,
                                       int flags)
{
    DECLARE_BASE_I(stream, def);
    if (!def->source) {
        return -1;
    }
    return def->sourceApi->seek(def->source, offset, flags);
}

//-----------------------------------------------------------------------------
size_t      defaultstream_get_width          (stream_obj* stream)
{
    DECLARE_BASE_I(stream, def);
    if (!def->source) {
        return -1;
    }
    return def->sourceApi->get_width(def->source);
}

//-----------------------------------------------------------------------------
size_t      defaultstream_get_height         (stream_obj* stream)
{
    DECLARE_BASE_I(stream, def);
    if (!def->source) {
        return -1;
    }
    return def->sourceApi->get_height(def->source);
}

//-----------------------------------------------------------------------------
int         defaultstream_get_pixel_format   (stream_obj* stream)
{
    DECLARE_BASE_I(stream, def);
    if (!def->source) {
        return -1;
    }
    return def->sourceApi->get_pixel_format(def->source);
}

//-----------------------------------------------------------------------------
int         defaultstream_read_frame        (stream_obj* stream,
                                            frame_obj** frame)
{
    DECLARE_BASE_I(stream, def);
    *frame = NULL;

    // skip all the passthrough links in the chain
    while (def->source != NULL) {
        stream_base* next = (stream_base*)def->source;
        if ( next->passthrough ) {
            def = next;
        } else {
            break;
        }
    }

    // sanity check before continuing
    if (!def->source) {
        return -1;
    }

    return def->sourceApi->read_frame(def->source, frame);
}

//-----------------------------------------------------------------------------
int         configure_stream            (stream_obj* stream,
                                        const char* prefix,
                                        log_fn_t logCb,
                                        ...)
{
    stream_api_t* api = stream_get_api(stream);
    if ( !api )
        return -1;

    int res = 0;
    va_list argp;
    va_start(argp, logCb);

    char buffer[1024];
    while (true) {
        const char* name = (const char*)va_arg(argp, char*);
        if ( !name )
            break;

        if ( prefix != NULL ) {
            sprintf(buffer, "%s.%s", prefix, name);
            name = buffer;
        }
        void* value = (void*)va_arg(argp, void*);
        res = api->set_param(stream, name, value);
        if ( res < 0 ) {
            logCb(logError, _FMT("Failed to set parameter " << name));
            break;
        }
    }

    va_end(argp);
    return res;
}

//-----------------------------------------------------------------------------
int         defaultstream_close             (stream_obj* stream)
{
    DECLARE_BASE_I(stream, def);
    if (def->source) {
        def->sourceApi->close(def->source);
        stream_unref(&def->source);
    }
    return 0;
}


stream_api_t _g_default_stream_api = {
    NULL,       // create
    defaultstream_set_source,
    defaultstream_set_log_cb,
    defaultstream_get_name,
    defaultstream_find_element,
    defaultstream_remove_element,
    defaultstream_insert_element,
    defaultstream_set_param,
    defaultstream_get_param,
    defaultstream_open_in,
    defaultstream_seek,
    defaultstream_get_width,
    defaultstream_get_height,
    defaultstream_get_pixel_format,
    defaultstream_read_frame,
    defaultstream_print_pipeline,
    defaultstream_close
};

SVCORE_API stream_api_t*     get_default_stream_api                  ()
{
    return &_g_default_stream_api;
}


//-----------------------------------------------------------------------------
#define STATS_IMPL(type,shorthand)\
void stats_##shorthand##_init  (stats_item_##shorthand##_t* item)\
{\
    item->min = item->max = item->cumulative = item->sampleCount = item->last = 0;\
}\
void stats_##shorthand##_update(stats_item_##shorthand##_t* item, type value)\
{\
    if (value<item->min || item->sampleCount==0) item->min=value;\
    if (value>item->max || item->sampleCount==0) item->max=value;\
    item->last = value;\
    item->cumulative += value;\
    item->sampleCount++;\
}\
void stats_##shorthand##_add(stats_item_##shorthand##_t* item, type value)\
{\
    type newvalue = item->last + value; \
    stats_##shorthand##_update(item, newvalue);\
}\
type stats_##shorthand##_average(stats_item_##shorthand##_t* item)\
{\
    return item->sampleCount ? item->cumulative / item->sampleCount : 0;\
}\
void stats_##shorthand##_combine(stats_item_##shorthand##_t* item, stats_item_##shorthand##_t* other)\
{\
    if (other->sampleCount!=0) {\
        if (other->min<item->min || item->sampleCount==0) item->min = other->min;\
        if (other->max>item->max || item->sampleCount==0) item->max = other->max;\
    }\
    item->sampleCount += other->sampleCount;\
    item->cumulative += other->cumulative;\
}


STATS_IMPL(INT64_T, int)
STATS_IMPL(double, d);


//-----------------------------------------------------------------------------
typedef struct  fps_limiter {
    size_t  msPerFrameWeightedAccumulatorSize;
    float   currentFps;
    float   desiredFps;
    size_t  framesAccepted;
    size_t  framesRejected;
    float   msPerFrameWeightedAccumulator;
    INT64_T prevFrameTime;
    INT64_T firstFrameTime;
    int     useTimestampAsDiff;
    int     useWallClock;
    int     useSecondIntervals;
} fps_limiter_t;
static unsigned int _fps_round_div(unsigned int dividend, unsigned int divisor)
{
    return (dividend + (divisor / 2)) / divisor;
}
extern "C" fps_limiter*    fps_limiter_create(size_t frameInterval, float limit)
{
    fps_limiter_t* l = new fps_limiter_t;
    l->msPerFrameWeightedAccumulatorSize = frameInterval;
    l->desiredFps = limit;
    l->framesAccepted = 0;
    l->framesRejected = 0;
    l->msPerFrameWeightedAccumulator = 0;
    l->prevFrameTime = 0;
    l->firstFrameTime = 0;
    l->currentFps = limit;
    l->useTimestampAsDiff = 0;
    l->useWallClock = 1;
    l->useSecondIntervals = 0;
    return l;
}

// Sometimes we need multiple fps limiters to accept the same set of frames. To accomplish that,
// first frame of every second is always accepted, and the acceptance of the rest is determined
// by the difference from previous timestamp.
extern "C" void            fps_limiter_use_second_intervals(fps_limiter* limiter, int useSecondIntervals )
{
    limiter->useSecondIntervals = useSecondIntervals;
}

extern "C" void            fps_limiter_use_wall_clock(fps_limiter* limiter, int useWallClock )
{
    limiter->useWallClock = useWallClock;
}

extern "C" void            fps_limiter_use_ts_as_diff(fps_limiter* limiter, int useTsAsDiff )
{
    limiter->useTimestampAsDiff = useTsAsDiff;
}

extern "C" int             fps_limiter_report_frame(fps_limiter* limiter, float* currentFps, int64_t pts)
{
    float   fps;
    bool    rejectFrame;
    int64_t timeElapsed;

    if ( limiter->framesAccepted > 0 ) {
        float msPerFrameWeightedAccumulator = limiter->msPerFrameWeightedAccumulator;

        if ( limiter->useTimestampAsDiff )
            timeElapsed = pts;
        else if ( limiter->useWallClock )
            timeElapsed = sv_time_get_elapsed_time(limiter->prevFrameTime);
        else
            timeElapsed = sv_time_get_time_diff(limiter->prevFrameTime, pts);

        if ( limiter->useSecondIntervals ) {
            // accept first frame of every second
            rejectFrame = ( limiter->prevFrameTime / 1000 == pts / 1000 ) &&
                          ( limiter->desiredFps != 0 ) &&
                          ( timeElapsed < 1000/limiter->desiredFps );
            // fake it!
            fps = limiter->desiredFps;
        } else {
            if ( msPerFrameWeightedAccumulator == 0 ) {
                msPerFrameWeightedAccumulator = timeElapsed * limiter->msPerFrameWeightedAccumulatorSize;
            } else {
                msPerFrameWeightedAccumulator = timeElapsed +
                              _fps_round_div(msPerFrameWeightedAccumulator * (limiter->msPerFrameWeightedAccumulatorSize-1),
                                        limiter->msPerFrameWeightedAccumulatorSize);

            }
            fps = (limiter->msPerFrameWeightedAccumulatorSize * 1000.0) / msPerFrameWeightedAccumulator;
            if (limiter->desiredFps != 0 && fps > limiter->desiredFps) {
                rejectFrame = true;
            } else {
                rejectFrame = false;
                limiter->msPerFrameWeightedAccumulator = msPerFrameWeightedAccumulator;
            }
        }
    } else {
        rejectFrame = false;
        fps = limiter->desiredFps;
        limiter->firstFrameTime = limiter->useWallClock
                            ? sv_time_get_current_epoch_time()
                            : pts;
    }

    if ( rejectFrame ) {
        limiter->framesRejected++;
    } else {
        limiter->framesAccepted++;
        limiter->prevFrameTime = limiter->useWallClock
                            ? sv_time_get_current_epoch_time()
                            : pts;
    }

    if (currentFps)
        *currentFps = fps;
    limiter->currentFps = fps;
    return rejectFrame ? 0 : 1;
}

extern "C" size_t          fps_limiter_get_frames_accepted(fps_limiter* limiter)
{
    return limiter->framesAccepted;
}

extern "C" size_t          fps_limiter_get_frames_rejected(fps_limiter* limiter)
{
    return limiter->framesRejected;
}

extern "C" float           fps_limiter_get_fps(fps_limiter* limiter)
{
    return limiter->currentFps;
}

extern "C" void            fps_limiter_destroy(fps_limiter** limiter)
{
    if ( limiter && *limiter ) {
        delete *limiter;
        *limiter = NULL;
    }
}





