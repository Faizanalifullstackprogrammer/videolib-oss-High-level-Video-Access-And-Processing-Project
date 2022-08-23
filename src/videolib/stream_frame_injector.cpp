/*****************************************************************************
 *
 * stream_frame_injector.cpp
 *   Source node providing the ability to inject frames into the graph using
 *   its set_param method. Not used in the context of SV.
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
#define SV_MODULE_VAR injector
#define SV_MODULE_ID "FRAMEINJECTOR"
#include "sv_module_def.hpp"

#include "streamprv.h"


#include "videolibUtils.h"

#define FRAME_INJ_STREAM_MAGIC 0x7541

//-----------------------------------------------------------------------------
typedef struct frame_injector_stream : public stream_base {
    frame_api_t*        frameAPI;
    frame_obj*          currentFrame;
} frame_injector_stream;

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
static int         fi_stream_get_param          (stream_obj* stream,
                                                const CHAR_T* name,
                                                void* value,
                                                size_t* size);
static int         fi_stream_open_in            (stream_obj* stream);
static int         fi_stream_seek               (stream_obj* stream,
                                                INT64_T offset,
                                                int flags);
static size_t      fi_stream_get_width          (stream_obj* stream);
static size_t      fi_stream_get_height         (stream_obj* stream);
static int         fi_stream_get_pixel_format   (stream_obj* stream);
static int         fi_stream_read_frame         (stream_obj* stream, frame_obj** frame);
static int         fi_stream_close              (stream_obj* stream);
static void        fi_stream_destroy            (stream_obj* stream);



//-----------------------------------------------------------------------------
static stream_api_t _g_inj_stream_provider = {
    fi_stream_create,
    NULL, // set_source,
    get_default_stream_api()->set_log_cb,
    get_default_stream_api()->get_name,
    get_default_stream_api()->find_element,
    get_default_stream_api()->remove_element,
    get_default_stream_api()->insert_element,
    fi_stream_set_param,
    fi_stream_get_param,
    fi_stream_open_in,
    NULL, // seek
    fi_stream_get_width,
    fi_stream_get_height,
    fi_stream_get_pixel_format,
    fi_stream_read_frame,
    get_default_stream_api()->print_pipeline,
    fi_stream_close,
    _set_module_trace_level
} ;


//-----------------------------------------------------------------------------
#define DECLARE_STREAM_FI(stream, name) \
    DECLARE_OBJ(frame_injector_stream, name,  stream, FRAME_INJ_STREAM_MAGIC, -1)

#define DECLARE_STREAM_FI_V(stream, name) \
    DECLARE_OBJ_V(frame_injector_stream, name,  stream, FRAME_INJ_STREAM_MAGIC)

#define DECLARE_DEMUX_FI(stream,name) \
    DECLARE_STREAM_FI(stream,name)

static stream_obj*   fi_stream_create                (const char* name)
{
    frame_injector_stream* res = (frame_injector_stream*)stream_init(sizeof(frame_injector_stream),
                                        FRAME_INJ_STREAM_MAGIC,
                                        &_g_inj_stream_provider,
                                        name,
                                        fi_stream_destroy );
    res->frameAPI = NULL;
    res->currentFrame = NULL;
    return (stream_obj*)res;
}

//-----------------------------------------------------------------------------
static int         fi_stream_set_param             (stream_obj* stream,
                                            const CHAR_T* name,
                                            const void* value)
{
    DECLARE_STREAM_FI(stream, injector);
    name = stream_param_name_apply_scope(stream, name);
    if ( !_stricmp(name, "frameAPI") ) {
        injector->frameAPI = (frame_api_t*)value;
        return 0;
    }
    if ( !_stricmp(name, "currentFrame") ) {
        frame_unref(&injector->currentFrame);
        injector->currentFrame = (frame_obj*)value;
        frame_ref(injector->currentFrame);
        return 0;
    }
    return -1;
}

//-----------------------------------------------------------------------------
static int         fi_stream_get_param             (stream_obj* stream,
                                            const CHAR_T* name,
                                            void* value,
                                            size_t* size)
{
    DECLARE_STREAM_FI(stream, injector);

    name = stream_param_name_apply_scope(stream, name);

    // we don't support audio yet
    COPY_PARAM_IF(injector, name, "audioCodecId",   int,   streamUnknown);
    // we assume always being fed RGB frames
    COPY_PARAM_IF(injector, name, "videoCodecId",   int,   streamBitmap);

    return -1;
}

//-----------------------------------------------------------------------------
static int         fi_stream_open_in                (stream_obj* stream)
{
    DECLARE_STREAM_FI(stream, injector);
    if (!injector->currentFrame || !injector->frameAPI) {
        injector->logCb(logError, "Failed to open injector - first frame object must be set");
        return -1;
    }
    return 0;
}

//-----------------------------------------------------------------------------
static size_t      fi_stream_get_width          (stream_obj* stream)
{
    DECLARE_STREAM_FI(stream, injector);
    if (injector->currentFrame == NULL) {
        injector->logCb(logError, "Injector must be initialized and used after injecting the first frame");
        return -1;
    }
    return injector->frameAPI->get_width(injector->currentFrame);
}

//-----------------------------------------------------------------------------
static size_t      fi_stream_get_height         (stream_obj* stream)
{
    DECLARE_STREAM_FI(stream, injector);
    if (injector->currentFrame == NULL) {
        injector->logCb(logError, "Injector must be initialized and used after injecting the first frame");
        return -1;
    }
    return injector->frameAPI->get_height(injector->currentFrame);
}

//-----------------------------------------------------------------------------
static int         fi_stream_get_pixel_format   (stream_obj* stream)
{
    DECLARE_STREAM_FI(stream, injector);
    if (injector->currentFrame == NULL) {
        injector->logCb(logError, "Injector must be initialized and used after injecting the first frame");
        return -1;
    }
    return injector->frameAPI->get_pixel_format(injector->currentFrame);
}

//-----------------------------------------------------------------------------
static int         fi_stream_read_frame        (stream_obj* stream, frame_obj** frame)
{
    DECLARE_STREAM_FI(stream, injector);
    *frame = injector->currentFrame;
    // we assume one set_param before each read_frame
    frame_unref(&injector->currentFrame);
    return 0;
}

//-----------------------------------------------------------------------------
static int         fi_stream_close             (stream_obj* stream)
{
    DECLARE_STREAM_FI(stream, injector);
    frame_unref(&injector->currentFrame);
    injector->frameAPI = NULL;
    return 0;
}

//-----------------------------------------------------------------------------
static void fi_stream_destroy         (stream_obj* stream)
{
    DECLARE_STREAM_FI_V(stream, injector);
    TRACE(_FMT("Destroying stream object " << (void*)stream));
    stream_destroy( stream );
}

//-----------------------------------------------------------------------------
SVVIDEOLIB_API stream_api_t*     get_frame_injector_stream_api             ()
{
    return &_g_inj_stream_provider;
}

