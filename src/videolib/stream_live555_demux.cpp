/*****************************************************************************
 *
 * stream_live555_demux.cpp
 *   Source node based on live555 library. Used in all RTSP scenarios.
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
#define SV_MODULE_NAME "live555"
#define SV_MODULE_ID "live555"
#include "sv_module_def.hpp"
#include "streamFactories.h"
#include "nalu.h"

#include <cmath>
#include <list>
#include <memory>

using std::string;

#define LIVE555_DEMUX_MAGIC 0x1313


#define RTSP_CLIENT_VERBOSITY_LEVEL     1 // by default, print verbose output from each "RTSPClient"
#define THREAD_ID sv_get_thread_id() << ": "

#define TRACE_CPP_L(l,y) if (_gTraceLevel>l) { logCb(logTrace,y); }
#define TRACE_CPP(y) TRACE_CPP_L(0,y)

static int _g_StreamID = 0;

#define M_STR(subsession) (const char*)subsession->mediumName() << "/" << (const char*)subsession->codecName()
#define S_ID_STR(buf)        "[URL:\"" << sv_sanitize_uri((const char*)url(), buf, sizeof(buf)) << "\"]"

#include "streamprv.h"
#include "frame_basic.h"
#include "nalu.h"

#include <svlive555.h>



// TODO: MOVEME
static const int kDefaultBufferSizeKb = 2*1024;
static const int kDefaultSocketBufferSizeKb = 3*kDefaultBufferSizeKb;
static const int kDefaultPacketTimeout = 5000;
static const int kDefaultInitTimeout = 10000;
static const int FF_INPUT_BUFFER_PADDING_SIZE = 64; // ffmpeg uses 32 .. we don't want to use their headers here, yet would like to produce output ready to be input to ffmpeg

extern sio::live555::ITimestampCreator* _CreateTimestampCreator(fn_stream_log logCb, const char* id);





//-----------------------------------------------------------------------------
typedef struct live555_packet_producer  : public stream_base  {
    sio::live555::DemuxRTSPClientImpl*             clientSession;
    sio::live555::MyDemuxBasicUsageEnvironment*    envir;
    sio::live555::ILive555APIBridge*               apiBridge;

    int                 forceTCP;

    char*               descriptor;     // provided externally

    int                 bufferSizeKb;
    int                 socketBufferSizeKb;
    size_t              width;
    size_t              height;
    int                 pixfmt;

    int                 compressedFrameSize;
    int                 statsIntervalSec;
    int                 disableGetParameter;
    int                 aggregateNALU;

    int                 poolFrames;

    basic_frame_obj*    firstFrame;

    int                 initTimeout;
    int                 packetTimeout;
} live555_packet_producer_t;

//-----------------------------------------------------------------------------
static int TranslateCodec(int l555codec)
{
    switch (l555codec) {
        case sio::live555::Codec::aac:   return streamAAC;
        case sio::live555::Codec::pcma:  return streamPCMA;
        case sio::live555::Codec::pcmu:  return streamPCMU;
        case sio::live555::Codec::h264:  return streamH264;
        case sio::live555::Codec::mp4:   return streamMP4;
        case sio::live555::Codec::mjpeg: return streamMJPEG;
        case sio::live555::Codec::unknown:
        default: return streamUnknown;
    }
}

//-----------------------------------------------------------------------------
class FrameBufferImpl : public sio::live555::FrameBuffer
{
    basic_frame_obj*    mFrameObj = nullptr;
    int                 mCodec = sio::live555::Codec::unknown;
    int                 mMediaType = sio::live555::MediaType::unknown;
    fn_stream_log       mLogCb = nullptr;
    int                 mNALU = 0;
    int                 mChunks = 1;

public:
    FrameBufferImpl( fn_stream_log cb, frame_allocator* fa, size_t frameSize )
        : mLogCb ( cb )
    {
        mFrameObj = alloc_basic_frame2(LIVE555_DEMUX_MAGIC, frameSize, mLogCb, fa);
        if ( mFrameObj == NULL ) {
            mLogCb(logError, _FMT("Failed to allocate new frame of " << frameSize << " bytes"));
            assert( false );
        }
    }

    ~FrameBufferImpl()
    {
        frame_unref((frame_obj**)&mFrameObj);
    }

    virtual int         GetCodec() const { return mCodec; }
    virtual void        SetCodec(int codec) { mCodec = codec; }
    virtual int         GetMediaType() const { return mMediaType; }
    virtual void        SetMediaType(int mediaType) { mMediaType = mediaType; }
    virtual bool        Merge(sio::live555::FrameBuffer* other)
    {
        FrameBufferImpl* fi = (FrameBufferImpl*)other;
        bool thisHasTimedData = CONTAINS_TIMED_DATA(mNALU);
        bool otherHasTimedData = CONTAINS_TIMED_DATA(fi->mNALU);

        if ( mFrameObj->pts != fi->mFrameObj->pts ) {
            if ( thisHasTimedData ) {
                // Merge with a different PTS isn't allowed, unless the frame doesn't contain video data (but only SPS/PPS)
                return false;
            }
            //mLogCb(logInfo, _FMT("Allowing the merge despite diff PTS: nalu=" << mNALU << " other=" << fi->mNALU));
        }

        if ( !fi ||
            fi->mCodec != mCodec ||
            fi->mCodec != mCodec ||
            fi->mMediaType != mMediaType ||
            !fi->mFrameObj ||
            !mFrameObj ) {
            // unexpected state, cant merge it
            return false;
        }

        // video frames come padded already, so overshoot to include the padding, and then adjust the size
        if ( append_basic_frame(mFrameObj, fi->mFrameObj->data, fi->mFrameObj->dataSize+FF_INPUT_BUFFER_PADDING_SIZE) < 0 ) {
            mLogCb(logError, _FMT("Failed to merge frames!"));
            return false;
        }


        mFrameObj->dataSize -= FF_INPUT_BUFFER_PADDING_SIZE;
        mChunks++;


        if ( thisHasTimedData && otherHasTimedData ) {
            // I don't think this is normal ... need to collect more data
            mLogCb(logWarning, _FMT("Merged frames " << mFrameObj->pts << "/" << mNALU << " " << fi->mFrameObj->pts << "/" << fi->mNALU << " combined size " << mFrameObj->dataSize << " total " << mChunks << " chunks"));
        } else {
            mLogCb(logTrace, _FMT("Merged frames " << mFrameObj->pts << "/" << mNALU << " " << fi->mFrameObj->pts << "/" << fi->mNALU << " combined size " << mFrameObj->dataSize ));
        }

        // Both frame's NALU's will now be contained within
        mNALU |= fi->mNALU;

        // this object is no longer needed
        frame_unref((frame_obj**)&fi->mFrameObj);
        return true;

    }

    virtual void        Shrink()
    {
        static const int kReallocFactor = 10;
        if ( mFrameObj->allocSize > mFrameObj->dataSize*kReallocFactor ) {
            basic_frame_obj* res = clone_basic_frame(mFrameObj, mLogCb, NULL);
            // This releases the frame into the pool ... it'll be ready for reuse when we need it
            frame_unref((frame_obj**)&mFrameObj);
            mFrameObj = res;
        }
    }

    virtual void        Release()
    {
        delete this;
    }

    virtual uint64_t    Pts() const { return mFrameObj->pts; }
    virtual void        SetPts(uint64_t ts) { mFrameObj->pts = ts; }
    virtual uint64_t    DataSize() const { return mFrameObj->dataSize; }
    virtual uint64_t    AllocSize() const { return mFrameObj->allocSize; }
    virtual bool        Append(uint8_t* data, size_t size, bool updateSize=true)
    {
        int prevSize = mFrameObj->allocSize;
        if  (append_basic_frame(mFrameObj, data, size) < 0 ) {
            mLogCb(logError, _FMT("Failed to allocate additional " << size <<
                                " bytes for the frame. Currently allocated " <<
                                prevSize));
            return false;
        }
        if (!updateSize) {
            mFrameObj->dataSize -= size;
        }
        return true;
    }
    virtual uint64_t    FreeSpace() const { return mFrameObj->allocSize - mFrameObj->dataSize; }
    virtual bool        EnsureFreeSpace(uint64_t additonalRequired)
    {
        int prevSize = mFrameObj->allocSize;
        if ( ensure_basic_frame_free_space(mFrameObj, additonalRequired) < 0 ) {
            mLogCb(logError, _FMT("Failed to grow the frame: prevSize=" << prevSize << " dataSize=" << additonalRequired));
            return false;
        }
        return true;
    }
    virtual uint8_t*    WritePtr() { return &mFrameObj->data[mFrameObj->dataSize]; }
    virtual uint8_t*    ReadPtr() { return mFrameObj->data; }
    virtual void        OnDataWritten(size_t size) { mFrameObj->dataSize += size; }
    virtual int         GetContainedNALU() const { return mNALU; }
    virtual bool        ParseNALU()
    {
        mNALU = videolibapi_contained_nalu( ReadPtr(), DataSize(), mLogCb );
        return true;
    }
    virtual bool        IsKeyframe() const
    {
        static const int mask = (1<<(kNALIFrame-1));
        return (mNALU & mask)!=0;
    }
    virtual bool        ContainsTimedData() const
    {
        return CONTAINS_TIMED_DATA(mNALU)!=0;
    }

public:
    basic_frame_obj*    GetFrameAndRelease()
    {
        basic_frame_obj* f = mFrameObj;
        switch (GetMediaType()) {
        case sio::live555::MediaType::video:
            f->mediaType = mediaVideo;
            f->keyframe = IsKeyframe();
            break;
        case sio::live555::MediaType::audio:
            f->mediaType = mediaAudio;
            f->keyframe = 1;
            break;
        default:
            f->mediaType = mediaUnknown;
            break;
        }
        frame_ref((frame_obj*)f);
        Release();
        return f;
    }
};

//-----------------------------------------------------------------------------
class Live555APIBridge : public sio::live555::ILive555APIBridge
{
public:
    Live555APIBridge(fn_stream_log logCb, int poolFrames)
        : mLogCb( logCb )
        , m_faVideo ( nullptr )
        , m_faAudio( nullptr )
    {
        if ( poolFrames ) {
            m_faVideo = create_frame_allocator("fa_live555video");
            m_faAudio = create_frame_allocator("fa_live555audio");
        }
    }

    ~Live555APIBridge()
    {
        destroy_frame_allocator(&m_faVideo, mLogCb);
        destroy_frame_allocator(&m_faAudio, mLogCb);
    }

public: // factories
    virtual sio::live555::ITimestampCreator* createTimestampCreator(const char* name) override { return _CreateTimestampCreator( mLogCb, name ); }
    virtual sio::live555::FrameBuffer*       createFrameBuffer(size_t size, bool isVideo) override { return new FrameBufferImpl(mLogCb, isVideo ? m_faVideo : m_faAudio, size); }
    virtual bool               audioTranscodingEnabled() override { return sv_transcode_audio(); }
    virtual const char*        sanitizeUrl(const char* str, char* buffer, size_t sizeOfBuffer) override { return sv_sanitize_uri(str, buffer, sizeOfBuffer); }
    virtual void               release() override { delete this; }

public: // logging
    virtual void logError(const char* msg) override { mLogCb(::logError, msg); }
    virtual void logWarning(const char* msg) override { mLogCb(::logWarning, msg); }
    virtual void logInfo(const char* msg) override { mLogCb(::logInfo, msg); }
    virtual void logDebug(const char* msg) override { mLogCb(::logDebug, msg); }
    virtual void logTrace(const char* msg) override { mLogCb(::logTrace, msg); }

private:
    frame_allocator*                m_faVideo;
    frame_allocator*                m_faAudio;
    fn_stream_log                   mLogCb;
};




//-----------------------------------------------------------------------------
// Stream API
//-----------------------------------------------------------------------------
#define DECLARE_DEMUX_LVL(param, name) \
    DECLARE_OBJ(live555_packet_producer_t, name,  param, LIVE555_DEMUX_MAGIC, -1)
#define DECLARE_DEMUX_LVL_V(param, name) \
    DECLARE_OBJ_V(live555_packet_producer_t, name,  param, LIVE555_DEMUX_MAGIC)

//-----------------------------------------------------------------------------
//  Forward declarations
//-----------------------------------------------------------------------------
static stream_obj* live555_stream_create             (const char* name);
static int         live555_stream_set_param          (stream_obj* stream,
                                                        const CHAR_T* name,
                                                        const void* value);
static int         live555_stream_get_param          (stream_obj* stream,
                                                        const CHAR_T* name,
                                                        void* value,
                                                        size_t* size);
static int         live555_stream_open_in            (stream_obj* stream);
static size_t      live555_stream_get_width          (stream_obj* stream);
static size_t      live555_stream_get_height         (stream_obj* stream);
static int         live555_stream_get_pixel_format   (stream_obj* stream);
static int         live555_stream_read_frame         (stream_obj* stream, frame_obj** frame);
static int         live555_stream_close              (stream_obj* stream);
static void        live555_stream_destroy            (stream_obj* stream);


//-----------------------------------------------------------------------------
static void        _set_module_trace_level_live555  (int nLevel)
{
    _gTraceLevel = nLevel;
    sio::live555::MyDemuxBasicUsageEnvironment::setTraceLevel(_gTraceLevel);
}

//-----------------------------------------------------------------------------
static stream_api_t _g_live555_stream_provider = {
    live555_stream_create,
    NULL, // set_source
    get_default_stream_api()->set_log_cb,
    get_default_stream_api()->get_name,
    get_default_stream_api()->find_element,
    get_default_stream_api()->remove_element,
    get_default_stream_api()->insert_element,
    live555_stream_set_param,
    live555_stream_get_param,
    live555_stream_open_in,
    NULL, // seek
    live555_stream_get_width,
    live555_stream_get_height,
    live555_stream_get_pixel_format,
    live555_stream_read_frame,
    get_default_stream_api()->print_pipeline,
    live555_stream_close,
    _set_module_trace_level_live555
};

//-----------------------------------------------------------------------------
static stream_obj*   live555_stream_create                (const char* name)
{
    const size_t sizeOf = sizeof(live555_packet_producer_t);
    live555_packet_producer_t* res = (live555_packet_producer_t*)stream_init(sizeOf,
                                            LIVE555_DEMUX_MAGIC,
                                            &_g_live555_stream_provider,
                                            name,
                                            live555_stream_destroy );
    res->descriptor = NULL;
    res->clientSession = NULL;
    res->envir = NULL;
    res->apiBridge = NULL;
    res->forceTCP = 0;
    res->bufferSizeKb = kDefaultBufferSizeKb;
    res->socketBufferSizeKb = kDefaultSocketBufferSizeKb;
    res->firstFrame = NULL;
    res->width = 0;
    res->height = 0;
    res->pixfmt = pfmtUndefined;
    res->disableGetParameter = 0;
    res->aggregateNALU = 1;
    res->poolFrames = 1;
    res->initTimeout = kDefaultInitTimeout;
    res->packetTimeout = kDefaultPacketTimeout;

    if (_gTraceLevel>15) res->statsIntervalSec = 1;
    else if (_gTraceLevel>10) res->statsIntervalSec = 5;
    else if (_gTraceLevel>5) res->statsIntervalSec = 10;
    else if (_gTraceLevel>2) res->statsIntervalSec = 30;
    else if (_gTraceLevel>0) res->statsIntervalSec = 60;
    else res->statsIntervalSec = 0;

    return (stream_obj*)res;
}

//-----------------------------------------------------------------------------
static int         live555_stream_set_param             (stream_obj* stream,
                                            const CHAR_T* name,
                                            const void* value)
{
    int dummy;
    DECLARE_DEMUX_LVL(stream, demux);
    TRACE_C(2, _FMT("set_param: name=" << name));

    name = stream_param_name_apply_scope(stream, name);
    if ( !_stricmp(name, "statsIntervalSec") ) {
        if ( *(int*)value < demux->statsIntervalSec || demux->statsIntervalSec == 0 ) {
            // only override the default if it results in more frequent stats
            demux->statsIntervalSec = *(int*)value;
        }
        TRACE_C(2, _FMT("set_param: statsIntervalSec=" << demux->statsIntervalSec));
        return 0;
    }
    SET_STR_PARAM_IF(stream, name, "url", demux->descriptor);
    SET_PARAM_IF(stream, name, "defaultToTCP", int, demux->forceTCP);
    SET_PARAM_IF(stream, name, "disableGetParameter", int, demux->disableGetParameter);
    SET_PARAM_IF(stream, name, "bufferSizeKb", int, demux->bufferSizeKb);
    SET_PARAM_IF(stream, name, "socketBufferSizeKb", int, demux->socketBufferSizeKb);
    SET_PARAM_IF(stream, name, "width", int, demux->width);
    SET_PARAM_IF(stream, name, "height", int, demux->height);
    SET_PARAM_IF(stream, name, "pixfmt", int, demux->pixfmt);
    SET_PARAM_IF(stream, name, "aggregateNALU", int, demux->aggregateNALU);
    SET_PARAM_IF(stream, name, "liveStream", int, dummy); // all streams are live, but need for compatibility
    SET_PARAM_IF(stream, name, "poolFrames", int, demux->poolFrames);
    SET_PARAM_IF(stream, name, "initTimeout", int, demux->initTimeout);
    SET_PARAM_IF(stream, name, "packetTimeout", int, demux->packetTimeout);

    TRACE_C(2, _FMT("Unknown param " << name));
    return -1;
}

//-----------------------------------------------------------------------------
static int         live555_stream_get_param             (stream_obj* stream,
                                            const CHAR_T* name,
                                            void* value,
                                            size_t* size)
{
    static const rational_t timebase = { 1, 1000 };

    DECLARE_DEMUX_LVL(stream, demux);
    TRACE_C(2, _FMT("get_param: name=" << name));

    name = stream_param_name_apply_scope(stream, name);
    COPY_PARAM_IF(demux, name, "maxFrameSizeKb", int, (demux->clientSession?demux->clientSession->GetBufferSize():0));
    COPY_PARAM_IF(demux, name, "timebase",       rational_t,  timebase);
    COPY_PARAM_IF(demux, name, "width", int, live555_stream_get_width(stream));
    COPY_PARAM_IF(demux, name, "height", int, live555_stream_get_height(stream));

    if (demux->clientSession) {
        COPY_PARAM_IF(demux, name, "pps", char*, demux->clientSession->GetPPS());
        COPY_PARAM_IF(demux, name, "ppsSize", int, demux->clientSession->GetPPSSize());
        COPY_PARAM_IF(demux, name, "sps", char*, demux->clientSession->GetSPS());
        COPY_PARAM_IF(demux, name, "spsSize", int, demux->clientSession->GetSPSSize());
        COPY_PARAM_IF(demux, name, "videoBitrate", int, demux->clientSession->GetVideoBitrate());
        COPY_PARAM_IF(demux, name, "audioBitrate", int, demux->clientSession->GetAudioBitrate());
        COPY_PARAM_IF(demux, name, "videoCodecId", int, TranslateCodec(demux->clientSession->GetVideoCodecId()));
        COPY_PARAM_IF(demux, name, "audioCodecId", int, TranslateCodec(demux->clientSession->GetAudioCodecId()));
        COPY_PARAM_IF(demux, name, "h264profile", int, demux->clientSession->GetH264Profile());
        COPY_PARAM_IF(demux, name, "h264level", int, demux->clientSession->GetH264Level());
        COPY_PARAM_IF(demux, name, "videoFramesProcessed", int, demux->clientSession->GetVideoFramesProcessed());
        COPY_PARAM_IF(demux, name, "videoFramesDropped", int, demux->clientSession->GetVideoFramesDropped());
        COPY_PARAM_IF(demux, name, "audioFramesProcessed", int, demux->clientSession->GetAudioFramesProcessed());
        COPY_PARAM_IF(demux, name, "audioFramesDropped", int, demux->clientSession->GetAudioFramesDropped());
        COPY_PARAM_ERR_IF(demux, name, "audioCodecProfile", int, demux->clientSession->GetAudioProfileId(), -1);
        COPY_PARAM_ERR_IF(demux, name, "audioCodecConfig", const char*, demux->clientSession->GetAudioConfig(), NULL);
        COPY_PARAM_ERR_IF(demux, name, "audioSampleRate", int, demux->clientSession->GetAudioSampleRate(), 0);
        COPY_PARAM_ERR_IF(demux, name, "audioChannels", int, demux->clientSession->GetAudioChannels(), 0);
        COPY_PARAM_ERR_IF(demux, name, "uptime", int64_t, demux->clientSession->GetUptime(), 0);
    }


    TRACE_C(2, _FMT("Unknown param " << name));
    return -1;
}


//-----------------------------------------------------------------------------
static size_t      live555_stream_get_width          (stream_obj* stream)
{
    DECLARE_DEMUX_LVL(stream, demux);
    return demux->width?demux->width:(size_t)-1;
}

//-----------------------------------------------------------------------------
static size_t      live555_stream_get_height         (stream_obj* stream)
{
    DECLARE_DEMUX_LVL(stream, demux);
    return demux->height?demux->height:(size_t)-1;
}

//-----------------------------------------------------------------------------
static int         live555_stream_get_pixel_format   (stream_obj* stream)
{
    DECLARE_DEMUX_LVL(stream, demux);
    return demux->pixfmt;
}

//-----------------------------------------------------------------------------
static int         live555_stream_open_in                (stream_obj* stream)
{
    DECLARE_DEMUX_LVL(stream, demux);
    int                 result = -1;
    // if the user forces TCP, we won't even attempt UDP connection
    bool                triedUDP = demux->forceTCP;
    bool                shouldTryTCP = false;
    bool                triedRemovingUP = false;
    bool                shouldTryRemovingUP = false;
    char                buffer[256+1];
    FrameBufferImpl*    fbi;

    if ( demux->clientSession != NULL ) {
        demux->logCb(logError, _FMT("Failed to open stream - already opened"));
        return result;
    }

    if ( demux->descriptor == NULL ) {
        demux->logCb(logError, _FMT("One of the required camera params isn't set - " <<
                                    " uri=" << sv_sanitize_uri(demux->descriptor, buffer, 256)));
        result = -2;
        goto Error;
    }

    shouldTryTCP = true;
    shouldTryRemovingUP = true;

TryAgain:
    demux->logCb (logInfo, _FMT("Opening "<< sv_sanitize_uri(demux->descriptor, buffer, 256) <<
                                " using " << (triedUDP?"TCP":"UDP") <<
                                " for transport (streamId="<<_g_StreamID<<")"));


    if ( demux->apiBridge == NULL ) {
        demux->apiBridge = new Live555APIBridge(demux->logCb, demux->poolFrames);
    }
    if ( demux->envir == NULL ) {
        demux->envir = sio::live555::MyDemuxBasicUsageEnvironment::createNew(demux->apiBridge);
    }
    sio::live555::MyDemuxBasicUsageEnvironment::setTraceLevel(_gTraceLevel);

    demux->clientSession = new sio::live555::DemuxRTSPClientImpl(demux->envir,
                            demux->apiBridge,
                            demux->descriptor,
                            RTSP_CLIENT_VERBOSITY_LEVEL,
                            _STR("sighthound"<<(void*)demux<<clock()<<rand()<<"-"<<_g_StreamID++),
                            triedUDP,
                            0,
                            triedRemovingUP);

    demux->clientSession->SetPacketTimeout(demux->packetTimeout);
    demux->clientSession->SetInitTimeout(demux->initTimeout);
    demux->clientSession->SetSocketBufferSize(demux->socketBufferSizeKb);
    demux->clientSession->SetBufferSize(demux->bufferSizeKb);
    demux->clientSession->SetStatsInterval(demux->statsIntervalSec);
    demux->clientSession->SetGetParameterEnabled(demux->disableGetParameter!=0);
    demux->clientSession->SetAggregateNALU(demux->aggregateNALU!=0);

    if ( demux->clientSession->Init() < 0 ) {
        demux->logCb(logError, _FMT("Failed to open the camera - " <<
                                    " uri=" << sv_sanitize_uri(demux->descriptor, buffer, 256) ));
        // failure to connect is not recoverable
        result = -3;
        goto Error;
    }

    fbi = (FrameBufferImpl*)demux->clientSession->ReadFrame();
    if ( !fbi ) {
        // we tried both TCP and UDP ... give up
        demux->logCb(logError, _FMT("Failed to read a frame - " <<
                                    " uri=" << sv_sanitize_uri(demux->descriptor, buffer, 256)));
        result = -5;
        goto Error;
    }
    demux->firstFrame = fbi->GetFrameAndRelease();
    demux->width = demux->clientSession->GetWidth();
    demux->height = demux->clientSession->GetHeight();
    TRACE_C(2, _FMT("Demux stream opened: " << demux->width << "x" << demux->height));
    return 0;

Error:
    if (demux->clientSession) {
        demux->clientSession->Close();
        demux->clientSession = NULL;
    }

    if ( shouldTryTCP && !triedUDP ) {
        demux->logCb(logWarning, _FMT("Failed to init using UDP, attempting TCP " <<
                                    " uri=" << sv_sanitize_uri(demux->descriptor, buffer, 256)));
        triedUDP = true;
        goto TryAgain;
    } else
    if ( shouldTryRemovingUP && !triedRemovingUP ) {
        demux->logCb(logWarning, _FMT("Now retrying without username/password in the URL " <<
                                    " uri=" << sv_sanitize_uri(demux->descriptor, buffer, 256)));
        triedRemovingUP = true;
        triedUDP = demux->forceTCP;
        goto TryAgain;
    }
    frame_unref((frame_obj**)&demux->firstFrame);
    live555_stream_close(stream);
    return result;
}


//-----------------------------------------------------------------------------
static int         live555_stream_read_frame        (stream_obj* stream,
                                                    frame_obj** frame)
{
    DECLARE_DEMUX_LVL(stream, demux);

    FrameBufferImpl* fbi;

    if (!demux->clientSession) {
        return -1;
    }

    if ( demux->firstFrame ) {
        *frame = (frame_obj*)demux->firstFrame;
        demux->firstFrame = NULL;
        return 0;
    }

    if ( !(fbi=(FrameBufferImpl*)demux->clientSession->ReadFrame()) ) {
        return -1;
    }

    basic_frame_obj* bfo = fbi->GetFrameAndRelease();
    *frame = (frame_obj*)bfo;
    return 0;
}

//-----------------------------------------------------------------------------
static int         live555_stream_close             (stream_obj* stream)
{
    DECLARE_DEMUX_LVL(stream, demux);
    frame_unref((frame_obj**)&demux->firstFrame);
    if (demux->clientSession) {
        try {
            demux->clientSession->Close();
            demux->clientSession = NULL;
        } catch(...) {
            demux->logCb(logError, _FMT("Exception while closing demux. Exiting."));
            exit(-1);
        }
    }
    sv_freep(&demux->descriptor);
    return 0;
}

//-----------------------------------------------------------------------------
static void live555_stream_destroy         (stream_obj* stream)
{
    DECLARE_DEMUX_LVL_V(stream, demux);
    live555_stream_close(stream); // make sure all the internals had been freed
    if (demux->envir) {
        sio::live555::MyDemuxBasicUsageEnvironment::destroy(demux->envir);
        demux->envir = NULL;
    }
    if (demux->apiBridge) {
        demux->apiBridge->release();
        demux->apiBridge = NULL;
    }
    stream_destroy( stream );
}



//-----------------------------------------------------------------------------
SVVIDEOLIB_API stream_api_t*     get_live555_demux_stream_api             ()
{
    return &_g_live555_stream_provider;
}

