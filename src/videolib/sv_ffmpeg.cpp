/*****************************************************************************
 *
 * sv_ffmpeg.cpp
 *   A collection of miscellaneous ffmpeg-related methods
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

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <map>
#include <mutex>

extern "C" {
#include <libavutil/opt.h>
#include <libavutil/log.h>
}

#include "logging.h"
#include "sv_os.h"
#include "sv_ffmpeg.h"
#include "buffered_file.h"

static log_fn_t   _ffmpegLogFn = NULL;
static int        _ffmpegLogEnabled = 0;
static sv_mutex* gInitMutex = sv_mutex_create();
static int       gInitCounter = 0;

typedef struct LogSpam {
    const char*     msg;
    int             length;
    int64_t         lastLogged;
    int             count;
} LogSpam;

static int       gSpamInterval = 60*1000; // allow spam at most every minute
static LogSpam gSpam[] = {
    { "Application provided invalid, non monotonically increasing dts", 0, 0, 0 },
    { "Frame num change", 0, 0, 0 },
    { "decode_slice_header error", 0, 0, 0 },
    { "No JPEG data found in image", 0, 0, 0 }
};

static int _filter_message(int lvl, const char* msg, va_list args)
{
    int res = 1;

    // iterate through known spam messages
    for (int nI=0; nI<sizeof(gSpam)/sizeof(LogSpam); nI++) {
        LogSpam& item = gSpam[nI];
        if (!strncmp(item.msg, msg, item.length)) {
            // if matched, see if we need to log it
            sv_mutex_enter(gInitMutex);
            int64_t currentTime = sv_time_get_current_epoch_time();
            int64_t timeDiff = sv_time_get_elapsed_time(item.lastLogged);
            if ( timeDiff > gSpamInterval ) {
                // yup, enough time had passed
                if ( item.count > 0 ) {
                    log_msg(_ffmpegLogFn, lvl, _FMT("The following message occurred " << item.count << " times in the last " << timeDiff/1000 << " seconds"));
                }
                log_impl(_ffmpegLogFn, lvl, msg, args);
                item.lastLogged = currentTime;
                item.count = 0;
            } else {
                item.count++;
            }
            sv_mutex_exit(gInitMutex);
            res = 0;
            break;
        }
    }

    return res;
}

static void _ffmpeg_log_cb(void* avc, int lvl, const char* fmt, va_list args)
{

    switch(lvl) {
    case AV_LOG_PANIC  :
    case AV_LOG_FATAL  : lvl = kLogLevelCritical; break;
    case AV_LOG_ERROR  : lvl = kLogLevelError   ; break;
    case AV_LOG_WARNING: lvl = kLogLevelWarning ; break;
    case AV_LOG_INFO   : lvl = kLogLevelInfo    ; break;
    case AV_LOG_VERBOSE:
    case AV_LOG_DEBUG  :
    default            : lvl = kLogLevelDebug   ; break;
    }
    if ( !_ffmpegLogEnabled && lvl < kLogLevelWarning )
        return;

    // only filter messages when it's repetitive errors;
    // by enabling ffmpeg log, user told us he wants to see everything
    if ( !_ffmpegLogEnabled &&
            !_filter_message(lvl, fmt, args) ) {
        return;
    }
    log_impl(_ffmpegLogFn, lvl, fmt, args);
}

SVVIDEOLIB_API
int  ffmpeg_log_pause()
{
    _ffmpegLogEnabled = 0;
    return 0;
}

SVVIDEOLIB_API
int  ffmpeg_log_resume()
{
    _ffmpegLogEnabled = 1;
    return 0;
}

SVVIDEOLIB_API
int ffmpeg_log_open(log_fn_t logFn, int logBufSize)
{
    char* env;

    // NOTE: always overwrite the old buffer, since we might originate from
    //       parent process which itself already opened logging. Replacing is
    //       not a problem, at least if done in the same thread.
    ffmpeg_log_close();

    for (int nI=0; nI<sizeof(gSpam)/sizeof(LogSpam); nI++) {
        LogSpam& item = gSpam[nI];
        item.length = strlen(item.msg);
    }

    env = getenv("SV_LOG_LEVEL_FFMPEG");
    if (env) {
        int logLevel;
        if (1 == sscanf(env, "%d", &logLevel)) {
            switch(logLevel) {
            case kLogLevelDebug   : logLevel = AV_LOG_DEBUG  ; break;
            case kLogLevelInfo    : logLevel = AV_LOG_INFO   ; break;
            case kLogLevelWarning : logLevel = AV_LOG_WARNING; break;
            case kLogLevelError   : logLevel = AV_LOG_ERROR  ; break;
            case kLogLevelCritical:
            default               : logLevel = AV_LOG_FATAL  ; break;
            }
            av_log_set_level(logLevel);
        }
        log_info(logFn, "FFmpeg log level set to %d (%s)", logLevel, env);
    }
    av_log_set_callback(_ffmpeg_log_cb);
    _ffmpegLogFn = logFn;
    log_info(logFn, "FFmpeg logging active");

    return 0;
}



SVVIDEOLIB_API
void ffmpeg_log_close()
{
    av_log_set_callback(av_log_default_callback);
}

SVVIDEOLIB_API
int  ffmpeg_init()
{
    sv_mutex_enter(gInitMutex);
    if ( gInitCounter++ == 0 ) {
#if LIBAVCODEC_VERSION_MAJOR < 58 // not needed after n4.0
        av_register_all();
#endif
        avformat_network_init();
        avdevice_register_all();
#if LIBAVFILTER_VERSION_MAJOR < 7 // not needed after n4.0
        avfilter_register_all();
#endif
    }
    sv_mutex_exit(gInitMutex);
    return 0;
}

SVVIDEOLIB_API
void ffmpeg_close()
{
    sv_mutex_enter(gInitMutex);
    gInitCounter--;
    sv_mutex_exit(gInitMutex);
}

static int _write_packet(void *opaque, uint8_t *buf, int buf_size)
{
    IBufferedFile* bf = (IBufferedFile*)opaque;
    return bf->write((char*)buf, buf_size) ? buf_size : -1;
}

static int64_t 	_seek(void *opaque, int64_t offset, int whence)
{
    IBufferedFile* bf = (IBufferedFile*)opaque;
    return bf->seek(offset, whence);
}

static std::mutex _gClosedFilesMutex;
static std::map<std::string, IBufferedFile*> _gClosedFiles;

static std::string _pathToName(const std::string& src)
{
    std::string::size_type pos = src.find_last_of("/\\");
    if (pos != std::string::npos) {
        return src.substr(pos);
    }
    return src;
}

SVVIDEOLIB_API
AVIOContext* ffmpeg_create_buffered_io(const char* filename)
{
    static const int _bufferSize = 2048;
    uint8_t* buffer = new uint8_t[_bufferSize];
    IBufferedFile* bf = _CreateBufferedFile(filename);
    bf->setOpaque(buffer);
    AVIOContext* pIOCtx = avio_alloc_context(buffer, _bufferSize, 1, bf, NULL, _write_packet, _seek);
    if ( pIOCtx ) {
        pIOCtx->direct = 1;
    } else {
        delete bf;
    }
    return pIOCtx;
}

SVVIDEOLIB_API
int         ffmpeg_close_buffered_io(AVIOContext* ctx)
{
    IBufferedFile* bf = (IBufferedFile*)ctx->opaque;
    avio_context_free( &ctx );

    uint8_t* buffer = (uint8_t*)bf->getOpaque();
    delete [] buffer;
    bf->setOpaque(nullptr);

    std::lock_guard<std::mutex> guard(_gClosedFilesMutex);
    _gClosedFiles[_pathToName(bf->getName())] = bf;
    return 0;
}

SVVIDEOLIB_API
int         ffmpeg_flush_buffered_io(log_fn_t logFn,
                                    const char* src,
                                    const char* dst)
{
    int          retval = -1;
    IBufferedFile* bf = NULL;


    {
        std::lock_guard<std::mutex> guard(_gClosedFilesMutex);

        auto it = _gClosedFiles.find(_pathToName(src));
        if ( it == _gClosedFiles.end() ) {
            log_err( logFn, "Could not copy the file -- no entry associated with %s", src);
            for ( auto ent: _gClosedFiles ) {
                log_err(logFn, "    entry=%s", ent.first.c_str());
            }
            return retval;
        }

        bf = it->second;

        // whatever happens, this memory buffer is gone
        _gClosedFiles.erase(it);
    }

    if ( bf->save(dst) ) {
        // the only success case
        retval = 0;
    } else if ( bf->save(src) ) {
        // saving to the destination failed; try saving to the filesystem, for future retries
        // it's still a failure from the client's perspective
        log_err( logFn, "Could not save the file to %s -- keeping it in %s", dst, src);
    } else {
        // it really is a disaster, and future retries will fail ... but for client,
        // it's no different from the case above
        log_err( logFn, "Could not save the file to both %s and %s", dst, src);
    }

    // free the memory associated with the buffer
    delete bf;

    return retval;
}

AVRational AVRATIONAL_MS = {1, 1000};