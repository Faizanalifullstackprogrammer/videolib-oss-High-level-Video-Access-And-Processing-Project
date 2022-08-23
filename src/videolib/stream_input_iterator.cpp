/*****************************************************************************
 *
 * stream_input_iterator.cpp
 *   Node iterating on a list of input files.
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
#define SV_MODULE_VAR strlist
#define SV_MODULE_ID "INPUTLIST"
#include "sv_module_def.hpp"

#include "streamprv.h"


#include "videolibUtils.h"

#define LIST_DEMUX_MAGIC 0x1301
#define MAX_PARAM 10

//-----------------------------------------------------------------------------
typedef struct list_param_set_obj_ {
    char*               paramName;
    void*               paramValue;
    size_t              paramValueSize;
} list_param_set_obj;

typedef struct stream_state {
    size_t              packetsProcessed;
    size_t              packetsProcessedInFile;
    uint64_t            fileOffset;
    uint64_t            lastFramePts;
} stream_state;

typedef struct list_stream  : public stream_base  {
    size_t              urlCount;
    size_t              currentURL;
    const char**        urlList;
    uint64_t*           offsets;

    stream_state        aState;
    stream_state        vState;
    int                 eof;

    list_param_set_obj* paramsToSet;
    size_t              paramsToSetCount;
    size_t              paramsToSetAlloc;

    // TODO: We really need to save and apply *all* params
    // but to accomplish that set_param API needs to change to include
    // the size of the parameter being set.
    int                 liveStream;
} list_stream_obj;

//-----------------------------------------------------------------------------
void list_param_set(list_param_set_obj* obj,
                    const char* name,
                    void* value,
                    size_t size)
{
    obj->paramName = strdup(name);
    obj->paramValue = malloc(size);
    memcpy(obj->paramValue, value, size);
    obj->paramValueSize = size;
}

//-----------------------------------------------------------------------------
void list_param_save(list_stream_obj* stream,
                    const char* name,
                    void* value,
                    size_t size)
{
    if ( stream->paramsToSetAlloc <= stream->paramsToSetCount ) {
        size_t newSize = stream->paramsToSetAlloc+10;
        stream->paramsToSet = (list_param_set_obj*)realloc(stream->paramsToSet,
                                                sizeof(list_param_set_obj)*newSize );
        stream->paramsToSetAlloc+=10;
    }
    list_param_set(&stream->paramsToSet[stream->paramsToSetCount],
                    name,
                    value,
                    size);
    stream->paramsToSetCount++;
}

//-----------------------------------------------------------------------------
void list_param_init(list_param_set_obj* obj)
{
    obj->paramName = NULL;
    obj->paramValue = NULL;
    obj->paramValueSize = 0;
}

//-----------------------------------------------------------------------------
void list_param_destroy(list_param_set_obj* obj)
{
    free(obj->paramName);
    free(obj->paramValue);
    list_param_init(obj);
}

//-----------------------------------------------------------------------------
void list_param_apply(list_param_set_obj* obj, stream_obj* stream)
{
    stream_api_t* api = stream_get_api(stream);
    api->set_param(stream, obj->paramName, obj->paramValue);
}



//-----------------------------------------------------------------------------
// Stream API
//-----------------------------------------------------------------------------

//-----------------------------------------------------------------------------
//  Forward declarations
//-----------------------------------------------------------------------------
static stream_obj* list_stream_create             (const char* name);
static int         list_stream_set_param          (stream_obj* stream,
                                                const CHAR_T* name,
                                                const void* value);
static int         list_stream_open_in            (stream_obj* stream);
static int         list_stream_seek               (stream_obj* stream,
                                                    INT64_T offset,
                                                    int flags);
static int         list_stream_read_frame         (stream_obj* stream, frame_obj** frame);
static int         list_stream_close              (stream_obj* stream);
static void        list_stream_destroy            (stream_obj* stream);


//-----------------------------------------------------------------------------
stream_api_t _g_list_stream_provider = {
    list_stream_create,
    get_default_stream_api()->set_source,
    get_default_stream_api()->set_log_cb,
    get_default_stream_api()->get_name,
    get_default_stream_api()->find_element,
    get_default_stream_api()->remove_element,
    get_default_stream_api()->insert_element,
    list_stream_set_param,
    get_default_stream_api()->get_param,
    list_stream_open_in,
    list_stream_seek,
    get_default_stream_api()->get_width,
    get_default_stream_api()->get_height,
    get_default_stream_api()->get_pixel_format,
    list_stream_read_frame,
    get_default_stream_api()->print_pipeline,
    list_stream_close,
    _set_module_trace_level
} ;


//-----------------------------------------------------------------------------
#define DECLARE_STREAM_LIST(stream, name) \
    DECLARE_OBJ(list_stream_obj, name,  stream, LIST_DEMUX_MAGIC, -1)

#define DECLARE_STREAM_LIST_V(stream, name) \
    DECLARE_OBJ_V(list_stream_obj, name,  stream, LIST_DEMUX_MAGIC)

static stream_obj*   list_stream_create                (const char* name)
{
    list_stream_obj* res = (list_stream_obj*)stream_init(sizeof(list_stream_obj),
                LIST_DEMUX_MAGIC,
                &_g_list_stream_provider,
                name,
                list_stream_destroy );

    res->currentURL = 0;
    res->urlList = NULL;
    res->offsets = 0;
    res->urlCount = 0;

    res->aState.packetsProcessed = 0;
    res->aState.packetsProcessedInFile = 0;
    res->aState.fileOffset = 0;
    res->aState.lastFramePts = 0;

    res->vState.packetsProcessed = 0;
    res->vState.packetsProcessedInFile = 0;
    res->vState.fileOffset = 0;
    res->vState.lastFramePts = 0;

    res->liveStream = 0;
    res->eof = 0;

    res->paramsToSet = NULL;
    res->paramsToSetCount = 0;
    res->paramsToSetAlloc = 0;

    return (stream_obj*)res;
}

//-----------------------------------------------------------------------------
static int         list_stream_set_param             (stream_obj* stream,
                                            const CHAR_T* name,
                                            const void* value)
{
    DECLARE_STREAM_LIST(stream, strlist);

    name = stream_param_name_apply_scope(stream, name);

    SET_PARAM_IF(stream, name, "count", int, strlist->urlCount);
    SET_PARAM_IF(stream, name, "liveStream", int, strlist->liveStream);

    if ( !_stricmp(name, "offsets" ) ) {
        if (strlist->urlCount<=0) {
            return -1;
        }
        strlist->offsets = (uint64_t*)malloc(strlist->urlCount*sizeof(uint64_t));
        memcpy(strlist->offsets, value, strlist->urlCount*sizeof(uint64_t));
        return 0;
    } else
    if ( !_stricmp(name, "urls" ) ) {
        if (strlist->urlCount<=0) {
            return -1;
        }
        const char** list = (const char**)value;
        strlist->urlList = (const char**)malloc(strlist->urlCount*sizeof(const char*));
        for (size_t nI=0; nI<strlist->urlCount; nI++) {
            strlist->urlList[nI] = strdup(list[nI]);
        }
        return 0;
    }
    return -1;
}

//-----------------------------------------------------------------------------
static int         list_stream_get_param             (stream_obj* stream,
                                            const CHAR_T* name,
                                            void* value,
                                            size_t* size)
{
    DECLARE_STREAM_LIST(stream, strlist);

    name = stream_param_name_apply_scope(stream, name);

    COPY_PARAM_IF(strlist, name, "eof", int, strlist->eof);

    return default_get_param(stream, name, value, size);
}


//-----------------------------------------------------------------------------
static int         _list_open_source                  (list_stream_obj* strlist)
{
    int res = 0;

    if (!strlist->source) {
        strlist->logCb(logError, _FMT("Source isn't set"));
        return -1;
    }
    if ( strlist->currentURL >= strlist->urlCount ) {
        strlist->logCb(logError, _FMT("URL count too small: current=" <<
                                    strlist->currentURL <<
                                    " count=" << strlist->urlCount));
        return -1;
    }
    if ( strlist->urlList == NULL || strlist->offsets == NULL ) {
        strlist->logCb(logError, _FMT("List or offsets isn't set: " <<
                                    " list=" << (void*)strlist->urlList <<
                                    " offsets=" << (void*)strlist->offsets ));
        return -1;
    }

    const char* url = strlist->urlList[strlist->currentURL];

    strlist->sourceApi->close(strlist->source);
    strlist->sourceApi->set_param(strlist->source, "url", url );
    strlist->sourceApi->set_param(strlist->source, "liveStream", &strlist->liveStream);
    res = strlist->sourceApi->open_in(strlist->source);
    if (res<0) {
        char buffer[2048];
        strlist->logCb(logError, _FMT("Failed to open input from " <<
                                        sv_sanitize_uri(url,
                                                    buffer,
                                                    sizeof(buffer))));
    }
    strlist->aState.packetsProcessedInFile=0;
    strlist->vState.packetsProcessedInFile=0;
    return res;
}

//-----------------------------------------------------------------------------
static int         list_stream_open_in                (stream_obj* stream)
{
    DECLARE_STREAM_LIST(stream, strlist);
    strlist->currentURL = 0;
    return _list_open_source(strlist);
}

//-----------------------------------------------------------------------------
static int         list_stream_seek               (stream_obj* stream,
                                                    INT64_T offset,
                                                    int flags)
{
    DECLARE_STREAM_LIST(stream, strlist);
    // determine the file containing the offset
    int fileID = 0;
    while (fileID < strlist->urlCount) {
        bool lastFile = (fileID == strlist->urlCount-1);
        if ( lastFile || offset < strlist->offsets[fileID+1] ) {
            break;
        }
        fileID++;
    }

    if ( fileID >= strlist->urlCount ) {
        strlist->logCb(logWarning, _FMT("Attempt to seek to pts " << offset << " with first input at offset " << strlist->offsets[0]));
        strlist->eof = 1;
        return 0;
    }

    int res = 0;
    if ( fileID != strlist->currentURL ) {
        TRACE(_FMT("Seek: switching to input #" << fileID <<
                  " from #" << strlist->currentURL));
        strlist->currentURL = fileID;
        res = _list_open_source(strlist);
    }

    if ( res == 0 ) {
        INT64_T offsetInFile = 0;
        if (offset >= strlist->offsets[strlist->currentURL])
            offsetInFile = offset - strlist->offsets[strlist->currentURL];
        TRACE(_FMT("Seek: seeking in input #" << strlist->currentURL <<
                  " at offset " << offsetInFile <<
                  " file offset " << strlist->offsets[strlist->currentURL] <<
                  " requested offset " << offset ));
        res = default_seek(stream, offsetInFile, flags);
    }
    return res;
}


//-----------------------------------------------------------------------------
static int         list_stream_read_frame        (stream_obj* stream, frame_obj** frame)
{
    DECLARE_STREAM_LIST(stream, strlist);
    if (!strlist->source) {
        return -1;
    }

    if (strlist->eof) {
        return -1;
    }

    int res;
    int retry;
    do {
        retry = 0;
        res = strlist->sourceApi->read_frame(strlist->source, frame);
        if ( res < 0 ) {
            int eof = 0;
            size_t size = sizeof(eof);
            if ( strlist->sourceApi->get_param(strlist->source,
                                                    "eof",
                                                    &eof,
                                                    &size) < 0 ||
                !eof ) {
                return res;
            }

            if ( strlist->currentURL == strlist->urlCount-1 ) {
                strlist->eof = 1;
                return -1;
            }

            strlist->currentURL++;
            TRACE(_FMT("Trying next input #" << strlist->currentURL));
            res = _list_open_source(strlist);
            if ( res == 0 ) {
                retry = 1;
            } else {
                strlist->logCb(logError, _FMT("Failed to open input #" << strlist->currentURL <<
                                          " : " << strlist->urlList[strlist->currentURL]));
            }
        } else {
            frame_api_t* api = frame_get_api(*frame);
            stream_state& state = api->get_media_type(*frame) == mediaVideo ? strlist->vState : strlist->aState;
            const char* type = api->get_media_type(*frame) == mediaVideo ? "video" : "audio";

            INT64_T framePts = api->get_pts(*frame);
            if ( state.packetsProcessedInFile == 0 ) {
                state.fileOffset = strlist->offsets[strlist->currentURL];
            }
            state.lastFramePts = framePts + state.fileOffset;
            api->set_pts(*frame, state.lastFramePts);
            api->set_dts(*frame, state.lastFramePts);
            TRACE(_FMT("File list iterator: pts=" << state.lastFramePts <<
                                            " type=" << type <<
                                            " framePts=" << framePts <<
                                            " fileOffset=" << state.fileOffset ));
            state.packetsProcessedInFile++;
            state.packetsProcessed++;
        }
    } while (strlist->currentURL < strlist->urlCount && retry );
    return res;
}

//-----------------------------------------------------------------------------
static int         list_stream_close             (stream_obj* stream)
{
    DECLARE_STREAM_LIST(stream, strlist);
    strlist->sourceApi->close(strlist->source);
    stream_unref(&strlist->source);

    if (strlist->urlList) {
        for (size_t nI=0; nI<strlist->urlCount; nI++ )
            free((void*)strlist->urlList[nI]);
        free(strlist->urlList);
        strlist->urlList = NULL;
    }
    if ( strlist->offsets ) {
        free(strlist->offsets);
        strlist->offsets = NULL;
    }
    return 0;
}


//-----------------------------------------------------------------------------
static void list_stream_destroy         (stream_obj* stream)
{
    DECLARE_STREAM_LIST_V(stream, strlist);
    strlist->logCb(logTrace, _FMT("Destroying stream object " << (void*)stream));
    list_stream_close(stream); // make sure all the internals had been freed
    stream_destroy( stream );
}

//-----------------------------------------------------------------------------
SVVIDEOLIB_API stream_api_t*     get_input_iterator_api                    ()
{
    return &_g_list_stream_provider;
}

