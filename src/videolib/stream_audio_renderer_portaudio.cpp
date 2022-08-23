/*****************************************************************************
 *
 * stream_ffmpeg_renderer_portaudio.cpp
 *   Audio rendering node based on portaudio library.
 *   Used for live audio playback in the native client.
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
#define SV_MODULE_VAR aur
#define SV_MODULE_ID "AUDIO"
#include "sv_module_def.hpp"

#include "streamprv.h"


#include "videolibUtils.h"
#include "portaudio.h"


#include <list>


#define AU_DEMUX_MAGIC 0x1533
#define MAX_PARAM 10

//-----------------------------------------------------------------------------
static int gInitialized = 0;
static const int gPrebuffer = 2;
static bool gFailedInitialization = false;


typedef struct au_stream  : public stream_base  {
    std::list<frame_obj*>*   bufferQueue;
    PaStream*   pa_stream;
    int         sampleRate;
    int         sampleSize;
    int         channels;
    int         threadRunning;
    int         buffersPlayed;
    int         bytesAvailable;
    int         initializationPending;
    int         muted;

    sv_mutex*   mutex;
    sv_event*   event;
    sv_thread*  thread;
} au_stream_obj;

//-----------------------------------------------------------------------------
// Stream API
//-----------------------------------------------------------------------------

//-----------------------------------------------------------------------------
//  Forward declarations
//-----------------------------------------------------------------------------
static stream_obj* au_stream_create             (const char* name);
static int         au_stream_set_param          (stream_obj* stream,
                                                const CHAR_T* name,
                                                const void* value);
static int         au_stream_open_in            (stream_obj* stream);
static int         au_stream_seek               (stream_obj* stream,
                                                INT64_T offset,
                                                int flags);
static int         au_stream_read_frame         (stream_obj* stream, frame_obj** frame);
static int         au_stream_close              (stream_obj* stream);
static void        au_stream_destroy            (stream_obj* stream);

static void       _au_flush_queue               (au_stream_obj* aur);

//-----------------------------------------------------------------------------
static stream_api_t _g_aup_stream_provider = {
    au_stream_create,
    get_default_stream_api()->set_source,
    get_default_stream_api()->set_log_cb,
    get_default_stream_api()->get_name,
    get_default_stream_api()->find_element,
    get_default_stream_api()->remove_element,
    get_default_stream_api()->insert_element,
    au_stream_set_param,
    get_default_stream_api()->get_param,
    au_stream_open_in,
    au_stream_seek,
    get_default_stream_api()->get_width,
    get_default_stream_api()->get_height,
    get_default_stream_api()->get_pixel_format,
    au_stream_read_frame,
    get_default_stream_api()->print_pipeline,
    au_stream_close,
    _set_module_trace_level
} ;

//-----------------------------------------------------------------------------
static void au_init(void)
{
    if ( gInitialized++ > 0 || gFailedInitialization )
        return;

    PaError err = Pa_Initialize();
    if ( err != paNoError ) {
        gInitialized--;
        gFailedInitialization=true;
    }
}

//-----------------------------------------------------------------------------
static void au_close(void)
{
    if ( --gInitialized > 0 )
        return;

    Pa_Terminate();
}

//-----------------------------------------------------------------------------
#define DECLARE_STREAM_AU(stream, name) \
    DECLARE_OBJ(au_stream_obj, name,  stream, AU_DEMUX_MAGIC, -1)

#define DECLARE_STREAM_AU_V(stream, name) \
    DECLARE_OBJ_V(au_stream_obj, name,  stream, AU_DEMUX_MAGIC)

static stream_obj*   au_stream_create                (const char* name)
{
    au_init();

    au_stream_obj* res = (au_stream_obj*)stream_init(sizeof(au_stream_obj),
                AU_DEMUX_MAGIC,
                &_g_aup_stream_provider,
                name,
                au_stream_destroy );

    res->bufferQueue = new std::list<frame_obj*>;

    res->pa_stream = NULL;
    res->buffersPlayed = 0;
    res->bytesAvailable = 0;
    res->sampleRate = -1;
    res->sampleSize = -1;
    res->channels = 1;

    res->event = sv_event_create(0,0);
    res->mutex = sv_mutex_create();
    res->thread = NULL;
    res->threadRunning = 0;
    res->initializationPending = 0;
    res->muted = 0;

    return (stream_obj*)res;
}

//-----------------------------------------------------------------------------
static int         au_stream_set_param             (stream_obj* stream,
                                            const CHAR_T* name,
                                            const void* value)
{
    DECLARE_STREAM_AU(stream, aur);

    name = stream_param_name_apply_scope(stream, name);
    if ( !_stricmp(name, "mute") ) {
        sv_mutex_enter(aur->mutex);
        int muted = *(int*)value;
        if ( muted && (aur->muted ^ muted) ) {
            _au_flush_queue(aur);
            aur->buffersPlayed = 0;
        }
        aur->muted = muted;
        sv_mutex_exit(aur->mutex);
        return 0;
    }
    return default_set_param(stream, name, value);
}

//-----------------------------------------------------------------------------
static void*      _au_thread_func               (void* param)
{
    au_stream_obj*  aur = (au_stream_obj*)param;
    int             playing = 0;

    int             wroteSinceStart = -1;
    sv_stopwatch    totalTimer, opTimer;
    INT64_T         timeInWrite = 0, timeInWait = 0;

    while (aur->threadRunning) {
        frame_obj* f = NULL;
        bool       needsStart = false;
        bool       needsStop = false;
        float      reqFps = 0, capFps = 0;
        size_t     sz = sizeof(float);

        sv_mutex_enter(aur->mutex);
        size_t qSize = aur->bufferQueue->size();
        if ( (playing && qSize>0) ||
             (!playing && qSize>gPrebuffer) ) {
            f = aur->bufferQueue->front();
            aur->bufferQueue->pop_front();
            aur->bytesAvailable -= frame_get_api(f)->get_data_size(f);
            needsStart = (aur->buffersPlayed==0 && !playing);
            aur->buffersPlayed++;

            if ( _gTraceLevel >= 10) {
                aur->sourceApi->get_param(aur->source, "requestFps", &reqFps, &sz);
                aur->sourceApi->get_param(aur->source, "captureFps", &capFps, &sz);
            }

        } else {
            needsStop = (aur->buffersPlayed==0 && playing);
            sv_event_reset(aur->event);
        }
        sv_mutex_exit(aur->mutex);

        if ( f == NULL && aur->threadRunning ) {
            if ( needsStop ) {
                TRACE(_FMT("Aborting audio playback after seek"));
                Pa_AbortStream( aur->pa_stream );
                playing = 0;
            }
            opTimer.reset();
            sv_event_wait(aur->event, 0);
            timeInWait += opTimer.stop();
            TRACE(_FMT("Buffer underrun -- waited " << opTimer.diff() << "ms for the data"));
        } else {
            PaError err;
            if ( needsStart ) {
                TRACE(_FMT("Restarting audio playback after seek"));
                err = Pa_StartStream( aur->pa_stream );
                if ( err != paNoError ) {
                    aur->logCb(logError, _FMT("Error starting audio stream: " << Pa_GetErrorText(err)));
                    frame_unref(&f);
                    break;
                }
                playing = 1;
                wroteSinceStart = 0;
                timeInWrite = 0;
                timeInWait = 0;
                totalTimer.reset();
            }

            char* samples = (char*)frame_get_api(f)->get_data(f);
            int   numSamples = frame_get_api(f)->get_data_size(f)/(aur->sampleSize*aur->channels);
            opTimer.reset();
            err = Pa_WriteStream(aur->pa_stream, samples, numSamples );
            if ( err != paNoError ) {
                aur->logCb((err==paOutputUnderflow?logDebug:logError),
                            _FMT("Error writing samples: " << Pa_GetErrorText(err)));
            }
            wroteSinceStart += numSamples;
            timeInWrite += opTimer.stop();
            TRACE_C(10, _FMT("Pa_WriteStream done: samples=" << numSamples << " samplesSinceStart=" << wroteSinceStart <<
                        " delta=" << wroteSinceStart - ((int)totalTimer.diff()*aur->sampleRate/1000) <<
                        " inWait=" << timeInWait << " inWrite=" << timeInWrite << " inQueue=" << aur->bytesAvailable/aur->sampleSize <<
                        " readFps=" << reqFps << " captureFps=" << capFps ));
            frame_unref(&f);
        }
    }

    if ( playing ) {
        Pa_AbortStream( aur->pa_stream );
        playing = 0;
    }
    aur->threadRunning = 0;
    return NULL;
}

//-----------------------------------------------------------------------------
static int         _au_attempt_init                 (au_stream_obj* aur,
                                                     bool secondAttempt)
{
    int                 channels, sampleRate, sampleFormat, sampleSize,
                        interleaved;
    size_t              size;
    std::ostringstream  err;
    int                 canRecover = 1;


    size = sizeof(channels);
    if ( aur->sourceApi->get_param(aur->source, "audioChannels", &channels, &size) < 0 ) {
        err << "Audio will be muted - cannot determine number of channels";
        goto Error;
    }

    size = sizeof(sampleRate);
    if ( aur->sourceApi->get_param(aur->source, "audioSampleRate", &sampleRate, &size) < 0 ) {
        err << "Failed to determine sample rate";
        goto Error;
    }

    size = sizeof(sampleFormat);
    if ( aur->sourceApi->get_param(aur->source, "audioSampleFormat", &sampleFormat, &size) < 0 ) {
        err << "Failed to determine sample rate";
        goto Error;
    }

    size = sizeof(interleaved);
    if ( aur->sourceApi->get_param(aur->source, "audioInterleaved", &interleaved, &size) < 0 ) {
        err << "Failed to determine if audio is interleaved";
        goto Error;
    }
    if ( !interleaved ) {
        err << "Audio renderer requires interleaved input";
        goto Error;
    }

    switch (sampleFormat)
    {
    case sfmtFloat: sampleFormat = paFloat32; sampleSize = 4; break;
    case sfmtInt32: sampleFormat = paInt32;   sampleSize = 4; break;
    case sfmtInt16: sampleFormat = paInt16;   sampleSize = 2; break;
    case sfmtInt8:  sampleFormat = paInt8;    sampleSize = 1; break;
    default:
        err << "Unsuppored sample format: " << sampleFormat;
        goto Error;
    }


    if ( gFailedInitialization ) {
        err << "Failed to initialized audio device";
        goto UnrecoverableError;
    }

    PaStreamParameters outputParameters;
    PaError paError;

    outputParameters.device = Pa_GetDefaultOutputDevice(); /* default output device */
    if (outputParameters.device == paNoDevice) {
        err << "Failed to find default audio device";
        goto UnrecoverableError;
    }
    outputParameters.channelCount = channels;
    outputParameters.sampleFormat = sampleFormat;
    outputParameters.suggestedLatency = Pa_GetDeviceInfo( outputParameters.device )->defaultLowOutputLatency;
    outputParameters.hostApiSpecificStreamInfo = NULL;

    paError = Pa_OpenStream(
                &aur->pa_stream,
                NULL, /* no input */
                &outputParameters,
                sampleRate,
                paFramesPerBufferUnspecified,
                paClipOff,      /* we won't output out of range samples so don't bother clipping them */
                NULL,
                NULL );
    if ( paError != paNoError ) {
        err << "Failed to create audio device for sample rate " << sampleRate << " sampleFormat=" << sampleFormat << " channels=" << channels << " err=" << paError;
        goto UnrecoverableError;
    }

    aur->threadRunning = 1;
    aur->sampleRate = sampleRate;
    aur->sampleSize = sampleSize;
    aur->initializationPending = 0;
    aur->channels = channels;
    aur->thread = sv_thread_create(_au_thread_func, aur);
    if (!aur->thread) {
        err << "Error creating audio playback thread";
        goto UnrecoverableError;
    }

    aur->passthrough = 0;
    TRACE(_FMT("Initialization completed: sampleRate="<<sampleRate<<" sampleSize="<<sampleSize<< " channels=" << channels));
    return 0;

UnrecoverableError:
    canRecover = 0;

Error:
    if (secondAttempt || !canRecover) {
        aur->logCb(logError, _FMT("Audio stream will be muted: " << err.str()));
        aur->initializationPending = 0;
        aur->passthrough = 1;
        return -1;
    }

    TRACE(_FMT("Initialization will be attempted when first frame is received"));
    aur->initializationPending = 1;
    aur->passthrough = 0;
    return 0;
}

//-----------------------------------------------------------------------------
static int         au_stream_open_in                (stream_obj* stream)
{
    DECLARE_STREAM_AU(stream, aur);
    int res = default_open_in(stream);
    if ( res < 0 ) {
        return res;
    }

    int     codec;
    size_t  size;

    size = sizeof(codec);
    bool hasAudio = (default_get_param(stream, "audioCodecId", &codec, &size) >= 0);
    if ( !hasAudio ) {
        TRACE(_FMT("Stream doesn't have audio"));
        return 0;
    }

    if ( codec != streamLinear ) {
        if ( codec != streamUnknown ) {
            aur->logCb(logInfo, _FMT("Audio will be muted - incompatible codec: " << codec));
        }
        return 0;
    }

    _au_attempt_init(aur, false);
    return 0;
}

//-----------------------------------------------------------------------------
static int         au_stream_seek               (stream_obj* stream,
                                                INT64_T offset,
                                                int flags)
{
    DECLARE_STREAM_AU(stream, aur);

    int res;
    sv_mutex_enter(aur->mutex);
    _au_flush_queue(aur);
    aur->buffersPlayed = 0;
    res = default_seek(stream, offset, flags);
    sv_mutex_exit(aur->mutex);

    return res;
}

//-----------------------------------------------------------------------------
static int         au_stream_read_frame         (stream_obj* stream, frame_obj** frame)
{
    DECLARE_STREAM_AU(stream, aur);

    frame_obj* tmp = NULL;
    int res = default_read_frame(stream, frame);
    if ( aur->passthrough || res < 0 || *frame == NULL ) {
        return res;
    }

    frame_api_t* tmpFrameAPI = frame_get_api(*frame);
    if ( tmpFrameAPI->get_media_type(*frame) == mediaAudio ) {
        if ( aur->initializationPending ) {
            _au_attempt_init(aur, true);
            if ( aur->passthrough ) {
                return res;
            }
        }

        int size = tmpFrameAPI->get_data_size(*frame);


        sv_mutex_enter(aur->mutex);
        if ( !aur->muted ) {
            frame_ref(*frame);
            aur->bufferQueue->push_back(*frame);
            aur->bytesAvailable += frame_get_api(*frame)->get_data_size(*frame);
            sv_event_set(aur->event);
        }
        sv_mutex_exit(aur->mutex);
    }

    return 0;
}

//-----------------------------------------------------------------------------
static void       _au_flush_queue               (au_stream_obj* aur)
{
    while (!aur->bufferQueue->empty()) {
        frame_obj* f = aur->bufferQueue->front();
        aur->bufferQueue->pop_front();
        frame_unref(&f);
    }
    aur->bytesAvailable = 0;
}

//-----------------------------------------------------------------------------
static int         au_stream_close              (stream_obj* stream)
{
    DECLARE_STREAM_AU(stream, aur);

    if (!aur->event || !aur->mutex) {
        // close may be called twice -- don't crash if it happens
        return 0;
    }

    PaError err = paNoError;

    // ensure thread exists before we proceed with destroying things
    aur->threadRunning = 0;
    if ( aur->pa_stream != NULL ) {
        // portaudio seems to have a deadlock (infinite busy wait), if
        // Pa_AbortStream is called on OSX while Pa_WriteStream is
        // in progress.
        // As a result, we won't try to abort the stream, rather waiting
        // for playout of the buffer to complete

        // Pa_AbortStream( aur->pa_stream );
    }
    sv_event_set(aur->event);
    sv_thread_destroy(&aur->thread);

    if ( aur->bufferQueue ) {
        _au_flush_queue( aur );
        delete aur->bufferQueue;
        aur->bufferQueue = NULL;
    }

    if ( aur->pa_stream != NULL ) {
        err = Pa_CloseStream( aur->pa_stream );
        aur->pa_stream = NULL;
        if ( err != paNoError ) {
            aur->logCb(logError, _FMT("Error closing stream: " << Pa_GetErrorText (err) ));
        }
    }

    sv_event_destroy(&aur->event);
    sv_mutex_destroy(&aur->mutex);
    return 0;
}

//-----------------------------------------------------------------------------
static void au_stream_destroy         (stream_obj* stream)
{
    DECLARE_STREAM_AU_V(stream, aur);
    aur->logCb(logTrace, _FMT("Destroying stream object " << (void*)stream));
    au_stream_close(stream );
    stream_destroy( stream );
    au_close();
}

//-----------------------------------------------------------------------------
SVVIDEOLIB_API stream_api_t*     get_audio_renderer_pa_api                    ()
{
    return &_g_aup_stream_provider;
}


