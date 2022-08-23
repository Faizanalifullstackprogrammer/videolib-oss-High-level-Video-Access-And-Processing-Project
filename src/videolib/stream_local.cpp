/*****************************************************************************
 *
 * stream_local.cpp
 *   Source node responsible integration with local / USB camera device
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
#define SV_MODULE_VAR demux
#define SV_MODULE_ID "LOCAL"
#include "sv_module_def.hpp"

#include "streamprv.h"
#include "frame_basic.h"
#include "sv_pixfmt.h"
#include "videolibUtils.h"

extern "C" {
#include <libavformat/avformat.h>

#include <localVideo.h>
}

#define LVL_DEMUX_MAGIC 0x1313

#define URI_LOCAL_CAMERA "device:"
#define NEW_DEVICE_ID_OFFSET 1000

//-----------------------------------------------------------------------------
typedef struct lvl_packet_producer  : public stream_base {
    // Drop the first few frames, to give the webcam time to adjust.
    // We do this more aggressively in CameraCapture.py.  Here we
    // just drop a few frames because they may be blank or washed out.
    int                 framesToSkip;
    int                 framesSkipped;

    char*               descriptor;     // provided externally
    char*               videoDir;       // provided externally
    int                 width;          // provided externally
    int                 height;         // provided externally

    int                 w;
    int                 h;
    int                 fmt;

    LocalVideoHandle    lvlHandle;      // handle to localVideoLib instance
    int                 localCamId;

    long                packetsRead;
    UINT64_T            prevPts;
} lvl_packet_producer_t;


//-----------------------------------------------------------------------------
// Given a local camera URI, return the device ID
static int _lvl_get_local_device_id_from_uri(lvl_packet_producer_t* demux,
                                            const char* inputFilename,
                                            fn_stream_log logCb)
{
    // If the URI doesn't have a device ID, abort
    unsigned int len = strlen(URI_LOCAL_CAMERA);
    if (strlen(inputFilename) <= len) {
        return -1;
    }

    int deviceId = atoi(inputFilename+len);

    // The ID can change if another camera is added or removed.  If the
    // name and ID don't match, we search for the name. This allows
    // for (1) IDs to change and (2) to find the right camera if we have
    // two of the same name.  Note that if you have three cameras with
    // two of the same name, removing one can result in the wrong selection.
    // The URI for local cameras is: "device:<localCamId>:<camName>".
    int n = lvlListDevicesWithoutResolutionList((log_fn_t)logCb);
    const char* strName = inputFilename + len;

    logCb(logInfo, _FMT("Searching " << inputFilename << ". " << n << " devices found." ));

    while (strName[0] != ':' && strName[0] != '\0')
        strName++;
    if (strName[0] != '\0')        // Skip the ":"
        strName++;
    if (strName[0] != '\0') {
        logCb(logInfo, _FMT( strName << " was previously " <<  deviceId));
        // In order to help alleviate confusion, we've moved to make the
        // device ID relative to the name of the camera.  Thus, if you have
        // one creative cam and two identical logitech cams, you might have:
        //   Name             Old ID      New ID
        //   Creative Webcam  0           1000
        //   Logitech Webcam  1           1000
        //   Logitech Webcam  2           1001
        //   Logitech Webcam  3           1002
        //
        // This is nice because removing the creative webcam from the system
        // doesn't mess up distinguishing between the logitech cameras.
        if (deviceId >= NEW_DEVICE_ID_OFFSET) {
            int i;

            // Remove the special offset that we added so we could tell this
            // was a new-style device ID.
            deviceId -= NEW_DEVICE_ID_OFFSET;

            // Loop through them all...  Note that we no longer fall back
            // to just picking the first name match if they specified a really
            // big device ID...
            for (i = 0; i < n; i++) {
                char* curName = lvlGetDeviceName(i);
                logCb(logInfo, _FMT("Checking against " << curName));
                if (!strcmp(curName, strName)) {
                    // Found a name match...
                    if (deviceId == 0) {
                        // We kept subtracting from deviceID as we found
                        // matches.  If we're at 0, then we've found ours!
                        return i;
                    } else {
                        // We're looking for a later one...
                        deviceId--;
                    }
                }
            }
        } else {
            logCb(logError, "Looking for VDV 1.0 style device id");
            // Old (Vitamin D Video 1.0) style: device ID is absolute...
            int i;
            if (deviceId < n &&
                !strcmp(lvlGetDeviceName(deviceId), strName)) {
               return deviceId;
            }
            for (i = 0; i < n; i++) {
                if (!strcmp(lvlGetDeviceName(i), strName)) {
                    return i;
                }
            }
        }
    }

    logCb(logError, _FMT("Failed to find device: " << strName));
    return -1;
}


//-----------------------------------------------------------------------------
// Stream API
//-----------------------------------------------------------------------------

//-----------------------------------------------------------------------------
//  Forward declarations
//-----------------------------------------------------------------------------
static stream_obj* lvl_stream_create             (const char* name);
static int         lvl_stream_set_param          (stream_obj* stream,
                                                    const CHAR_T* name,
                                                    const void* value);
static int         lvl_stream_get_param          (stream_obj* stream,
                                                    const CHAR_T* name,
                                                    void* value,
                                                    size_t* size);
static int         lvl_stream_open_in            (stream_obj* stream);
static size_t      lvl_stream_get_width          (stream_obj* stream);
static size_t      lvl_stream_get_height         (stream_obj* stream);
static int         lvl_stream_get_pixel_format   (stream_obj* stream);
static int         lvl_stream_read_frame         (stream_obj* stream, frame_obj** frame);
static int         lvl_stream_close              (stream_obj* stream);
static void        lvl_stream_destroy            (stream_obj* stream);


//-----------------------------------------------------------------------------
stream_api_t _g_lvl_stream_provider = {
    lvl_stream_create,
    NULL, // set_source
    get_default_stream_api()->set_log_cb,
    get_default_stream_api()->get_name,
    get_default_stream_api()->find_element,
    get_default_stream_api()->remove_element,
    get_default_stream_api()->insert_element,
    lvl_stream_set_param,
    lvl_stream_get_param,
    lvl_stream_open_in,
    NULL, // seek
    lvl_stream_get_width,
    lvl_stream_get_height,
    lvl_stream_get_pixel_format,
    lvl_stream_read_frame,
    get_default_stream_api()->print_pipeline,
    lvl_stream_close,
    _set_module_trace_level
} ;

//-----------------------------------------------------------------------------
#define DECLARE_DEMUX_LVL(stream, name) \
    DECLARE_OBJ(lvl_packet_producer_t, name,  stream, LVL_DEMUX_MAGIC, -1)

#define DECLARE_DEMUX_LVL_V(stream, name) \
    DECLARE_OBJ_V(lvl_packet_producer_t, name,  stream, LVL_DEMUX_MAGIC)

static stream_obj*   lvl_stream_create                (const char* name)
{
    lvl_packet_producer_t* res = (lvl_packet_producer_t*)stream_init(sizeof(lvl_packet_producer_t),
                                    LVL_DEMUX_MAGIC,
                                    &_g_lvl_stream_provider,
                                    name,
                                    lvl_stream_destroy );
    res->descriptor = NULL;
    res->videoDir = NULL;
    res->width = -1;
    res->height = -1;
    res->lvlHandle = NULL;
    res->framesToSkip = 5;
    res->framesSkipped = 0;
    res->packetsRead = 0;
    res->prevPts = 0;

    return (stream_obj*)res;
}

//-----------------------------------------------------------------------------
static int         lvl_stream_set_param             (stream_obj* stream,
                                            const CHAR_T* name,
                                            const void* value)
{
    DECLARE_DEMUX_LVL(stream, demux);
    name = stream_param_name_apply_scope(stream, name);
    SET_STR_PARAM_IF(stream, name, "url", demux->descriptor);
    SET_STR_PARAM_IF(stream, name, "dir", demux->videoDir);
    SET_PARAM_IF(stream, name, "width", int, demux->width);
    SET_PARAM_IF(stream, name, "height", int, demux->height);
    return -1;
}

//-----------------------------------------------------------------------------
static int         lvl_stream_get_param             (stream_obj* stream,
                                            const CHAR_T* name,
                                            void* value,
                                            size_t* size)
{
    static const rational_t timebase = { 1, 1000 };

    DECLARE_DEMUX_LVL(stream, demux);
    name = stream_param_name_apply_scope(stream, name);
    COPY_PARAM_IF(demux, name, "isLocalSource", int, 1);
    COPY_PARAM_IF(demux, name, "videoCodecId", int, streamBitmap);
    COPY_PARAM_IF(demux, name, "audioCodecId", int, streamUnknown);
    COPY_PARAM_IF(demux, name, "timebase",     rational_t,  timebase);
    COPY_PARAM_IF(demux, name, "width",        int,  demux->width);
    COPY_PARAM_IF(demux, name, "height",       int,  demux->height);
    return -1;
}

//-----------------------------------------------------------------------------
static basic_frame_obj* _lvl_create_frame(lvl_packet_producer_t* demux)
{
    // TODO: a bit of an abstraction leak, as we use FFMPEG's pixel format enumeration,
    // as well as their utility to calculate the size
    enum AVPixelFormat fmt = svpfmt_to_ffpfmt(demux->fmt, NULL);
    size_t allocSize = av_image_get_buffer_size(fmt, demux->w, demux->h+1, _kDefAlign); // see scale bug
    size_t dataSize = av_image_get_buffer_size(fmt, demux->w, demux->h, _kDefAlign);

    // prevent this frame object from being freed by the client code
    basic_frame_obj* f = alloc_basic_frame(LVL_DEMUX_MAGIC, allocSize, demux->logCb);
    // set up export frame
    f->width = demux->w;
    f->height = demux->h;
    f->pixelFormat = demux->fmt;
    f->dataSize = dataSize;
    f->mediaType = mediaVideo;
    return f;
}

//-----------------------------------------------------------------------------
static int         lvl_stream_open_in                (stream_obj* stream)
{
    DECLARE_DEMUX_LVL(stream, demux);
    int result;
    int localCamId = -1;
    int lvlFmt;

    if ( demux->videoDir == NULL ||
        demux->descriptor == NULL ||
        demux->width == -1 ||
        demux->height == -1 ) {
        demux->logCb(logError, _FMT("One of the required camera params isn't set - " <<
                                    " dir=" << (demux->videoDir?demux->videoDir:"NULL") <<
                                    " uri=" << (demux->descriptor?demux->descriptor:"NULL") <<
                                    " h=" << demux->height <<
                                    " w=" << demux->width ));
        result = -2;
        goto Error;
    }

    demux->logCb(logDebug, _FMT("Opening local camera - " <<
                                " dir=" << demux->videoDir <<
                                " uri=" << demux->descriptor <<
                                " size=" << demux->width << "x" << demux->height));

    // TODO: move to some global module initialization routine
    lvlSetVerbose(FALSE);

    // Determine camera identifier
    localCamId = _lvl_get_local_device_id_from_uri(demux, demux->descriptor, demux->logCb);
    if (localCamId < 0) {
        demux->logCb(logError, _FMT("Failed to get device id for URI " << demux->descriptor));
        result = -3;
        goto Error;
    }

    // open the device
    demux->lvlHandle = lvlNew((log_fn_t)demux->logCb, demux->videoDir);
    if (!demux->lvlHandle) {
        demux->logCb(logError, _FMT("Failed to get device handle for URI="
                        << demux->descriptor << " dir=" << demux->videoDir ));
        result = -4;
        goto Error;
    }

    demux->logCb(logInfo, _FMT("Opening local camera uri=" << demux->descriptor <<
                                            " id=" << localCamId <<
                                            " w=" << demux->width <<
                                            " h=" << demux->height <<
                                            " dir=" << demux->videoDir ));
    result = lvlSetupDevice(demux->lvlHandle, localCamId, demux->width, demux->height);
    if (result == 0) {
        demux->logCb(logError, _FMT("Failed to setup device for URI " << demux->descriptor));
        result = -5;
        goto Error;
    }
    demux->localCamId = localCamId;


    demux->w = lvlGetWidth(demux->lvlHandle, demux->localCamId);
    demux->h = lvlGetHeight(demux->lvlHandle, demux->localCamId);

    lvlFmt = lvlGetPixelFormat();
    switch  (lvlFmt) {
    case PIXEL_FORMAT_RGB24:        demux->fmt = pfmtRGB24; break;
    case PIXEL_FORMAT_YUYV422:      demux->fmt = pfmtYUYV422; break;
    default:
        demux->logCb(logWarning, _FMT("Unexpected pixfmt value from localVideoLib: " << lvlFmt));
#ifdef WIN32 // This should not happen, but for now have a bit of inside knowledge
        demux->fmt = pfmtYUYV422;
#else
        demux->fmt = pfmtRGB24;
#endif
        break;
    }

    return 0;

Error:
    lvl_stream_close(stream);
    return result;
}


//-----------------------------------------------------------------------------
static size_t      lvl_stream_get_width          (stream_obj* stream)
{
    DECLARE_DEMUX_LVL(stream, demux);
    return demux->w;
}

//-----------------------------------------------------------------------------
static size_t      lvl_stream_get_height         (stream_obj* stream)
{
    DECLARE_DEMUX_LVL(stream, demux);
    return demux->h;
}

//-----------------------------------------------------------------------------
static int         lvl_stream_get_pixel_format   (stream_obj* stream)
{
    DECLARE_DEMUX_LVL(stream, demux);
    return demux->fmt;
}

//-----------------------------------------------------------------------------
static int         lvl_stream_read_frame        (stream_obj* stream, frame_obj** frame)
{
    DECLARE_DEMUX_LVL(stream, demux);

    int res;
    if ( demux->localCamId < 0 ) {
        return -1;
    }

    static const int kTimeout = 5000;
    // 16ms is distance close to 60fps ... if we see frames more often that that, ignore them
    static const int kMinFrameDistance = 16;
    UINT64_T            start = sv_time_get_current_epoch_time(),
                        elapsed, diff;
    basic_frame_obj*    f = _lvl_create_frame(demux);

TryAgain:
    *frame = NULL;
    res = lvlGetPixels(demux->lvlHandle,
                        demux->localCamId,
                        (unsigned char*)f->data,
                        TRUE,
                        TRUE);
    elapsed = sv_time_get_elapsed_time(start);
    if (res != 0) {
        if ( demux->framesToSkip >= demux->framesSkipped ) {
            demux->framesSkipped++;
            goto TryAgain;
        }
        f->pts = sv_time_get_current_epoch_time();
        diff = f->pts - demux->prevPts;
        if ( diff < kMinFrameDistance ) {
            TRACE(_FMT("Ignoring frame: pts=" << f->pts << " delta=" << diff << " wait=" << elapsed));
            goto TryAgain;
        }
        TRACE(_FMT("New frame: pts=" << f->pts << " delta=" << diff << " wait=" << elapsed));
        demux->prevPts = f->pts;
        *frame = (frame_obj*)f;
        return 0;
    }

    if (elapsed<kTimeout) {
        sv_sleep(10);
        goto TryAgain;
    }

    demux->logCb(logError, _FMT("Local camera error: no frame had been available in " << kTimeout << "ms"));
    frame_unref((frame_obj**)&f);
    return -1;
}

//-----------------------------------------------------------------------------
static int         lvl_stream_close             (stream_obj* stream)
{
    DECLARE_DEMUX_LVL(stream, demux);
    if ( demux->lvlHandle ) {
        if ( demux->localCamId != -1 ) {
            lvlStopDevice(demux->lvlHandle, demux->localCamId);
            demux->localCamId = -1;
        }
        lvlDelete(demux->lvlHandle);
        demux->lvlHandle = NULL;
    }
    sv_freep(&demux->descriptor);
    sv_freep(&demux->videoDir);
    demux->width = -1;
    demux->height = -1;
    demux->framesToSkip = 5;
    demux->framesSkipped = 0;
    return 0;
}

//-----------------------------------------------------------------------------
static void lvl_stream_destroy         (stream_obj* stream)
{
    DECLARE_DEMUX_LVL_V(stream, demux);
    lvl_stream_close(stream); // make sure all the internals had been freed
    stream_destroy( stream );
}

//-----------------------------------------------------------------------------
SVVIDEOLIB_API stream_api_t*     get_lvl_stream_api             ()
{
    return &_g_lvl_stream_provider;
}

