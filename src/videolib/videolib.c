/*****************************************************************************
 *
 * videolib.c
 *   C high-level API used by Sighthound Video application
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
#include "sv_pcap.h"
#include "sv_ffmpeg.h"

#include "videolib.h"
#include "videolibUtils.h"
#include "streamFactories.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#ifndef _WIN32
#include <unistd.h>
#include <sys/time.h>
#endif
#include <assert.h>
#include <ctype.h>



#define DEBUG_FRAME_FLOW 0


// NOTE: About lines tagged 'scaleBug'
// There appears to be a bug in SWS_FAST_BILINEAR.  Specifically, on this
// line in 'hcscale_fast':
//    dst[i+VOFW]=(src2[xx]*(xalpha^127)+src2[xx+1]*xalpha);
// The problem is that it seems to go one past the end of src2 when it's doing
// src2[xx+1].  In the case that we saw, src2 was 320 big, and xx was 319.
// That means we were accessing the 321st entry, which can cause a crash.
//
// To 'work around' the problem, we always make our buffers 1 line bigger
// than they need to be.  Technically, we only think we need 1 extra byte,
// but it seems better to be safe and allocate a whole extra line.
//
// Even though the code is now accessing invalid memory, we think it's OK
// because it will (we think) always end up masking out the result, since
// xalpha is always 0.

// Constants


// The width and height of images for processing is currently fixed (almost).
// This is the desired width / height, but it might be slightly off if we have
// a source image w/ a different aspect ratio...
//
// Our current algorithms depend greatly on minimum object height so we always
// want to scale to ___x240 (PROC_IMAGE_WIDTH should be arbitrarily large).
#define PROC_IMAGE_HEIGHT 240

// FPS at which we run analytics
#define DEFAULT_PROC_FPS 10
#define PROC_FPS_VAR "SV_PROC_FPS"
#define GET_FPS() sv_get_int_env_var(PROC_FPS_VAR, DEFAULT_PROC_FPS)
#define GET_FPS_EX(def) sv_get_int_env_var(PROC_FPS_VAR, def)

// Users can put this in their URL to hack a different value for analyzeduration
#define ANALYZE_DURATION_URL_KEY     "analyzeduration="
#define FORCE_MJPEG_URL_KEY          "svforcemjpeg"
#define FORCE_TCP_URL_KEY            "svforcetcp"
#define FORCE_FLV_FIX_KEY            "svforceflvfix"
#define DEFAULT_TO_TCP_URL_KEY       "svdefaulttotcp"
#define BUFFER_SIZE_KB               "svbuffersize"
#define SOCKET_BUFFER_SIZE_KB        "svsocketbuffersize"
#define DISABLE_GET_PARAMETER_KEY    "svdisablegetparameter"

// FFmpeg used to parse tcp from query strings but this is now deprecated. We
// still need to check for this and do the right thing as some users have
// manually added it, and we have it in some of our configured camera streams.
// We should only check if it appears at the end of the URL, and not strip it.
#define FORCE_TCP_LEGACY_URL_KEY     "tcp"

// Prefix for FFmpeg messages joining our log stream.
#define FFMPEG_LOG_PREFIX  "FFMPEG >> "


// Private functions
static CodecConfig* copy_codec_config(CodecConfig* config);
static void free_codec_config(CodecConfig** config);
static int _pause_mmap(StreamData* data, int pause);
static void _close_mmap(StreamData* data);

static sv_lib*                  pcapLib = NULL;
static sv_capture_traffic_t     sv_pcap_start = NULL;
static sv_stop_capture_t        sv_pcap_stop = NULL;
static sv_capture_possible_t    sv_pcap_check = NULL;
static sv_pcap*                 activeCapture = NULL;


typedef stream_api_t*    (*module_factory)             ();

const char* gModuleNames [] = {
    "FFMPEG Demux",
    "Decoder",
    "Encoder",
    "Recorder",
#if LOCAL_CAMERA_SUPPORT
    "Local Camera",
#endif
    "Live555 Demux",
    "Input Iterator",
    "Resize",
    "Splitter",
    "Limiter",
    "MMAP",
    "FFMPEG Filters",
    "Thread Connector",
    "Metadata Injector",
    "Audio renderer",
    "Mask Filter",
    "Audio resample",
#if WIN32
    "MediaFoundation Decoder",
#endif
    "Recorder Sync",
    "Sync Buffer",
    "Clip Reader",
    NULL
};

module_factory gModules[] = {
    get_ffmpeg_demux_api          ,
    get_ffdec_stream_api          ,
    get_ffenc_stream_api          ,
    get_ffsink_stream_api         ,
#if LOCAL_CAMERA_SUPPORT
    get_lvl_stream_api            ,
#endif
    get_live555_demux_stream_api  ,
    get_input_iterator_api        ,
    get_resize_filter_api         ,
    get_splitter_api              ,
    get_limiter_filter_api        ,
    get_mmap_sink_api             ,
    get_ff_filter_api             ,
    get_tc_api                    ,
    get_metainject_filter_api     ,
    get_audio_renderer_pa_api     ,
    get_pixelate_filter_api       ,
    get_resample_filter_api       ,
#ifdef WIN32
    get_mfdec_stream_api          ,
#endif
    get_recorder_sync_api         ,
    get_jitbuf_stream_api         ,
};

const char** get_module_names()
{
    return gModuleNames;
}

static int _gClipDebugEnabled = -1;

void set_module_trace_level(const char* module, int level)
{
    int nI=0;

    // process the one-offs, for which there's no stream provider API
    if (!_stricmp(module, "Clip Reader")) {
        _gClipDebugEnabled = level;
        return;
    }

    while (gModuleNames[nI] != NULL) {
        if (!_stricmp(gModuleNames[nI], module)) {
            gModules[nI]()->set_trace_level(level);
            return;
        }
        nI++;
    }
}


static const int _kOne=1;
static const int _kTwo=2;
static const int _kZero=0;
static const int _kMediaVideo=mediaVideo;
static const int _kMediaAudio=mediaAudio;
static const int _kCodecH264=streamH264;
static const int _kCodecAAC=streamAAC;
static const int _kCodecLinear=streamLinear;


static int _find_and_strip_url_param(char* filename,
                                        const char* option,
                                        log_fn_t logFn);
static int _preload_bounding_boxes( stream_obj** pCtx,
                                int flags,
                                int numBoxes,
                                BoxOverlayInfo* boxes,
                                uint64_t fileStart,
                                const char* sMetainjName,
                                log_fn_t logFn);
static stream_api_t* _enable_bounding_boxes( stream_obj** pCtx,
                                int flags,
                                int numBoxes,
                                BoxOverlayInfo* boxes,
                                uint64_t fileStart,
                                const char* sInsertAfter,
                                int useFFMPEGForBoxes,
                                log_fn_t logFn);
static stream_api_t* _enable_regions( stream_obj** pCtx,
                                int flags,
                                int numBoxes,
                                BoxOverlayInfo* boxes,
                                uint64_t fileStart,
                                const char* sInsertAfter,
                                log_fn_t logFn);

//-----------------------------------------------------------------------------
int        _enable_file_recording(int enable,
                                int recordInMemory,
                                int timestampFlags,
                                int enableAudioRecording,
                                stream_obj** ctx_ptr,
                                const char* insert_before,
                                const char* dir,
                                int h,
                                int w,
                                InputData2* input,
                                CodecConfig* codecConfig,
                                log_fn_t logFn)
{
    stream_obj   *ctx = *ctx_ptr,
                 *subgraph = NULL,
                 *timestamp = NULL,
                 *splitter = NULL,
                 *sync = NULL;
    stream_api_t *api = stream_get_api(ctx),
                 *splitter_api = get_splitter_api(),
                 *subgraph_api = get_default_stream_api();
    char         buffer[2048];


    int removalNeeded = 0;
    int res = -1, inserted,
        codecH264=streamH264,
        codecAAC=streamAAC,
        typeAudio=mediaAudio,
        typeVideo=mediaVideo;

    if ( !enable ) {
        removalNeeded = 1;
        res = 0;
    } else
    if ( api->find_element(ctx, "fileRecorder") != NULL ) {
        // already enabled
        log_info(logFn, "File recording is already enabled");
    } else {
        // Create all the filters we'll need

        if (!input->rawFrameRecording) {
            int pixfmt = (int)OUTPUT_PIX_FMT;
            // resize filter is present when operating on non-H264 frames --
            // but may operate as passthrough, if no resizing is required,
            // and pixfmt of the source matches OUTPUT_PIX_FMT
            APPEND_FILTER(subgraph_api, subgraph, resize_factory_api, "resize");
            if ( subgraph_api->set_param(subgraph, "resize.height", &h) < 0 ||
                 subgraph_api->set_param(subgraph, "resize.width",  &w) < 0 ||
                 subgraph_api->set_param(subgraph, "resize.pixfmt", &pixfmt) < 0
                 ) {
                log_err(logFn, "Failed to configure recorder's resize filter");
                goto Cleanup;
            }
        }


        if (sv_transcode_audio()) {
            APPEND_FILTER(subgraph_api, subgraph, audioenc_api, "audioEncoder");
        }
        APPEND_FILTER(subgraph_api, subgraph, ffenc_stream_api, "encoder");
        APPEND_FILTER(subgraph_api, subgraph, ffsink_stream_api, "recorder");


        splitter = splitter_api->create("fileRecorder");
        splitter_api->set_log_cb(splitter,  (fn_stream_log)logFn);
        stream_ref(splitter);

        if ( splitter_api->set_param(splitter, "fileRecorder.subgraph", subgraph) < 0 ) {
            log_err(logFn, "Failed to configure recorder splitter");
            goto Cleanup;
        }

        int counter = 1;
        if ( subgraph_api->set_param(subgraph, "encoder.hls", &_kZero) < 0 || counter++ == 0 ||
             subgraph_api->set_param(subgraph, "encoder.dstCodecId", &_kCodecH264) < 0 || counter++ == 0 ||
             subgraph_api->set_param(subgraph, "encoder.encoderType", &_kMediaVideo) < 0 || counter++ == 0 ||
             ( sv_transcode_audio() && subgraph_api->set_param(subgraph, "audioEncoder.encoderType", &_kMediaAudio) < 0 ) ||
             ( sv_transcode_audio() && subgraph_api->set_param(subgraph, "audioEncoder.dstCodecId", &_kCodecAAC) < 0 ) ||
             subgraph_api->set_param(subgraph, "recorder.hls", &_kZero) < 0 || counter++ == 0 ||
             subgraph_api->set_param(subgraph, "recorder.audioOn", &enableAudioRecording) < 0 || counter++ == 0 ||
             subgraph_api->set_param(subgraph, "recorder.outputLocation", dir) < 0 || counter++ == 0 ||
             subgraph_api->set_param(subgraph, "recorder.recordInRAM", &recordInMemory) < 0 || counter++ == 0
             ) {
            log_err(logFn, "Failed to configure recorder: %d", counter  );
            goto Cleanup;
        }


        if ( codecConfig != NULL ) {
            if ( codecConfig->gop_size > 0) {
                subgraph_api->set_param(subgraph, "encoder.gop_size", &codecConfig->gop_size );
            }
            if ( codecConfig->keyint_min > 0) {
                subgraph_api->set_param(subgraph, "encoder.keyint_min", &codecConfig->keyint_min );
            }
            if ( codecConfig->max_bit_rate > 0) {
                subgraph_api->set_param(subgraph, "encoder.max_bitrate", &codecConfig->max_bit_rate );
            }
            if ( codecConfig->bit_rate_multiplier > 0) {
                subgraph_api->set_param(subgraph, "encoder.bit_rate_multiplier", &codecConfig->bit_rate_multiplier );
            }
            if ( codecConfig->preset != NULL) {
                subgraph_api->set_param(subgraph, "encoder.preset", codecConfig->preset );
            }
            if ( codecConfig->sv_profile != svvpNotSpecified ) {
                subgraph_api->set_param(subgraph, "encoder.videoQualityPreset", &codecConfig->sv_profile );
            }
        }

        removalNeeded = 1;
        inserted = api->insert_element(ctx_ptr,
                                        &api,
                                        insert_before,
                                        splitter,
                                        svFlagStreamInitialized | svFlagStreamOpen);
        if (  inserted < 0 ) {
            log_err(logFn, "Failed to enable file recording");
            goto Cleanup;
        }

        sync = get_recorder_sync_api()->create("fileRecorderSync");
        stream_get_api(sync)->set_param(sync, "recorderName", "fileRecorder.subgraph.recorder" );
        inserted = api->insert_element(ctx_ptr,
                                        &api,
                                        NULL,
                                        sync,
                                        svFlagStreamInitialized | svFlagStreamOpen);
        if (  inserted < 0 ) {
            log_err(logFn, "Failed to enable file recording -- failed to insert fileRecorderSync");
            goto Cleanup;
        }


        api->print_pipeline(*ctx_ptr, buffer, 2048);
        log_dbg(logFn, "Recording started. New pipeline: %s", buffer);
        res = 0;
        removalNeeded = 0;
    }

Cleanup:
    if (removalNeeded) {
        int removed1 = api->remove_element(ctx_ptr, NULL, "fileRecorder", NULL);
        int removed2 = api->remove_element(ctx_ptr, NULL, "fileRecorderSync", NULL);
        log_info(logFn, "Disabled fileRecorder; removed=%d", removed1 + removed2);
    }

    stream_unref(&subgraph);
    stream_unref(&splitter);
    return res;
}

//-----------------------------------------------------------------------------
int        _enable_audio_playback(InputData2* input,
                                    stream_obj** pCtx,
                                    const char* insertionPoint,
                                    int audioVolume,
                                    int flags )
{
    if ( !input->hasAudio ) {
        return 0;
    }

    if (pCtx == NULL || audioVolume <= 0 ) {
        pCtx = &input->streamCtx;
    }
    stream_api_t* api = stream_get_api(*pCtx);
    int initFlag = (flags&svFlagStreamInitialized);

    if ( audioVolume == input->audioVolume ||
        // TODO: when we add volume control, this will need to go away
         !( audioVolume>0 ^ input->audioVolume>0)
        ) {
        log_dbg(input->logFn, "Audio is already %s", audioVolume>0?"enabled":"disabled");
        return 0;
    }

    if ( audioVolume>0 ) {
        stream_obj*     dec = get_audiodec_api()->create("audio_decode");
        stream_obj*     render = get_audio_renderer_pa_api()->create("audio_render");

        api->insert_element(pCtx, &api, insertionPoint, dec, initFlag);
        api->set_param(*pCtx, "audio_decode.decoderType", &_kMediaAudio);
        api->set_param(*pCtx, "audio_decode.dstCodecId", &_kCodecLinear);

        api->insert_element(pCtx, &api, "audio_decode", render, svFlagNone);

        if (initFlag) {
            stream_get_api(render)->open_in(render);
        }

        input->audioVolume = audioVolume;
    } else {
        api->remove_element(pCtx, &api, "audio_decode", NULL);
        api->remove_element(pCtx, &api, "audio_render", NULL);

        input->audioVolume = 0;
    }

    return 0;
}

//-----------------------------------------------------------------------------
static int   open_input(InputData2* input,
                        const char* filename,
                        const char* dir,
                        int recordWidth,
                        int recordHeight,
                        int requestedWidth,
                        int requestedHeight,
                        int requestedPixFmt,
                        int fpsLimit,
                        int initStreamBufferSize,
                        int flags,
                        int timestampFlags,
                        int numBoxes,
                        BoxOverlayInfo* boxes,
                        int numRegions,
                        BoxOverlayInfo* regions,
                        uint64_t timestampOffset,
                        CodecConfig* config,
                        log_fn_t logFn )
{
/*
    [DEMUX] -> [RECORDER] -> [DECODER] -> [LIVE/HLS] -> [MMAP] -> [RECORDER] -> [PROCRESIZE] -> (out)
                   *2           *1         #3                        *2

    *1) Optional -- not used in case of a local camera
    *2) For h264 input, recorder sits immediately after demux. For everything else,
        we put it behind FPS limiter.
    *3) Optional -- only if live view is enabled. Inserted after decoder
*/

    char            buffer[256];
    stream_api_t*   api;
    stream_obj*     ctx;
    int             needDecoder = 1;
    int             inserted;
    const char*     fpsInsertionPoint = "tc_edge";
    const char*     filterInsertionPoint = NULL;
    size_t          size;
    int             param;
    int             res;
    int             rtsp =

    input->rawFrameRecording = 0;
    input->audioVolume = 0;
    input->logFn = logFn;
    input->fps = fpsLimit;

    // there is always a demux
    input->auxInsertionPoint = "demux";

    int wantTCP = (oifWantTCP & flags) ? 1 : 0;
    int shouldRecord = (oifShouldRecord & flags) ? 1 : 0;
    int liveStream = (oifLiveStream & flags) ? 1 : 0;
    int enableAudioRecording = (oifDisableAudio & flags ) ? 0 : 1;
    int keyframeOnly = (oifKeyframeOnly & flags ) ? 1 : 0;
    int threaded = (flags & oifEdgeThread) ? 1 : 0;
    int recordInMemory = (flags & oifRecordInMemory) ? 1 : 0;
    int simulation = (flags & oifSimulation) ? 1 : 0;

    if (!strncmp(filename, URI_LOCAL_CAMERA, strlen(URI_LOCAL_CAMERA))) {
#if LOCAL_CAMERA_SUPPORT
        api = get_lvl_stream_api();
        needDecoder = 0;
#else
        log_info(logFn, "This version of videolib doesn't support local cameras");
        return -1;
#endif
    } else
    if (!strncmp(filename, URI_RTSP_CAMERA, strlen(URI_RTSP_CAMERA))) {
        api = get_live555_demux_stream_api();
    } else {
        api = get_ffmpeg_demux_api();
    }
    ctx = api->create("demux");
    stream_ref(ctx);
    api->set_log_cb(ctx, (fn_stream_log)logFn);

    api->set_param(ctx, "demux.keyframeOnly", &keyframeOnly);
    if (needDecoder) {
        inserted = APPEND_FILTER(api, ctx, ffdec_stream_api, "decoder");
        // we can tap recorder/HLS/mmap here now
        input->auxInsertionPoint = "decoder";
    } else {
        // local camera's demux requires some additional params
        api->set_param(ctx, "demux.width", &recordWidth);
        api->set_param(ctx, "demux.height", &recordHeight);
        api->set_param(ctx, "demux.dir", dir);
    }
    filterInsertionPoint = input->auxInsertionPoint;

    // Thread will serve as insertion point for mmap, and possible for
    // HLS and recorder streams (if raw H264 tapping isn't a possibility)
    if ( threaded ) {
        int maxQueueSize = 30;
        // live555 demux will timeout in 5 seconds -- this timeout should
        // never take effect, unless using mjpeg camera
        int timeout = 8000;
        inserted = APPEND_FILTER(api, ctx, tc_api, "tc_edge");
        api->set_param(ctx, "tc_edge.maxQueueSize", &maxQueueSize);
        api->set_param(ctx, "tc_edge.timeout", &timeout);
        if ( fpsLimit ) {
            api->set_param(ctx, "tc_edge.fpsLimit", &fpsLimit);
        }
        if ( liveStream ) {
            int interval = 60*10;
            api->set_param(ctx, "tc_edge.lossy", simulation ? &_kZero : &_kOne);
            api->set_param(ctx, "tc_edge.statsIntervalSec", &interval);
        }
    }


    if ( requestedWidth >= 0 &&
         requestedHeight >= 0 &&
         requestedPixFmt != pfmtUndefined) {
        // only add the resize filter if we're not in a remux/clip creation mode

        // for local clips playback, we want to resize before queueing
        const char* insertionPoint = (liveStream && threaded ? "tc_edge" : input->auxInsertionPoint);

        inserted = INSERT_FILTER(api, ctx, resize_factory_api, "procPixfmt", insertionPoint);
        api->set_param(ctx, "procPixfmt.pixfmt", &requestedPixFmt);
        filterInsertionPoint = "procPixfmt";

        if ( liveStream && (requestedWidth > 0 || requestedHeight > 0) ) {
            // retaining original frames is memory intensive, so we limit those
            // to the specified interval
            int originalFrameInterval = 500;

            // for analytics we may need to retain the source frame; in this case
            // make resize a separate filter, and ask it to retain the source frame
            inserted = INSERT_FILTER(api, ctx, resize_factory_api, "procResize", filterInsertionPoint);
            api->set_param(ctx, "procResize.width", &requestedWidth);
            api->set_param(ctx, "procResize.height", &requestedHeight);
            api->set_param(ctx, "procResize.retainSourceFrameInterval", &originalFrameInterval);
            filterInsertionPoint = "procResize";
        } else {
            // if the source frame isn't needed, resize as needed in the context
            // of the pixfmt conversion filter
            api->set_param(ctx, "procPixfmt.width", &requestedWidth);
            api->set_param(ctx, "procPixfmt.height", &requestedHeight);
        }
    }

    // draw on return image only if necessary, and after resizing had been done
    if ( !shouldRecord ) {
        api = _enable_timestamp(&ctx, flags, timestampFlags, timestampOffset, "ts", filterInsertionPoint, logFn );
    }
    if ( numRegions )
        flags |= oifShowRegions;
    // By this point we have RGB, and our filter is the more efficient way to deal with it.
    int useFFMPEGForBoxes = 0;
    api = _enable_bounding_boxes( &ctx, flags, numBoxes, boxes, timestampOffset,
                                filterInsertionPoint, useFFMPEGForBoxes, logFn);
    api = _enable_regions( &ctx, flags, numRegions, regions, timestampOffset, filterInsertionPoint, logFn);



    char* filenameCopy = strdup(filename);
    /*int analyzeDuration = - unused */_find_and_strip_url_param(
                                filenameCopy,
                                ANALYZE_DURATION_URL_KEY,
                                logFn);
    int forceMjpeg      = _find_and_strip_url_param(
                                filenameCopy,
                                FORCE_MJPEG_URL_KEY,
                                logFn);
    int forceFlvFix     = _find_and_strip_url_param(
                                filenameCopy,
                                FORCE_FLV_FIX_KEY,
                                logFn);
    int forceTcp        = _find_and_strip_url_param(
                                filenameCopy,
                                FORCE_TCP_URL_KEY,
                                logFn) ||
                          _find_and_strip_url_param(
                                filenameCopy,
                                DEFAULT_TO_TCP_URL_KEY,
                                logFn) ||
                          wantTCP;
    int bufferSize      = _find_and_strip_url_param(
                                filenameCopy,
                                BUFFER_SIZE_KB,
                                logFn);
    int disableGetParam = _find_and_strip_url_param(
                                filenameCopy,
                                DISABLE_GET_PARAMETER_KEY,
                                logFn);
    if ( bufferSize == 0 ) {
        bufferSize = initStreamBufferSize;
    }
    int socketBufferSize= _find_and_strip_url_param(
                                filenameCopy,
                                SOCKET_BUFFER_SIZE_KB,
                                logFn);

    if (ctx) {
        api->set_param(ctx, "url", filenameCopy);
        api->set_param(ctx, "liveStream", &liveStream);
        if (bufferSize)       api->set_param(ctx, "bufferSizeKb", &bufferSize);
        if (socketBufferSize) api->set_param(ctx, "socketBufferSizeKb", &socketBufferSize);
        api->set_param(ctx, "forceTCP", &forceTcp);
        api->set_param(ctx, "defaultToTCP", &forceTcp);
        api->set_param(ctx, "forceMJPEG", &forceMjpeg);
        api->set_param(ctx, "forceFlvFix", &forceFlvFix);
        api->set_param(ctx, "disableGetParameter", &disableGetParam);
        int result = api->open_in(ctx);
        if ( result < 0 ) {
            log_err(logFn, "Failed to open stream context res=%d", result);
            stream_unref(&ctx);
            input->streamCtx = NULL;
        } else if (shouldRecord) {

            const char* insertBefore = input->auxInsertionPoint;
            int recWidth = recordWidth, recHeight = recordHeight;
            if ( recordHeight == 0 ) {
                size = sizeof(int);
                res = api->get_param(ctx, "demux.videoCodecId", &param, &size);
                if ( res >=0 ) {
                    if ( param == streamH264) {
                        // we can record H264 directly, but do it outside of "real-time" thread
                        insertBefore="demux";
                        recWidth = 0;
                        recHeight = 0;
                        timestampFlags = 0; // can't apply a filter when operating before decoder
                        input->rawFrameRecording = 1;
                    } else {
                        // we get MJPEG or RAW frames that'll need encoding ... cap that at 10 fps
                        input->fps = fpsLimit = GET_FPS();
                        fpsInsertionPoint = insertBefore;
                    }
                }
            }

            _enable_file_recording(1, recordInMemory, timestampFlags, enableAudioRecording, &ctx, insertBefore, dir,
                                    recHeight, recWidth, input, config, logFn);
            api = stream_get_api(ctx);

            // by default, log stats message every hour ... this can be changed by debug trace settings
            int statsIntervalSec = 60*60;
            api->set_param(ctx, "statsIntervalSec", &statsIntervalSec);
        }

        if ( result >= 0 ) {
            // determine if the input has audio
            size = sizeof(int);

            int resGetParam = api->get_param(ctx, "demux.audioCodecId", &param, &size);
            input->hasAudio = (resGetParam >= 0 && param != streamUnknown) ? 1 : 0;
            if ( flags & oifRenderAudio ) {
                _enable_audio_playback(input, &ctx, NULL, 1, svFlagStreamInitialized);
                api = stream_get_api(ctx);
            }
        }

        if ( fpsLimit && !threaded ) {
            // only instantiate fps limiter, if edge thread isn't performing this function
            stream_obj* limiter = get_limiter_filter_api()->create("fpslimiter");
            api->set_param(ctx, "fpslimiter.fps", &fpsLimit);
            inserted = api->insert_element(&ctx,
                                &api,
                                fpsInsertionPoint,
                                limiter,
                                svFlagStreamInitialized | svFlagStreamOpen);
            if ( inserted < 0 ) {
                log_err(logFn, "Failed to initialize fps limiter");
            }
        }
    } else {
        log_err(logFn, "Failed to create stream context");
    }

    // at this point we are safe to free this copy
    free( filenameCopy );

    if (ctx==NULL) {
        return -1;
    }
    input->width  = api->get_width(ctx);
    input->height = api->get_height(ctx);
    input->pixFmt = api->get_pixel_format(ctx);

    int passthrough = 0;
    size = sizeof(passthrough);
    input->hasResize = api->get_param(ctx, "procResize.passthrough", &passthrough, &size) >= 0 &&
                        !passthrough;

    input->streamCtx = ctx;

    log_dbg(logFn, "Opened stream: uri=%s, dir=%s, proc=%dx%d, buf=%d",
                            sv_sanitize_uri(filename, buffer, sizeof(buffer)),
                            dir?dir:"NULL",
                            input->width,
                            input->height,
                            bufferSize);
    return 0;
}

//-------------------------------------------------------------------------------------------------
static void     pcap_log_cb(void* ctx, int level, const char* msg)
{
    int l = kLogLevelInfo;

    if (level == svpllError) {
        l = kLogLevelError;
    }


    ((log_fn_t)ctx)( l, msg );
}

//-------------------------------------------------------------------------------------------------
// Enables packet capture
// returns
// 0 on success,
// -1 on insufficient priveledges,
// -2 if driver isn't installed or inactive
// -3 if the capture is already enabled
SVVIDEOLIB_API int enable_packet_capture(const char* captureLocation, const char* cameraUri, log_fn_t logFn)
{
    int res;

    if ( activeCapture != NULL ) {
        return -1;
    }

    if ( pcapLib != NULL ) {
        sv_unload(&pcapLib);
    }

    pcapLib = sv_load("svpcap");
    if (!pcapLib) {
        return -2;
    }
    sv_pcap_start   = sv_get_sym( pcapLib, "sv_capture_traffic" );
    sv_pcap_stop    = sv_get_sym( pcapLib, "sv_stop_capture" );
    sv_pcap_check   = sv_get_sym( pcapLib, "sv_capture_possible" );
    if ( !sv_pcap_start || !sv_pcap_stop || !sv_pcap_check ) {
        return -2;
    }

    res = sv_pcap_check();
    if ( res < 0 ) {
        return res;
    }
    activeCapture = sv_pcap_start(cameraUri, captureLocation, 0, 0, pcap_log_cb, (void*)logFn );
    if ( activeCapture == NULL ) {
        return -4;
    }
    return 0;
}


//-----------------------------------------------------------------------------
// Attempts to open an input stream and begin a read thread.  Returns a pointer
// to an allocated StreamData structure or NULL if there was an error.
// If record is 0, no video will be recorded.
SVVIDEOLIB_API StreamData* open_stream(const char* inputFilename,
                        const char* sanitizedFilename,
                        int width, int height, int fps,
                        const char* videoDir,
                        CodecConfig* codecConfig,
                        int initialFrameBufferSize, int flags,
                        log_fn_t logFn)
{
    // Uncomment the following if you want to pause for debugger attach
    //fprintf(stderr, "Pausing... To debug, attach to PID %i.", getpid());
    //pause();

    if ( !inputFilename || !*inputFilename ) {
        log_err( logFn, "Cannot open the stream - empty URL provided.");
        return NULL;
    }


    // Allocate the data structure and open the stream
    StreamData* data = av_mallocz(sizeof(*data));
    data->openStartTime = sv_time_get_current_epoch_time();
    data->framesServed = 0;

    // Initialize pointers to NULL
    data->isRunning = 1;
    data->mmapWidth = 0;
    data->mmapHeight = 0;
    data->mmapFps = 0;
    data->mmapPaused = 0;
    data->shouldRecord = ( flags & oifShouldRecord );
    data->mmapFilename = NULL;
    data->mmapSubgraph = NULL;
    data->mmapPosition = NULL;
    data->codecConfig = NULL;
    data->lastFrameRead = NULL;
    data->enableAudioRecording = (( flags & oifDisableAudio ) == 0);

    data->liveCodecConfig = NULL;
    data->liveTimestampsEnabled = 0;

    data->hlsProfiles = NULL;
    data->hlsProfilesCount = 0;
    data->hlsMaxResolution = -1;
    data->hlsMaxBitrate = -1;

    data->mutex = sv_mutex_create();
    data->graphMutex = sv_mutex_create();

    // Init logger...
    stream_set_default_log_cb(logFn);
    data->logFn = logFn;

    flags |= oifLiveStream | oifEdgeThread;
    fps = GET_FPS_EX( fps ? fps : DEFAULT_PROC_FPS );
    log_info(logFn, "============================ Attempting to open camera at '%s' (%dx%d), flags=%d, fps=%d, dir=%s ============================",
             sanitizedFilename?sanitizedFilename:"NULL", width, height, flags, fps, videoDir);

    if (open_input(&data->inputData2,
                    inputFilename,
                    videoDir,
                    width,
                    height,
                    0,
                    PROC_IMAGE_HEIGHT,
                    GET_FRAME_PIX_FMT,
                    fps,
                    initialFrameBufferSize,
                    flags,
                    0,
                    0, NULL,
                    0, NULL,
                    0,
                    codecConfig,
                    logFn) < 0) {
        log_err(logFn, "Failed to open camera at %s", sanitizedFilename);
        goto open_stream_error;
    }

    // Copy the codec configuration
    data->codecConfig = copy_codec_config(codecConfig);
    if (!data->codecConfig) {
        log_err(logFn, "Couldn't copy the codec configuration.");
        goto open_stream_error;
    }

    refresh_hls_profiles(data);
    if ( flags & oifFastStart ) {
        for (int nI=1; nI<=data->hlsProfilesCount; nI++ ) {
            prepare_live_stream(data, nI);
        }
    }

    data->openEndTime = sv_time_get_current_epoch_time();
    return data;

open_stream_error:
    // Cleanup, return with nothing.
    free_stream_data(&data);
    return NULL;
}

//-----------------------------------------------------------------------------
SVVIDEOLIB_API int move_recorded_file(log_fn_t logFn, const char* src, const char* dst)
{
    return ffmpeg_flush_buffered_io( logFn, src, dst);
}

//-----------------------------------------------------------------------------
// Get the requestFps and captureFps.  Here's what they mean:
// - requestFps: The average # of times per second that get_new_frame() has
//   been called.  Note: this can be higher than captureFps, since
//   get_new_frame() effectively polls for a new frame.
// - captureFps: The average # of times per second that a new frame came in
//   from the camera.
SVVIDEOLIB_API void get_fps_info(StreamData *data, float* requestFps, float* captureFps)
{
    if (data) {
        sv_mutex_enter(data->graphMutex);

        stream_obj*       ctx = data->inputData2.streamCtx;
        stream_api_t*     api = stream_get_api(ctx);

        if ( api && ctx ) {
            if ( requestFps ) {
                size_t size = sizeof(float);
                if ( api->get_param(ctx,
                                    "tc_edge.requestFps",
                                    requestFps,
                                    &size) < 0 ) {
                    float fps = (float)data->inputData2.fps;
                    *requestFps = fps ? fps : DEFAULT_PROC_FPS;
                }
            }
            if ( captureFps ) {
                size_t size = sizeof(float);
                if ( api->get_param(ctx,
                                    "tc_edge.captureFps",
                                    captureFps,
                                    &size) < 0 ) {
                    float fps = (float)data->inputData2.fps;
                    *captureFps = fps ? fps : DEFAULT_PROC_FPS;
                }
            }
        }
        sv_mutex_exit(data->graphMutex);
    }
}


//-----------------------------------------------------------------------------
// Return info about the size we're processing video at.
SVVIDEOLIB_API int get_proc_width(StreamData *data)
{
    return data->inputData2.width;
}

SVVIDEOLIB_API int get_proc_height(StreamData *data)
{
    return data->inputData2.height;
}

//-----------------------------------------------------------------------------
SVVIDEOLIB_API int is_running(StreamData* data)
{
    if (data) {
        return data->isRunning;
    }
    return 0;
}

//-----------------------------------------------------------------------------
// Returns a FrameData* for the most recently obtained frame, or NULL if there
// has been no new frame since the last call.
SVVIDEOLIB_API void* get_new_frame(StreamData* data, int isLive)
{
    frame_obj*   graphFrame = NULL;
    FrameData*   frameData;
    struct timeval readTime;
    int64_t ms, frameMs;

    if (!data || !data->isRunning)
        return NULL;

Retry:
    sv_mutex_enter(data->graphMutex);

    frame_api_t*  frameAPI  = NULL;
    stream_obj*   streamCtx = data->inputData2.streamCtx;
    stream_api_t* streamAPI = stream_get_api(streamCtx);

    if ( !data || !streamAPI || !streamCtx ) {
        log_err(data->logFn, "NULL in one of the major handles");
        sv_mutex_exit(data->graphMutex);
        data->isRunning = 0;
        return NULL;
    }
    int res = streamAPI->read_frame(streamCtx, &graphFrame);
    if ( res < 0 || graphFrame == NULL ) {
        log_err(data->logFn, "Failed to read a frame");
        sv_mutex_exit(data->graphMutex);
        data->isRunning = 0;
        return NULL;
    }
    sv_mutex_exit(data->graphMutex);

    frameAPI = frame_get_api(graphFrame);
    int nType = frameAPI->get_media_type(graphFrame);
    if ( nType != mediaVideo && nType != mediaVideoTime ) {
        // this level of code doesn't do anything with audio frames
        // log_info(data->logFn, "Ignoring frame with media type %d", frameAPI->get_media_type(myFrame));
        frame_unref(&graphFrame);
        goto Retry;
    }
    frameMs  = frameAPI->get_pts(graphFrame);
    if ( frameMs != INVALID_PTS ) {
        ms = frameMs;
        sv_time_ms_to_timeval(frameMs, &readTime );
    } else {
        gettimeofday(&readTime, NULL);
        ms = sv_time_timeval_to_ms(&readTime);
    }


    frameData = videolibutils_prepare_proc_frame( data, graphFrame );
    if ( !frameData ) {
        log_err(data->logFn, "Failed to obtain a frame for processing");
        frame_unref(&graphFrame);
        return NULL; // may as well give up here ...
    }


#if DEBUG_FRAME_FLOW
    if ( frameData ) {
        log_err(data->logFn, "Returning a frame for processing ptr=%p w=%d h=%d",
                        frameData->procBuffer,
                        frameData->procWidth,
                        frameData->procHeight );
    }
#endif

    if ( nType == mediaVideo ) {
        sv_mutex_enter(data->mutex);
        // remember the current frame, in case we're asked to return it as jpeg
        frame_unref(&data->lastFrameRead);
        data->lastFrameRead = graphFrame;
        frame_ref(data->lastFrameRead);
        sv_mutex_exit(data->mutex);
        if ( data->framesServed == 0 ) {
            log_dbg(data->logFn, "Time to first frame=%d, time to init=%d",
                        (int)(sv_time_get_current_epoch_time()-data->openStartTime),
                        (int)(data->openEndTime-data->openStartTime));
        }
        data->framesServed++;
    }

    return frameData;
}

//-----------------------------------------------------------------------------
static const char* _get_mmap_position(StreamData* data)
{
    // by default, we add after we've resized to 240p
    const char* sInsertAfter = "procResize";
    if (data->mmapHeight > PROC_IMAGE_HEIGHT || data->mmapHeight == 0) {
        // resize in RGB domain, if we can; otherwise we'll be force to run pixfmt conversion twice
        stream_obj* pipeline = data->inputData2.streamCtx;
        if ( stream_get_api(pipeline)->find_element(pipeline, "procPixfmt") != NULL ) {
            sInsertAfter = "procPixfmt";
        } else {
            sInsertAfter = "tc_edge";
        }

        if (data->mmapFps > data->inputData2.fps || data->inputData2.fps == 0 ) {
            // and then, only for large images a high fps will be allowed
            sInsertAfter = data->inputData2.auxInsertionPoint;
        }
    }
    return sInsertAfter;
}


//-----------------------------------------------------------------------------
// Open a memory map to use for viewing rgb frames.  On windows filename will
// be the name of the shared memory rather than an actual path.
// We store the name in the StreamData so that we can reopen the memory
// map for the large view. If mmapFilename is NULL, we are to use
// data->mmapFilename instead. mmapFilename will only be NULL when the mmap
// is being reopened at a different resolution. The only function that should
// be passing NULL to open_mmap is enable_large_frames().
SVVIDEOLIB_API int open_mmap(StreamData* data, char* mmapFilename)
{
    log_dbg(data->logFn, "open mmap");
    char before[2048], after[2048];
    if (mmapFilename == NULL) {
        return 1;
    }

    if ( data->mmapFilename != NULL &&
        !_stricmp(data->mmapFilename, mmapFilename)) {
        // mmap is active and doesn't need to be reopened ... make sure it isn't paused
        _pause_mmap(data, 0);
        return 1;
    }

    // make sure mmap element is gone
    _close_mmap(data);

    if ( mmapFilename != NULL ) {
        if ( data->mmapFilename ) {
            free(data->mmapFilename);
        }
        data->mmapFilename = strdup(mmapFilename);
    }

    stream_obj *subgraph=NULL, *splitter=NULL;

    stream_obj   *ctx = data->inputData2.streamCtx;
    stream_api_t *api = stream_get_api(ctx),
                 *s_api = get_splitter_api(),
                 *subgraph_api = get_default_stream_api();

    data->mmapPosition = _get_mmap_position(data);

    sv_mutex_enter(data->graphMutex);

    api->print_pipeline(data->inputData2.streamCtx, before, 2048);

    int res = -1;
    int removalNeeded = 0;
    int inserted;

    APPEND_FILTER(subgraph_api, subgraph, limiter_filter_api, "mmapFpsLimit");
    APPEND_FILTER(subgraph_api, subgraph, resize_factory_api, "resize");
    APPEND_FILTER(subgraph_api, subgraph, mmap_sink_api, "mmap");
    subgraph_api->set_log_cb(subgraph, (fn_stream_log)data->logFn);


    int kMmapPixFmt = GET_FRAME_PIX_FMT;
    if ( (data->mmapHeight > 0 && subgraph_api->set_param(subgraph, "resize.height", &data->mmapHeight) < 0) ||
            (data->mmapWidth > 0 && subgraph_api->set_param(subgraph, "resize.width",  &data->mmapWidth) < 0 ) ||
            subgraph_api->set_param(subgraph, "resize.pixfmt",  &kMmapPixFmt) < 0 ) {
        log_err(data->logFn, "Failed to configure mmap resize");
        goto Cleanup;
    }

    log_dbg(data->logFn, "Opening mmap: fps=%d, %dx%d", data->mmapFps, data->mmapWidth, data->mmapHeight);
    if ( subgraph_api->set_param(subgraph, "mmapFpsLimit.fps", &data->mmapFps) < 0 ) {
        log_err(data->logFn, "Failed to configure mmap fps");
        goto Cleanup;
    }

    if (subgraph_api->set_param(subgraph, "mmap.filename", data->mmapFilename) < 0 ) {
        log_err(data->logFn, "Failed to configure mmap sink");
        goto Cleanup;
    }



    splitter = s_api->create("mmapSplitter");
    stream_ref(splitter);

    if ( s_api->set_param(splitter, "mmapSplitter.subgraph", subgraph) < 0 ) {
        log_err(data->logFn, "Failed to configure mmap splitter");
        goto Cleanup;
    }

    inserted = api->insert_element(&data->inputData2.streamCtx,
                                &api,
                                data->mmapPosition,
                                splitter,
                                svFlagStreamInitialized | svFlagStreamOpen);
    if (  inserted < 0 ) {
        log_err(data->logFn, "Failed to enable mmap");
        // may have failed at initialization; clean it up just in case
        removalNeeded = 1;
        goto Cleanup;
    }
    res = 0;
    data->mmapPaused = 0;

Cleanup:
    if (removalNeeded) {
        int removed = api->remove_element(&data->inputData2.streamCtx,
                                    &api,
                                    "mmapSplitter",
                                    NULL);
        log_info(data->logFn, "Disabled mmap; removed=%d", removed);
    }
    if ( res == 0 ) {
        api->print_pipeline(data->inputData2.streamCtx, after, 2048);
        log_dbg(data->logFn, "pipeline (open_mmap): before=%s", before);
        log_dbg(data->logFn, "pipeline (open_mmap): after=%s", after);
    }

    // whether success or failure, local references can go
    stream_unref(&subgraph);
    stream_unref(&splitter);

    sv_mutex_exit(data->graphMutex);

    // mmap has reversed return values
    return res==0?1:0;
}

//-----------------------------------------------------------------------------
// Close an open memory map.
static void _close_mmap(StreamData* data)
{
    InputData2* input = &data->inputData2;
    char before[2048], after[2048];
    if ( input == NULL ||
         input->streamCtx == NULL ) {
        return;
    }

    sv_mutex_enter(data->graphMutex);
    stream_api_t* api = stream_get_api(input->streamCtx);
    api->print_pipeline(input->streamCtx, before, 2048);
    api->remove_element(&input->streamCtx, &api, "mmapSplitter", NULL);
    api->print_pipeline(input->streamCtx, after, 2048);
    sv_mutex_exit(data->graphMutex);

    if (data->mmapSubgraph) {
        stream_get_api(data->mmapSubgraph)->close(data->mmapSubgraph);
        stream_unref(&data->mmapSubgraph);
    }
    sv_freep(&data->mmapFilename);
    log_dbg(data->logFn, "pipeline (close_mmap): before=%s", before);
    log_dbg(data->logFn, "pipeline (close_mmap): after=%s", after);
}

//-----------------------------------------------------------------------------
static int _pause_mmap(StreamData* data, int pause)
{
    log_dbg(data->logFn, "pause mmap %d", pause);
    if ( data->mmapPaused == pause || data->mmapFilename == NULL ) {
        log_dbg(data->logFn, "pause mmap %d - already in this state", pause);
        return 0;
    }

    stream_obj   *ctx = data->inputData2.streamCtx;
    stream_api_t *api = stream_get_api(ctx);
    int res;

    if ( pause ) {
        if (data->mmapSubgraph != NULL) {
            log_err(data->logFn, "mmapSubgraph is not NULL when attempting to pause!");
            return -1;
        }
        res = api->remove_element(&ctx, NULL, "mmapSplitter", &data->mmapSubgraph );
        if (res <= 0) {
            log_err(data->logFn, "Failed to remove mmapSubgraph from the graph when pausing %d!", res);
            return -1;
        }
        data->mmapPosition = NULL;
        data->mmapPaused = 1;
    } else {
        if (data->mmapSubgraph == NULL) {
            log_err(data->logFn, "mmapSubgraph is NULL when attempting to unpause!");
            return -1;
        }

        data->mmapPosition = _get_mmap_position(data);
        res = api->insert_element(&ctx,
                            &api,
                            data->mmapPosition,
                            data->mmapSubgraph,
                            svFlagStreamInitialized | svFlagStreamOpen);
        if ( res < 0 ) {
            log_err(data->logFn, "Failed to insert mmapSubgraph when unpausing stream!");
            return -1;
        }
        data->mmapSubgraph = NULL; // TODO: should we unref?
        data->mmapPaused = 0;
    }

    log_dbg(data->logFn, "pause mmap - done %d %d", pause, res);
    return res >= 0 ? 1 : -1;
}

void close_mmap(StreamData* data)
{
    // set the mmap on low burner
    log_dbg(data->logFn, "close mmap");
    _pause_mmap(data, 1);
}


//-----------------------------------------------------------------------------
// Enable sharing of higher resolution frames through an open mmap.
SVVIDEOLIB_API void set_mmap_params(StreamData* data, int enableLargeView, int width, int height, int fps)
{
    int realWidth = width;
    int realHeight = height;
    if ( width > 0 && height > 0 ) {
        preserve_aspect_ratio( W_IN(data),
                H_IN(data),
                &realWidth,
                &realHeight,
                1 );
    }

    // This function sometimes gets called repeatedly. So just
    // check this value, and if it's already set, return.
    // If we didn't return, we risk reopening the mmap when we
    // don't need to.
    if (realWidth == data->mmapWidth &&
        realHeight == data->mmapHeight &&
        fps == data->mmapFps ) {
        return;
    }

    log_dbg(data->logFn, "changing mmap params: size=%dx%d fps=%d",
        realWidth,
        realHeight,
        fps);

    data->mmapWidth = realWidth;
    data->mmapHeight = realHeight;
    data->mmapFps = fps;


    // If a mmap is already open, we need to reopen it at the right resolution.
    if (data->mmapFilename != NULL) {
        // restart mmap, if that's a better approach or required by the settings
        const char* newMmapPosition = _get_mmap_position(data);
        if ( newMmapPosition != data->mmapPosition ) {
            // it's cheaper to reopen mmap, so it'll be positioned in a different location in the graph
            log_dbg(data->logFn, "Repositioning mmap ...");
            char* oldFilename = strdup(data->mmapFilename);
            _close_mmap(data);
            open_mmap(data, oldFilename);
            free(oldFilename);
            return;
        }

        // reconfigure mmap if needed
        size_t size = sizeof(int);
        int width, height, fps, forceUpdate = 0;
        stream_obj   *ctx = data->inputData2.streamCtx;
        stream_api_t *api = stream_get_api(ctx);

        if ( data->mmapSubgraph == NULL ) {
            // only attempt to query current mmap if it isn't paused ... if it is, we'll update its setting anyway
            if ( api->get_param(ctx, "mmapSplitter.subgraph.resize.width", &width, &size) < 0 ||
                api->get_param(ctx, "mmapSplitter.subgraph.resize.height", &height, &size) < 0 ||
                api->get_param(ctx, "mmapSplitter.subgraph.mmapFpsLimit.desiredFps", &fps, &size) < 0 ) {
                log_err(data->logFn, "Failed to query mmap filter for its current settings");
                return;
            }
        } else {
            forceUpdate = 1;
        }

        if ( forceUpdate || width != data->mmapWidth || height != data->mmapHeight || fps != data->mmapFps ) {
            int paused = _pause_mmap(data, 1);

            stream_obj* mmap = data->mmapSubgraph;
            stream_api_t* mmapAPI = stream_get_api(mmap);

            if ( forceUpdate || width != data->mmapWidth || height != data->mmapHeight ) {
                int newSize[] = { data->mmapWidth, data->mmapHeight };
                if ( mmapAPI->set_param(mmap, "mmapSplitter.subgraph.resize.updateSize", newSize) ) {
                    log_err(data->logFn, "Failed to update mmap size from %dx%d to %dx%d", width, height, data->mmapWidth, data->mmapHeight);
                } else {
                    log_dbg(data->logFn, "Updated mmap size from %dx%d to %dx%d", width, height, data->mmapWidth, data->mmapHeight);
                }
            }
            if ( forceUpdate || fps != data->mmapFps ) {
                if ( mmapAPI->set_param(mmap, "mmapSplitter.subgraph.mmapFpsLimit.fps", &data->mmapFps) < 0 ) {
                    log_err(data->logFn, "Failed to update mmap fps from %d to %d", fps, data->mmapFps);
                } else {
                    log_dbg(data->logFn, "Updated mmap fps from %d to %d", fps, data->mmapFps);
                }
            }

            if ( paused ) {
                _pause_mmap(data, 0);
            }
        }
    }
}

//-----------------------------------------------------------------------------
// Controls audio volume on the stream
SVVIDEOLIB_API void set_audio_volume(StreamData* data, int volume)
{
    if ( sv_transcode_audio() ) {
        _enable_audio_playback(&data->inputData2, NULL, "tc_edge", volume,
                            svFlagStreamInitialized);
    }
}

//-----------------------------------------------------------------------------
// Closes and frees all resources associated with a StreamData structure.
SVVIDEOLIB_API void free_stream_data(StreamData** dataPtr)
{
    if (!dataPtr)
        return;

    StreamData* data = *dataPtr;
    if (data) {
        _close_mmap(data);

        if (data->codecConfig)
            free_codec_config(&data->codecConfig);

        free_input_data(&data->inputData2);
        sv_mutex_destroy(&data->mutex);
        sv_mutex_destroy(&data->graphMutex);
        sv_freep(&data->mmapFilename);

        av_log_set_callback(av_log_default_callback);

        frame_unref(&data->lastFrameRead);

        sv_freep(&data->hlsProfiles);
        stream_set_default_log_cb(NULL);
    }

    av_freep(dataPtr);
}


//-----------------------------------------------------------------------------
SVVIDEOLIB_API void free_input_data(InputData2* dataPtr)
{
    if (dataPtr->streamCtx) {
        stream_get_api(dataPtr->streamCtx)->close(dataPtr->streamCtx);
    }
    stream_unref(&dataPtr->streamCtx);
}

//-----------------------------------------------------------------------------
// Look for Sighthound specific options in the URL.  If we find some,
// we will strip them from the filename. Returns 1 if present, 0 if not.
static int _find_and_strip_url_param(char* filename,
                                        const char* option,
                                        log_fn_t logFn)
{
    char* optionPtr;
    int   optionLen=strlen(option);

    // Search for optionkey.  If present, we'll strip it and return 1.
    //   http://1.2.3.4/image.cgi?type=motion&someoption
    //   http://1.2.3.4/image.cgi?someoption&type=motion
    optionPtr = strstr(filename, option);
    if ((optionPtr != NULL)                                     &&
        (optionPtr != filename)                                 &&
        ((optionPtr[-1] == '?') || (optionPtr[-1] == '&')) &&
        ((optionPtr[optionLen] == '=') || (optionPtr[optionLen] == '&') || (optionPtr[optionLen] == '\0')) ) {

        int retval = 1;
        if (optionPtr[optionLen] == '=' &&
            isdigit(optionPtr[optionLen+1])) {
            retval = atoi(&optionPtr[optionLen+1]);
        }

        // If we found it and it wasn't at the beginning of the filename
        // (shouldn't be, but good to make sure we don't clobber bad memory
        // with the [-1])...
        char* nextOpt;

        // Clear the option.  May need to copy the next option on top of
        // this one if it's there...
        nextOpt = strchr(optionPtr, '&');
        if (nextOpt != NULL) {
            // Yup, next option exists; keep our separator...
            memmove(optionPtr, nextOpt+1, strlen(nextOpt));
        } else {
            // No next option.  Clear, including our separator...
            optionPtr[-1] = '\0';
        }

        //log_info(logFn, "Found %s", option);
        //log_info(logFn, "Stripped filename: %s", inputData->filename);
        return retval;
    }
    return 0;
}

//-----------------------------------------------------------------------------
static int _preload_metadata( stream_obj** pCtx,
                                int isRegionMarkers,
                                int flags,
                                int numBoxes,
                                BoxOverlayInfo* boxes,
                                uint64_t fileStart,
                                const char* sFilterName,
                                log_fn_t logFn)
{
        INT64_T         currPts;
        int             boxesCount = 0;
        char*           buffer;
        char            color[16];
        char            name[128];
        int             x,y,w,h,t,procW,procH,nBox,uid;
        stream_api_t*   api;

        if ( numBoxes <= 0 ) {
            return 0;
        }

        api = stream_get_api(*pCtx);
        buffer = (char*)malloc(numBoxes*128);

        for ( nBox=0; nBox<numBoxes; nBox++ ) {
            // boxOverlay.append([frameTime, "drawbox=%d:%d:%d:%d:%s:t=%d" %
            //        (x1, y1, x2-x1, y2-y1, labelColor, lineSize)])
            if ( !isRegionMarkers &&
                boxesCount>0 &&
                currPts != (boxes[nBox].readTimeMs - fileStart) ) {
                // set param
                sprintf( name, "%s.metadata."I64FMT, sFilterName, currPts );
                api->set_param(*pCtx, name, buffer);
                boxesCount = 0;
            }

            if ( boxesCount == 0 ) {
                // with approx. 33ms between frames, we wouldn't want a bounding
                // box to linger more than 1.5 frames
                // interpolation algorithm may extend this value, if the object
                // appears in both frames being interpolated
                #define BBOX_DURATION "duration=50;"

                // starting a new set of boxes
                currPts = (boxes[nBox].readTimeMs - fileStart);
                if ( isRegionMarkers ) {
                    strcpy(buffer, "type=drawline;");
                } else {
                    strcpy(buffer, "type=boundingbox;");
                    if (flags&oifDebugClips)
                        strcat(buffer, "type=bboxid;");
                }
                strcat(buffer, BBOX_DURATION);
            }

            int nRead = sscanf(boxes[nBox].drawboxParams,
                 "drawbox=%d:%d:%d:%d:%d:%d:%d:%[a-z]:t=%d",
                 &x, &y, &w,
                 &h, &procW, &procH, &uid, color, &t);
            sprintf( &buffer[strlen(buffer)], "%d:%d:%d:%d:%d:%d:%d:%d:%s;", x, y, w, h, procW, procH, t, uid, color);
            boxesCount++;
        }

        if ( boxesCount > 0 ) {
            if ( isRegionMarkers ) {
                sprintf( name, "%s.metadata", sFilterName );
            } else {
                sprintf( name, "%s.metadata."I64FMT, sFilterName, currPts );
            }
            api->set_param(*pCtx, name, buffer);
        }

        sv_freep(&buffer);
        return 0;
}

//-----------------------------------------------------------------------------
static stream_api_t* _enable_bounding_boxes( stream_obj** pCtx,
                                int flags,
                                int numBoxes,
                                BoxOverlayInfo* boxes,
                                uint64_t fileStart,
                                const char* sInsertAfter,
                                int useFFMPEGForBoxes,
                                log_fn_t logFn)
{
    stream_api_t *api = *pCtx?stream_get_api(*pCtx):get_default_stream_api();
    stream_api_t *factory = useFFMPEGForBoxes
                                ? get_ff_filter_api()
                                : get_pixelate_filter_api();
    if ( numBoxes ) {
        api->insert_element(pCtx,
                            &api,
                            sInsertAfter,
                            get_metainject_filter_api()->create("bounding_injector"),
                            svFlagNone);
        api->insert_element(pCtx,
                            &api,
                            "bounding_injector",
                            factory->create("bounding"),
                            svFlagNone);
        api->set_param(*pCtx, "bounding.modifyInPlace", &_kOne);
        api->set_param(*pCtx, "bounding.filterType", "boundingbox");

        if ( flags & oifDebugClips ) {
            api->insert_element(pCtx,
                                &api,
                                "bounding",
                                get_ff_filter_api()->create("objID"),
                                svFlagNone);
            api->set_param(*pCtx, "objID.filterType", "bboxid");
        }
        if ( flags & oifShowRegions ) {
            api->set_param(*pCtx, "bounding.markCenter", &_kOne);
        }

        _preload_metadata( pCtx, 0, flags, numBoxes, boxes, fileStart,
                            "bounding_injector", logFn);
    }

    return api;
}

//-----------------------------------------------------------------------------
static stream_api_t* _enable_regions( stream_obj** pCtx,
                                int flags,
                                int numBoxes,
                                BoxOverlayInfo* boxes,
                                uint64_t fileStart,
                                const char* sInsertAfter,
                                log_fn_t logFn)
{
    stream_api_t *api = *pCtx?stream_get_api(*pCtx):get_default_stream_api();

    if ( numBoxes ) {
        api->insert_element(pCtx,
                            &api,
                            sInsertAfter,
                            get_pixelate_filter_api()->create("regions"),
                            svFlagNone);
        api->set_param(*pCtx, "regions.modifyInPlace", &_kOne);
        api->set_param(*pCtx, "regions.useLine", &_kOne);

        _preload_metadata( pCtx, 1, flags, numBoxes, boxes, fileStart,
                            "regions", logFn);
    }

    return api;
}

//-----------------------------------------------------------------------------
stream_api_t* _enable_timestamp( stream_obj** pCtx,
                                int flags,
                                int timestampFlags,
                                uint64_t offset,
                                const char* name,
                                const char* sInsertAfter,
                                log_fn_t logFn)
{
    stream_obj* ctx = *pCtx;
    stream_api_t *api = *pCtx?stream_get_api(*pCtx):get_default_stream_api();

    if ( !timestampFlags ) {
        return api;
    }

    static const char* USDate = "%b\\, %d %Y ";
    static const char* intlDate = "%d %b %Y ";
#ifdef _WIN32
    // Note use of dots instead of colon. The reason is insane ffmpeg escaping rules.
    // I've managed to find the magic incantation from console (which involves 5 backslashes before colons in pts expansion)
    // ./.install/x86_64-apple-darwin/3rdparty/bin/ffmpeg -loglevel trace -t 5 -i ~/native.mp4 -y -vf "drawtext=text='%{pts \: localtime \: 0 \: \\, %I\\\\\:%M\\\\\:%S%p}':x=0:y=0:fontsize=26:fontcolor=white@0.75:box=1:boxcolor=black@0.35:fontfile=/tmp/Inconsolata.otf" ~/out.mp4
    // however the same exact filter configuration doesn't work in code.
    static const char* USTime = "%I.%M.%S%p";
    static const char* militaryTime = "%H.%M.%S";
#else
    static const char* USTime = "%r";
    static const char* militaryTime = "%T";
#endif

    char    buffer[64] = "";
    int     bufferPos = 0;
    strcat(buffer, (timestampFlags&tsfUSDate) ? USDate : intlDate);
    strcat(buffer, (timestampFlags&tsf12HrTime) ? USTime : militaryTime);

    INSERT_FILTER(api, ctx, ff_filter_api, name, sInsertAfter );
    CONFIG_FILTER(ctx, name, logFn, Cleanup,
                "filterType", (flags&oifDebugClips)?"timer":"timestamp",
                "timestampOffset", &offset,
                "strftime", buffer,
                NULL);

Cleanup:
    *pCtx = ctx;
    return api;
}


//-----------------------------------------------------------------------------
static const char* _formatFileList(const char** filenames, char* buffer, int bufferSize)
{
    static const char* kDelim = ", ";
    int index = 0;
    while (filenames[index] != NULL) {
        const char* current = filenames[index];
        size_t len = strlen(current);
        int delimSize = index ? strlen(kDelim) : 0;
        int totalLen = len + delimSize;
        if ( bufferSize < totalLen + 1 ) {
            if (index > 0) {
                strcat(buffer, kDelim);
            }
            strcat(buffer, current);
            bufferSize -= totalLen;
            index ++;
        } else {
            if ( bufferSize >= 4 ) {
                strcat(buffer, "...");
            }
            break;
        }
    }
    return buffer;
}

//-----------------------------------------------------------------------------
// Create a single clip from a one or more video files
static uint64_t create_clip_base(int numFiles,
                const char** filenames,
                uint64_t* fileOffsetMs,
                uint64_t firstMs,
                uint64_t lastMs,
                const char* outfile,
                CodecConfig* codecConfig,
                int timestampFlags,
                int numBoxes,
                BoxOverlayInfo* boxes,
                const char* format,
                int fps,
                log_fn_t logFn,
                progress_fn_t progCb)
{
    int64_t       realFirstMs = -1;
    size_t        framesRead = 0, videoFramesRead = 0;
    frame_obj*    frame = NULL;
    int           isVideo;
    int           useDecoder;
    int           overTime = 0;
    int           canceled = 0;
    uint64_t      t = sv_time_get_current_epoch_time(), elapsed;
    char          buffer[256] = "\0";
    char          fileList[1024] = "\0";
    CodecConfig   defaultCodecConfigOriginal = { 0, 0, 0,  0, NULL,        0, 0, svvpOriginal };
    CodecConfig   defaultCodecConfigEncode =   { 0, 0, 50, 0, "ultrafast", 0, 0, svvpLow      };

    int           useHLSFlag = 0;
    int           dstCodecId = streamH264;
    int           seekFlags = sfBackward;

    if ( _gClipDebugEnabled < 0 ) {
        _gClipDebugEnabled = sv_get_int_env_var("SV_CLIP_DEBUG", 0);
    }

    if ( !_stricmp(format, "hls") ) {
        useHLSFlag = 2;
    } else
    if ( !_stricmp(format, "mjpeg") ) {
        dstCodecId = streamMJPEG;
    } else
    if ( !_stricmp(format, "jpg") ) {
        dstCodecId = streamJPG;
    } else
    if ( !_stricmp(format, "gif") ) {
        dstCodecId = streamGIF;
    };


    if ( codecConfig != NULL ) {
        useDecoder = (codecConfig->sv_profile != svvpOriginal);
    } else {
        useDecoder = (timestampFlags != 0 || numBoxes > 0 || dstCodecId != streamH264 );
        codecConfig = useDecoder ? &defaultCodecConfigEncode : &defaultCodecConfigOriginal;
    }

    stream_obj   *ctx = get_ffmpeg_demux_api()->create("demux");
    stream_api_t *api = stream_get_api(ctx);

    stream_ref(ctx);
    api->set_log_cb(ctx, (fn_stream_log)logFn);

    APPEND_FILTER(api, ctx, input_iterator_api, "inputIterator");
    CONFIG_FILTER(ctx, "inputIterator", logFn, create_clip_cleanup,
                "count", &numFiles,
                "urls", filenames,
                "offsets", fileOffsetMs,
                NULL);

    if ( useDecoder ) {
        // if we're applying filters, we'd want pixfmt of RGB
        // otherwise, we'd want it to be whatever came out of decoded H264
        int pixfmt = (timestampFlags || numBoxes>0) ? pfmtRGB24 : pfmtUndefined;
        APPEND_FILTER(api, ctx, ffdec_stream_api, "decoder");

        if ( fps > 0 ) {
            APPEND_FILTER(api, ctx, limiter_filter_api, "fpslimit");
            CONFIG_FILTER(ctx, "fpslimit", logFn, create_clip_cleanup,
                        "fps", &fps,
                        "useWallClock", &_kZero,
                        NULL);
        }

        api = _enable_timestamp(&ctx, 0, timestampFlags, 0, "ts", NULL, logFn );

        APPEND_FILTER(api, ctx, resize_factory_api, "resize");
        CONFIG_FILTER(ctx, "resize", logFn, create_clip_cleanup,
                    "width", &codecConfig->max_width,
                    "height", &codecConfig->max_height,
                    "pixfmt", &pixfmt,
                    "allowUpsize", &_kZero,
                    NULL);

        // if we need any boxes, we're likely to need RGB anyway
        int useFFMPEGForBoxes = 0;
        api = _enable_bounding_boxes( &ctx, 0, numBoxes, boxes, 0,
                            NULL, useFFMPEGForBoxes, logFn);

        APPEND_FILTER(api, ctx, ffenc_stream_api, "encoder");
        CONFIG_FILTER(ctx, "encoder", logFn, create_clip_cleanup,
                            "hls", &useHLSFlag,
                            "dstCodecId", &dstCodecId,
                            "encoderType", &_kMediaVideo,
                            "canUpdatePixfmt", &_kZero,
                            NULL);

        if ( codecConfig->gop_size > 0) {
            sprintf(&buffer[strlen(buffer)], " gop_size=%d", codecConfig->gop_size );
            api->set_param(ctx, "encoder.gop_size", &codecConfig->gop_size );
        }
        if ( codecConfig->keyint_min > 0) {
            int keyint = codecConfig->keyint_min;
            sprintf(&buffer[strlen(buffer)], " keyint_min=%d", keyint );
            api->set_param(ctx, "encoder.keyint_min", &keyint );
        }
        if ( codecConfig->max_bit_rate > 0) {
            sprintf(&buffer[strlen(buffer)], " bit_rate=%d", codecConfig->max_bit_rate );
            api->set_param(ctx, "encoder.max_bitrate", &codecConfig->max_bit_rate );
        }
        if ( codecConfig->bit_rate_multiplier > 0) {
            api->set_param(ctx, "encoder.bit_rate_multiplier", &codecConfig->bit_rate_multiplier );
        }
        if ( codecConfig->preset != NULL) {
            sprintf(&buffer[strlen(buffer)], " preset=%s", codecConfig->preset );
            api->set_param(ctx, "encoder.preset", codecConfig->preset );
        }
        if ( codecConfig->sv_profile != svvpNotSpecified ) {
            sprintf(&buffer[strlen(buffer)], " videoQualityPreset=%d", codecConfig->sv_profile );
            api->set_param(ctx, "encoder.videoQualityPreset", &codecConfig->sv_profile );
        }

        // make sure the decoder skips the frames leading to requested timestamp
        seekFlags |= sfPrecise;
    }

    APPEND_FILTER(api, ctx, ffsink_stream_api, "fileRecorder");
    CONFIG_FILTER(ctx, "fileRecorder", logFn, create_clip_cleanup,
                        "uri", outfile,
                        "hls", &useHLSFlag,
                        NULL );

    api->set_param(ctx, "liveStream", &_kZero);

    if (api->open_in(ctx) < 0) {
        log_err(logFn, "Failed to open clip at %s", _formatFileList(filenames, fileList, 1024) );
        goto create_clip_cleanup;
    }
    if ( api->seek(ctx, firstMs, seekFlags) < 0 ) {
        log_err(logFn, "Failed to seek to offset "I64FMT" in clip %s", firstMs, _formatFileList(filenames, fileList, 1024) );
        goto create_clip_cleanup;
    }

    int lastPct = 0, currentPct = 0;
    while (!overTime && !canceled) {
        if (api->read_frame(ctx, &frame) < 0) {
            log_err(logFn, "Failed to read from stream at frame %lu", framesRead );
            goto create_clip_cleanup;
        }
        frame_api_t* frameApi = frame_get_api(frame);
        assert ( frameApi != NULL );
        INT64_T frameMs = frameApi->get_pts(frame);
        isVideo = frameApi->get_media_type(frame)==mediaVideo;
        if ( videoFramesRead == 0 && isVideo ) {
            realFirstMs = frameMs;
        }
        if ( progCb != NULL ) {
            if ( frameMs < firstMs || lastMs == firstMs ) {
                // seek may cause a couple of frames prior to the requested ts to be returned
                currentPct = 0;
            } else {
                currentPct = (frameMs - firstMs)*100 / (lastMs - firstMs);
            }
            int progRes = progCb( currentPct );
            if ( progRes < 0 ) {
                log_dbg(logFn, "Clip creation canceled %d!", progRes);
                canceled = 1;
            }
            lastPct = currentPct;
        }
        overTime = (frameMs > 0 && frameMs >= lastMs && isVideo ) ? 1 : 0;
        framesRead++;
        if (isVideo) videoFramesRead++;
        frame_unref(&frame);
    }

    api->close(ctx);

create_clip_cleanup:
    frame_unref(&frame);
    stream_unref(&ctx);
    elapsed = sv_time_get_elapsed_time(t);
    log_info(logFn, "Clip %s complete, returning "I64FMT". firstMs="I64FMT", lastMs="I64FMT", format=%s. Written %lu (%lu) frames in " I64FMT "ms (%f fps) %s%s",
                        outfile, realFirstMs, firstMs, lastMs, format, framesRead, videoFramesRead, elapsed, videoFramesRead*1000/(float)elapsed, buffer[0]?"Params=":"", buffer);
    if ( canceled ) {
        remove( outfile );
    }
    return realFirstMs;
}

//-----------------------------------------------------------------------------
// Create a single clip from a one or more video files
SVVIDEOLIB_API uint64_t create_clip(int numFiles,
                const char** filenames,
                uint64_t* fileOffsetMs,
                uint64_t firstMs,
                uint64_t lastMs,
                const char* outfile,
                CodecConfig* codecConfig,
                int timestampFlags,
                int numBoxes,
                BoxOverlayInfo* boxes,
                const char* format,
                int fps,
                log_fn_t logFn,
                progress_fn_t progCb)
{
    int64_t realFirstMs = create_clip_base(numFiles,
                            filenames,
                            fileOffsetMs,
                            firstMs,
                            lastMs,
                            outfile,
                            codecConfig,
                            timestampFlags,
                            numBoxes,
                            boxes,
                            format,
                            fps,
                            logFn,
                            progCb );
    return realFirstMs>=0 ? 0 : -1;
}

//-----------------------------------------------------------------------------
// Create a video clip from all or part of one or more exixting video clips.
// Will return -1 on error, otherwise the actual ms offset of the first frame.
// This will likely be different than 'firstMs' as we must begin on key frames.
// This function assumes that codec settings are identical across all inputs.
SVVIDEOLIB_API uint64_t fast_create_clip(int numFiles, const char** filenames, uint64_t* fileOffsetMs,
                     uint64_t firstMs, uint64_t lastMs, const char* outfile,
                     const char* format, log_fn_t logFn, progress_fn_t progCb)
{
    uint64_t realFirstMs = create_clip_base(numFiles,
                            filenames,
                            fileOffsetMs,
                            firstMs,
                            lastMs,
                            outfile,
                            NULL,
                            0,
                            0,
                            NULL,
                            format,
                            0,
                            logFn,
                            progCb );
    return realFirstMs;
}


//-----------------------------------------------------------------------------
// Returns a count of the number of local cameras on the system, and
// populates the cache of device names for get_local_camera_name(),
// and supported resolutions.
SVVIDEOLIB_API int get_local_camera_count(log_fn_t logFn)
{
#if LOCAL_CAMERA_SUPPORT
    return lvlListDevices(logFn);
#else
    return 0;
#endif
}

//-----------------------------------------------------------------------------
// Returns a count of the number of local cameras on the system, and
// populates the cache of device names for get_local_camera_name().
int get_local_camera_count_without_resolution_cache(log_fn_t logFn)
{
#if LOCAL_CAMERA_SUPPORT
    return lvlListDevicesWithoutResolutionList(logFn);
#else
    return 0;
#endif
}

//-----------------------------------------------------------------------------
// Returns the name of a local camera, given its device ID.
// If the list of devices changes, you have to call get_local_camera_count()
// again first.
SVVIDEOLIB_API char* get_local_camera_name(int deviceId)
{
#if LOCAL_CAMERA_SUPPORT
    return lvlGetDeviceName(deviceId);
#else
    return "unsupported";
#endif
}

//-----------------------------------------------------------------------------
// Returns the number of supported resolutions of the given device. You must
// call get_local_camera_count() first and every time a device is added or removed.
SVVIDEOLIB_API int get_number_of_supported_resolutions_of_local_camera(int deviceId)
{
#if LOCAL_CAMERA_SUPPORT
    return lvlGetNumSupportedResolutionsOfDevice(deviceId);
#else
    return 0;
#endif
}

//-----------------------------------------------------------------------------
// Returns a single pair of dimensions (width and height) of the given device
// and dimension index.  You must call get_local_camera_count() first and every
// time a device is added or removed.
#if !LOCAL_CAMERA_SUPPORT
static Dimensions dummyDimens = { 100, 100 };
#endif

SVVIDEOLIB_API Dimensions* get_supported_resolution_pair_of_device(int deviceId, int resPair)
{
#if LOCAL_CAMERA_SUPPORT
    return lvlGetSupportedResolutionPairOfDevice(deviceId, resPair);
#else
    return &dummyDimens;
#endif
}


//-----------------------------------------------------------------------------
static CodecConfig* copy_codec_config(CodecConfig* config) {
    if (!config) {
        return NULL;
    }

    CodecConfig* copy = av_mallocz(sizeof(*copy));
    if (!copy)
        return NULL;
    memcpy(copy, config, sizeof(*copy));

    if (config->preset) {
        copy->preset = av_mallocz(strlen(config->preset)+1);
        if (!copy->preset) {
            free_codec_config(&copy);
            return NULL;
        }
        strcpy(copy->preset, config->preset);
    } else {
        copy->preset = NULL;
    }

    return copy;
}

//-----------------------------------------------------------------------------
static void free_codec_config(CodecConfig** config)
{
    if (!config)
        return;

    CodecConfig* c = *config;
    if (c) {
        if (c->preset)
            av_freep(&c->preset);
    }

    av_freep(config);
}

//-----------------------------------------------------------------------------
// TODO: we could make the JPEG grabbing/generation more efficient by keeping
//       context and saving the scaling and buffer instances, but our current
//       performance tests show that there seems to be no significant bottleneck
//       right now ...
SVVIDEOLIB_API void* get_newest_frame_as_jpeg(StreamData* data, int width, int height,
                               int* size) {
    int                sz, err;
    frame_obj*         lastFrameRead = NULL;
    frame_api_t*       frameApi;
    int                inputWidth;
    int                inputHeight;
    int                finished;
    void*              result = NULL;
    uint8_t*           resizedFrameBuffer = NULL;
    AVFrame*           frameCopy = NULL;
    AVFrame*           resizedFrame = NULL;
    AVCodecContext*    codecCtx = NULL;
    AVCodec*           codec;
    AVPacket           packet;
    struct SwsContext* swsCtx = NULL;
    int                inputPixFmt;

     // some sanity checks to avoid the worst
    if (width < 0 || height < 0 || !data) {
        goto get_newest_frame_as_jpeg_exit;
    }

    // grab a 1:1 copy of the latest frame as quickly and safely as possible,
    // so regular queue processing does not get paused for too long; this
    // function is usually called from a different thread (from the web server
    // or application respectively at the time of this writing) ...
    sv_mutex_enter(data->mutex);
    lastFrameRead = data->lastFrameRead;
    frame_ref(lastFrameRead);
    sv_mutex_exit(data->mutex);

    if (lastFrameRead==NULL) {
        log_err(data->logFn, "no frame available!");
        goto get_newest_frame_as_jpeg_exit;
    }

    // assumption: the input data (if available) never changes
    frameApi = frame_get_api(lastFrameRead);
    inputPixFmt = frameApi->get_pixel_format(lastFrameRead);
    inputWidth  = frameApi->get_width(lastFrameRead);
    inputHeight = frameApi->get_height(lastFrameRead);

    frameCopy = av_frame_alloc();
    if (!frameCopy) {
        log_err(data->logFn, "no frame allocated");
        goto get_newest_frame_as_jpeg_exit;
    }

    err = av_image_fill_arrays(frameCopy->data,
                    frameCopy->linesize,
                    frameApi->get_data(lastFrameRead),
                    svpfmt_to_ffpfmt(inputPixFmt, NULL),
                    inputWidth,
                    inputHeight,
                    _kDefAlign);
    frameCopy->width = inputWidth;
    frameCopy->height = inputHeight;
    frameCopy->format = svpfmt_to_ffpfmt(inputPixFmt, NULL);
    frameCopy->color_range = AVCOL_RANGE_JPEG;
    if (err < 0) {
        log_err(data->logFn, "cannot allocate frame copy buffer fmt=%d, w=%d, h=%d, err=%s", inputPixFmt, inputWidth, inputHeight, av_err2str(err) );
        goto get_newest_frame_as_jpeg_exit;
    }

    // resize the frame to the final size needed, and avoid ever scaling up;
    // also force a minimum size of 16x16 because at the time we got a crash in
    // sws_getCachedContext() on very small sizes (e.g. 3x2)
    width = width < 16 ? 16 : width;
    width = width > inputWidth ? inputWidth : width;
    height = height < 16 ? 16 : height;
    height = height > inputHeight ? inputHeight : height;
    preserve_aspect_ratio( inputWidth, inputHeight, &width, &height, 1);
    swsCtx = sws_getCachedContext(NULL,
        inputWidth, inputHeight, svpfmt_to_ffpfmt(inputPixFmt, NULL),
        width, height, AV_PIX_FMT_YUV420P, SWS_FAST_BILINEAR,
        0, NULL, NULL);
        // NOTE: AV_PIX_FMT_YUV420P instead of AV_PIX_FMT_YUVJ420P, as below, to avoid
        //       warnings saying "deprecated pixel format used, make sure you
        //       did set range correctly" flooding the log for no good reason
    if (!swsCtx) {
        log_err(data->logFn, "cannot get scaling context");
        goto get_newest_frame_as_jpeg_exit;
    }
    resizedFrame = av_frame_alloc();
    if (!resizedFrame) {
        log_err(data->logFn, "cannot allocate frame");
        goto get_newest_frame_as_jpeg_exit;
    }
    sz = av_image_get_buffer_size(AV_PIX_FMT_YUVJ420P, width, height + 1, _kDefAlign);
    resizedFrameBuffer = (uint8_t*)av_malloc(sz);
    if (!resizedFrameBuffer) {
        log_err(data->logFn, "cannot allocated resized frame buffer");
        goto get_newest_frame_as_jpeg_exit;
    }

    av_image_fill_arrays(resizedFrame->data,
                   resizedFrame->linesize,
                   resizedFrameBuffer,
                   AV_PIX_FMT_YUVJ420P,
                   width,
                   height,
                   _kDefAlign);
    sws_scale(swsCtx,
        (const uint8_t* const*)frameCopy->data,
        frameCopy->linesize,
        0,
        inputHeight,
        resizedFrame->data,
        resizedFrame->linesize);
    resizedFrame->width = width;
    resizedFrame->height = height;
    resizedFrame->format = AV_PIX_FMT_YUV420P;

    // do the JPEG compression by leveraging FFmpeg's MJPEG codec
    codec = avcodec_find_encoder(AV_CODEC_ID_MJPEG);
    if (!codec) {
        log_err(data->logFn, "cannot find encoder");
        goto get_newest_frame_as_jpeg_exit;
    }
    codecCtx = avcodec_alloc_context3(codec);
    if (!codecCtx) {
        log_err(data->logFn, "cannot allocated codec context");
        goto get_newest_frame_as_jpeg_exit;
    }
    codecCtx->bit_rate = 100*1000*1000; // dummy
    codecCtx->width = width;
    codecCtx->height = height;
    codecCtx->pix_fmt = AV_PIX_FMT_YUVJ420P;
    codecCtx->codec_id = AV_CODEC_ID_MJPEG;
    codecCtx->codec_type = AVMEDIA_TYPE_VIDEO;
    codecCtx->time_base.num = 1; // dummy
    codecCtx->time_base.den = 1; // dummy
    codecCtx->mb_lmin = codecCtx->qmin * FF_QP2LAMBDA;
    codecCtx->mb_lmax = codecCtx->qmax * FF_QP2LAMBDA;
    codecCtx->flags = AV_CODEC_FLAG_QSCALE;
    codecCtx->global_quality = codecCtx->qmin * FF_QP2LAMBDA;
    // TODO: don't let all of this quality setting fool you, as it turns out it
    //       does not change a thing - at this moment the FFMPEG MJPEG codec
    //       always uses the same quality (there seems to be a lossless option,
    //       but we don't use it) - ideally we'd use a more flexible encoder
    //       (e.g. libjepg) to support different quality settings and thus
    //       better adaption in different bandwidth scenarios...
    err = avcodec_open2(codecCtx, codec, NULL);
    if (err) {
        log_err(data->logFn, "cannot open codec (%d)", err);
        goto get_newest_frame_as_jpeg_exit;
    }
    resizedFrame->pts = 1;
    resizedFrame->quality = codecCtx->global_quality;


    av_init_packet(&packet);
    packet.data = NULL;
    packet.size = 0;
    err = avcodec_send_frame(codecCtx, resizedFrame);
    if (err) {
        goto get_newest_frame_as_jpeg_exit;
    }

    err = avcodec_receive_packet(codecCtx, &packet);
    if (err) {
        goto get_newest_frame_as_jpeg_exit;
    }

    // extract the data from the packet, no need for another copy action
    result = av_malloc(packet.size);
    if (!result) {
        goto get_newest_frame_as_jpeg_exit;
    }
    memcpy(result, packet.data, packet.size);
    *size = packet.size;

get_newest_frame_as_jpeg_exit:
    // cleanup, any "exception" also lands here...
    if (resizedFrame) {
        av_free(resizedFrameBuffer);
        av_frame_free(&resizedFrame);
    }
    sws_freeContext(swsCtx);
    if (codecCtx) {
        avcodec_close(codecCtx);
        avcodec_free_context(&codecCtx);
    }
    av_packet_unref(&packet);
    av_frame_free(&frameCopy);
    frame_unref(&lastFrameRead);
    return result;
}


//-----------------------------------------------------------------------------
SVVIDEOLIB_API void free_newest_frame(void* ptr)
{
    av_free(ptr);
}

//-----------------------------------------------------------------------------
SVVIDEOLIB_API int get_initial_frame_buffer_size(StreamData* data)
{
    int     value = 0;
    if (data) {
        stream_obj*       ctx = data->inputData2.streamCtx;
        stream_api_t*     api = stream_get_api(ctx);

        if ( api && ctx ) {
            size_t size = sizeof(int);
            if ( api->get_param(ctx,
                                "maxFrameSizeKb",
                                &value,
                                &size) < 0 ) {
                value = 0;
            }
        }
    }

    return value;
}



#define CLIP_INF(...) if ( _gClipDebugEnabled > 0 ) log_info (__VA_ARGS__)
#define CLIP_DBG(...) if ( _gClipDebugEnabled > 0 ) log_dbg (__VA_ARGS__)

//-----------------------------------------------------------------------------
// Attempts to open a clip to be read
SVVIDEOLIB_API ClipStream* open_clip(const char* filename, int width, int height,
                    uint64_t timestampOffset, int enableAudio, int enableThread,
                    int enableDebug, int timestampFlags, int keyframeOnly,
                    int numBoxes, BoxOverlayInfo* boxes,
                    int numRegions, BoxOverlayInfo* regions,
                    log_fn_t logFn)
{
    ClipStream* stream = (ClipStream*)malloc(sizeof(ClipStream));
    stream->lastMsReturned = (int64_t)-1;
    stream->logFn = logFn;
    stream->nextFrame = NULL;
    stream->filename = strdup(filename);
    stream->muted = (enableAudio ? 0 : -1);
    stream->outstandingFrames = 0;
    stream->freed = 0;
    stream->keyframeOnly = keyframeOnly;

    int flags = 0;
    if ( enableAudio && !keyframeOnly ) flags |= oifRenderAudio;
    if ( enableDebug ) flags |= oifDebugClips;
    if ( enableThread ) flags |= oifEdgeThread;
    if ( keyframeOnly) flags |= oifKeyframeOnly;

    CLIP_INF(logFn, "ClipUtils-%p: Opening %s, ts="I64FMT", audio=%s, boxes=%d, keyonly=%d", stream, filename, timestampOffset, ((flags&oifRenderAudio)?"on":"off"), numBoxes, keyframeOnly );

    if ( open_input(&stream->input,
                    filename,
                    NULL,
                    0,
                    0,
                    width,
                    height,
                    GET_FRAME_PIX_FMT,
                    0,
                    0,
                    flags,
                    timestampFlags,
                    numBoxes,
                    boxes,
                    numRegions,
                    regions,
                    timestampOffset,
                    NULL,
                    logFn ) < 0 ) {
        log_warn(logFn, "Failed to open %s", filename);
        free_clip_stream(&stream);
    } else {
        size_t size;

        stream_obj* ctx = stream->input.streamCtx;
        stream_api_t* api = stream->api = stream_get_api(ctx);

        size = sizeof(int);
        const char* failedQuery = NULL;
        if ( (stream->outWidth = (int)api->get_width(ctx)) < 0 ) {
            failedQuery = "output width";
        } else if ( (stream->outHeight = (int)api->get_height(ctx)) < 0 ) {
            failedQuery = "output height";
        } else if ( api->get_param(ctx, "demux.width", &stream->srcWidth,  &size) < 0 ) {
            failedQuery = "input width";
        } else if ( api->get_param(ctx, "demux.height",  &stream->srcHeight, &size) < 0 ) {
            failedQuery = "input height";
        }

        if ( failedQuery != NULL ) {
            log_err(stream->logFn, "failed to determine %s", failedQuery);
            free_clip_stream(&stream);
        }

        char buffer[2048];
        api->print_pipeline(ctx, buffer, 2047);
        log_dbg(stream->logFn, "Clip pipeline: %s", buffer);
    }

    return stream;
}

//-----------------------------------------------------------------------------
// Returns 1 if the clip has audio, and 0 otherwise
SVVIDEOLIB_API int clip_has_audio(ClipStream* stream)
{
    return stream->input.streamCtx != NULL && stream->input.hasAudio;
}

//-----------------------------------------------------------------------------
// Set the desired size for returned frames. 0 on success, -1 on error.
SVVIDEOLIB_API int set_output_size(ClipStream* stream, int width, int height)
{
    if (!stream || !stream->input.streamCtx ) {
        log_err(stream->logFn, "no stream is currently open");
        return -1;
    }


    stream_obj* ctx = stream->input.streamCtx;
    stream_api_t* api = stream->api;

    // If we were called with -1 in any dimension use the native value.
    if (width <= 0)
        width = stream->srcWidth;
    if (height <= 0)
        height = stream->srcHeight;

    videolibapi_preserve_aspect_ratio( stream->srcWidth, stream->srcHeight, &width, &height, 1 );

    if ( width == stream->outWidth && height == stream->outHeight ) {
        log_info(stream->logFn, "dimensions have not changed");
        return 0;
    }

    int newSize[] = { width, height };
    if (api->set_param(ctx, "procResize.updateSize", &newSize[0]) < 0 ) {
        log_err(stream->logFn, "failed to modify resize parameters");
        return -1;
    }

    stream->outWidth = api->get_width(ctx);
    stream->outHeight = api->get_height(ctx);

    return 0;
}


//-----------------------------------------------------------------------------
// Return the length of the clip in milliseconds, -1 on error
SVVIDEOLIB_API int64_t get_clip_length(ClipStream* stream)
{
    if (!stream || !stream->input.streamCtx ) {
        log_err(stream->logFn, "no stream is currently open");
        return (int64_t)-1;
    }

    stream_obj* ctx = stream->input.streamCtx;
    stream_api_t* api = stream->api;

    int64_t duration = 0;
    size_t  size = sizeof(duration);
    if (api->get_param(ctx, "demux.duration", &duration, &size) < 0 ) {
        log_err(stream->logFn, "failed to retrieve duration parameter");
        return -1;
    }

    CLIP_DBG(stream->logFn, "ClipUtils-%p: Duration of clip %s is "I64FMT, stream, stream->filename, duration);
    return duration;
}

//-----------------------------------------------------------------------------
static void free_clip_stream_internal(ClipStream** dataPtr)
{
    if (!dataPtr || !*dataPtr) {
        return;
    }

    ClipStream* data = *dataPtr;
    if (data->freed) {
        if (data->outstandingFrames==0) {
            CLIP_INF(data->logFn, "ClipUtils-%p: Closing clip stream for %s", data, data->filename);
            free_input_data(&data->input);
            sv_freep(&data->filename);
            free_clip_frame(&data->nextFrame);
            sv_freep(dataPtr);
        } else {
            CLIP_INF(data->logFn,
                    "ClipUtils-%p: Not yet closing clip stream for %s, %d frames are still out there",
                    data, data->filename, data->outstandingFrames);
        }
    }
}

//-----------------------------------------------------------------------------
SVVIDEOLIB_API void  free_clip_frame(ClipFrame** pFrame)
{
    if (pFrame && *pFrame) {
        ClipFrame* frame = *pFrame;
        ClipStream* stream = frame->stream;
        stream->outstandingFrames--;
        if ( stream->freed ) {
            CLIP_INF(stream->logFn, "ClipUtils: freeing frame %p, left %d", frame, stream->outstandingFrames);
        } else {
            CLIP_DBG(stream->logFn, "ClipUtils: freeing frame %p, left %d", frame, stream->outstandingFrames);
        }
        frame_unref(&frame->frame);
        sv_freep(pFrame);
        // attempt to free the stream, if this frame was the last reference to it
        free_clip_stream_internal(&stream);
    }
}

//-----------------------------------------------------------------------------
// Closes and frees all resources associated with a ClipStream structure.
SVVIDEOLIB_API void free_clip_stream(ClipStream** dataPtr)
{
    if (!dataPtr)
        return;

    ClipStream* data = *dataPtr;
    if (data) {
        // free any frames the stream itself may reference
        free_clip_frame(&data->nextFrame);
        // set the flag
        data->freed = 1;
        // free the stream if we can
        free_clip_stream_internal(dataPtr);
    }
}


//-----------------------------------------------------------------------------
static void* create_frame(ClipStream* stream,
                        frame_obj* newFrame)
{
    frame_api_t* api = frame_get_api(newFrame);
    ClipFrame* clipFrame = (ClipFrame*)malloc(sizeof(ClipFrame));
    clipFrame->frame = newFrame;
    clipFrame->ms = api->get_pts(newFrame);
    clipFrame->buffer = (uint8_t*)api->get_data(newFrame);
    clipFrame->stream = stream;
    stream->outstandingFrames++;
    CLIP_DBG(stream->logFn, "ClipUtils: created frame %p, total=%d", clipFrame, stream->outstandingFrames);
    return clipFrame;
}


//-----------------------------------------------------------------------------
// Retrieve the next frame from a file. Returns a ClipFrame*, NULL on error
// or when there are no more frames.
SVVIDEOLIB_API void* get_next_frame(ClipStream* stream)
{
    INT64_T startTime = sv_time_get_current_epoch_time();
    int retries = 0;

    if (!stream || !stream->input.streamCtx ) {
        log_err(stream->logFn, "no stream is currently open");
        return NULL;
    }

    ClipFrame* res = NULL;
    if ( stream->nextFrame ) {
        res = stream->nextFrame;
        stream->nextFrame = NULL;
    } else {
        stream_obj* ctx = stream->input.streamCtx;
        stream_api_t* api = stream->api;
        frame_obj* frame = NULL;

Retry:
        if ( api->read_frame(ctx, &frame) < 0 || frame == NULL) {
            res = NULL;
        } else
        if ( frame_get_api(frame)->get_media_type(frame) != mediaVideo ) {
            frame_unref(&frame);
            retries ++;
            goto Retry;
        } else {
            res = (ClipFrame*)create_frame(stream, frame);
            stream->lastMsReturned = res->ms;
        }
    }

    if (res) {
        CLIP_DBG(stream->logFn, "ClipUtils-%p: got next frame %p - ts="I64FMT" dur="I64FMT" retries=%d", stream, res, res->ms,
                        sv_time_get_current_epoch_time()-startTime, retries);
    } else {
        CLIP_INF(stream->logFn, "ClipUtils-%p: Failed to get next frame on %s - outstanding %d frames", stream, stream->filename, stream->outstandingFrames);
    }
    return res;
}

//-----------------------------------------------------------------------------
static void* get_frame_at_or_before(ClipStream* stream, INT64_T ms)
{
    int                 goBackMs = stream->keyframeOnly ? 500 : 10;
    int64_t             seekTo = 0;
    stream_obj*         ctx = stream->input.streamCtx;
    stream_api_t*       api = stream->api;
    ClipFrame*          prevFrame = NULL;
    ClipFrame*          curFrame = NULL;

    free_clip_frame(&stream->nextFrame);

    if ( ms < 0 ) {
        log_warn(stream->logFn, "Invalid frame offset requested: "I64FMT" in clip %s", ms, stream->filename);
        return NULL;
    }

    CLIP_DBG(stream->logFn, "ClipUtils-%p: Getting frame at ts="I64FMT, stream, ms);

    do {
        // seek backwards to a previous keyframe
        seekTo = ms > goBackMs ? ms - goBackMs : 0;

        if ( api->seek(ctx, seekTo, sfBackward|sfPrecise) < 0 ) {
            log_err(stream->logFn, "Failed to seek to "I64FMT" in clip %s", seekTo, stream->filename );
            return NULL;
        }

        int keepReading = 0;

        do {
            curFrame = (ClipFrame*)get_next_frame(stream);
            if ( curFrame == NULL ) {
                int64_t prevPts = ( prevFrame ? prevFrame->ms : -1 );
                CLIP_DBG(stream->logFn, "ClipUtils-%p: Couldn't read a frame ... prevPts="I64FMT" requested="I64FMT, stream, prevPts, ms);
                // failed to read the next frame
                if ( prevFrame == NULL ) {
                    // the requested offset is past the end of the stream, or past the last keyframe in keyframe-only mode
                    if ( seekTo == 0 ) {
                        return NULL;
                    }
                    goBackMs *= 2;
                } else if ( prevFrame->ms <= ms ) {
                    // last frame returned likely was the last
                    CLIP_DBG(stream->logFn, "ClipUtils-%p: Returning prev frame at ts="I64FMT, stream, prevFrame->ms);
                    return prevFrame;
                } else if ( seekTo > 0 ) {
                    // we could try and seek further
                    goBackMs *= 2;
                } else {
                    CLIP_DBG(stream->logFn, "ClipUtils-%p: prev=%p next=%p WTF IS THIS?!", stream, prevFrame, curFrame);
                    return NULL;
                }
            } else if ( curFrame->ms == ms ) {
                // bingo!
                CLIP_DBG(stream->logFn, "ClipUtils-%p: Returning cur frame at ts="I64FMT, stream, prevFrame?prevFrame->ms:-1);
                free_clip_frame(&prevFrame);
                return curFrame;
            } else if ( curFrame->ms < ms ) {
                // current frame is before the last read frame,
                // save it and read one more
                free_clip_frame(&prevFrame);
                prevFrame = curFrame;
                keepReading = 1;
            } else if ( prevFrame ) {
                // common termination case -- we have a previous frame,
                // and current is at or after the last frame returned
                CLIP_DBG(stream->logFn, "ClipUtils-%p: Returning prev frame at ts="I64FMT" cur at "I64FMT, stream, prevFrame->ms, curFrame->ms);
                stream->nextFrame = curFrame;
                return prevFrame;
            } else if ( seekTo > 0 ) {
                // need to seek further up
                goBackMs *= 2;
                free_clip_frame(&prevFrame);
                free_clip_frame(&curFrame);
            } else {
                CLIP_DBG(stream->logFn, "ClipUtils-%p: prev=%p next=%p WTF IS THIS 2?!", stream, prevFrame, curFrame);
                free_clip_frame(&curFrame);
                return NULL;
            }
        } while (keepReading);
    } while ( seekTo > 0 );

    CLIP_DBG(stream->logFn, "ClipUtils-%p: prev=%p next=%p WTF IS THIS 3?!", stream, prevFrame, curFrame);
    return NULL;

}

//-----------------------------------------------------------------------------
// Retrieve the prev frame from a file. Returns a ClipFrame*, NULL on error
// or when there are no more frames.
SVVIDEOLIB_API void* get_prev_frame(ClipStream* stream)
{
    if (!stream || !stream->input.streamCtx ) {
        log_err(stream->logFn, "no stream is currently open");
        return NULL;
    }

    CLIP_DBG(stream->logFn, "ClipUtils-%p: Getting previous frame before "I64FMT, stream, stream->lastMsReturned);

    if (stream->lastMsReturned == (int64_t)-1) {
        // get the first frame
        return get_next_frame(stream);
    }
    if (stream->lastMsReturned == 0) {
        // no such thing as prev frame
        return NULL;
    }

    ClipFrame* res = (ClipFrame*)get_frame_at_or_before(stream, stream->lastMsReturned-1);
    stream->lastMsReturned = res ? res->ms : -1;
    return res;
}


//-----------------------------------------------------------------------------
// Retrieve the ms offset of the next frame or -1 if no more frames exist.
SVVIDEOLIB_API int64_t get_next_frame_offset(ClipStream* stream)
{
    CLIP_DBG(stream->logFn, "ClipUtils-%p: Getting next frame offset after "I64FMT, stream, stream->lastMsReturned );
    if ( stream->nextFrame == NULL ) {
        stream->nextFrame = (ClipFrame*)get_next_frame(stream);
    }
    return ( stream->nextFrame == NULL ) ? -1 : stream->nextFrame->ms;
}


//-----------------------------------------------------------------------------
// Retrieve the frame closest to the given ms offset.  Returns a ClipFrame*,
// NULL on error.
SVVIDEOLIB_API void* get_frame_at(ClipStream* stream, int64_t ms)
{
    CLIP_DBG(stream->logFn, "Getting frame at "I64FMT, ms);
    ClipFrame* res = (ClipFrame*)get_frame_at_or_before(stream, ms);
    stream->lastMsReturned = res ? res->ms : -1;
    return res;
}


//-----------------------------------------------------------------------------
// Return an array of millisecond offsets for each frame in the clip.  The
// first entry in the array is the number of frames.  Returns NULL on error.
// This function rewinds the clip to the beginning.
SVVIDEOLIB_API int64_t* get_ms_list(ClipStream* stream)
{
    return get_ms_list2(stream->filename, stream->logFn);
}

SVVIDEOLIB_API int64_t* get_ms_list2(const char* filename, log_fn_t logFn)
{
    int64_t numFrames = 0;
    int64_t *msList = NULL;
    unsigned int msListSize = 0;
    static const int kMaxClipLenSec = 120;
    static const int kMaxFps = 30;
    static const int kListGrowDelta = 3600; // kMaxFps*kMaxClipLenSec;


    stream_api_t*   api = get_ffmpeg_demux_api();
    stream_obj*     ctx = api->create("demux");
    frame_obj*      frame = NULL;

    if ( ctx ) {
        stream_ref(ctx);

        api->set_log_cb(ctx, (fn_stream_log)logFn);
        api->set_param(ctx, "url", filename);
        api->set_param(ctx, "liveStream", &_kZero);
        if (api->open_in(ctx) >= 0) {
            while ( api->read_frame(ctx, &frame)>=0 && frame ) {
                frame_api_t* fapi = frame_get_api(frame);
                if (fapi->get_media_type(frame) == mediaVideo)  {
                    int64_t pts = fapi->get_pts(frame);
                    if ( numFrames+1 >= msListSize ) {
                        msListSize += kListGrowDelta;
                        msList = (int64_t*)realloc(msList, msListSize*sizeof(int64_t));
                    }
                    msList[numFrames+1] = pts;
                    numFrames++;
                }
                frame_unref(&frame);
            }
        }
        stream_unref(&ctx);
    }

    if ( msList ) {
        msList[0] = numFrames;
        log_dbg(logFn, "Created ms list for clip %s: pts[0]="I64FMT" pts[1]="I64FMT" pts[numFrames-1]="I64FMT" total="I64FMT,
                        filename,
                        msList[1],
                        msList[2],
                        msList[numFrames],
                        numFrames);
    } else {
        log_err(logFn, "Failed to retrieve ms list for %s", filename);
    }
    return msList;
}


//-----------------------------------------------------------------------------
SVVIDEOLIB_API int64_t get_duration(const char* filename, log_fn_t logFn)
{
    stream_api_t*   api = get_ffmpeg_demux_api();
    stream_obj*     ctx = api->create("demux");
    frame_obj*      frame = NULL;
    int64_t         duration = -1;
    size_t          size = sizeof(duration);

    if ( ctx ) {
        stream_ref(ctx);

        api->set_log_cb(ctx, (fn_stream_log)logFn);
        api->set_param(ctx, "url", filename);
        api->set_param(ctx, "liveStream", &_kZero);
        if (api->open_in(ctx) >= 0) {
            if (api->get_param(ctx, "demux.duration", &duration, &size) < 0 ) {
                duration = -1;
            }
        }
        stream_unref(&ctx);
    }

    if ( duration < 0 ) {
        log_err(logFn, "failed to retrieve duration parameter");
    }
    return duration;
}


//-----------------------------------------------------------------------------
// Frees the memory allocated by a call to get_ms_list.
SVVIDEOLIB_API void free_ms_list(int64_t** msListPtr)
{
    sv_freep(msListPtr);
}


//-----------------------------------------------------------------------------
// Return the height and with that retrieved frames will be in
SVVIDEOLIB_API int get_output_height(ClipStream* stream)
{
    return stream->outHeight;
}

//-----------------------------------------------------------------------------
int get_output_width(ClipStream* stream)
{
    return stream->outWidth;
}


//-----------------------------------------------------------------------------
// Return the height and with that the input file had
SVVIDEOLIB_API int get_input_height(ClipStream* stream)
{
    return stream->srcHeight;
}


//-----------------------------------------------------------------------------
SVVIDEOLIB_API int get_input_width(ClipStream* stream)
{
    return stream->srcWidth;
}


//-----------------------------------------------------------------------------
// mute/unmute audio playback
SVVIDEOLIB_API int set_mute_audio(ClipStream* stream, int mute)
{
    if ( stream->muted <0 ) {
        return 0;
    }
    if ( (stream->muted ^ mute) == 0 ) {
        return 0;
    }
    stream->muted = mute;
    if ( stream->input.streamCtx ) {
        if ( stream->api->set_param(stream->input.streamCtx, "audio_render.mute", &stream->muted) < 0 ) {
            log_err(stream->logFn, "Failed to mute/unmute audio");
            stream->muted = !stream->muted;
            return -1;
        }
    }
    return 0;
}

//-----------------------------------------------------------------------------
SVVIDEOLIB_API void videolib_set_path(int pathType, const char* value)
{
    sv_set_path(pathType, value);
}
