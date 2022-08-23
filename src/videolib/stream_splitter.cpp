/*****************************************************************************
 *
 * stream_thread_connector.cpp
 *   A node in the graph acting as a splitter.
 *   Allows setting a subgraph object - every frame passing through the node
 *   is pushed into subgraph as well. It is expected that subgraph's output is not
 *   something the rest of the graph is interesed in. Used for recording, serving HLS,
 *   generating memory-mapped view etc.
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
#define SV_MODULE_VAR splitter
#define SV_MODULE_ID "SPLITTER"
#include "sv_module_def.hpp"

#include "streamprv.h"


#include "videolibUtils.h"

#include <list>

#define SPLITTER_DEMUX_MAGIC 0x1275

typedef struct splitter_stream  : public stream_base  {
    FrameList*      source_frames;
    stream_api_t*   subgraph_api;
    stream_obj*     subgraph;
    bool            successfullyOpened;
    bool            subgraph_read;
    bool            subgraph_opening;
    bool            subgraph_closing;
    bool            subgraph_set_log_cb;
    bool            flushing_subgraph;
    sv_mutex*       subgraphMutex;
} splitter_stream_obj;


//-----------------------------------------------------------------------------
// Stream API
//-----------------------------------------------------------------------------

//-----------------------------------------------------------------------------
//  Forward declarations
//-----------------------------------------------------------------------------
static stream_obj* splitter_stream_create             (const char* name);
static int         splitter_set_log_cb                (stream_obj* stream, fn_stream_log log);
static int         splitter_stream_set_param          (stream_obj* stream,
                                                const CHAR_T* name,
                                                const void* value);
static int         splitter_stream_get_param             (stream_obj* stream,
                                            const CHAR_T* name,
                                            void* value,
                                            size_t* size);
static int         splitter_stream_open_in            (stream_obj* stream);
static int         splitter_stream_read_frame         (stream_obj* stream, frame_obj** frame);
static int         splitter_stream_close              (stream_obj* stream);
static void        splitter_stream_destroy            (stream_obj* stream);

static int         _splitter_generate_frame           (splitter_stream_obj* splitter);
static int         _splitter_open_subgraph            (splitter_stream_obj* splitter);

//-----------------------------------------------------------------------------
stream_api_t _g_splitter_stream_provider = {
    splitter_stream_create,
    get_default_stream_api()->set_source,
    splitter_set_log_cb,
    get_default_stream_api()->get_name,
    get_default_stream_api()->find_element,
    get_default_stream_api()->remove_element,
    get_default_stream_api()->insert_element,
    splitter_stream_set_param,
    splitter_stream_get_param,
    splitter_stream_open_in,
    get_default_stream_api()->seek,
    get_default_stream_api()->get_width,
    get_default_stream_api()->get_height,
    get_default_stream_api()->get_pixel_format,
    splitter_stream_read_frame,
    get_default_stream_api()->print_pipeline,
    splitter_stream_close,
    _set_module_trace_level
} ;


//-----------------------------------------------------------------------------
#define DECLARE_STREAM_SPLITTER(stream, name) \
    DECLARE_OBJ(splitter_stream_obj, name,  stream, SPLITTER_DEMUX_MAGIC, -1)

#define DECLARE_STREAM_SPLITTER_V(stream, name) \
    DECLARE_OBJ_V(splitter_stream_obj, name,  stream, SPLITTER_DEMUX_MAGIC)

static stream_obj*   splitter_stream_create                (const char* name)
{
    splitter_stream_obj* res = (splitter_stream_obj*)stream_init(sizeof(splitter_stream_obj),
                SPLITTER_DEMUX_MAGIC,
                &_g_splitter_stream_provider,
                name,
                splitter_stream_destroy );

    res->subgraph = NULL;
    res->subgraph_api = NULL;
    res->successfullyOpened = false;
    res->source_frames = frame_list_create();
    res->subgraph_read = false;
    res->subgraph_closing = false;
    res->subgraph_opening = false;
    res->subgraph_set_log_cb = false;
    res->flushing_subgraph = false;
    res->subgraphMutex = sv_mutex_create();
    return (stream_obj*)res;
}

//-----------------------------------------------------------------------------
static void       _splitter_subgraph_get_root            (splitter_stream_obj* splitter,
                                                          stream_obj* subgraph,
                                                          stream_obj** root,
                                                          stream_api_t** root_api)
{
    *root = subgraph;
    *root_api=NULL;

    stream_obj* parent = *root;

    while ( parent != NULL && parent != (stream_obj*)splitter ) {
        *root_api = stream_get_api(*root);
        parent = (*root_api)->find_element(*root, NULL);
        if ( parent != NULL && parent != (stream_obj*)splitter ) {
            *root = parent;
            *root_api = stream_get_api(*root);
        }
    }
}

//-----------------------------------------------------------------------------
static int        _splitter_subgraph_assign              (splitter_stream_obj* splitter,
                                                          stream_obj* subgraph)
{
    stream_obj* root;
    stream_api_t* root_api;
    if ( subgraph == splitter->subgraph ) {
        return 0; // noop
    }


    int res = 0;

    sv_mutex_enter(splitter->subgraphMutex);

    if (splitter->subgraph) {
        splitter->subgraph_closing = true;
        _splitter_subgraph_get_root(splitter, splitter->subgraph, &root, &root_api);
        root_api->set_source(root, NULL, svFlagNone);
        splitter->subgraph_api->close(splitter->subgraph);
        stream_set(&splitter->subgraph, NULL);
        splitter->subgraph_api = NULL;
        splitter->subgraph_closing = false;
    }

    if (subgraph) {
        _splitter_subgraph_get_root(splitter, subgraph, &root, &root_api);
        root_api->set_source(root, (stream_obj*)splitter, svFlagStreamInitialized);
        stream_set(&splitter->subgraph, subgraph);
        splitter->subgraph_api = stream_get_api(subgraph);
        splitter->subgraph_set_log_cb = true;
        splitter->subgraph_api->set_log_cb(splitter->subgraph,splitter->logCb);
        splitter->subgraph_set_log_cb = false;

        if ( splitter->successfullyOpened ) {
            // no open call will come to init the subgraph
            if ( _splitter_open_subgraph(splitter) < 0 ) {
                _splitter_subgraph_assign(splitter, NULL);
                res = -1;
            }
        }
    } else {
        frame_list_clear(splitter->source_frames);
    }

    sv_mutex_exit(splitter->subgraphMutex);

    return res;
}

//-----------------------------------------------------------------------------
static int         splitter_set_log_cb         (stream_obj* stream, fn_stream_log log)
{
    DECLARE_STREAM_SPLITTER(stream, splitter);
    if (splitter->subgraph_set_log_cb) {
        return 0;
    }
    if (splitter->subgraph) {
        splitter->subgraph_set_log_cb = true;
        splitter->subgraph_api->set_log_cb(splitter->subgraph, log);
        splitter->subgraph_set_log_cb = false;
    }
    return default_set_log_cb(stream, log);
}


//-----------------------------------------------------------------------------
static int         splitter_stream_set_param             (stream_obj* stream,
                                            const CHAR_T* name,
                                            const void* value)
{
    DECLARE_STREAM_SPLITTER(stream, splitter);

    name = stream_param_name_apply_scope(stream, name);
    if (!_strnicmp(name, "subgraph.", 9)) {
        int res = -1;
        sv_mutex_enter(splitter->subgraphMutex);
        if (splitter->subgraph != NULL) {
            res = splitter->subgraph_api->set_param(splitter->subgraph, &name[9], value);
        }
        sv_mutex_exit(splitter->subgraphMutex);
        return res;
    }

    if (!_stricmp(name, "subgraph")) {
        return _splitter_subgraph_assign(splitter, (stream_obj*)value);
    }

    return default_set_param(stream, name, value);
}

//-----------------------------------------------------------------------------
static int         splitter_stream_get_param             (stream_obj* stream,
                                            const CHAR_T* name,
                                            void* value,
                                            size_t* size)
{
    DECLARE_STREAM_SPLITTER(stream, splitter);

    name = stream_param_name_apply_scope(stream, name);
    if (!_strnicmp(name, "subgraph.", 9)) {
        int res = -1;
        sv_mutex_enter(splitter->subgraphMutex);
        if (splitter->subgraph != NULL) {
            res = splitter->subgraph_api->get_param(splitter->subgraph, &name[9], value, size);
        }
        sv_mutex_exit(splitter->subgraphMutex);
        return res;
    }
    COPY_PARAM_IF(splitter, name, "subgraph", stream_obj*, splitter->subgraph);
    return default_get_param(stream, name, value, size);
}

//-----------------------------------------------------------------------------
static int         splitter_stream_open_in            (stream_obj* stream)
{
    DECLARE_STREAM_SPLITTER(stream, splitter);
    if ( splitter->subgraph_opening ) {
        return 0;
    }

    if ( default_open_in(stream) < 0 ) {
        return -1;
    }

    splitter->successfullyOpened = true;
    return _splitter_open_subgraph(splitter);
}

//-----------------------------------------------------------------------------
static int         _splitter_open_subgraph         (splitter_stream_obj* splitter)
{
    if (!splitter->subgraph)
        return 0;

    sv_mutex_enter(splitter->subgraphMutex);
    splitter->subgraph_opening = true;
    int res = splitter->subgraph_api->open_in(splitter->subgraph);
    splitter->subgraph_opening = false;
    sv_mutex_exit(splitter->subgraphMutex);
    return res;
}

//-----------------------------------------------------------------------------
static int         _splitter_run_subgraph          (splitter_stream_obj* splitter)
{
    if ( !splitter->subgraph )
        return 0;

    sv_mutex_enter(splitter->subgraphMutex);

    // read and save the next frame from the subgraph
    frame_obj*  tmp = NULL;
    int         res;
    int         gotFrame = 0;
    splitter->subgraph_read = true;
    do {
        res = splitter->subgraph_api->read_frame(splitter->subgraph, &tmp);
        gotFrame = (res >= 0 && tmp != NULL);
        frame_unref(&tmp);
    } while ( gotFrame );
    splitter->subgraph_read = false;

    sv_mutex_exit(splitter->subgraphMutex);

    return res;
}

//-----------------------------------------------------------------------------
static int         splitter_stream_read_frame        (stream_obj* stream, frame_obj** frame)
{
    DECLARE_STREAM_SPLITTER(stream, splitter);
    int res;

    *frame = NULL;

    if (!splitter->source) {
        if ( splitter->flushing_subgraph ) {
            return 0;
        }
        splitter->logCb(logError, _FMT("Source isn't set"));
        return -1;
    }

    // recursive read from subgraph
    if ( splitter->subgraph_read ) {
        size_t size = splitter->source_frames->size();
        TRACE_C(10, _FMT("reading the source frame in subgraph " << splitter->name <<
                    " - have " << size));
        if ( size > 0 ) {
            *frame = splitter->source_frames->front();
            splitter->source_frames->pop_front();
            return 0;
        } else {
            *frame = NULL;
            return (splitter->flushing_subgraph?-1:0);
        }
    }


    // read and save the next frame from our provider
    res = default_read_frame( stream, frame );
    if (res >=0 && *frame && splitter->subgraph ) {
        frame_ref(*frame);
        splitter->source_frames->push_back(*frame);
    }

    _splitter_run_subgraph(splitter);

    TRACE_C(10, _FMT("Successfully generated a frame to consumer in subgraph " << splitter->name));
    return 0;
}

//-----------------------------------------------------------------------------
static int         splitter_stream_close             (stream_obj* stream)
{
    DECLARE_STREAM_SPLITTER(stream, splitter);
    if (splitter->subgraph_closing) {
        // prevent recursive calls from subgraph
        return 0;
    }
    TRACE(_FMT("Flushing splitter object " << splitter->name));
    if (splitter->subgraph) {
        splitter->flushing_subgraph = true;
        _splitter_run_subgraph(splitter);
        splitter->flushing_subgraph = false;
    }
    TRACE(_FMT("Closing splitter object " << splitter->name));
    _splitter_subgraph_assign(splitter, NULL);
    return 0;
}


//-----------------------------------------------------------------------------
static void splitter_stream_destroy         (stream_obj* stream)
{
    DECLARE_STREAM_SPLITTER_V(stream, splitter);
    TRACE( _FMT("Destroying stream object " <<
                splitter->name <<
                (void*)stream));
    splitter_stream_close(stream); // make sure all the internals had been freed
    frame_list_destroy(&splitter->source_frames);
    stream_destroy( stream );
}

//-----------------------------------------------------------------------------
SVVIDEOLIB_API stream_api_t*     get_splitter_api                    ()
{
    return &_g_splitter_stream_provider;
}

