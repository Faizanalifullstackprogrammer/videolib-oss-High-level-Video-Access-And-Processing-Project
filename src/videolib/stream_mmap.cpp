/*****************************************************************************
 *
 * stream_mmap.cpp
 *   Node making the current frame available in mapped memory buffer.
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
#define SV_MODULE_VAR mmapsink
#define SV_MODULE_ID "MMAP"
#include "sv_module_def.hpp"

#include "streamprv.h"
#include "videolibUtils.h"

#define MMAPSINK_STREAM_MAGIC 0x4275

typedef struct mmapsink_stream  : public stream_base  {
    sv_mmap*    mmapobj;
    char*       filename;
    size_t      frameCounter;
    size_t      width;
    size_t      height;
    int         fpsErrorLogged;
} mmapsink_stream_obj;


// The header size of memory mapped files
static const int kMMAPHeaderSize = 32;

//-----------------------------------------------------------------------------
// Stream API
//-----------------------------------------------------------------------------

//-----------------------------------------------------------------------------
//  Forward declarations
//-----------------------------------------------------------------------------
static stream_obj* mmapsink_stream_create             (const char* name);
static int         mmapsink_stream_set_param          (stream_obj* stream,
                                                const CHAR_T* name,
                                                const void* value);
static int         mmapsink_open_in                   (stream_obj* stream);
static int         mmapsink_stream_read_frame         (stream_obj* stream, frame_obj** frame);
static int         mmapsink_stream_close              (stream_obj* stream);
static void        mmapsink_stream_destroy            (stream_obj* stream);


//-----------------------------------------------------------------------------
stream_api_t _g_mmapsink_stream_provider = {
    mmapsink_stream_create,
    get_default_stream_api()->set_source,
    get_default_stream_api()->set_log_cb,
    get_default_stream_api()->get_name,
    get_default_stream_api()->find_element,
    get_default_stream_api()->remove_element,
    get_default_stream_api()->insert_element,
    mmapsink_stream_set_param,
    get_default_stream_api()->get_param,
    mmapsink_open_in,
    get_default_stream_api()->seek,
    get_default_stream_api()->get_width,
    get_default_stream_api()->get_height,
    get_default_stream_api()->get_pixel_format,
    mmapsink_stream_read_frame,
    get_default_stream_api()->print_pipeline,
    mmapsink_stream_close,
    _set_module_trace_level
} ;


//-----------------------------------------------------------------------------
#define DECLARE_STREAM_MMAPSINK(stream, name) \
    DECLARE_OBJ(mmapsink_stream_obj, name,  stream, MMAPSINK_STREAM_MAGIC, -1)

#define DECLARE_STREAM_MMAPSINK_V(stream, name) \
    DECLARE_OBJ_V(mmapsink_stream_obj, name,  stream, MMAPSINK_STREAM_MAGIC)

static stream_obj*   mmapsink_stream_create                (const char* name)
{
    mmapsink_stream_obj* res = (mmapsink_stream_obj*)stream_init(sizeof(mmapsink_stream_obj),
                MMAPSINK_STREAM_MAGIC,
                &_g_mmapsink_stream_provider,
                name,
                mmapsink_stream_destroy );

    res->mmapobj = NULL;
    res->filename = NULL;
    res->frameCounter = 0;
    res->fpsErrorLogged = 0;
    return (stream_obj*)res;
}

//-----------------------------------------------------------------------------
static int         mmapsink_stream_set_param             (stream_obj* stream,
                                            const CHAR_T* name,
                                            const void* value)
{
    DECLARE_STREAM_MMAPSINK(stream, mmapsink);

    name = stream_param_name_apply_scope(stream, name);
    SET_STR_PARAM_IF(stream, name, "filename", mmapsink->filename);

    return default_set_param(stream, name, value);
}

//-----------------------------------------------------------------------------
static int        _mmapsink_open                      (mmapsink_stream_obj* mmapsink)
{
    sv_close_mmap(&mmapsink->mmapobj);

    int w = mmapsink->width = default_get_width((stream_obj*)mmapsink);
    int h = mmapsink->height = default_get_height((stream_obj*)mmapsink);

    size_t size = kMMAPHeaderSize +
                av_image_get_buffer_size(svpfmt_to_ffpfmt(pfmtRGB24, NULL), w, h, _kDefAlign) +
                1024;

    mmapsink->mmapobj =  sv_open_mmap(mmapsink->filename, size);

    if (mmapsink->mmapobj==NULL) {
        mmapsink->logCb(logError, _FMT("Failed to open mmap at " << mmapsink->filename << " size=" << size));
        return -1;
    }

    TRACE(_FMT("Opened mmap at " << mmapsink->filename << " size=" << size));
    return 0;
}


//-----------------------------------------------------------------------------
static int         mmapsink_open_in                    (stream_obj* stream)
{
    DECLARE_STREAM_MMAPSINK(stream, mmapsink);
    if (mmapsink->filename == NULL ) {
        mmapsink->logCb(logError, _FMT("Filename isn't set"));
        return -1;
    }
    if (mmapsink->mmapobj != NULL) {
        return 0;
    }

    int res = default_open_in(stream);
    if (res <0) {
        mmapsink->logCb(logError, _FMT("Failed to initialize source for mmap sink"));
        return -1;
    }

    return _mmapsink_open(mmapsink);
}

//-----------------------------------------------------------------------------
static int         mmapsink_stream_read_frame        (stream_obj* stream, frame_obj** frame)
{
    DECLARE_STREAM_MMAPSINK(stream, mmapsink);
    int res;


    res = default_read_frame(stream, frame);
    if ( res < 0 ) {
        mmapsink->logCb(logError, _FMT("Error reading a frame in mmap sink"));
        return res;
    } else if ( *frame == NULL ) {
        return res;
    }

    frame_api_t* api = frame_get_api(*frame);
    if ( api->get_media_type(*frame) != mediaVideo ) {
        return res;
    }
    size_t              frameSize = api->get_data_size(*frame);
    size_t              frameH    = api->get_height(*frame);
    size_t              frameW    = api->get_width(*frame);
    int                 pixfmt    = api->get_pixel_format(*frame);

    float  requestFps = 0.0;
    float  captureFps = 0.0;
    size_t size = sizeof(float);

    if ( default_get_param(stream, "captureFps", &captureFps, &size) < 0 ) {
        if ( (mmapsink->fpsErrorLogged & 0x0F) == 0 ) {
            mmapsink->logCb(logError, _FMT("Cannot determine the current capture fps" ));
            mmapsink->fpsErrorLogged |= 0x0F;
        }
        captureFps = 0.0;
    }
    if ( default_get_param(stream, "requestFps", &requestFps, &size) < 0 ) {
        if ( (mmapsink->fpsErrorLogged & 0xF0) == 0 ) {
            mmapsink->logCb(logError, _FMT("Cannot determine the current request fps" ));
            mmapsink->fpsErrorLogged |= 0x0F;
        }
        requestFps = captureFps;
    }

    if ( frameH != mmapsink->height || frameW != mmapsink->width ) {
        mmapsink->logCb(logInfo, _FMT("Output size had changed from " << mmapsink->width << "x" <<
                                    mmapsink->height << " to " << frameW << "x" << frameH ));
        mmapsink->height = frameH;
        mmapsink->width = frameW;
        if ( _mmapsink_open(mmapsink) < 0 ) {
            return -1;
        }
    }

    if ( frameSize+kMMAPHeaderSize <= sv_mmap_get_size(mmapsink->mmapobj) ) {
        uint8_t* ptr = sv_mmap_get_ptr(mmapsink->mmapobj);
        TRACE(_FMT("Filled a buffer: size=" << frameW << "x" << frameH <<
              " bytesToCopy=" << frameSize));
        memcpy(ptr+kMMAPHeaderSize,
               api->get_data(*frame),
               frameSize );
        // If we wrote a new frame add the header.
#ifdef _WIN32
        sprintf((char*)ptr, "%9lu%4lu%4lu%7.2f%7.2f\n",
#else
        sprintf((char*)ptr, "%9zd%4zd%4zd%7.2f%7.2f\n",
#endif
                mmapsink->frameCounter, frameW, frameH,
                requestFps,
                captureFps);
#ifdef _WIN32
        sprintf((char*)ptr+kMMAPHeaderSize+frameSize, "%4lu%4lu\n", frameW, frameH );
#else
        sprintf((char*)ptr+kMMAPHeaderSize+frameSize, "%4zd%4zd\n", frameW, frameH );
#endif

        TRACE(_FMT("Copied frame of " << frameW << "x" << frameH << " to shared memory. " <<
                                " captureFps=" << captureFps <<
                                " dataSize=" << frameSize <<
                                " pixfmt=" << pixfmt ));
        mmapsink->frameCounter = (mmapsink->frameCounter+1) % 1000000000;
    } else {
        mmapsink->logCb(logError, _FMT("Cannot output mmap frame: expectedSize="<<
                                    sv_mmap_get_size(mmapsink->mmapobj) <<
                                    " actualSize=" << frameSize ));
    }

    return 0;
}

//-----------------------------------------------------------------------------
static int         mmapsink_stream_close             (stream_obj* stream)
{
    DECLARE_STREAM_MMAPSINK(stream, mmapsink);
    sv_close_mmap(&mmapsink->mmapobj);
    sv_freep ( &mmapsink->filename );
    return 0;
}


//-----------------------------------------------------------------------------
static void mmapsink_stream_destroy         (stream_obj* stream)
{
    DECLARE_STREAM_MMAPSINK_V(stream, mmapsink);
    mmapsink->logCb(logTrace, _FMT("Destroying stream object " << (void*)stream));
    mmapsink_stream_close(stream); // make sure all the internals had been freed
    stream_destroy( stream );
}

//-----------------------------------------------------------------------------
SVVIDEOLIB_API stream_api_t*     get_mmap_sink_api                    ()
{
    return &_g_mmapsink_stream_provider;
}

