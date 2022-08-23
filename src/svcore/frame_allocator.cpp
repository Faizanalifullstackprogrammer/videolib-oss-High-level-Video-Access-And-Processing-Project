/*****************************************************************************
 *
 * frame_allocator.cpp
 *   Pool of preallocated frame objects.
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

#undef SV_MODULE_NAME
#define SV_MODULE_NAME "frameallocator"

#include "frame_allocator.h"

#include <set>

//-----------------------------------------------------------------------------
typedef struct frame_allocator {
    int               freed;
    frame_pooled_t*   freeList;
    int               freeListCount;
    frame_pooled_t*   allocList;
    int               allocListCount;

    int64_t           lastAllocEvent;
    int               reductionTimeThreshold;
    int               desiredCount;
    sv_mutex*         mutex;
    char*             name;
} frame_allocator;


//-----------------------------------------------------------------------------
SVCORE_API frame_allocator* create_frame_allocator(const char* name)
{
    frame_allocator* res = (frame_allocator*)malloc(sizeof(frame_allocator));
    res->freeList = NULL;
    res->freeListCount = 0;
    res->allocList = NULL;
    res->allocListCount = 0;
    res->freed = 0;
    res->lastAllocEvent = 0;
    res->desiredCount = 5; // TODO: arbitrary number ... needs to be a parameter to factory
    res->reductionTimeThreshold = 2000; // TODO: probably needs to be parametrize as well
    res->mutex = sv_mutex_create();
    res->name = name ? strdup(name) : NULL;
    return res;
}


//-----------------------------------------------------------------------------
static frame_pooled_t* _frame_allocator_free(frame_allocator* fa, frame_pooled_t* f,
                                    fn_stream_log logCb)
{
    int ownerTag = f->ownerTag;
    frame_pooled_t* next = f->next;
    f->fa = NULL;
    frame_ref((frame_obj*)f);
    REF_T ref = frame_unref((frame_obj**)&f);
    if (ref != 0 && logCb) {
        logCb(logError, _FMT("Frame refcount at release is " << ref << "; tag=" << ownerTag ));
    }
    return next;
}


//-----------------------------------------------------------------------------
static int _frame_allocator_reduce_list(frame_allocator* fa)
{
    if ( fa->freeListCount > fa->desiredCount &&
         sv_time_get_elapsed_time(fa->lastAllocEvent) > fa->reductionTimeThreshold &&
         fa->freeList != NULL ) {
        // destroy one extra to reduce the list ...
        fa->freeList = _frame_allocator_free(fa, fa->freeList, NULL);
        fa->freeListCount--;
        return 0;
    }
    return -1;
}

//-----------------------------------------------------------------------------
SVCORE_API frame_pooled_t* frame_allocator_get(frame_allocator* fa)
{
    frame_pooled_t* res = NULL;
    sv_mutex_enter(fa->mutex);
    if ( fa->freeList != NULL ) {
        res = fa->freeList;
        fa->freeList = res->next;
        fa->freeListCount--;
        fa->allocListCount++;
        res->next = NULL;
        if ( res->resetter ) {
            res->resetter((frame_obj*)res);
        }

        _frame_allocator_reduce_list( fa );
    }
    sv_mutex_exit(fa->mutex);
    return res;
}

//-----------------------------------------------------------------------------
SVCORE_API void frame_allocator_register_frame(frame_allocator* fa, frame_pooled_t* newFrame)
{
    newFrame->fa = fa;
    fa->lastAllocEvent = sv_time_get_current_epoch_time();
    fa->allocListCount++;
}

//-----------------------------------------------------------------------------
SVCORE_API const char* frame_allocator_get_name(frame_allocator* fa)
{
    return fa->name;
}

//-----------------------------------------------------------------------------
SVCORE_API void frame_allocator_get_stats(frame_allocator* fa, int* freect, int* allocct)
{
    sv_mutex_enter(fa->mutex);
    *freect = fa->freeListCount;
    *allocct = fa->allocListCount;
    sv_mutex_exit(fa->mutex);
}

//-----------------------------------------------------------------------------
SVCORE_API void frame_allocator_return(frame_allocator* fa, frame_pooled_t* frame)
{
    bool freeAllocator = false;
    sv_mutex_enter(fa->mutex);
    if ( _frame_allocator_reduce_list(fa) >= 0 ) {
        // we have more frames in the allocator than we want to ... free this frame
        _frame_allocator_free(fa, frame, NULL);
    } else {
        frame->next = fa->freeList;
        frame->refcount = 0; // someone may have called _unref without _ref first, hitting a negative #
        fa->freeList = frame;
        fa->freeListCount++;
    }
    fa->allocListCount--;
    freeAllocator = (fa->freed && fa->allocListCount == 0);
    sv_mutex_exit(fa->mutex);

    if ( freeAllocator ) {
        destroy_frame_allocator(&fa, NULL);
    }
}

//-----------------------------------------------------------------------------
SVCORE_API void destroy_frame_allocator(frame_allocator** pfa, fn_stream_log logCb)
{
    if ( pfa && *pfa ) {
        frame_allocator* fa = *pfa;
        bool proceed = true;
        sv_mutex_enter(fa->mutex);
        if ( fa->allocListCount ) {
#if DEBUG_FRAME_ALLOC>0
            if ( logCb ) {
                logCb(logError, _FMT("Attempt to free frame allocator " << fa->name <<
                                    " with " << fa->allocListCount << " outstanding frames"));
#if DEBUG_FRAME_ALLOC>2
                print_allocated_frames( logCb );
#endif
            }
#endif
            proceed = false;
            fa->freed = 1;
        }
        sv_mutex_exit(fa->mutex);

        if (!proceed) {
            return;
        }

        frame_pooled_t* current = fa->freeList;
        while (current) {
            current = _frame_allocator_free(fa, current, logCb);
        }
        sv_mutex_destroy(&fa->mutex);
        sv_freep(&fa->name);
        sv_freep(pfa);
    }
}
