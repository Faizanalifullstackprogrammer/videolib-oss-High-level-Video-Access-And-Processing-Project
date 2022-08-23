/*****************************************************************************
 *
 * stream_mediafoundation_decoder.cpp
 *   Node responsible for audio decoding on Windows.
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
#define SV_MODULE_VAR xcoder
#define SV_MODULE_ID "MFDECODER"
#include "sv_module_def.hpp"

#include <iostream>
#include <string.h>
#include <winapifamily.h>
#include <mfapi.h>
#include <mfidl.h>
#include <mfreadwrite.h>
#include <mferror.h>
#include <propvarutil.h>
#include <assert.h>
#include <algorithm>

#include "streamprv.h"

#include "videolibUtils.h"
#include "frame_basic.h"

#define MFDEC_STREAM_MAGIC 0x1723
#define MAX_PARAM 10
#define _DUMP_DEBUG 0




// Multimedia format types are marked with DWORDs built from four 8-bit
// chars and known as FOURCCs. New multimedia AM_MEDIA_TYPE definitions include
// a subtype GUID. In order to simplify the mapping, GUIDs in the range:
//    XXXXXXXX-0000-0010-8000-00AA00389B71
// are reserved for FOURCCs.

#define GUID_Data2      0
#define GUID_Data3     0x10
#define GUID_Data4_1   0xaa000080
#define GUID_Data4_2   0x719b3800

class FOURCCMap : public GUID
{
public:
    FOURCCMap()
    {
        InitGUID();
        SetFOURCC( DWORD(0));
    }
    FOURCCMap(DWORD fourcc)
    {
        InitGUID();
        SetFOURCC(fourcc);
    }
    FOURCCMap(const GUID * pGuid)
    {
        InitGUID();
        SetFOURCC(pGuid);
    }


    DWORD GetFOURCC(void) { return Data1; }
    void SetFOURCC(DWORD fourcc) { Data1 = fourcc; }
    void SetFOURCC(const GUID * pGuid)
    {
        FOURCCMap * p = (FOURCCMap*) pGuid;
        SetFOURCC(p->GetFOURCC());
    }

private:
    void InitGUID()
    {
        Data2 = GUID_Data2;
        Data3 = GUID_Data3;
        ((DWORD *)Data4)[0] = GUID_Data4_1;
        ((DWORD *)Data4)[1] = GUID_Data4_2;
    }
};



typedef HRESULT (__stdcall *_fpMFTEnumEx_t)(GUID guidCategory, UINT32 Flags,
                                 const MFT_REGISTER_TYPE_INFO *pInputType,
                                 const MFT_REGISTER_TYPE_INFO *pOutputType,
                                 IMFActivate ***pppMFTActivate, UINT32 *pcMFTActivate);
typedef HRESULT (__stdcall *_fpMFCreateSample_t)(IMFSample **ppIMFSample);
typedef HRESULT (__stdcall *_fpMFCreateMemoryBuffer_t)(DWORD cbMaxLength, IMFMediaBuffer **ppBuffer);
typedef HRESULT (__stdcall *_fpMFCreateAlignedMemoryBuffer_t)(DWORD cbMaxLength, DWORD fAlignmentFlags, IMFMediaBuffer **ppBuffer);

extern "C" stream_api_t*     get_mfdec_stream_api             ();
extern "C" stream_api_t*     get_resample_filter_api          ();

//-----------------------------------------------------------------------------
typedef struct
{
    HINSTANCE hDll;
    _fpMFTEnumEx_t                     fpMFTEnumEx;
    _fpMFCreateSample_t                fpMFCreateSample;
    _fpMFCreateMemoryBuffer_t          fpMFCreateMemoryBuffer;
    _fpMFCreateAlignedMemoryBuffer_t   fpMFCreateAlignedMemoryBuffer;
} mfplat;


//-----------------------------------------------------------------------------
typedef struct mfdec_stream  : public stream_base {
    int                 mediaType;

    int                 srcCodecId;
    int                 dstCodecId;
    int                 srcSampleFormat;
    int                 dstSampleFormat;
    int                 srcBitsPerSample;
    int                 dstBitsPerSample;
    int                 srcSampleRate;
    int                 isEncoder;
    const char*         modeName;

    mfplat              mfplatDll;
    IMFTransform*       mft;
    const GUID*         majorType;
    const GUID*         inputSubtype;
    const GUID*         outputSubtype;


    /* Input stream */
    DWORD               inputStreamId;
    IMFMediaType*       inputType;

    /* Output stream */
    DWORD               outputStreamId;
    IMFSample*          outputSample;
    int                 outputBufferSize;
    bool                mfManagedBuffers;
    IMFMediaType*       outputType;

    int                 channels;


    UINT64_T            prevCaptureTimeMs;
    int                 captureHasStabilized;

    ssize_t             packetsProcessed;// number of packets read or written
    UINT64_T            firstPts;   // last PTS returned
    UINT64_T            lastOutputPts;   // last PTS returned
    ssize_t             framesProcessed; // number of frames read or written
    ssize_t             framesIgnored;   // number of frames the decoder had ignored

    int                 errorOccurred;
    UINT8               extradata[32];
    UINT32              extradata_size;

#if _DUMP_DEBUG
    FILE*               debugFileIn;
    FILE*               debugFileOut;
#endif

    frame_allocator*    fa;
} mfdec_stream_obj;

//-----------------------------------------------------------------------------
static          GUID mulawGuid = (GUID)FOURCCMap(MAKEFOURCC(7,0,0,0));
static          GUID alawGuid = (GUID)FOURCCMap(MAKEFOURCC(6,0,0,0));
static const int  kAudioSpecificConfigOffset = 12;

//-----------------------------------------------------------------------------
// Stream API
//-----------------------------------------------------------------------------

//-----------------------------------------------------------------------------
//  Forward declarations
//-----------------------------------------------------------------------------
static stream_obj* mfdec_stream_create             (const char* name);
static int         mfdec_stream_set_param          (stream_obj* stream,
                                                    const CHAR_T* name,
                                                    const void* value);
static int         mfdec_stream_get_param          (stream_obj* stream,
                                                    const CHAR_T* name,
                                                    void* value,
                                                    size_t* size);
static int         mfdec_stream_open_in            (stream_obj* stream);
static int         mfdec_stream_seek               (stream_obj* stream,
                                                    INT64_T offset,
                                                    int flags);
static size_t      mfdec_stream_get_width          (stream_obj* stream);
static size_t      mfdec_stream_get_height         (stream_obj* stream);
static int         mfdec_stream_get_pixel_format   (stream_obj* stream);
static int         mfdec_stream_read_frame         (stream_obj* stream, frame_obj** frame);
static int         mfdec_stream_close              (stream_obj* stream);
static void        mfdec_stream_destroy            (stream_obj* stream);

//-----------------------------------------------------------------------------
stream_api_t _g_mfdec_stream_provider = {
    mfdec_stream_create,
    default_set_source,
    default_set_log_cb,
    default_get_name,
    default_find_element,
    default_remove_element,
    default_insert_element,
    mfdec_stream_set_param,
    mfdec_stream_get_param,
    mfdec_stream_open_in,
    mfdec_stream_seek,
    mfdec_stream_get_width,
    mfdec_stream_get_height,
    mfdec_stream_get_pixel_format,
    mfdec_stream_read_frame,
    default_print_pipeline,
    mfdec_stream_close,
    _set_module_trace_level
} ;

#define _CHECK(call, msg)\
    hr = call; \
    if (FAILED(hr)) {\
        LPVOID pText = 0;\
        ::FormatMessage(FORMAT_MESSAGE_ALLOCATE_BUFFER|FORMAT_MESSAGE_FROM_SYSTEM|FORMAT_MESSAGE_IGNORE_INSERTS, \
                    NULL,hr,MAKELANGID(LANG_NEUTRAL,SUBLANG_DEFAULT),(LPSTR)&pText,0,NULL);\
        xcoder->logCb(logError, _FMT(msg << "; hr=" << std::setbase(16) << hr << ": " << (LPSTR)pText));\
        LocalFree(pText);\
        goto Error;\
    }

#define COM_RELEASE(obj)\
    if ( obj ) { obj->Release(); obj = NULL; }


//-----------------------------------------------------------------------------
static bool _mfdec_get_output_size(mfdec_stream_obj* xcoder, size_t inputSize)
{
    int factor;

    switch (xcoder->dstCodecId)
    {
    case streamPCMU:
    case streamPCMA: factor = 2; break;
    case streamAAC : factor = 10; break;
    default        : return -1;
    }

    return xcoder->isEncoder ? inputSize / factor : inputSize * factor;
}

//-----------------------------------------------------------------------------
static bool _mfdec_alloc_sample_in(mfdec_stream_obj* xcoder, DWORD size,
            IMFSample** out)
{
    HRESULT hr;
    IMFSample *pInputSample = NULL;
    IMFMediaBuffer *pInputMediaBuffer = NULL;

    MFT_INPUT_STREAM_INFO input_info;
    hr = xcoder->mft->GetInputStreamInfo(xcoder->inputStreamId, &input_info);
    if (SUCCEEDED(hr)) {
        hr = xcoder->mfplatDll.fpMFCreateSample(&pInputSample);
    }

    if (SUCCEEDED(hr)) {
        DWORD allocationSize = std::max(input_info.cbSize, (DWORD)size);
        hr = xcoder->mfplatDll.fpMFCreateMemoryBuffer(allocationSize, &pInputMediaBuffer);
    }

    if (SUCCEEDED(hr)) {
        hr = pInputSample->AddBuffer(pInputMediaBuffer);
    }

    if (SUCCEEDED(hr)) {
        pInputMediaBuffer->Release();
        *out = pInputSample;
        return true;
    }

    xcoder->logCb(logError, _FMT("Failed to allocate input sample"));
    if (pInputSample)
        pInputSample->Release();
    if (pInputMediaBuffer)
        pInputMediaBuffer->Release();
    *out = NULL;
    return false;
}

//-----------------------------------------------------------------------------
static bool _mfdec_alloc_sample_out(mfdec_stream_obj* xcoder, size_t size)
{
    HRESULT hr;

    if ( (xcoder->outputSample != NULL &&
         xcoder->outputBufferSize >= size) ||
         xcoder->mfManagedBuffers ) {
        return true;
    }

    COM_RELEASE( xcoder->outputSample );

    IMFMediaBuffer *pOutputMediaBuffer = NULL;

    MFT_OUTPUT_STREAM_INFO outputInfo;
    hr = xcoder->mft->GetOutputStreamInfo(xcoder->outputStreamId, &outputInfo);
    if (SUCCEEDED(hr)) {
        DWORD mask = (MFT_OUTPUT_STREAM_PROVIDES_SAMPLES |
                      MFT_OUTPUT_STREAM_CAN_PROVIDE_SAMPLES);
        if (outputInfo.dwFlags & mask ) {
            // The MFT will provide an allocated sample.
            TRACE(_FMT("Relying on MFT to provide samples"));
            xcoder->mfManagedBuffers = true;
            return true;
        }
        TRACE(_FMT("Creating an output sample"));
        hr = xcoder->mfplatDll.fpMFCreateSample(&xcoder->outputSample);
    }


    if (SUCCEEDED(hr)) {
        DWORD allocationSize = std::max( outputInfo.cbSize, (DWORD)size );
        DWORD alignment = outputInfo.cbAlignment;
        TRACE(_FMT("Creating buffer of size " << allocationSize << " with alignment of " << alignment ));
        if (alignment > 0)
            hr = xcoder->mfplatDll.fpMFCreateAlignedMemoryBuffer(allocationSize, alignment - 1,
                                         &pOutputMediaBuffer);
        else
            hr = xcoder->mfplatDll.fpMFCreateMemoryBuffer(allocationSize, &pOutputMediaBuffer);
        xcoder->outputBufferSize = allocationSize;
    }

    if (SUCCEEDED(hr)) {
        hr = xcoder->outputSample->AddBuffer(pOutputMediaBuffer);
    }

    if (SUCCEEDED(hr)) {
        return true;
    }

    xcoder->logCb(logError, _FMT("Failed to allocate output sample"));
    COM_RELEASE( xcoder->outputSample );
    xcoder->outputBufferSize = 0;
    return false;
}


//-----------------------------------------------------------------------------
#define DECLARE_STREAM_FF(stream, name) \
    DECLARE_OBJ(mfdec_stream, name,  stream, MFDEC_STREAM_MAGIC, -1)
#define DECLARE_STREAM_FF_V(stream, name) \
    DECLARE_OBJ_V(mfdec_stream, name,  stream, MFDEC_STREAM_MAGIC)


static stream_obj*   mfdec_stream_create                (const char* name)
{
    mfdec_stream* res = (mfdec_stream*)stream_init(sizeof(mfdec_stream),
                                        MFDEC_STREAM_MAGIC,
                                        &_g_mfdec_stream_provider,
                                        name,
                                        mfdec_stream_destroy );

    res->prevCaptureTimeMs = 0;
    res->captureHasStabilized = 0;

    res->srcCodecId = streamUnknown;
    res->dstCodecId = streamUnknown;
    // this property isn't settable yet
    res->srcSampleFormat = sfmtUndefined;
    res->dstSampleFormat = sfmtUndefined;
    res->srcBitsPerSample = -1;
    res->dstBitsPerSample = -1;
    res->isEncoder = false;
    res->modeName = NULL;

    res->packetsProcessed = 0;
    res->lastOutputPts = INVALID_PTS;
    res->firstPts = INVALID_PTS;
    res->framesProcessed = 0;
    res->framesIgnored = 0;

    res->errorOccurred = 0;

    res->mfplatDll.hDll = NULL;

    res->mft = NULL;
    res->majorType = &MFMediaType_Audio;
    res->inputSubtype = NULL;
    res->outputSubtype = NULL;

    res->inputStreamId = 0;
    res->inputType = NULL;

    /* Output stream */
    res->outputStreamId = 0;
    res->outputSample = NULL;
    res->mfManagedBuffers = false;
    res->outputBufferSize = 0;
    res->outputType = NULL;

    res->channels = -1;
    res->srcSampleRate = -1;

    memset(res->extradata, 0, sizeof(res->extradata));
    res->extradata_size = 0;

    res->fa = create_frame_allocator(_STR("xcoder_"<<name));

    return (stream_obj*)res;
}

//-----------------------------------------------------------------------------
static int         mfdec_stream_set_param             (stream_obj* stream,
                                            const CHAR_T* name,
                                            const void* value)
{
    DECLARE_STREAM_FF(stream, xcoder);

    name = stream_param_name_apply_scope(stream, name);

    SET_PARAM_IF(stream, name, "decoderType", int, xcoder->mediaType);
    SET_PARAM_IF(stream, name, "encoderType", int, xcoder->mediaType);
    SET_PARAM_IF(stream, name, "dstCodecId", int, xcoder->dstCodecId);

    // pass it on, if we can
    return default_set_param(stream, name, value);
}

//-----------------------------------------------------------------------------
static int         mfdec_stream_get_param             (stream_obj* stream,
                                            const CHAR_T* name,
                                            void* value,
                                            size_t* size)
{
    DECLARE_STREAM_FF(stream, xcoder);

    name = stream_param_name_apply_scope(stream, name);

    bool pass = xcoder->passthrough && !xcoder->errorOccurred;

    if ( !pass ) {
        COPY_PARAM_IF(xcoder, name, "audioCodecId",      int, xcoder->dstCodecId);
        COPY_PARAM_IF(xcoder, name, "audioCodecConfig",  const char*, NULL);
        COPY_PARAM_IF(xcoder, name, "AudioSpecificConfigSize", int,
                            xcoder->extradata_size>kAudioSpecificConfigOffset
                                                ? xcoder->extradata_size-kAudioSpecificConfigOffset
                                                : 0);
        if ( !_stricmp(name, "AudioSpecificConfig") ) {
            if ( xcoder->extradata_size>kAudioSpecificConfigOffset ) {
                memcpy((char*)value,
                        &xcoder->extradata[kAudioSpecificConfigOffset],
                        xcoder->extradata_size-kAudioSpecificConfigOffset);
                return 0;
            }
        }

    }
    COPY_PARAM_IF(xcoder, name, "audioInterleaved",  int, 1);
    COPY_PARAM_IF(xcoder, name, "audioSampleFormat", int, xcoder->dstSampleFormat);
    COPY_PARAM_IF(xcoder, name, "framesProcessed",   int, xcoder->framesProcessed);
    COPY_PARAM_IF(xcoder, name, "framesDropped",     int, xcoder->framesIgnored);


    // pass it on, if we can
    return default_get_param(stream, name, value, size);
}

//-----------------------------------------------------------------------------
static int          _mfdec_sample_format_to_bps(mfdec_stream_obj* xcoder, int sf)
{
    switch (sf)
    {
    case sfmtInt8: return 8;
    case sfmtInt16: return 16;
    case sfmtFloat:
    case sfmtInt32: return 32;
    default:
        TRACE(_FMT("Unexpected sample format " << sf));
        return -1;
    }
}

//-----------------------------------------------------------------------------
static int          _mfdec_bps_to_sample_format(mfdec_stream_obj* xcoder, int bps)
{
    switch (bps)
    {
    case 8:     return sfmtInt8;
    case 16:    return sfmtInt16;
    case 32:    return sfmtInt32;
    default:
        TRACE(_FMT("Unexpected sample size " << bps));
        return -1;
    }
}


//-----------------------------------------------------------------------------
static int          _mfdec_validate_sample_size(mfdec_stream_obj* xcoder,
                                            IMFMediaType* mediaType,
                                            bool isInput )
{
    HRESULT         hr;
    int             res = -1;

    int codec = isInput ? xcoder->srcCodecId : xcoder->dstCodecId;
    if ( codec != streamLinear ) {
        TRACE(_FMT("Codec doesn't need sample size verification: "<< codec));
        return 0;
    }

    UINT32 bitsPerSample;
    _CHECK( mediaType->GetUINT32(MF_MT_AUDIO_BITS_PER_SAMPLE, &bitsPerSample),
            _FMT("Failed to determine bits-per-sample") );
    if ( isInput ) {
        if ( xcoder->srcBitsPerSample != bitsPerSample ) {
            TRACE(_FMT("Skipping input format with bitsPerSample=" << bitsPerSample <<
                       " expected=" << xcoder->srcBitsPerSample ));
        } else {
            res = 0;
        }
        TRACE(_FMT("Ouput: srcBitsPerSample=" << xcoder->srcBitsPerSample << " srcSampleFormat=" << xcoder->srcSampleFormat << " bitsPerSample=" << bitsPerSample));
    } else if ( xcoder->dstCodecId == streamAAC ) {
        xcoder->dstBitsPerSample = bitsPerSample;
        assert( bitsPerSample == 32 );
        xcoder->dstSampleFormat = sfmtFloat;
    } else {
        xcoder->dstBitsPerSample = bitsPerSample;
        xcoder->dstSampleFormat = _mfdec_bps_to_sample_format(xcoder, bitsPerSample);
        TRACE(_FMT("Ouput: dstBitsPerSample=" << xcoder->dstBitsPerSample << " dstSampleFormat=" << xcoder->dstSampleFormat));
        res = (xcoder->dstSampleFormat >= 0) ? 0 : -1;
    }

Error:
    return res;
}

//-----------------------------------------------------------------------------
static IMFMediaType*  _mfdec_find_stream    (mfdec_stream_obj* xcoder,
                                            bool isInput,
                                            bool& error)
{
    HRESULT         hr;
    IMFMediaType*   mediaType = NULL;
    int             streamId;
    const GUID*     guid;
    const char*     descr;
    int             codecId;

    error = true;

    if ( isInput ) {
        streamId = xcoder->inputStreamId;
        guid = xcoder->inputSubtype;
        codecId = xcoder->srcCodecId;
        descr = "input";
    } else {
        streamId = xcoder->outputStreamId;
        guid = xcoder->outputSubtype;
        codecId = xcoder->dstCodecId;
        descr = "output";
    }

    for (int i = 0; ; ++i) {
        hr = isInput ? xcoder->mft->GetInputAvailableType(streamId, i, &mediaType)
                     : xcoder->mft->GetOutputAvailableType(streamId, i, &mediaType);
        if (hr == MF_E_NO_MORE_TYPES) {
            TRACE(_FMT("No more " << descr << " streams: " << i));
            break;
        } else if (hr == MF_E_TRANSFORM_TYPE_NOT_SET) {
            /* The output type must be set before setting the input type for this MFT. */
            error = false;
            TRACE(_FMT(descr << " transform type not set: " << i));
            break;
        } else if (FAILED(hr)) {
            _CHECK( hr, _FMT("Failed to get " << descr << " type " << i));
        }

        TRACE(_FMT("Checking " << descr << " type: " << i ));

        GUID subtype;
        _CHECK( mediaType->GetGUID(MF_MT_SUBTYPE, &subtype),
                _FMT("Failed to get input media subtype") );
        if (IsEqualGUID(subtype, *guid)) {
            TRACE(_FMT("Checking " << descr << " type: " << i << "... yay, equal GUID"));

            if ( _mfdec_validate_sample_size(xcoder, mediaType, isInput ) >= 0 ) {
                TRACE((_FMT("Found matching " << descr << " stream")));
                error = false;
                return mediaType;
            }
        }

        COM_RELEASE(mediaType);
    }

Error:
    COM_RELEASE(mediaType);
    return NULL;
}


//-----------------------------------------------------------------------------
static int       _mfdec_set_input_type (mfdec_stream_obj* xcoder, bool secondCall)
{
    HRESULT hr;
    bool    error;

    IMFMediaType* inputType = _mfdec_find_stream(xcoder, true, error);
    if ( inputType == NULL ) {
        return (error || secondCall) ? -1 : 0;
    }


    _CHECK(inputType->SetUINT32(MF_MT_ORIGINAL_WAVE_FORMAT_TAG, xcoder->inputSubtype->Data1),
            _FMT("Failed to set format tag") );
    _CHECK(inputType->SetUINT32(MF_MT_AUDIO_SAMPLES_PER_SECOND, xcoder->srcSampleRate),
            _FMT("Failed to set sample rate") );
    _CHECK(inputType->SetUINT32(MF_MT_AUDIO_NUM_CHANNELS, xcoder->channels),
            _FMT("Failed to set channels count") );

    if ( xcoder->srcBitsPerSample > 0 ) {
        TRACE(_FMT("Setting input sample size to " << xcoder->srcBitsPerSample));
        // _CHECK(inputType->SetUINT32(MF_MT_SAMPLE_SIZE, xcoder->srcBitsPerSample),
        //         _FMT("Failed to set sample size") );
        _CHECK(inputType->SetUINT32(MF_MT_AUDIO_BITS_PER_SAMPLE, xcoder->srcBitsPerSample),
                _FMT("Failed to set sample size#2") );

        UINT32 avgBps = xcoder->srcSampleRate*xcoder->srcBitsPerSample/8;
        _CHECK(inputType->SetUINT32(MF_MT_AUDIO_AVG_BYTES_PER_SECOND, avgBps),
                _FMT("Failed to set avg bytes per sec") );
        _CHECK(inputType->SetUINT32(MF_MT_AUDIO_BLOCK_ALIGNMENT, xcoder->channels * xcoder->srcBitsPerSample/8),
                _FMT("Failed to set block alignment") );
        _CHECK(inputType->SetUINT32(MF_MT_ALL_SAMPLES_INDEPENDENT, 1),
                _FMT("Failed to set block alignment") );
    }

    TRACE(_FMT("Setting input type: inputStreamId=" << xcoder->inputStreamId <<
                        " sampleRate=" << xcoder->srcSampleRate <<
                        " channels=" << xcoder->channels <<
                        " sampleSize=" << xcoder->srcBitsPerSample ));
    _CHECK( xcoder->mft->SetInputType(xcoder->inputStreamId, inputType, 0),
            _FMT("Failed to set input type") );

    xcoder->inputType = inputType;
    return 0;

Error:
    COM_RELEASE(inputType);
    return -1;
}

//-----------------------------------------------------------------------------
static int       _mfdec_set_output_type (mfdec_stream_obj* xcoder)
{
    HRESULT hr;
    bool    error;

    IMFMediaType* outputType = _mfdec_find_stream(xcoder, false, error);
    if ( outputType == NULL ) {
        // give it another chance with a different sample format
        if ( xcoder->outputSubtype == &MFAudioFormat_PCM ) {
            xcoder->outputSubtype = &MFAudioFormat_Float;
            error = false;
            outputType = _mfdec_find_stream(xcoder, false, error);
        }
        if ( outputType == NULL ) {
            return -1;
        }
    }

    _CHECK(outputType->SetUINT32(MF_MT_AUDIO_SAMPLES_PER_SECOND, xcoder->srcSampleRate),
            _FMT("Failed to set sample rate") );
    _CHECK(outputType->SetUINT32(MF_MT_AUDIO_NUM_CHANNELS, xcoder->channels),
            _FMT("Failed to set channels count") );

    if ( xcoder->dstCodecId == streamLinear ) {
        TRACE(_FMT("Setting output sample size to " << xcoder->dstBitsPerSample));
        // _CHECK(inputType->SetUINT32(MF_MT_SAMPLE_SIZE, xcoder->srcBitsPerSample),
        //         _FMT("Failed to set sample size") );
        _CHECK(outputType->SetUINT32(MF_MT_AUDIO_BITS_PER_SAMPLE, xcoder->dstBitsPerSample),
                _FMT("Failed to set sample size#2") );
        _CHECK(outputType->SetUINT32(MF_MT_SAMPLE_SIZE, xcoder->dstBitsPerSample),
                _FMT("Failed to set sample size#2") );

    }

    TRACE(_FMT("Setting output type: outputStreamId=" << xcoder->outputStreamId <<
                        " sampleRate=" << xcoder->srcSampleRate <<
                        " channels=" << xcoder->channels ));
    _CHECK( xcoder->mft->SetOutputType(xcoder->outputStreamId, outputType, 0),
            _FMT("Error in SetOutputType") );

    GUID subtype;
    _CHECK( outputType->GetGUID(MF_MT_SUBTYPE, &subtype),
            _FMT("Error in GetGUID") );

    // p_dec->fmt_out.audio = p_dec->fmt_in.audio;

    // UINT32 bitspersample = 0;
    // hr = IMFMediaType_GetUINT32(output_media_type, &MF_MT_AUDIO_BITS_PER_SAMPLE, &bitspersample);
    // if (SUCCEEDED(hr) && bitspersample)
    //     p_dec->fmt_out.audio.i_bitspersample = bitspersample;

    // UINT32 channels = 0;
    // hr = IMFMediaType_GetUINT32(output_media_type, &MF_MT_AUDIO_NUM_CHANNELS, &channels);
    // if (SUCCEEDED(hr) && channels)
    //     p_dec->fmt_out.audio.i_channels = channels;

    // UINT32 rate = 0;
    // hr = IMFMediaType_GetUINT32(output_media_type, &MF_MT_AUDIO_SAMPLES_PER_SECOND, &rate);
    // if (SUCCEEDED(hr) && rate)
    //     p_dec->fmt_out.audio.i_rate = rate;

    // vlc_fourcc_t fourcc;
    // wf_tag_to_fourcc(subtype.Data1, &fourcc, NULL);
    // p_dec->fmt_out.i_codec = vlc_fourcc_GetCodecAudio(fourcc, p_dec->fmt_out.audio.i_bitspersample);

    // p_dec->fmt_out.audio.i_physical_channels = pi_channels_maps[p_dec->fmt_out.audio.i_channels];
    // p_dec->fmt_out.audio.i_original_channels = p_dec->fmt_out.audio.i_physical_channels;


    /* Need to get extradata */
    if ( xcoder->dstCodecId == streamAAC ) {
        _CHECK(outputType->GetBlob(MF_MT_USER_DATA, xcoder->extradata, sizeof(xcoder->extradata), &xcoder->extradata_size),
                _FMT("Failed to retrieve MF_MT_USER_DATA"));
    }

    xcoder->outputType = outputType;
    return 0;

Error:
    COM_RELEASE(outputType);
    return -1;
}

//-----------------------------------------------------------------------------
static int       _mfdec_release_mft (mfdec_stream_obj* xcoder)
{
    COM_RELEASE(xcoder->inputType);
    if (xcoder->outputSample) {
        IMFMediaBuffer *outputBuffer = NULL;
        HRESULT hr = xcoder->outputSample->GetBufferByIndex(0, &outputBuffer);
        if (SUCCEEDED(hr)) {
            COM_RELEASE(outputBuffer);
        }
        COM_RELEASE(xcoder->outputSample);
    }
    COM_RELEASE(xcoder->outputType);
    COM_RELEASE(xcoder->mft);
    return 0;
}


//-----------------------------------------------------------------------------
static int       _mfdec_try_init_xcoder (mfdec_stream_obj* xcoder)
{
    HRESULT hr;

    IMFAttributes *attributes = NULL;
    hr = xcoder->mft->GetAttributes(&attributes);
    if (hr != E_NOTIMPL && FAILED(hr))
        goto Error;
    if (SUCCEEDED(hr)) {
        UINT32 async = 0;
        hr = attributes->GetUINT32(MF_TRANSFORM_ASYNC, &async);
        if (hr != MF_E_ATTRIBUTENOTFOUND && FAILED(hr))
            goto Error;
        if (async) {
            xcoder->logCb(logError, _FMT("Async transforms are not supported by the " << xcoder->modeName));
            goto Error;
        }
    }

    DWORD inStreamsCount, outStreamsCount;
    _CHECK( xcoder->mft->GetStreamCount(&inStreamsCount, &outStreamsCount),
            _FMT("Failed to retrieve streams count") );
    if (inStreamsCount != 1 || outStreamsCount != 1) {
        xcoder->logCb(logError, _FMT("Unsupported stream count"));
        goto Error;
    }

    hr = xcoder->mft->GetStreamIDs(1, &xcoder->inputStreamId, 1, &xcoder->outputStreamId);
    if (hr == E_NOTIMPL) {
        // not an error, according to the docs
        xcoder->inputStreamId = 0;
        xcoder->outputStreamId = 0;
    } else if (FAILED(hr)) {
        xcoder->logCb(logError, _FMT("Failed to get stream IDs: hr=" << hr));
        goto Error;
    }

    if (_mfdec_set_input_type(xcoder, false)) {
        TRACE(_FMT("Error in _mfdec_set_input_type"));
        goto Error;
    }

    if (_mfdec_set_output_type(xcoder)) {
        TRACE(_FMT("Error in _mfdec_set_output_type"));
        goto Error;
    }

    /*
     * The input type was not set by the previous call to
     * SetInputType, try again after setting the output type.
     */
    if (!xcoder->inputType) {
        if (_mfdec_set_input_type(xcoder, true)) {
            TRACE(_FMT("Error in _mfdec_set_input_type#2"));
            goto Error;
        }
    }

    /* This call can be a no-op for some MFT decoders, but it can potentially reduce starting time. */
    _CHECK( xcoder->mft->ProcessMessage(MFT_MESSAGE_NOTIFY_BEGIN_STREAMING, (ULONG_PTR)0),
            _FMT("Failed to begin streaming") );

    /* This event is required for asynchronous MFTs, optional otherwise. */
    _CHECK( xcoder->mft->ProcessMessage(MFT_MESSAGE_NOTIFY_START_OF_STREAM, (ULONG_PTR)0),
            _FMT("Failed to notify about start stream") );

    TRACE(_FMT("_mfdec_try_init_xcoder - success"));
    return 0;

Error:
    _mfdec_release_mft(xcoder);
    return -1;
}


//-----------------------------------------------------------------------------
static const GUID*  _mfdec_find_codec(mfdec_stream_obj* xcoder, int id, int sfmt)
{
    TRACE(_FMT("Looking for codec matching " << id));
    switch (id) {
    case streamAAC:     TRACE(_FMT("AAC"));  return &MFAudioFormat_AAC;
    case streamPCMU:    TRACE(_FMT("MLAW")); return &mulawGuid;
    case streamPCMA:    TRACE(_FMT("ALAW")); return &alawGuid;
    case streamLinear:  TRACE(_FMT("PCM"));  return &MFAudioFormat_PCM;
        // switch (sfmt) {
        // case sfmtFloat: TRACE(_FMT("FLOAT"));  return &MFAudioFormat_Float;
        // default       : TRACE(_FMT("PCM"));  return &MFAudioFormat_PCM;
        // }
        // break;
    default:
        xcoder->logCb(logError, _FMT("Unsupported codec: " << id));
        return NULL;
    }
}

//-----------------------------------------------------------------------------
static int       _mfdec_prepare_audio_xcoder (mfdec_stream_obj* xcoder)
{
    int             res = -1;
    size_t          size;
    GUID            category;
    HRESULT         hr;
    mfplat&         mfp = xcoder->mfplatDll;
    HMODULE&        hDll = mfp.hDll;
    UINT32          flags;
    MFT_REGISTER_TYPE_INFO inputType, outputType;
    IMFActivate**   objects = NULL;
    UINT32          objectsCount = 0;


    size = sizeof(int);
    if (default_get_param((stream_obj*)xcoder,
                        "audioChannels",
                        &xcoder->channels,
                        &size) < 0 ) {
        xcoder->logCb(logError, _FMT("Failed to determine channel count."));
        goto Error;
    }


    xcoder->inputSubtype = _mfdec_find_codec(xcoder, xcoder->srcCodecId, xcoder->srcSampleFormat);
    if ( xcoder->inputSubtype == NULL ) {
        return -1;
    }
    xcoder->outputSubtype = _mfdec_find_codec(xcoder, xcoder->dstCodecId, sfmtUndefined );
    if ( xcoder->outputSubtype == NULL ) {
        return -1;
    }
    category = xcoder->isEncoder ? MFT_CATEGORY_AUDIO_ENCODER : MFT_CATEGORY_AUDIO_DECODER;


    TRACE(_FMT("Loading mfplat.dll library"));
    hDll = LoadLibrary(TEXT("mfplat.dll"));
    if (!hDll ) {
        xcoder->logCb(logError, _FMT("mfplat.dll can't be loaded"));
        return -1;
    }

    TRACE(_FMT("Loading mfplat.dll library methods"));
    mfp.fpMFTEnumEx = (_fpMFTEnumEx_t)GetProcAddress(hDll, "MFTEnumEx");
    mfp.fpMFCreateSample = (_fpMFCreateSample_t)GetProcAddress(hDll, "MFCreateSample");
    mfp.fpMFCreateMemoryBuffer = (_fpMFCreateMemoryBuffer_t)GetProcAddress(hDll, "MFCreateMemoryBuffer");
    mfp.fpMFCreateAlignedMemoryBuffer = (_fpMFCreateAlignedMemoryBuffer_t)GetProcAddress(hDll, "MFCreateAlignedMemoryBuffer");
    if (!mfp.fpMFTEnumEx ||
        !mfp.fpMFCreateSample ||
        !mfp.fpMFCreateMemoryBuffer ||
        !mfp.fpMFCreateAlignedMemoryBuffer) {
        xcoder->logCb(logError, _FMT("mfplat.dll methods can't be loaded"));
        return -1;
    }


    flags = MFT_ENUM_FLAG_SORTANDFILTER | MFT_ENUM_FLAG_LOCALMFT
                 | MFT_ENUM_FLAG_SYNCMFT
                 | MFT_ENUM_FLAG_HARDWARE | MFT_ENUM_FLAG_TRANSCODE_ONLY;

    inputType = { *xcoder->majorType, *xcoder->inputSubtype };
    outputType = { *xcoder->majorType, *xcoder->outputSubtype };
    _CHECK( mfp.fpMFTEnumEx(category, flags, xcoder->isEncoder?NULL:&inputType, xcoder->isEncoder?&outputType:NULL, &objects, &objectsCount),
            _FMT("Failed to enumerate available media transforms"));

    if (objectsCount == 0) {
        xcoder->logCb(logError, _FMT("No media transforms found"));
        goto Error;
    }

    for (int nI = 0; nI < objectsCount; ++nI) {
        TRACE(_FMT("ActivateObject - " << nI << " objectsCount=" << objectsCount));
        hr = objects[nI]->ActivateObject(IID_IMFTransform, (void**)&xcoder->mft);
        COM_RELEASE(objects[nI]);
        if (FAILED(hr)) {
            TRACE(_FMT("Failed to activate stream " <<nI << " hr=" << std::setbase(16) << hr));
            continue;
        }

        TRACE(_FMT("_mfdec_try_init_xcoder - " << nI));
        if (_mfdec_try_init_xcoder(xcoder) == 0) {
            break;
        }
    }
    CoTaskMemFree(objects);
    if ( xcoder->mft == NULL ) {
        xcoder->logCb(logError, _FMT("No media transforms could be configured"));
        goto Error;
    }

    res = 0;
Error:
    return res;
}

//-----------------------------------------------------------------------------
static int         mfdec_stream_open_in                (stream_obj* stream)
{
    DECLARE_STREAM_FF(stream, xcoder);
    int             res = -1;
    size_t          size;
    int             recoverable = 0;

    res = default_open_in(stream);
    if (res < 0) {
        goto Error;
    }
    recoverable = 1;

    if ( xcoder->dstCodecId == streamUnknown ||
         xcoder->mediaType == mediaUnknown ) {
        xcoder->logCb(logError, _FMT("Either dstCodecId or mediaType is not set. Transcoder will operate in passthrough mode."));
        xcoder->passthrough = 1;
        return 0;
    }

    TRACE(_FMT("Querying codec"));
    size = sizeof(xcoder->srcCodecId);
    if ( default_get_param(stream,
                            "audioCodecId",
                            &xcoder->srcCodecId,
                            &size) < 0 ||
         xcoder->srcCodecId == streamUnknown ) {
        xcoder->logCb(logDebug, _FMT("Failed to determine source 'audioCodecId'. Transcoder operates as passthrough."));
        xcoder->passthrough = 1;
        return 0;
    }

    if ( xcoder->srcCodecId == xcoder->dstCodecId ) {
        xcoder->logCb(logDebug, _FMT("Transcoder will operate as passthrough."));
        xcoder->passthrough = 1;
        return 0;
    }

    size = sizeof(int);
    if (default_get_param((stream_obj*)xcoder,
                        "audioSampleRate",
                        &xcoder->srcSampleRate,
                        &size) < 0 ) {
        xcoder->logCb(logError, _FMT("Failed to determine sample rate."));
        goto Error;
    }


    // we made it that far -- check if we need to add a decoder
    if ( xcoder->dstCodecId != streamLinear &&
         xcoder->srcCodecId != streamLinear ) {
        TRACE(_FMT("Auto-inserting audio decoder"));
        int kMediaAudio = mediaAudio;
        int kCodecLinear = streamLinear;
        std::string elname(_STR(xcoder->name<<".decoder"));
        default_insert_element(&xcoder->source, &xcoder->sourceApi, NULL, get_mfdec_stream_api()->create(elname.c_str()), svFlagStreamInitialized);

        if ( default_set_param(stream, _STR(elname<<".decoderType"), &kMediaAudio) < 0 ||
             default_set_param(stream, _STR(elname<<".dstCodecId"), &kCodecLinear) < 0 ||
             // initialize new element directly
             xcoder->sourceApi->open_in(xcoder->source) < 0 ) {
            xcoder->logCb(logError, _FMT("Failed to auto-insert a decoder object"));
            goto Error;
        }
        xcoder->srcCodecId = streamLinear;
        TRACE(_FMT("Auto-inserting audio decoder - done"));
    }

    if ( xcoder->dstCodecId == streamAAC &&
         xcoder->srcSampleRate != 48000 &&
         xcoder->srcSampleRate != 44100 ) {
        TRACE(_FMT("Auto-inserting audio resampler"));
        int kSampleRate = 48000;
        std::string elname(_STR(xcoder->name<<".resampler"));
        default_insert_element(&xcoder->source, &xcoder->sourceApi, NULL, get_resample_filter_api()->create(elname.c_str()), svFlagStreamInitialized);

        if ( default_set_param(stream, _STR(elname<<".audioSampleRate"), &kSampleRate) < 0 ||
             // initialize new element directly
             xcoder->sourceApi->open_in(xcoder->source) < 0 ) {
            xcoder->logCb(logError, _FMT("Failed to auto-insert a resampler object"));
            goto Error;
        }
        xcoder->srcSampleRate = kSampleRate;
        TRACE(_FMT("Auto-inserting audio resampler - done"));
    }


    xcoder->isEncoder = (xcoder->srcCodecId == streamLinear);
    xcoder->modeName = xcoder->isEncoder?"encoder":"decoder";
    TRACE(_FMT("Opening audio " << xcoder->modeName));


    if ( xcoder->srcCodecId == streamLinear ) {
        // for linear stream we must know sample format
        size = sizeof(xcoder->srcSampleFormat);
        if ( default_get_param(stream,
                                "audioSampleFormat",
                                &xcoder->srcSampleFormat,
                                &size) < 0 ||
             xcoder->srcSampleFormat == streamUnknown ) {
            xcoder->logCb(logDebug, _FMT("Failed to determine source 'audioSampleFormat'. Transcoder operates as passthrough."));
            xcoder->passthrough = 1;
            return 0;
        }
        xcoder->srcBitsPerSample = _mfdec_sample_format_to_bps(xcoder, xcoder->srcSampleFormat);
        if ( xcoder->srcBitsPerSample < 0 ) {
            xcoder->logCb(logError, _FMT("Unexpected value for sample format: " << xcoder->srcSampleFormat));
            xcoder->passthrough = 1;
            return 0;
        }
    } else if ( xcoder->srcCodecId == streamPCMA ||
                xcoder->srcCodecId == streamPCMU ) {
        xcoder->srcBitsPerSample = 8;
    }

    TRACE(_FMT("Preparing the " << xcoder->modeName));
    xcoder->passthrough = 0;
    res = _mfdec_prepare_audio_xcoder(xcoder);
    if ( res < 0 ) {
        goto Error;
    }

#if _DUMP_DEBUG
    xcoder->debugFileIn = fopen(_STR("Y:\\audio-in-"<<xcoder->srcSampleRate), "w+b");
    xcoder->debugFileOut = fopen(_STR("Y:\\audio-out-"<<xcoder->srcSampleRate), "w+b");
#endif

    if (!_mfdec_alloc_sample_out(xcoder, 4096)) {
        res = -1;
        xcoder->logCb(logError, _FMT("Failed to allocate output sample"));
        goto Error;
    }

    TRACE(_FMT("Opened " << xcoder->modeName << " object " << (void*)stream));
    return 0;

Error:
    if ( recoverable ) {
        xcoder->logCb(logWarning, _FMT("Failed to open audio coder. Audio will not be available for this camera."));
        xcoder->dstCodecId = streamUnknown;
        xcoder->errorOccurred = 1;
        xcoder->passthrough = 1;
        res = 0;
    } else {
        xcoder->logCb(logError, _FMT("Failed to open the coder"));
        if ( res != 0 ) {
            mfdec_stream_close(stream);
        }
    }

    return res;
}


//-----------------------------------------------------------------------------
static int         mfdec_stream_seek               (stream_obj* stream,
                                                       INT64_T offset,
                                                       int flags)
{
    DECLARE_STREAM_FF(stream, xcoder);

    int res = default_seek(stream, offset, flags);
    if ( res >= 0 ) {
        xcoder->framesProcessed = 0;
    }
    return res;
}

//-----------------------------------------------------------------------------
static size_t      mfdec_stream_get_width          (stream_obj* stream)
{
    DECLARE_STREAM_FF(stream, xcoder);
    return default_get_width(stream);
}

//-----------------------------------------------------------------------------
static size_t      mfdec_stream_get_height         (stream_obj* stream)
{
    DECLARE_STREAM_FF(stream, xcoder);
    return default_get_height(stream);
}

//-----------------------------------------------------------------------------
static int         mfdec_stream_get_pixel_format   (stream_obj* stream)
{
    DECLARE_STREAM_FF(stream, xcoder);
    return default_get_pixel_format(stream);
}

//-----------------------------------------------------------------------------
#define LOG_ERROR_FRAMES 0
#if LOG_ERROR_FRAMES
static void        _mfdec_pb_cb           ( const char* line, void* ctx )
{
    ((stream_base*)ctx)->logCb(logError, line);
}
#endif

//-----------------------------------------------------------------------------
static basic_frame_obj*
_mfdec_alloc_frame                                (mfdec_stream_obj* xcoder,
                                                    int nRequiredSize)
{
    basic_frame_obj*    frameOut = NULL;
    bool bFree = false;
    if (frameOut == NULL) {
        frameOut = alloc_basic_frame2(MFDEC_STREAM_MAGIC,
                                    nRequiredSize,
                                    xcoder->logCb,
                                    xcoder->fa );
        frameOut->mediaType = mediaAudio;
        if ( frameOut == NULL ) {
            xcoder->logCb(logError, _FMT("Failed to allocate frame of " <<
                                            nRequiredSize << " bytes"));
        }
    } else if ( frameOut->allocSize < nRequiredSize ) {
        xcoder->logCb(logError, _FMT("Frame format change had been detected. Restarting the stream."));
        bFree = true;
    }


    if ( bFree ) {
        frame_unref((frame_obj**)&frameOut);
    }
    return frameOut;
}


//-----------------------------------------------------------------------------
static int         mfdec_stream_read_frame        (stream_obj* stream, frame_obj** frame)
{
    DECLARE_STREAM_FF(stream, xcoder);

    int                 res = -1;
    int                 size = 4096;
    int                 eof = 0;
    frame_obj*          frameIn = NULL;
    frame_api_t*        fapi;
    basic_frame_obj*    frameOut = NULL;
    IMFSample*          inputSample = NULL;
    IMFMediaBuffer      *inputBuffer = NULL, *outputBuffer = NULL;
    IMFSample*          outputSample = NULL;
    HRESULT             hr;
    BYTE*               bufferStart;
    MFT_OUTPUT_DATA_BUFFER outputBufferStruct = { xcoder->outputStreamId, xcoder->outputSample, 0, NULL };
    DWORD               outputStatus = 0;
    LONGLONG            sampleTime;
    DWORD               sampleLength, bufferCount;
    bool                outputPending;
    int64_t             ts;

    if ( xcoder->errorOccurred || xcoder->passthrough ) {
        return default_read_frame(stream, frame);
    }

Retry:
    COM_RELEASE( outputBufferStruct.pEvents );
    COM_RELEASE( inputSample );
    COM_RELEASE( inputBuffer );
    if ( xcoder->mfManagedBuffers ) {
        COM_RELEASE( outputSample );
    }
    frame_unref(&frameIn);
    outputStatus = 0;

    outputPending = xcoder->mft != NULL &&
                    xcoder->mft->GetOutputStatus(&outputStatus) == S_OK &&
                    outputStatus == MFT_OUTPUT_STATUS_SAMPLE_READY;

    if ( !outputPending ) {
        res = default_read_frame(stream, &frameIn);
        if ( res < 0 || frameIn == NULL ) {
            goto Error;
        }

        fapi = frame_get_api(frameIn);

        if ( fapi->get_media_type(frameIn) != mediaAudio ||
             xcoder->passthrough ) {
            // pass frames we don't decode through
            *frame = frameIn;
            return 0;
        }

        size = fapi->get_data_size(frameIn);
        ts   = fapi->get_pts(frameIn);
#if _DUMP_DEBUG
        fwrite(fapi->get_data(frameIn), 1, size, xcoder->debugFileIn);
#endif
        TRACE(_FMT("Read new audio frame, size=" << size << " ts=" << ts << " ptr=" << (void*)frameIn));

        TRACE_C(100, _FMT("Alloc sample in: " << size));
        if (!_mfdec_alloc_sample_in(xcoder, size, &inputSample)) {
            xcoder->logCb(logError, _FMT("Failed to allocate input sample"));
            goto Error;
        }

        _CHECK( inputSample->GetBufferByIndex(0, &inputBuffer),
                _FMT("Failed to obtain input buffer" ) );

        _CHECK( inputBuffer->Lock(&bufferStart, NULL, NULL),
                _FMT("Failed to lock input buffer" ) );

        memcpy(bufferStart, fapi->get_data(frameIn), size);

        _CHECK( inputBuffer->Unlock(),
                _FMT("Failed to unlock input buffer" ) );

        _CHECK( inputBuffer->SetCurrentLength(size),
                _FMT("Failed to set current buffer length" ) );

        // Convert from milliseconds to 100 nanoseconds unit.
        _CHECK( inputSample->SetSampleTime(ts * 10 * 1000),
                _FMT("Failed to set sample time") );

        _CHECK( xcoder->mft->ProcessInput(xcoder->inputStreamId, inputSample, 0),
                _FMT("Failed to process input") );
    }


    TRACE_C(100, _FMT("Attempting to process output: outputStreamId=" << xcoder->outputStreamId));
    if (!xcoder->mfManagedBuffers) {
        // make sure we have big enough output buffer
        int sizeOut = _mfdec_get_output_size(xcoder, size);
        if (!_mfdec_alloc_sample_out(xcoder, sizeOut)) {
            xcoder->logCb(logError, _FMT("Failed to encode a buffer of " << sizeOut << " bytes"));
            goto Error;
        }
        outputBufferStruct = { xcoder->outputStreamId, xcoder->outputSample, 0, NULL };
    } else {
        outputBufferStruct = { xcoder->outputStreamId, NULL, 0, NULL };
    }


    hr = xcoder->mft->ProcessOutput(0, 1, &outputBufferStruct, &outputStatus),

    // Use the returned sample since it can be provided by the MFT.
    outputSample = outputBufferStruct.pSample;

    if ( hr == S_OK ) {

        if (!outputSample)
            goto Retry;


        _CHECK( outputSample->GetSampleTime(&sampleTime),
            _FMT("Failed to get output sample time") );
        // Convert from 100 nanoseconds unit to milliseconds.
        sampleTime /= (10 * 1000);

        _CHECK( outputSample->GetTotalLength(&sampleLength),
            _FMT("Failed to get output sample size") );

        _CHECK( outputSample->GetBufferCount(&bufferCount),
            _FMT("Failed to get output buffer count") );

        TRACE(_FMT("Got output: length="<<sampleLength<<" buffers="<<bufferCount));

        _CHECK( outputSample->GetBufferByIndex(0, &outputBuffer),
                _FMT("Failed to obtain output buffer") );

        _CHECK( outputBuffer->Lock(&bufferStart, NULL, NULL),
                _FMT("Failed to lock output buffer") );

        if ( !frameOut || frameOut->allocSize < sampleLength ) {
            frame_unref((frame_obj**)&frameOut);
            frameOut = _mfdec_alloc_frame(xcoder, sampleLength);
        }
        memcpy(frameOut->data, bufferStart, sampleLength);
#if _DUMP_DEBUG
        fwrite(bufferStart, 1, sampleLength, xcoder->debugFileOut);
#endif
        frameOut->dataSize = sampleLength;


        _CHECK( outputBuffer->Unlock(),
                _FMT("Failed to unlock output buffer") );

        if ( !xcoder->mfManagedBuffers ) {
            // Sample is not provided by the MFT: clear its content.
            _CHECK( outputBuffer->SetCurrentLength(0),
                    _FMT("Failed to reset the buffer") );
        }
        if ( xcoder->framesProcessed == 0 ) {
            xcoder->firstPts = sampleTime;
            frameOut->pts = sampleTime;
        } else {
            frameOut->pts = sampleTime > xcoder->lastOutputPts ? sampleTime : xcoder->lastOutputPts+1;
        }
        xcoder->lastOutputPts = frameOut->pts;
        xcoder->framesProcessed++;
        TRACE(_FMT("Got new frame! id=" << xcoder->framesProcessed <<
                            " pts=" << sampleTime <<
                            " relative=" << sampleTime - xcoder->firstPts <<
                            " size=" << sampleLength <<
                            " size2=" << frameOut->dataSize <<
                            " framep=" << (void*)frameOut ));
    } else if ( hr == MF_E_TRANSFORM_STREAM_CHANGE ) {
        xcoder->logCb(logError, _FMT("Stream had changed"));
        goto Error;
    } else if ( hr == MF_E_TRANSFORM_TYPE_NOT_SET ) {
        xcoder->logCb(logError, _FMT("Transform type not set"));
        goto Error;
    } else if (hr == MF_E_TRANSFORM_NEED_MORE_INPUT) {
        TRACE_C(100, _FMT("Need more input!"));
        goto Retry;
    } else {
        _CHECK(hr, _FMT("Unknown error"));
    }

    res = 0;
Error:
    if (res<0) {
        frame_unref((frame_obj**)&frameOut);
        if (!eof) {
            // we may rewind and restart decoding
            xcoder->errorOccurred++;
        }
    }

    COM_RELEASE( outputBufferStruct.pEvents );
    COM_RELEASE( inputSample );
    COM_RELEASE( inputBuffer );

    if ( xcoder->mfManagedBuffers ) {
        COM_RELEASE(outputSample);
    }

    frame_unref(&frameIn);

    *frame = (frame_obj*)frameOut;
    return res;
 }

//-----------------------------------------------------------------------------
static int         mfdec_stream_close             (stream_obj* stream)
{
    DECLARE_STREAM_FF(stream, xcoder);

    default_close(stream);
    stream_unref(&xcoder->source);

    xcoder->prevCaptureTimeMs = 0;
    return 0;
}

//-----------------------------------------------------------------------------
static void mfdec_stream_destroy         (stream_obj* stream)
{
    DECLARE_STREAM_FF_V(stream, xcoder);
    xcoder->logCb(logTrace, _FMT("Destroying stream object " << (void*)stream));

    COM_RELEASE(xcoder->outputType);
    COM_RELEASE(xcoder->outputSample);
    COM_RELEASE(xcoder->inputType);
    COM_RELEASE(xcoder->mft);
#if _DUMP_DEBUG
    fclose(xcoder->debugFileIn);
    fclose(xcoder->debugFileOut);
#endif
    if ( xcoder->mfplatDll.hDll ) {
        FreeLibrary( xcoder->mfplatDll.hDll );
        xcoder->mfplatDll.hDll = NULL;
    }

    mfdec_stream_close(stream); // make sure all the internals had been freed
    destroy_frame_allocator( &xcoder->fa, xcoder->logCb );
    stream_destroy( stream );
}

bool bInitialized = false;

//-----------------------------------------------------------------------------
SVVIDEOLIB_API stream_api_t*     get_mfdec_stream_api             ()
{
    if ( !bInitialized ) {
        bInitialized = true;
        CoInitialize(NULL);
    }
    return &_g_mfdec_stream_provider;
}


