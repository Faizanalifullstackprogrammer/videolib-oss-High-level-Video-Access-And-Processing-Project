/*****************************************************************************
 *
 * sv_os.cpp
 *   OS, time, file-system, etc primitives used by videolib.
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

#ifdef WIN32
#include <Shlwapi.h>
#include <process.h>
#else
#include <sys/time.h>
#include <sys/mman.h>
#include <errno.h>
#include <libgen.h>
#include <dlfcn.h>
#endif
#include <atomic>
#include <shared_mutex>


//-----------------------------------------------------------------------------
#ifdef _WIN32
char *sv_strcasestr(char *str, const char *sub)
{
    size_t len_str;
    size_t len_sub;

    if (!str || !sub)
        return NULL;

    len_str = strlen(str);
    len_sub = strlen(sub);

    char* start = str;
    bool  found = true;
    while (len_str >= len_sub) {
        for (size_t nI=0; nI<=len_sub; nI++)
            if ( tolower(start[nI]) != tolower(sub[nI]) ) {
                found = false;
                break;
            }
        if ( found ) {
            return start;
        }

        found = true;
        start ++;
        len_str--;
    }
    return NULL;
}
#endif

//-----------------------------------------------------------------------------
SVCORE_API UINT64_T sv_time_get_current_epoch_time()
{
	UINT64_T 		result;
    struct timeval nowTime;
#ifdef WIN32
    static const UINT64_T kEpoch = ((UINT64_T) 116444736000000000ULL);

    SYSTEMTIME  sysTime;
    FILETIME    fileTime;
    UINT64_T    epochTime;

    GetSystemTime( &sysTime );
    SystemTimeToFileTime( &sysTime, &fileTime );
    epochTime =  ((UINT64_T)fileTime.dwLowDateTime )      ;
    epochTime += ((UINT64_T)fileTime.dwHighDateTime) << 32;

    nowTime.tv_sec  = (long) ((epochTime - kEpoch) / 10000000L);
    nowTime.tv_usec = (long) (sysTime.wMilliseconds * 1000);
#else
    gettimeofday(&nowTime,NULL);
#endif
    result = sv_time_timeval_to_ms(&nowTime);
    return result;
}

//-----------------------------------------------------------------------------
SVCORE_API UINT64_T sv_time_get_elapsed_time(UINT64_T from)
{
	return sv_time_get_time_diff(from,
					sv_time_get_current_epoch_time() );
}

//-----------------------------------------------------------------------------
SVCORE_API UINT64_T sv_time_get_time_diff(UINT64_T from, UINT64_T to)
{
	if (from > to) return 0;
	return to - from;
}

//-----------------------------------------------------------------------------
SVCORE_API UINT64_T sv_time_timeval_to_ms(struct timeval* t)
{
    return ((UINT64_T)t->tv_sec)*1000 + t->tv_usec/1000;
}

//-----------------------------------------------------------------------------
SVCORE_API void     sv_time_ms_to_timeval(UINT64_T ms, struct timeval* time)
{
	time->tv_sec  = ms/1000;
	time->tv_usec = (ms%1000)*1000;
}

//-----------------------------------------------------------------------------
SVCORE_API UINT64_T sv_time_get_timeval_diff_ms(struct timeval* from,
											struct timeval* to)
{
	UINT64_T toMs = sv_time_timeval_to_ms(to),
			 fromMs = sv_time_timeval_to_ms(from);

	if ( toMs < fromMs )
		return 0;
	return toMs - fromMs;
}

//-----------------------------------------------------------------------------
ATOMIC_T   sv_atomic_inc(ATOMIC_T* val)
{
#ifdef WIN32
    return InterlockedIncrement(val);
#else
    return __sync_add_and_fetch(val, 1);
#endif
}

//-----------------------------------------------------------------------------
SVCORE_API ATOMIC_T   sv_atomic_dec(ATOMIC_T* val)
{
#ifdef WIN32
    return InterlockedDecrement(val);
#else
    return __sync_sub_and_fetch(val, 1);
#endif
}

//-----------------------------------------------------------------------------
SVCORE_API ATOMIC_T   sv_atomic_add(ATOMIC_T* val, ATOMIC_T toAdd)
{
#ifdef WIN32
    return InterlockedExchangeAdd(val, toAdd);
#else
    return __sync_add_and_fetch(val, toAdd);
#endif
}

//-----------------------------------------------------------------------------
SVCORE_API THREADID_T sv_get_thread_id()
{
#ifdef WIN32
	return GetCurrentThreadId();
#else
	return pthread_self();
#endif
}

//-----------------------------------------------------------------------------
SVCORE_API void       sv_sleep(UINT64_T ms)
{
#ifdef WIN32
    Sleep(ms);
#else
    static const int kMSecInUsec = 1000;
    usleep(ms*kMSecInUsec);
#endif
}

//-----------------------------------------------------------------------------
#ifdef _WIN32
static std::wstring strtowstr(const char* cstr)
{
    // Convert an ASCII string to a Unicode String
    std::string  str = cstr;
    std::wstring wstrTo;
    wchar_t *wszTo = new wchar_t[str.length() + 1];
    wszTo[str.size()] = L'\0';
    MultiByteToWideChar(CP_UTF8, 0, str.c_str(), -1, wszTo, (int)str.length());
    wstrTo = wszTo;
    delete[] wszTo;
    return wstrTo;
}
#endif

//-----------------------------------------------------------------------------
SVCORE_API FILE*     sv_open_file(const char* name, const char* mode)
{
#ifdef _WIN32
    return _wfopen(strtowstr(name).c_str(), strtowstr(mode).c_str());
#else
    return fopen(name, mode);
#endif
}

//-----------------------------------------------------------------------------
SVCORE_API int       sv_rename_file(const char* src, const char* dst)
{
#ifdef WIN32
    std::wstring srcStr = strtowstr(src);
    std::wstring dstStr = strtowstr(dst);

    return (MoveFileExW(srcStr.c_str(), dstStr.c_str(), MOVEFILE_COPY_ALLOWED|
                                MOVEFILE_REPLACE_EXISTING|
                                MOVEFILE_WRITE_THROUGH) != 0 ? 0 : -1);
#else
    return (rename(src, dst) == 0 ? 0 : -1);
#endif
}

//-----------------------------------------------------------------------------
SVCORE_API int       sv_get_env_var(const char* name, char* value, size_t* bufSize)
{
#ifndef WIN32
	const char* res = getenv(name);
	if ( res == NULL ) {
		*bufSize = 0;
		return -1;
	}
	size_t len = strlen(res);
	if ( len >= *bufSize ) {
		*bufSize = len+1;
		return -1;
	}
	strcpy(value, res);
	return 0;

#else
	DWORD res = GetEnvironmentVariable(name, value, *bufSize);
	if ( res > *bufSize ) {
		*bufSize = res;
		return -1;
	}
	if ( res == 0 ) {
		*bufSize = 0;
		return -1;
	}
	return 0;
#endif
}

//-----------------------------------------------------------------------------
SVCORE_API int       sv_get_int_env_var(const char* name, int defaultVal)
{
    char buffer[128];
    int  res = defaultVal;
    size_t size = sizeof(buffer);
    if ( sv_get_env_var(name, buffer, &size) >= 0 ) {
        char*   endPtr = NULL;
        long    nVal = strtol(buffer, &endPtr, 10);
        if (!nVal) {
            if (endPtr == NULL || *endPtr != '\0' || endPtr == buffer ) {
                nVal = defaultVal;
            }
        }
        res = nVal;
    }
    return res;
}

//-----------------------------------------------------------------------------
SVCORE_API int       sv_get_trace_level(const char* varName)
{
    return sv_get_int_env_var(varName, 0);
}

//-----------------------------------------------------------------------------
// Look for URL of form
// .*//.*/.*
SVCORE_API const char* sv_sanitize_uri(const char* uri,
                                    char* buffer,
                                    size_t bufSize)
{
    // sanity checks
    if ( buffer == NULL || bufSize == 0 ) {
        return NULL;
    }

    if ( uri == NULL ) {
        uri = "NULL";
    }

    // make a copy
    strncpy(buffer, uri, bufSize-1);
    buffer[bufSize-1] = '\0';

    int nPos = 0;
    int nLen = strlen(buffer);
    bool bReplacing = false;
    bool bPrevSlash = false;
    while (nPos<nLen) {
        char ch = buffer[nPos];
        // determine whether we need to start or stop replacing chars
        if ( ch == '?' ) {
            break;
        } else
        if ( ch == '/' ) {
            if ( bReplacing && !bPrevSlash ) {
                // we were in the 'host' portion of the URI,
                // and encountered '/' -- nothing else to replace
                break;
            }
            if ( bPrevSlash ) {
                // at least two slashes seen consecutively -- time to start replacing
                bReplacing = true;
            }
            bPrevSlash = true;
        } else {
            if ( bReplacing && bPrevSlash ) {
                // we've just started replacing, and this is first non-slash char
                // determine if we have u:p ...
                const char* atPtr    = strchr(&buffer[nPos], '@');

                if ( atPtr != NULL ) {
                    // sanitize u/p, if present
                    const char* slashPtr = strchr(&buffer[nPos], '/');
                    const char* qmPtr    = strchr(&buffer[nPos], '?');
                    const char* termPtr;

                    if ( slashPtr != NULL && qmPtr != NULL ) {
                        termPtr = (slashPtr<qmPtr)?slashPtr:qmPtr;
                    } else if (slashPtr != NULL) {
                        termPtr = slashPtr;
                    } else if (qmPtr != NULL) {
                        termPtr = qmPtr;
                    } else {
                        termPtr = &buffer[nLen-1];
                    }

                    if ( atPtr < termPtr ) {
                        int atPos = atPtr - buffer;
                        // we have u/p, lets sanitize it
                        if (atPos-nPos>=3) {
                            buffer[nPos++] = 'u';
                            buffer[nPos++] = ':';
                            buffer[nPos++] = 'p';
                            nLen -= (atPos-nPos);
                            memmove(&buffer[nPos], atPtr, strlen(atPtr)+1);
                        }

                        // after the replacement, move on to the next char
                        // (which may be '@', unless we have only a very short username)
                        ch = buffer[nPos];
                    }
                }
            }
            bPrevSlash = false;
        }


        if ( bReplacing ) {
            if ( ch != '/' &&
                 ch != '.' &&
                 ch != '@' &&
                 ch != ':' ) {
                buffer[nPos] = 'x';
            }
        }

        nPos++;
    } // while

    return buffer;
}

//-----------------------------------------------------------------------------
typedef struct sv_mmap   {
    char*           filename;
    size_t          size;
    uint8_t*        mmap;
#ifdef _WIN32
    HANDLE          handle;
    HANDLE          handleFile;
#else
    FILE*           handle;
#endif
    char            storage[1];
} sv_mmap;

//-----------------------------------------------------------------------------
SVCORE_API sv_mmap*  sv_open_mmap(const char* location, size_t size)
{
    sv_mmap* res = (sv_mmap*)malloc(sizeof(sv_mmap)+strlen(location)+1);
    res->size = size;
    res->filename = &res->storage[0];
    res->handle = NULL;
    res->mmap = NULL;
    strcpy(res->filename, location);


    char zeros[4096];
    memset(zeros, 0, sizeof(zeros));
    int zeroCount = res->size - 1;

#ifndef _WIN32

    // Attempt to open the file.
    res->handle = fopen(res->filename, "r+b");
    if (!res->handle) {
        res->handle = fopen(res->filename, "w+b");
    }
    if (res->handle) {
        // Seek to the beginning and zero the file. Code elsewhere will delete the
        // file if the entire header is zeroed.  For some reason in python we were
        // able to create a file, zero and flush it before writing the frame and
        // the other process wouldn't delete it, but when moved to c this doesn't
        // seem to be the case.  Set a non-zero bit to prevent this.
        fseek(res->handle, 0, SEEK_SET);
        fputc(0x01, res->handle);
        for (int i = 0; i < zeroCount / sizeof(zeros); i++)
            fwrite(zeros, 1, sizeof(zeros), res->handle);
        fwrite(zeros, 1, zeroCount % sizeof(zeros), res->handle);
        fflush(res->handle);

        res->mmap = (uint8_t*)mmap(0,
                        res->size,
                        PROT_READ | PROT_WRITE, MAP_SHARED,
                        fileno(res->handle),
                        0);
    }


#else
    res->handleFile = NULL;

    // Create or open the file which is then mapped into memory (and thus
    // shared). In case of creation we need to make sure that the other side
    // has full access to it. In case that the back-end running in session 0
    // created the file the fact that it is placed in a globally accessible spot
    // allows us (in a regular user session) to open it with all of the required
    // privileges.
    WCHAR filenameW[MAX_PATH] = { 0 };
    if (MultiByteToWideChar(CP_UTF8, 0, res->filename, -1, filenameW, MAX_PATH)) {
        res->handleFile = CreateFileW(filenameW,
                GENERIC_READ | GENERIC_WRITE,
                FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                NULL,
                OPEN_ALWAYS,
                FILE_ATTRIBUTE_NORMAL,
                NULL);
    }

    if ( res->handleFile ) {
        // Clear the file first.
        DWORD written;
        char one = 1;
        WriteFile(res->handleFile, &one, 1, &written, NULL);
        for (int i = 0; i < zeroCount / sizeof(zeros); i++) {
            WriteFile(res->handleFile, zeros, sizeof(zeros), &written, NULL);
        }
        WriteFile(res->handleFile, zeros, zeroCount % sizeof(zeros), &written, NULL);
        FlushFileBuffers(res->handleFile);
        // Now map the file into memory.
        res->handle = CreateFileMapping(res->handleFile, NULL, PAGE_READWRITE, 0,
                res->size, NULL);
        if (res->handle) {
            res->mmap = (uint8_t*)MapViewOfFile(res->handle, FILE_MAP_WRITE, 0, 0, 0);
        }
    }
#endif // _WIN32

    if (!res->handle || !res->mmap) {
        sv_close_mmap(&res);
    }
    return res;
}

//-----------------------------------------------------------------------------
SVCORE_API void      sv_close_mmap(sv_mmap** pMap)
{
    if (pMap == NULL || *pMap == NULL) {
        return;
    }
    sv_mmap* res = *pMap;

    if (res->mmap) {
        // kinda silly, but the client side currently won't let go until
        // if gets what is perceived as invalid data ... and will fail to remove the file
        memset(res->mmap, 0, res->size);
    }
#ifdef _WIN32
    if ( res->mmap ) {
        UnmapViewOfFile(res->mmap);
    }
    if ( res->handle ) {
        CloseHandle(res->handle);
    }
    if ( res->handleFile ) {
        CloseHandle(res->handleFile);
    }
#else
    if ( res->mmap ) {
        munmap(res->mmap, res->size);
    }
    if ( res->handle ) {
        fclose(res->handle);
    }
#endif
    free ( res );
    *pMap = NULL;
}

//-----------------------------------------------------------------------------
SVCORE_API uint8_t*  sv_mmap_get_ptr(sv_mmap* pMap)
{
    return pMap ? pMap->mmap : NULL;
}

//-----------------------------------------------------------------------------
SVCORE_API size_t    sv_mmap_get_size(sv_mmap* pMap)
{
    return pMap ? pMap->size : 0;
}

//-----------------------------------------------------------------------------
SVCORE_API void      sv_print_buffer(char* buf,
                                size_t size, fn_sv_print_buffer_line_cb cb,
                                void* ctx)
{
    static const int kCharsPerRow = 16;
    static const int kNumColumns  = 4;
    static const char* kNumFormat = "%02x ";
    static const char* kNumHolder = "   ";
    static const int kNumFormatLen = 3;
    static const char* kCharFormat = "%c";
    static const int kCharFormatLen = 1;
    static const char* kOffsetFormat = "%04d: ";
    static const int kOffsetFormatLen = 6;
    static const int kMaxCharsPerCol = kCharsPerRow/kNumColumns;

    char hexLine[1024];
    int  hexLineSize = 0;
    char ascLine[1024];
    int  ascLineSize = 0;
    int  pos = 0;

    size_t roundUp = (size/kCharsPerRow)*kCharsPerRow;
    if (size%kCharsPerRow > 0) {
        roundUp += kCharsPerRow;
    }
    while (pos<=roundUp) {
        if ( (pos%kCharsPerRow)==0 ) {
            if (pos!=0) {
                cb(_STR(hexLine<<"\t|\t"<<ascLine), ctx);
            }
            sprintf(hexLine, kOffsetFormat, pos);
            hexLineSize = kOffsetFormatLen;
            ascLineSize = 0;
        }

        unsigned char ch = (pos<size)?buf[pos]:' ';

        if (pos<size) {
            sprintf(&hexLine[hexLineSize], kNumFormat, ch);
            hexLineSize += kNumFormatLen;
        } else {
            strcpy(&hexLine[hexLineSize], kNumHolder);
            hexLineSize += kNumFormatLen;
        }
        if (isprint(ch)) {
            sprintf(&ascLine[ascLineSize], kCharFormat, ch);
        } else {
            sprintf(&ascLine[ascLineSize], kCharFormat, '.');
        }
        ascLineSize += kCharFormatLen;

        pos++;

        if ((pos%kMaxCharsPerCol) == 0) {
            sprintf(&hexLine[hexLineSize++], " " );
            sprintf(&ascLine[ascLineSize++], " " );
        }
    }
}

//-----------------------------------------------------------------------------
typedef struct sv_mutex   {
#ifdef _WIN32
    CRITICAL_SECTION    cs;
#else
    pthread_mutexattr_t pt_mutex_attr;
    bool                pt_mutex_attr_inited;
    pthread_mutex_t     pt_mutex;
#endif
    bool                mutex_inited;
} sv_mutex;
typedef struct sv_event   {
#ifdef _WIN32
    HANDLE          hEvent;
#else
    pthread_mutex_t pt_mutex;
    pthread_cond_t  pt_cond;
    bool            triggered;
    bool            autoreset;
#endif
} sv_event;
typedef struct sv_thread  {
#ifndef _WIN32
    pthread_t       pt_thread;
#else
    HANDLE          hThread;
    unsigned        threadId;
#endif
    bool            threadRunning;
    sv_thread_func  entry_point;
    void*           context;
} sv_thread;
typedef struct sv_rwlock  {
    bool                rwlock_inited;
#if SV_HAS_PTHREADS
    pthread_rwlock_t    pt_rwlock;
#else
    std::shared_mutex   cpp_rwlock;
    std::atomic_int     writers_waiting;
#endif
} sv_rwlock;

//-----------------------------------------------------------------------------
SVCORE_API sv_rwlock* sv_rwlock_create()
{
    sv_rwlock* res = new sv_rwlock;
    res->rwlock_inited = false;

#if SV_HAS_PTHREADS
    if ( pthread_rwlock_init(&res->pt_rwlock, NULL) != 0 ) {
        goto Error;
    }
#else
    res->writers_waiting = 0;
#endif
    res->rwlock_inited = true;

Error:
    if ( res && !res->rwlock_inited ) {
        sv_rwlock_destroy(&res);
    }
    return res;
}

//-----------------------------------------------------------------------------
SVCORE_API void       sv_rwlock_destroy(sv_rwlock** rwlock)
{
    if (rwlock) {
        sv_rwlock* val = *rwlock;
        if ( val ) {
            if ( val->rwlock_inited ) {
#if SV_HAS_PTHREADS
                pthread_rwlock_destroy(&val->pt_rwlock);
#endif
                val->rwlock_inited = false;
            }
            delete val;
        }
        *rwlock = NULL;
    }
}

//-----------------------------------------------------------------------------
SVCORE_API void       sv_rwlock_lock_read(sv_rwlock* rwlock)
{
#if SV_HAS_PTHREADS
    pthread_rwlock_rdlock(&rwlock->pt_rwlock);
#else
    // Seems like shared_lock implementation on windows causes writer starvation.
    // In videolib's specific scenario, writers are rare, and are relation to initialization
    // and config. Reader sits in a loop, reading frames while holding the read lock
    // (thus preventing changes to the pipeline while a read is in progress)
    // Under such conditions, we want to give writers a preference.
    int waited = 0, period = 1;
    const int kMaxWait = 20;
    while ( rwlock->writers_waiting > 0 && waited < kMaxWait) {
        sv_sleep(period);
        waited += period;
        period *= 2;
    }
    rwlock->cpp_rwlock.lock_shared();
#endif
}

//-----------------------------------------------------------------------------
SVCORE_API void       sv_rwlock_lock_write(sv_rwlock* rwlock)
{
#if SV_HAS_PTHREADS
    pthread_rwlock_wrlock(&rwlock->pt_rwlock);
#else
    rwlock->writers_waiting++;
    rwlock->cpp_rwlock.lock();
    rwlock->writers_waiting--;
#endif
}

//-----------------------------------------------------------------------------
SVCORE_API void       sv_rwlock_unlock_read(sv_rwlock* rwlock)
{
#if SV_HAS_PTHREADS
    pthread_rwlock_unlock(&rwlock->pt_rwlock);
#else
    rwlock->cpp_rwlock.unlock_shared();
#endif
}

//-----------------------------------------------------------------------------
SVCORE_API void       sv_rwlock_unlock_write(sv_rwlock* rwlock)
{
#if SV_HAS_PTHREADS
    pthread_rwlock_unlock(&rwlock->pt_rwlock);
#else
    rwlock->cpp_rwlock.unlock();
#endif
}

//-----------------------------------------------------------------------------
SVCORE_API sv_mutex* sv_mutex_create()
{
    sv_mutex* res = new sv_mutex;
#ifndef _WIN32
    res->mutex_inited = false;
    res->pt_mutex_attr_inited = false;

    if ( pthread_mutexattr_init(&res->pt_mutex_attr) != 0 ) {
        goto Error;
    }
    res->pt_mutex_attr_inited = true;

    if ( pthread_mutexattr_settype(&res->pt_mutex_attr, PTHREAD_MUTEX_RECURSIVE) != 0) {
        goto Error;
    }
    if ( pthread_mutex_init(&res->pt_mutex, &res->pt_mutex_attr) != 0 ) {
        goto Error;
    }
    res->mutex_inited = true;
Error:
    if ( res && !res->mutex_inited ) {
        sv_mutex_destroy(&res);
    }
#else
    InitializeCriticalSection(&res->cs);
    res->mutex_inited = true;
#endif

    return res;
}

//-----------------------------------------------------------------------------
SVCORE_API void      sv_mutex_destroy(sv_mutex** mutex)
{
    if (mutex) {
        sv_mutex* val = *mutex;
        if ( val ) {
            if ( val->mutex_inited ) {
#ifdef _WIN32
                DeleteCriticalSection(&val->cs);
#else
                pthread_mutex_destroy(&val->pt_mutex);
#endif
                val->mutex_inited = false;
            }
#ifndef _WIN32
            if ( val->pt_mutex_attr_inited ) {
                pthread_mutexattr_destroy(&val->pt_mutex_attr);
                val->pt_mutex_attr_inited = false;
            }
#endif
            delete val;
        }
        *mutex = NULL;
    }
}
//-----------------------------------------------------------------------------
SVCORE_API void      sv_mutex_enter(sv_mutex* mutex)
{
#ifdef _WIN32
    EnterCriticalSection(&mutex->cs);
#else
    pthread_mutex_lock(&mutex->pt_mutex);
#endif
}
//-----------------------------------------------------------------------------
SVCORE_API void      sv_mutex_exit(sv_mutex* mutex)
{
#ifdef _WIN32
    LeaveCriticalSection(&mutex->cs);
#else
    pthread_mutex_unlock(&mutex->pt_mutex);
#endif
}


//-----------------------------------------------------------------------------
SVCORE_API sv_event* sv_event_create(int autoreset, int triggered_at_start)
{
    sv_event* res = new sv_event;
#ifdef _WIN32
    res->hEvent = CreateEvent( NULL,
                    autoreset ? FALSE : TRUE,
                    triggered_at_start ? TRUE : FALSE,
                    NULL );
#else
    pthread_mutex_init(&res->pt_mutex, NULL);
    pthread_cond_init(&res->pt_cond, 0);
    res->triggered = (triggered_at_start!=0);
    res->autoreset = (autoreset!=0);
#endif
    return res;

}

//-----------------------------------------------------------------------------
SVCORE_API void      sv_event_destroy(sv_event** pEv)
{
    if (pEv && *pEv) {
        sv_event* ev = *pEv;
#ifdef _WIN32
        CloseHandle(ev->hEvent);
#else
        pthread_mutex_destroy(&ev->pt_mutex);
        pthread_cond_destroy(&ev->pt_cond);
#endif
        delete ev;
        *pEv = NULL;
    }
}

//-----------------------------------------------------------------------------
SVCORE_API void      sv_event_set(sv_event* ev)
{
#ifdef _WIN32
    SetEvent(ev->hEvent);
#else
    pthread_mutex_lock(&ev->pt_mutex);
    ev->triggered = true;
    pthread_cond_signal(&ev->pt_cond);
    pthread_mutex_unlock(&ev->pt_mutex);
#endif
}

//-----------------------------------------------------------------------------
SVCORE_API void      sv_event_reset(sv_event* ev)
{
#ifdef _WIN32
    ResetEvent(ev->hEvent);
#else
    pthread_mutex_lock(&ev->pt_mutex);
    ev->triggered = false;
    pthread_mutex_unlock(&ev->pt_mutex);
#endif
}

//-----------------------------------------------------------------------------
SVCORE_API int       sv_event_wait(sv_event* ev, int timeoutMs)
{
    int    result = 0;

#ifdef _WIN32
    DWORD res = WaitForSingleObject(ev->hEvent, timeoutMs ? timeoutMs : INFINITE );
    switch (res) {
    case WAIT_OBJECT_0: result = 0; break;
    case WAIT_TIMEOUT: result = 1; break;
    default: result = -1; break;
    }
#else
    static const int Thousand = 1000;
    static const int Million = 1000000;
    static const int Billion = 1000000000;

    struct timespec wait_until;
    struct timeval  now;

    if (timeoutMs == 0) {
        pthread_mutex_lock(&ev->pt_mutex);
        while (!ev->triggered && result == 0) {
            pthread_cond_wait(&ev->pt_cond, &ev->pt_mutex);
        }
    } else {
        gettimeofday(&now,NULL);

        time_t paramWholeSec = timeoutMs / Thousand;
        long   paramWholeMSec = timeoutMs % Thousand;

        wait_until.tv_sec = now.tv_sec + paramWholeSec;
        wait_until.tv_nsec = now.tv_usec*Thousand + paramWholeMSec*Million;
        wait_until.tv_sec += wait_until.tv_nsec/Billion;
        wait_until.tv_nsec %= Billion;

        pthread_mutex_lock(&ev->pt_mutex);
        while (!ev->triggered && result == 0) {
            result = pthread_cond_timedwait(&ev->pt_cond, &ev->pt_mutex, &wait_until);
        }
    }

    // normalize the result
    if ( result == ETIMEDOUT ) {
        result = 1;
    } else if ( result != 0 ) {
        result = -1;
    } else if ( ev->autoreset ) {
        ev->triggered = false;
    }
    pthread_mutex_unlock(&ev->pt_mutex);
#endif

    return result;
}

//-----------------------------------------------------------------------------
#ifdef _WIN32
static unsigned _sv_thread_entry_point(void* context)
{
    sv_thread* t = (sv_thread*)context;
    CoInitialize(NULL);
    t->entry_point(t->context);
    CoUninitialize();
    return 0;
}
#else
static void* _sv_thread_entry_point(void* context)
{
    int oldState;
    sv_thread* t = (sv_thread*)context;
    pthread_setcancelstate(PTHREAD_CANCEL_DISABLE, &oldState);
    t->entry_point(t->context);
    pthread_setcancelstate(PTHREAD_CANCEL_ENABLE, &oldState);
    return NULL;
}
#endif

//-----------------------------------------------------------------------------
SVCORE_API sv_thread* sv_thread_create(sv_thread_func func, void* context)
{
    sv_thread* res = new sv_thread;
    res->entry_point = func;
    res->context = context;
#ifndef _WIN32
    if (pthread_create(&res->pt_thread, NULL, _sv_thread_entry_point, (void*)res) != 0 ) {
#else
    res->hThread = INVALID_HANDLE_VALUE;
    res->hThread = (HANDLE)_beginthreadex( NULL, 0, &_sv_thread_entry_point, res, 0, &res->threadId );
    if ( res->hThread == INVALID_HANDLE_VALUE ) {
#endif
        delete res;
        res = NULL;
    }
    return res;
}

//-----------------------------------------------------------------------------
SVCORE_API int       sv_thread_destroy(sv_thread** thread)
{
    if (!thread || !*thread) {
        return 0;
    }

    sv_thread* th = *thread;
#ifdef _WIN32
    if ( th->hThread != INVALID_HANDLE_VALUE ) {
        if ( sv_get_thread_id() != th->threadId ) {
            WaitForSingleObject(th->hThread, INFINITE);
        }
        CloseHandle( th->hThread );
    }
#else
    pthread_cancel(th->pt_thread);
    int err = pthread_join(th->pt_thread, NULL);
    if ( err != 0 ) {
        return err;
    }
#endif
    delete th;
    *thread = NULL;
    return 0;
}

//-----------------------------------------------------------------------------
SVCORE_API int        sv_thread_is_running(sv_thread* thread)
{
#ifndef _WIN32
    return pthread_equal(thread->pt_thread, pthread_self());
#else
    return thread->threadId == GetCurrentThreadId();
#endif
}


//-----------------------------------------------------------------------------
SVCORE_API void      sv_freep(void* pPtr)
{
    void **ptr = (void **)pPtr;
    if ( ptr && *ptr ) {
        free(*ptr);
        *ptr = NULL;
    }
}


// ----------------------------------------- Libraries  -----------------------
typedef struct sv_lib {
#ifdef _WIN32
    HMODULE     hLib;
#else
    void*       hLib;
#endif
} sv_lib;



//-----------------------------------------------------------------------------
SVCORE_API char* sv_get_module_path(const char* name, void* method)
{
#if defined(WIN32)
     HMODULE handle;
     char module_name[MAX_PATH];
     char *result;

     handle = GetModuleHandle(name);
     if (handle == NULL) {
         return NULL;
     }
     if (GetModuleFileName(handle, module_name, sizeof (module_name)) == 0) {
         return NULL;
     }
     result = (char*)malloc(strlen(module_name)+1);
     if (result != NULL) {
         strcpy(result, module_name);
     }
     return result;
 #else
     Dl_info dli;
     char *result;
     if (dladdr((void *)method, &dli) == 0) {
         return NULL;
     }
     result = (char*)malloc(strlen(dli.dli_fname)+1);
     if (result != NULL) {
         strcpy(result, dli.dli_fname);
     }
     return result;
 #endif
 }

//-----------------------------------------------------------------------------
SVCORE_API char* sv_get_module_dir(const char* name, void* method)
{
    char* full = sv_get_module_path(name, method);
    if (!full)
        return NULL;
#ifdef WIN32
    if ( !PathRemoveFileSpec(full) ) {
        sv_freep(&full);
        return NULL;
    }
    return full;
#else
    return dirname(full);
#endif
}

//-----------------------------------------------------------------------------
static char* gThisModulePath = sv_get_module_dir(NULL, (void*)&sv_get_module_dir);

//-----------------------------------------------------------------------------
static sv_lib*   _sv_load_helper(const char* path, const char* library)
{
#ifdef _WIN32
    const char* ext = ".dll";
    const char* pre = "";
#elif __APPLE__
    const char* ext = ".dylib";
    const char* pre = "lib";
#else
    const char* ext = ".so";
    const char* pre = "lib";
#endif

    sv_lib* res = (sv_lib*)malloc(sizeof(sv_lib));
    res->hLib = NULL;
#ifdef _WIN32
    res->hLib = LoadLibrary(_STR(path<<pre<<library<<ext));
#else
    res->hLib = dlopen(_STR(path<<pre<<library<<ext), RTLD_NOW);
#endif
    if (res->hLib == NULL) {
        sv_freep(&res);
    }
    return res;

}

//-----------------------------------------------------------------------------
SVCORE_API sv_lib*   sv_load(const char* library)
{
    sv_lib* res;
    // try to load as is
    res = _sv_load_helper("", library);
    // try to load using this module's path
    if ( !res && gThisModulePath && strchr(library, PATH_SEPA) == NULL)
        res = _sv_load_helper(_STR(gThisModulePath<<PATH_SEPA), library);
    return res;
}


//-----------------------------------------------------------------------------
SVCORE_API void*     sv_get_sym(sv_lib* handle, const char* name)
{
    if ( handle == NULL )
        return NULL;
#ifdef _WIN32
    return (void*)GetProcAddress(handle->hLib, name);
#else
    return dlsym(handle->hLib, name);
#endif
}

//-----------------------------------------------------------------------------
SVCORE_API void      sv_unload(sv_lib** handle)
{
    if (!handle || !*handle) {
        return;
    }
#ifdef _WIN32
    FreeLibrary( (*handle)->hLib );
#else
    dlclose( (*handle)->hLib );
#endif
    sv_freep(handle);
}

static sv_mutex* _gDataMutex = sv_mutex_create();
static char*     _gCurrentModulePath = NULL;
static char*     _gDataPath = NULL;
static char*     _gTempPath = NULL;

//-----------------------------------------------------------------------------
SVCORE_API void      sv_set_path(int pathType, const char* value)
{
    char** ppPath = NULL;
    switch (pathType) {
    case DataPath: ppPath = &_gDataPath; break;
    case TempPath: ppPath = &_gTempPath; break;
    return;
    }
    sv_mutex_enter(_gDataMutex);
    sv_freep(ppPath);
    if ( value ) {
        *ppPath = strdup(value);
    }
    sv_mutex_exit(_gDataMutex);
}

//-----------------------------------------------------------------------------
SVCORE_API char*     sv_get_path(int pathType)
{
#ifdef _WIN32
    #define LIBNAME "videolib.dll"
#elif defined MACOSX
    #define LIBNAME "libvideolib.dylib"
#else
    #define LIBNAME "libvideolib.so"
#endif
    char** ppPath = NULL;
    const char*  defPath = NULL;
    switch (pathType) {
    case DataPath:
        ppPath = &_gDataPath;
        defPath = "data";
        break;
    case TempPath:
        ppPath = &_gTempPath;
        defPath = "temp";
        break;
    default:
        return NULL;
    }

    char* res = NULL;
    sv_mutex_enter(_gDataMutex);
    if (*ppPath) {
        res = strdup(*ppPath);
    } else {

        if ( _gCurrentModulePath == NULL ) {
            _gCurrentModulePath = sv_get_module_dir(LIBNAME, (void*)&sv_get_path);
            if (!_gCurrentModulePath) {
                sv_mutex_exit(_gDataMutex);
                return NULL;
            }
        }
        res = (char*)malloc(strlen(_gCurrentModulePath)+strlen(defPath)+16);
        sprintf(res, "%s%c..%c%s", _gCurrentModulePath, PATH_SEPA, PATH_SEPA, defPath );
    }

    sv_mutex_exit(_gDataMutex);
    return res;
}

//-----------------------------------------------------------------------------
SVCORE_API int       sv_get_cpu_count()
{
#ifdef _WIN32
    SYSTEM_INFO sysinfo;
    GetSystemInfo(&sysinfo);
    return sysinfo.dwNumberOfProcessors;
#else
    return sysconf (_SC_NPROCESSORS_ONLN);
#endif
}

//-----------------------------------------------------------------------------
#ifdef _WIN32
extern "C" int
gettimeofday(struct timeval * tp, struct timezone * tzp)
{
    static const unsigned __int64 epoch = ((unsigned __int64) 116444736000000000ULL);

    FILETIME    file_time;
    SYSTEMTIME  system_time;
    ULARGE_INTEGER ularge;

    GetSystemTime(&system_time);
    SystemTimeToFileTime(&system_time, &file_time);
    ularge.LowPart = file_time.dwLowDateTime;
    ularge.HighPart = file_time.dwHighDateTime;

    tp->tv_sec = (long) ((ularge.QuadPart - epoch) / 10000000L);
    tp->tv_usec = (long) (system_time.wMilliseconds * 1000);

    return 0;
}
#endif

//-----------------------------------------------------------------------------
SVCORE_API int       sv_transcode_audio()
{
#ifdef _WIN32
    static const int gTranscodingEnabledDefault = 1;
#else
    static const int gTranscodingEnabledDefault = 0;
#endif
    static int gTranscodingEnabled = sv_get_int_env_var("SVAUDIO_TRANSCODING", gTranscodingEnabledDefault);

    return gTranscodingEnabled;
}
