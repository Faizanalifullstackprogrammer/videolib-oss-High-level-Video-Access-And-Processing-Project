/*****************************************************************************
 *
 * stream_debug.cpp
 *   Debug filter used to dump incoming frames to file.
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

/*
*
* This filter seeds the stream with a predefined set of NALUs it reads from a provided file
*
*/


#undef SV_MODULE_VAR
#define SV_MODULE_VAR dbg
#define SV_MODULE_ID "DBG"
#include "sv_module_def.hpp"

#include "streamprv.h"
#include "frame_basic.h"
#include "videolibUtils.h"

#define DBG_FILTER_MAGIC 0x9278

//-----------------------------------------------------------------------------
typedef struct dbg_filter  : public stream_base  {
    FILE*   file;
} dbg_filter_obj;

//-----------------------------------------------------------------------------
// Stream API
//-----------------------------------------------------------------------------

//-----------------------------------------------------------------------------
//  Forward declarations
//-----------------------------------------------------------------------------
static stream_obj* dbg_filter_create             (const char* name);
static int         dbg_filter_set_param          (stream_obj* stream,
                                                const CHAR_T* name,
                                                const void* value);
static int         dbg_filter_read_frame         (stream_obj* stream, frame_obj** frame);
static int         dbg_filter_close              (stream_obj* stream);
static void        dbg_filter_destroy            (stream_obj* stream);


//-----------------------------------------------------------------------------
stream_api_t _g_dbg_filter_provider = {
    dbg_filter_create,
    get_default_stream_api()->set_source,
    get_default_stream_api()->set_log_cb,
    get_default_stream_api()->get_name,
    get_default_stream_api()->find_element,
    get_default_stream_api()->remove_element,
    get_default_stream_api()->insert_element,
    dbg_filter_set_param,
    get_default_stream_api()->get_param,
    get_default_stream_api()->open_in,
    get_default_stream_api()->seek,
    get_default_stream_api()->get_width,
    get_default_stream_api()->get_height,
    get_default_stream_api()->get_pixel_format,
    dbg_filter_read_frame,
    get_default_stream_api()->print_pipeline,
    dbg_filter_close,
    _set_module_trace_level
};


//-----------------------------------------------------------------------------
#define DECLARE_DBG_FILTER(stream, name) \
    DECLARE_OBJ(dbg_filter_obj, name,  stream, DBG_FILTER_MAGIC, -1)

#define DECLARE_DBG_FILTER_V(stream, name) \
    DECLARE_OBJ_V(dbg_filter_obj, name,  stream, DBG_FILTER_MAGIC)

static stream_obj*   dbg_filter_create                (const char* name)
{
    dbg_filter_obj* res = (dbg_filter_obj*)stream_init(sizeof(dbg_filter_obj),
                DBG_FILTER_MAGIC,
                &_g_dbg_filter_provider,
                name,
                dbg_filter_destroy );
    res->file = NULL;

    return (stream_obj*)res;
}

//-----------------------------------------------------------------------------
static int         dbg_filter_set_param             (stream_obj* stream,
                                            const CHAR_T* name,
                                            const void* value)
{
    DECLARE_DBG_FILTER(stream, dbg);
    name = stream_param_name_apply_scope(stream, name);
    if ( !_stricmp(name, "dbgfile" ) ) {
        dbg->file = fopen((const char*)value, "w+b");
        if (!dbg->file) {
            dbg->logCb(logError, _FMT("Failed to open file at " << value));
        }
        return dbg->file != NULL ? 0 : -1;
    }
    return default_set_param(stream, name, value);
}


//-----------------------------------------------------------------------------
static int         dbg_filter_read_frame        (stream_obj* stream,
                                                    frame_obj** frame)
{
    DECLARE_DBG_FILTER(stream, dbg);
    int res = -1;

    res = dbg->sourceApi->read_frame(dbg->source, frame);
    if ( !res && *frame ) {
        // DJI drone sometimes sends SPS/PPS frames mid-stream, which seem
        // to piss off FFMPEG's decoder. Stripping those seem to work
        frame_api_t* fapi = frame_get_api(*frame);
        if ( fapi ) {
            uint8_t* data = (uint8_t*)fapi->get_data(*frame);
            size_t size = fapi->get_data_size(*frame);
            if ( dbg->file ) {
                fwrite( data, 1, size, dbg->file );
            }
        }
    }
    return res;
}

//-----------------------------------------------------------------------------
static int         dbg_filter_close             (stream_obj* stream)
{
    DECLARE_DBG_FILTER(stream, dbg);
    if ( dbg->file ) {
        fflush( dbg->file );
        fclose( dbg->file );
        dbg->file = NULL;
    }
    return 0;
}


//-----------------------------------------------------------------------------
static void dbg_filter_destroy         (stream_obj* stream)
{
    DECLARE_DBG_FILTER_V(stream, dbg);
    dbg->logCb(logTrace, _FMT("Destroying stream object " << (void*)stream));
    dbg_filter_close(stream); // make sure all the internals had been freed
    stream_destroy( stream );
}

//-----------------------------------------------------------------------------
SVVIDEOLIB_API stream_api_t*     get_dbg_filter_api                    ()
{
    return &_g_dbg_filter_provider;
}

