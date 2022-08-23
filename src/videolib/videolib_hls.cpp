/*****************************************************************************
 *
 * videolib_hls.cpp
 *   a set of utilities for controlling HLS stream creation
 *   in Sighthound Video pipelines (live or replay)
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

extern "C" {
#include "videolib.h"
};
#include "videolibUtils.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#ifndef _WIN32
#include <unistd.h>
#endif
#include <assert.h>
#include <ctype.h>




//-----------------------------------------------------------------------------
// See the following articles for background on choices:
// https://bitmovin.com/video-bitrate-streaming-hls-dash/
// https://developer.apple.com/library/content/technotes/tn2224/_index.html
static const HLSProfile _kHLSProfiles[] = {
    { 1, 145000,  10, 234,  300,  0, 0, 0, h264Baseline, 30, 0 },
    { 2, 365000,  10, 270,  360,  0, 0, 0, h264Baseline, 30, 0 },
    { 3, 730000,  12, 360,  480,  0, 0, 0, h264Baseline, 31, 0 },
    { 4, 2000000, 12, 540,  720,  0, 0, 0, h264Baseline, 32, 0 },
    { 5, 4500000, 15, 720,  960,  0, 0, 0, h264Baseline, 40, 0 },
    { 6, 7800000, 15, 1080, 1440, 0, 0, 0, h264Baseline, 41, 0 },
    { 0, 0,       0,  0,    0,    0, 0, 0, 0, 0, 0 }, // placeholder for remux
    { 0, 0,       0,  0,    0,    0, 0, 0, 0, 0, 0 }  // placeholder for termination
};
static const int _kProfilesCount = sizeof(_kHLSProfiles)/sizeof(HLSProfile);

static const int _kBufferDurationWhenRunning = 1000;
static const int _kBufferDurationWhenPaused  = 10000;
static const int _kOne=1;
static const int _kTwo=2;
static const int _kJumpstartFps=1;
static const int _kZero=0;
static const int _kMediaVideo=mediaVideo;
static const int _kMediaAudio=mediaAudio;
static const int _kCodecH264=streamH264;
static const int _kCodecAAC=streamAAC;
static const int _kCodecLinear=streamLinear;
static const int _kFPS10=10;
static const int _kDummyBitrate = 5000000;


//-----------------------------------------------------------------------------
SVVIDEOLIB_API
HLSProfile*     create_hls_profiles     ( stream_obj* ctx,
                                          int maxRes, int maxBitrate,
                                          int streamWidth, int streamHeight,
                                          int* profilesCountRes,
                                          log_fn_t logCb );


SVVIDEOLIB_API
int set_live_stream_limits(StreamData* data, int maxRes, int maxBitrate)
{
    if ( data == NULL ) {
        return -1;
    }

    data->hlsMaxBitrate = maxBitrate;
    data->hlsMaxResolution = maxRes;
    refresh_hls_profiles(data);
    return 0;
}

//-----------------------------------------------------------------------------
SVVIDEOLIB_API
int refresh_hls_profiles(StreamData* data)
{
    if ( data == NULL ) {
        return -1;
    }

    size_t          size = sizeof(int);
    int             w, h, profilesCount, codec;
    stream_obj*     ctx     = data->inputData2.streamCtx;
    stream_api_t*   api     = stream_get_api(ctx);
    HLSProfile*     res     = NULL;

    if ( api->get_param(ctx, "demux.width", &w, &size) < 0 ||
         api->get_param(ctx, "demux.height", &h, &size) < 0 ) {
        log_err(data->logFn, "Failed to determine stream parameters");
        return -1;
    }

    res = create_hls_profiles( ctx, data->hlsMaxResolution, data->hlsMaxBitrate,
                               w, h, &profilesCount, data->logFn);

    sv_freep(&data->hlsProfiles);
    data->hlsProfiles = res;
    data->hlsProfilesCount = profilesCount;

    log_info(data->logFn, "Generated %d HLS profiles", profilesCount);
    return 0;
}

//-----------------------------------------------------------------------------
SVVIDEOLIB_API
HLSProfile* create_hls_profiles(stream_obj* ctx,
                                int maxRes, int maxBitrate,
                                int streamWidth, int streamHeight,
                                int* profilesCountRes,
                                log_fn_t logCb)
{
    HLSProfile*     result = (HLSProfile*)malloc(_kProfilesCount*sizeof(HLSProfile));
    float           ratio  = streamWidth/(float)streamHeight;
    bool            is4_3  = (ratio <= 1.35);
    HLSProfile*     dst;

    size_t          size = sizeof(int);
    int             param, h264profile, h264level, h264bitrate;
    stream_api_t*   api = stream_get_api(ctx);

    int             profilesCount = 0;
    bool            useRemux = false;

    if ( api != NULL &&
         api->get_param(ctx, "demux.videoCodecId", &param, &size) >= 0 &&
         param == streamH264 &&
         api->get_param(ctx, "demux.h264Profile", &h264profile, &size) >= 0 &&
         api->get_param(ctx, "demux.h264Level", &h264level, &size) >= 0 ) {
        if ( (maxRes > 0 && streamHeight > maxRes)  ||
             (maxBitrate > 0 && h264bitrate > maxBitrate) ) {
            log_dbg(logCb, "Skipping remux profile due to user settings");
        } else {
            useRemux = true;
            if ( api->get_param(ctx, "demux.videoBitrate", &h264bitrate, &size) < 0 ||
                 h264bitrate == 0 ) {
                h264bitrate = _kDummyBitrate;
            }
        }
    }

    while ( profilesCount<_kProfilesCount ) {
        const HLSProfile* src = &_kHLSProfiles[profilesCount];
        int h = is4_3 ? src->height4_3 : src->height16_9;
        int bitrate = ( src->bitrate < h264bitrate || !useRemux )
                    ? src->bitrate
                    // don't go over remux bitrate --
                    // if we do, find a point between remux and last profile
                    : (result[profilesCount-1].bitrate/2 + h264bitrate/2);

        bool use = (h < streamHeight && h != 0);

        if ( (maxRes > 0 && h > maxRes) ||
             (maxBitrate > 0 && src->bitrate > maxBitrate) ) {
            use = false;
        }

        if ( !use ) {
            if (profilesCount==0 && !useRemux) {
                // not a single stream -- probably due to very low resolution of the source
                h = streamHeight;
                use = true;
            }
        }

        log_info(logCb, "%s profile %d of %d: h=%d, streamHeight=%d, profile=%d, level=%d",
                use?"Using":"Skipping", profilesCount, _kProfilesCount, h, streamHeight, src->h264profile, src->h264level);
        if ( !use ) {
            break;
        }
        dst = &result[profilesCount];
        *dst = *src;
        dst->bitrate = bitrate;
        dst->height = h;
        dst->width = ceil(h*ratio);
        dst->remux = 0;
        profilesCount++;
    }


    if ( useRemux ) {
        log_info(logCb, "Adding remux profile %d: streamHeight=%d, profile=%d, level=%d",
                    profilesCount, streamHeight, h264profile, h264level);

        dst = &result[profilesCount];
        dst->id = profilesCount + 1;
        dst->bitrate = h264bitrate;
        dst->fps = 0;
        dst->remux = 1;
        dst->height = streamHeight;
        dst->width = streamWidth;
        dst->h264profile = h264profile;
        dst->h264level = h264level;
        profilesCount++;
    }

    memset(&result[profilesCount], 0, sizeof(HLSProfile));
    *profilesCountRes = profilesCount;
    return result;
}

//-----------------------------------------------------------------------------
static int _get_h264_codec_string(char* buf, int profile, int level)
{
    const char* sProfile;

    switch (profile) {
    case h264Baseline:    sProfile = "42E0"; break;
    case h264Main:        sProfile = "4D40"; break;
    case h264High:        sProfile = "6400"; break;
    case h264Extended:    sProfile = "58A0"; break;
    default:              return -1;
    };

    switch (level) {
    case 30:
    case 31:
    case 32:
    case 40:
    case 41:
    case 42:
    case 50:
    case 51:             sprintf(buf, "%s%X", sProfile, level); break;
    default:             return -1;
    }

    return 0;
}

//-----------------------------------------------------------------------------
static int generate_master_hls(StreamData* data, const char* cpath)
{
    std::string name = cpath;
    std::string tmppath = _STR(cpath << "-" << sv_time_get_current_epoch_time() << ".tmp");

    size_t sep = name.find_last_of("\\/");
    if (sep != std::string::npos)
        name = name.substr(sep + 1, name.size() - sep - 1);

    size_t dot = name.find_last_of(".");
    if (dot != std::string::npos) {
        name = name.substr(0, dot);
    }

    int   hasAudio = data->inputData2.hasAudio;
    FILE* f = sv_open_file(tmppath.c_str(), "w+");
    if ( !f ) {
        log_err(data->logFn, "Failed to create master HLS file at %s", tmppath.c_str());
        return -1;
    }
    fprintf(f, "#EXTM3U\n");
    fprintf(f, "#EXT-X-VERSION:3\n");
    fprintf(f, "#EXT-X-INDEPENDENT-SEGMENTS\n");
    for (int nI=1; nI<=data->hlsProfilesCount; nI++) {
        HLSProfile* hls = &data->hlsProfiles[nI-1];
        char        buf[32] = "";
        if ( _get_h264_codec_string(buf, hls->h264profile, hls->h264level) < 0 ) {
            log_err(data->logFn, "Failed to generate H264 profile string for profile=%d level=%d", hls->h264profile, hls->h264level );
            continue;
        }
        fprintf(f, "#EXT-X-STREAM-INF:BANDWIDTH=%d,AVERAGE-BANDWIDTH=%d,RESOLUTION=%ux%u,CODECS=\"avc1.%s%s\"\n", hls->bitrate, hls->bitrate, hls->width, hls->height, buf, (hasAudio?",mp4a.40.2":"") );
        fprintf(f, "%s-%d.m3u8\n", name.c_str(), hls->id);
    }
    fclose(f);

    static const int _kMaxRetries = 5;
    int nRetries = 0;
    while ( nRetries++ < _kMaxRetries ) {
        if ( sv_rename_file(tmppath.c_str(), cpath) == 0 ) {
            return 0;
        }
        sv_sleep(10*nRetries);
    }

    log_err(data->logFn, "Failed to move HLS file from %s to %s", tmppath.c_str(), cpath);
    return -1;
}

//-----------------------------------------------------------------------------
static const char* _uniqf(char* buffer, const char* name, int id)
{
    sprintf(buffer, name, id);
    return buffer;
}

//-----------------------------------------------------------------------------
static
int enable_live_stream_base(StreamData* data, int profileId, const char* path,
                         int timestampFlags, int paused, int64_t startIndex )
{
    if ( data == NULL ) {
        return -1;
    }

    if ( profileId == 0 ) {
        return generate_master_hls(data, path);
    }

    if ( profileId > data->hlsProfilesCount || profileId < 1 ) {
        log_err(data->logFn, "Invalid profile ID %d - only %d HLS profiles exist",
                        profileId,
                        data->hlsProfilesCount );
        return -1;
    }


    int  res = -1;
    char hlsCompName[32], b[1024];
    sprintf(hlsCompName, "hls%d", profileId);

#define _U(name) _uniqf(b, name, profileId)

    sv_mutex_enter(data->graphMutex);

    stream_obj*     ctx     = data->inputData2.streamCtx;
    stream_api_t*   api     = stream_get_api(ctx);
    stream_obj*     hlsObj  = api->find_element(ctx, _U("hls%d"));
    stream_api_t*   hlsObjApi = stream_get_api(hlsObj);
    stream_obj*     subgraph = NULL;
    stream_api_t*   subgraphApi = get_default_stream_api();
    stream_obj*     recSubgraph = NULL;
    stream_api_t*   recSubgraphApi = get_default_stream_api();
    HLSProfile*     profile = &data->hlsProfiles[profileId-1];
    size_t          size = sizeof(stream_obj*);
    const char*     name;
    const char*     insertBefore;
    bool            hasDecoder = (api->find_element(ctx, "decoder") != NULL);
    bool            needsEncoder = (!profile->remux || !hasDecoder);
    int             fps =  paused ? _kJumpstartFps : profile->fps;

    if ( !hlsObj ) {
        if ( needsEncoder ) {
            name = _U("hls%dfpslimit");
            APPEND_FILTER(subgraphApi, subgraph, limiter_filter_api, name);
            CONFIG_FILTER(subgraph, name, data->logFn, Cleanup,
                         "variable", &_kOne,
                         "useWallClock", &_kZero,
                         "useSecondIntervals", &_kOne,
                         "fps", &fps,
                         NULL );
        }

        name = _U("hls%djitbuf");
        APPEND_FILTER(subgraphApi, subgraph, jitbuf_stream_api, name);
        CONFIG_FILTER(subgraph, name, data->logFn, Cleanup,
                        "bufferDuration", &_kBufferDurationWhenRunning,
                        "jumpstartWithPastFrames", &_kOne,
                        "jumpstartFps", &_kJumpstartFps,
                        "targetFps", &profile->fps,
                        "bufferDurationWhenPaused", &_kBufferDurationWhenPaused,
                        "paused", &paused,
                        NULL);

        name = _U("hls%drecordSubgraph");
        APPEND_FILTER(subgraphApi, subgraph, splitter_api, name);

        insertBefore = ( needsEncoder && hasDecoder ) ? "decoder" : "demux";

        name = _U("hls%d");
        INSERT_FILTER_F(api, data->inputData2.streamCtx, splitter_api, name,
                        insertBefore, svFlagStreamInitialized | svFlagStreamOpen)
        CONFIG_FILTER(data->inputData2.streamCtx, name, data->logFn, Cleanup,
                     "subgraph", subgraph,
                     NULL );

        hlsObj = api->find_element(data->inputData2.streamCtx, name);
        hlsObjApi = stream_get_api(hlsObj);
    } else {
        if ( needsEncoder ) {
            sprintf( b, "hls%d.subgraph.hls%dfpslimit", profileId, profileId );
            CONFIG_FILTER(hlsObj, b, data->logFn, Cleanup,
                         "fps", &fps,
                         NULL );
        }
    }


    if ( hlsObjApi->get_param(hlsObj, _U("subgraph.hls%drecordSubgraph.subgraph"), &recSubgraph, &size) < 0 ) {
        log_err(data->logFn, "Failed to query for recorder subgraph");
        goto Cleanup;
    }

    if ( recSubgraph != NULL ) {
        log_dbg(data->logFn, "Recorder subgraph already exists");
        recSubgraph = NULL; // prevents freeing the object in Cleanup
        res = 0;
        goto Cleanup;
    }

    if ( paused ) {
        log_dbg(data->logFn, "Keeping HLS stream %d in paused state", profileId);
        res = 0;
        goto Cleanup;
    }

    if ( needsEncoder ) {
        int pixfmt = pfmtRGB24;

        name = _U("hls%dresize");
        APPEND_FILTER(recSubgraphApi, recSubgraph, resize_factory_api, name);
        CONFIG_FILTER(recSubgraph, name, data->logFn, Cleanup,
                    "height", &profile->height,
                    "width", &profile->width,
                    "pixfmt", &pixfmt,
                    NULL);

        recSubgraphApi = _enable_timestamp(&recSubgraph, 0, timestampFlags, 0, _U("ts%d"), NULL, data->logFn);

        name = _U("hls%dencoder");
        APPEND_FILTER(recSubgraphApi, recSubgraph, ffenc_stream_api, name);
        CONFIG_FILTER(recSubgraph, name, data->logFn, Cleanup,
                    "hls", &_kOne,
                    "dstCodecId", &_kCodecH264,
                    "encoderType", &_kMediaVideo,
                    "canUpdatePixfmt", &_kZero, // unfortunately, the encoder cannot set pixfmt -- otherwise resize and timestamp may fail
                    "max_bitrate", &profile->bitrate,
                    "h264profile", &profile->h264profile,
                    "h264level", &profile->h264level,
                    NULL);

        // encoder may affect ordering of the A/V frames ... and iOS doesn't like it
        name = _U("hls%djitbufenc");
        APPEND_FILTER(recSubgraphApi, recSubgraph, jitbuf_stream_api, name);
        CONFIG_FILTER(recSubgraph, name, data->logFn, Cleanup, "bufferDuration", &_kZero, NULL);
    }

    name = _U("hls%drecorder");
    APPEND_FILTER(recSubgraphApi, recSubgraph, ffsink_stream_api, name);
    CONFIG_FILTER(recSubgraph, name, data->logFn, Cleanup,
                    "hls", &_kOne,
                    "uri", path,
                    "audioOn", &data->enableAudioRecording,
                    "hlsStartIndex", &startIndex,
                    NULL);

    if ( hlsObjApi->set_param(hlsObj, _U("subgraph.hls%drecordSubgraph.subgraph"), recSubgraph) < 0 ||
         hlsObjApi->set_param(hlsObj, _U("subgraph.hls%djitbuf.paused"), &_kZero ) ) {
        log_err(data->logFn, "Failed to start recording subgraph for HLS stream");
        goto Cleanup;
    }

    res = 0;

Cleanup:
    sv_mutex_exit(data->graphMutex);
    stream_unref(&subgraph);
    stream_unref(&recSubgraph);

    return res;
#undef _U
}

//-----------------------------------------------------------------------------
SVVIDEOLIB_API
int enable_live_stream(StreamData* data, int profileId, const char* path,
                         int timestampFlags, int64_t startIndex )
{
    return enable_live_stream_base(data, profileId, path, timestampFlags, 0, startIndex );
}


//-----------------------------------------------------------------------------
SVVIDEOLIB_API
int prepare_live_stream(StreamData* data, int profileId )
{
    return enable_live_stream_base(data, profileId, NULL, 0, 1, 0 );
}



//-----------------------------------------------------------------------------
SVVIDEOLIB_API
void disable_live_stream(StreamData* data, int profileId )
{
    char paramName[1024];

    if ( data == NULL ||
         profileId > data->hlsProfilesCount ||
         profileId < 1 )
        return;

    sv_mutex_enter(data->graphMutex);
    stream_obj*     ctx     = data->inputData2.streamCtx;
    stream_api_t*   api     = stream_get_api(ctx);

    sprintf(paramName, "hls%d.subgraph.hls%drecordSubgraph.subgraph", profileId, profileId);
    api->set_param(ctx, paramName, NULL);
    sprintf(paramName, "hls%d.subgraph.hls%djitbuf.paused", profileId, profileId );
    api->set_param(ctx, paramName, &_kOne );
    sprintf( paramName, "hls%d.subgraph.hls%dfpslimit.fps", profileId, profileId );
    api->set_param(ctx, paramName, &_kJumpstartFps );

    sv_mutex_exit(data->graphMutex);

}

