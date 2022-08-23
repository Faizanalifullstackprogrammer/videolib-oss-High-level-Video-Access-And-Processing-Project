/*****************************************************************************
 *
 * stream_filter_inserter.cpp
 *   Sole job of this filter is to detect image rotation, and insert
 *   and configure a rotation filter if so.
 *   Not used in the context of SV.
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
#define SV_MODULE_VAR finj
#define SV_MODULE_ID "FI"
#include "sv_module_def.hpp"

#include "streamprv.h"


#include "videolibUtils.h"

#define FI_DEMUX_MAGIC 0x1302
#define MAX_PARAM 10

//-----------------------------------------------------------------------------

typedef struct fi_stream  : public stream_base  {
} fi_stream_obj;

//-----------------------------------------------------------------------------
// Stream API
//-----------------------------------------------------------------------------

//-----------------------------------------------------------------------------
//  Forward declarations
//-----------------------------------------------------------------------------
static stream_obj* fi_stream_create             (const char* name);
static int         fi_stream_set_param          (stream_obj* stream,
                                                const CHAR_T* name,
                                                const void* value);
static int         fi_stream_open_in            (stream_obj* stream);
static void        fi_stream_destroy            (stream_obj* stream);


//-----------------------------------------------------------------------------
stream_api_t _g_fi_stream_provider = {
    fi_stream_create,
    get_default_stream_api()->set_source,
    get_default_stream_api()->set_log_cb,
    get_default_stream_api()->get_name,
    get_default_stream_api()->find_element,
    get_default_stream_api()->remove_element,
    get_default_stream_api()->insert_element,
    fi_stream_set_param,
    get_default_stream_api()->get_param,
    fi_stream_open_in,
    get_default_stream_api()->seek,
    get_default_stream_api()->get_width,
    get_default_stream_api()->get_height,
    get_default_stream_api()->get_pixel_format,
    get_default_stream_api()->read_frame,
    get_default_stream_api()->print_pipeline,
    get_default_stream_api()->close,
    _set_module_trace_level
} ;


//-----------------------------------------------------------------------------
#define DECLARE_STREAM_FI(stream, name) \
    DECLARE_OBJ(fi_stream_obj, name,  stream, FI_DEMUX_MAGIC, -1)

#define DECLARE_STREAM_FI_V(stream, name) \
    DECLARE_OBJ_V(fi_stream_obj, name,  stream, FI_DEMUX_MAGIC)

static stream_obj*   fi_stream_create                (const char* name)
{
    fi_stream_obj* res = (fi_stream_obj*)stream_init(sizeof(fi_stream_obj),
                FI_DEMUX_MAGIC,
                &_g_fi_stream_provider,
                name,
                fi_stream_destroy );

    return (stream_obj*)res;
}

//-----------------------------------------------------------------------------
static int         fi_stream_set_param             (stream_obj* stream,
                                            const CHAR_T* name,
                                            const void* value)
{
    DECLARE_STREAM_FI(stream, finj);

    name = stream_param_name_apply_scope(stream, name);
    return default_set_param(stream, name, value);
}

//-----------------------------------------------------------------------------
extern "C" stream_api_t*     get_ff_filter_api                    ();

//-----------------------------------------------------------------------------
static int         fi_stream_open_in                (stream_obj* stream)
{
    DECLARE_STREAM_FI(stream, finj);
    int res = default_open_in(stream);
    if ( res < 0 ) {
        return res;
    }

    int rotation=0;
    size_t size = sizeof(int);

    if ( finj->sourceApi->get_param(finj->source, "rotation", &rotation, &size)>=0 &&
         rotation != 0 ) {
        finj->logCb(logInfo, _FMT("Detected rotation of " << rotation << " degrees"));
        stream_api_t*   api=get_ff_filter_api();
        stream_obj*     f=api->create("autoinserted_rotate");
        stream_ref(f);
        if ( api->set_param(f, "autoinserted_rotate.filterType", "rotate" ) < 0 ||
             api->set_param(f, "autoinserted_rotate.rotation", &rotation ) < 0 ) {
            finj->logCb(logError, _FMT("Failed to configure rotation filter params"));
            stream_unref(&f);
            return -1;
        }
        int inserted = finj->sourceApi->insert_element(&finj->source,
                                                      &finj->sourceApi,
                                                      finj->sourceApi->get_name(finj->source),
                                                      f,
                                                      svFlagStreamInitialized);
        if ( inserted < 0 ) {
            finj->logCb(logError, _FMT("Failed to insert rotation filter"));
            stream_unref(&f);
            return -1;
        }
        if ( api->open_in(f) < 0 ) {
            finj->logCb(logError, _FMT("Failed to init rotation filter"));
            return -1;
        }
    }

    return 0;
}


//-----------------------------------------------------------------------------
static void fi_stream_destroy         (stream_obj* stream)
{
    DECLARE_STREAM_FI_V(stream, finj);
    finj->logCb(logTrace, _FMT("Destroying stream object " << (void*)stream));
    default_close(stream); // make sure all the internals had been freed
    stream_destroy( stream );
}

//-----------------------------------------------------------------------------
SVVIDEOLIB_API stream_api_t*     get_filter_inserter_api                    ()
{
    return &_g_fi_stream_provider;
}

