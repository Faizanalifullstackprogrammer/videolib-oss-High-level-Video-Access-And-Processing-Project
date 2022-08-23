/*****************************************************************************
 *
 * videolibUtils.cpp
 *   collection of random methods that didn't fit anywhere else
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
#include "videolibUtils.h"

extern "C" {
#include <libavutil/mem.h>
}

#if SIGHTHOUND_VIDEO

////////////////////////////////////////////////////////////////////////////////////////////////
SVVIDEOLIB_API
int  flush_output(StreamData* data)
{
    int res = 0;
    int paramInt = 1;
    log_dbg(data->logFn, "Request to flush output file");

    sv_mutex_enter(data->graphMutex);
    stream_obj*   ctx = data->inputData2.streamCtx;
    stream_api_t* api = stream_get_api(ctx);
    if ( api == NULL || api->set_param(ctx,
                        "fileRecorder.subgraph.recorder.newFile",
                        &paramInt) < 0 ) {
        log_err(data->logFn, "Request to flush output file failed");
        res = -1;
    }

    sv_mutex_exit(data->graphMutex);
    return res;
}


////////////////////////////////////////////////////////////////////////////////////////////////
static
FrameData*  videolibutils_prepare_proc_frame_base( const char* filename,
                                                int isRunning,
                                                frame_obj* srcFrame )
{
    FrameData*      result;
    frame_api_t*    frameAPI = frame_get_api(srcFrame);
    size_t          filenameSize = 0;
    int64_t         frameTs = frameAPI->get_pts(srcFrame);
    size_t          frameW = frameAPI->get_width(srcFrame);
    size_t          frameH = frameAPI->get_height(srcFrame);

    if ( filename != NULL ) {
        filenameSize = strlen(filename)+1;
    }

    result = (FrameData*)malloc(sizeof(FrameData)+filenameSize);

    if ( filenameSize ) {
        // Set the filename the frame was stored in
        result->filename = &result->storage[0];
        strcpy((char*)result->filename, filename);
        result->ms = frameTs;
    } else {
        result->ms = 0;
        result->filename = NULL;
    }

    result->frame      = srcFrame;
    result->procBuffer = (uint8_t*)frameAPI->get_data(srcFrame);
    result->procWidth  = frameW;
    result->procHeight = frameH;
    result->isRunning = isRunning;
    result->wasResized = 0;

    return result;
}

////////////////////////////////////////////////////////////////////////////////////////////////
extern "C"
FrameData*  videolibutils_prepare_proc_frame( StreamData* data,
                                              frame_obj* srcFrame )
{
    const char*     filename = NULL;
    size_t          size = sizeof(filename);

    if ( data->shouldRecord ) {
        stream_obj*   ctx = data->inputData2.streamCtx;
        stream_api_t* api = stream_get_api(ctx);
        if ( api->get_param(ctx, "fileRecorderSync.filename", &filename, &size) < 0 ||
             filename == NULL ) {
            log_err(data->logFn, "Failed to determine filename for frame");
            return NULL;
        }
    }

    FrameData* res = videolibutils_prepare_proc_frame_base(filename,
                                        data->isRunning, srcFrame);
    if (res) {
        res->wasResized = data->inputData2.hasResize;
    }
    return res;
}

////////////////////////////////////////////////////////////////////////////////////////////////
void            videolibapi_free_proc_frame( FrameData** dataPtr )
{
    if (!dataPtr)
        return;

    FrameData* data = *dataPtr;
    if (data) {
        if (data->frame) {
            frame_unref(&data->frame);
        }
        free(data);
    }
}


////////////////////////////////////////////////////////////////////////////////////////////////
// Get the frame prior to resize, or NULL if doesn't exist
extern "C"
FrameData* get_large_frame(FrameData* smallFrame)
{
    if (smallFrame == NULL) {
        return NULL;
    }
    frame_obj* srcFrameSmall = smallFrame->frame;
    frame_api* fapi = frame_get_api(srcFrameSmall);
    if ( fapi == NULL ) {
        return NULL;
    }
    frame_obj* srcFrameLarge = (frame_obj*)fapi->get_backing_obj(srcFrameSmall, "srcFrame");
    if ( srcFrameLarge == NULL ) {
        return NULL;
    }
    FrameData* res = videolibutils_prepare_proc_frame_base((const char*)smallFrame->filename,
                                        smallFrame->isRunning, srcFrameLarge);
    if ( res ) {
        res->wasResized = 0;
        // we've just taken an extra reference to the source frame
        frame_ref(srcFrameLarge);
    }
    return res;
}

////////////////////////////////////////////////////////////////////////////////////////////////
FrameData*  videolibutils_get_empty_proc_frame( )
{
    // If the stream is no longer being read, we need to return a
    // FrameData structure with isRunning = 0 to convey this.
    FrameData* frameData = (FrameData*)malloc(sizeof(FrameData));
    if (!frameData)
        return NULL;
    frameData->procBuffer = NULL;
    frameData->filename = NULL;
    frameData->frame = NULL;
    frameData->ms = 0;
    frameData->isRunning = 0;
    return frameData;
}
#endif // #if SIGHTHOUND_VIDEO

////////////////////////////////////////////////////////////////////////////////////////////////
SVVIDEOLIB_API
void videolibapi_preserve_aspect_ratio          ( int origWidth,
                                                  int origHeight,
                                                  int* targetWidth,
                                                  int* targetHeight,
                                                  int forceAlignment) {
    if (*targetWidth <= 0 || *targetHeight <= 0) {
        *targetWidth = origWidth;
        *targetHeight = origHeight;
        return;
    }

    float widthRatio = (float)origWidth/ *targetWidth;
    float heightRatio = (float)origHeight/ *targetHeight;

    if (widthRatio != heightRatio) {
        if (widthRatio > heightRatio)
            *targetHeight = (int)(origHeight/widthRatio + .5);
        else
            *targetWidth = (int)(origWidth/heightRatio + .5);
    }

    // Some codecs have trouble if dimensions are not divisible by 2, 4, 8,
    // even 16. Everything we're using now (11/19/2013) seems to be happy at 4.
    // This may skew things a bit, but a bit of skew is better than failing to
    // run, could alternatively crop, but that involves a filter somewhere.
    if (forceAlignment) {
        *targetWidth = *targetWidth & (~3);
        *targetHeight = *targetHeight & (~3);
    }
}


////////////////////////////////////////////////////////////////////////////////////////////////
static char kAnnexBHeader[] = { 0,0,0,1 };
static const size_t kAnnexBHeaderSize = sizeof(kAnnexBHeader);

////////////////////////////////////////////////////////////////////////////////////////////////
extern "C"
uint8_t*    videolibapi_spspps_to_extradata     ( uint8_t* sps,
                                                  int spsSize,
                                                  uint8_t* pps,
                                                  int ppsSize,
                                                  int ffmpegAlloc,
                                                  int* extradataSize )
{
    // extradata is based on externally provided sps/pps
    int bAnnexB = 1;
    size_t size = 0;
    const size_t headerSize = 11;

    if (!pps || !sps || !ppsSize || !spsSize) {
        *extradataSize = 0;
        return NULL;
    }

    if (ppsSize < 4 || memcmp(pps, kAnnexBHeader, kAnnexBHeaderSize) != 0 ||
        spsSize < 4 || memcmp(sps, kAnnexBHeader, kAnnexBHeaderSize) != 0 ) {
        // not AnnexB, need to write a header
        bAnnexB = 0;
    }
    size_t allocSize = ppsSize +
                        spsSize +
                        headerSize +
                        AV_INPUT_BUFFER_PADDING_SIZE;
    uint8_t* extradata = (uint8_t*)(ffmpegAlloc ? av_malloc(allocSize) : malloc(allocSize));
    if (!bAnnexB) {
        extradata[size++] = 1; /* version */
        extradata[size++] = sps[1]; /* profile */
        extradata[size++] = sps[2]; /* profile compat */
        extradata[size++] = sps[3]; /* level */
        extradata[size++] = 0xff; /* 6 bits reserved (111111) + 2 bits nal size length - 1 (11) */
        extradata[size++] = 0xe1; /* 3 bits reserved (111) + 5 bits number of sps (00001) */
        extradata[size++] = ((uint8_t*)&spsSize)[1];
        extradata[size++] = ((uint8_t*)&spsSize)[0];
    }
    if ( spsSize ) memcpy(extradata+size, sps, spsSize );
    size += spsSize;
    if (!bAnnexB) {
        extradata[size++] = 1; /* number of pps */
        extradata[size++] = ((uint8_t*)&ppsSize)[1];
        extradata[size++] = ((uint8_t*)&ppsSize)[0];
    }
    if ( ppsSize ) memcpy(extradata+size, pps, ppsSize );
    size += ppsSize;
    memset(extradata + size, 0, AV_INPUT_BUFFER_PADDING_SIZE);
    *extradataSize = size;
    return extradata;
}

////////////////////////////////////////////////////////////////////////////////////////////////
// Shamelessly borrowed from ffmpeg
#define SV_RB16(x)                              \
    ((((const uint8_t*)(x))[0] << 8) |          \
      ((const uint8_t*)(x))[1])

static
int ff_avc_write_annexb_extradata(const uint8_t *in, uint8_t **buf, int *size)
{
    uint16_t sps_size, pps_size;
    uint8_t *out;
    int out_size;

    *buf = NULL;
    if (*size < 11 || in[0] != 1)
        return -1;

    sps_size = SV_RB16(&in[6]);
    if (11 + sps_size > *size)
        return -1;
    pps_size = SV_RB16(&in[9 + sps_size]);
    if (11 + sps_size + pps_size > *size)
        return -1;
    out_size = 8 + sps_size + pps_size;
    out = (uint8_t*)malloc(out_size + AV_INPUT_BUFFER_PADDING_SIZE);
    if (!out)
        return -1;
    memcpy(&out[0], kAnnexBHeader, kAnnexBHeaderSize);
    memcpy(out + 4, &in[8], sps_size);
    memcpy(&out[4 + sps_size], kAnnexBHeader, kAnnexBHeaderSize);
    memcpy(out + 8 + sps_size, &in[11 + sps_size], pps_size);
    *buf = out;
    *size = out_size;
    return 0;
}

////////////////////////////////////////////////////////////////////////////////////////////////
extern "C"
int videolibapi_extradata_to_spspps(uint8_t *extradata,
                                int extradataSize,
                                uint8_t** sps,
                                size_t* spsSize,
                                uint8_t** pps,
                                size_t* ppsSize )
{
    int res;
    uint8_t *data = extradata;
    int dataSize = extradataSize;

    if ( extradata[0] == 1 ) {
        if ( ff_avc_write_annexb_extradata(extradata, &data, &dataSize) )
            return -1;
    }


    if ( videolibapi_extract_nalu(data, dataSize, kNALPPS, pps, ppsSize, NULL, NULL ) < 0 )
        res = -1;
    else
    if ( videolibapi_extract_nalu(data, dataSize, kNALSPS, sps, spsSize, NULL, NULL ) < 0 )
        res = -1;
    else
        res = 0;
    if ( data != extradata )
        sv_freep(&data);
    if ( res < 0 ) {
        sv_freep(sps);
        sv_freep(pps);
        *spsSize = *ppsSize = 0;
    }
    return res;
}

////////////////////////////////////////////////////////////////////////////////////////////////
extern "C"
int         videolibapi_get_ffmpeg_rotation     ( AVFormatContext* f, int stream)
{
    AVDictionary*   meta = NULL;
    int             result = 0;

    meta = f->streams[stream]->metadata;
    if ( meta != NULL ) {
        AVDictionaryEntry *t = NULL;
        t = av_dict_get( meta, "rotate", NULL, AV_DICT_IGNORE_SUFFIX);
        if ( t && t->value ) {
            int rotation = 0;
            int res = sscanf(t->value, "%d", &rotation);
            if ( res == 1 &&
                 (rotation==90 || rotation==-90 || abs(rotation)==180) ) {
//                printf("Detected rotation of %d degrees\n", rotation);
                result = rotation;
//            } else {
//                printf("Error parsing rotation: res=%d rotation=%d\n", res, rotation);
            }
        } else {
//            printf("av_dict_get: no 'rotation' entry\n");
        }
    } else {
//        printf("No metadata on video stream\n");
    }
    return result;
}
