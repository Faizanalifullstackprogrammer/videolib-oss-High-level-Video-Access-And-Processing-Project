/*****************************************************************************
 *
 * event_basic.cpp
 *   Basic event object representation
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
#define SV_MODULE_NAME "eventbasic"


#include "sv_os.h"
#include "streamprv.h"
#include "event_basic.h"



//-----------------------------------------------------------------------------
//  Forward declarations
//-----------------------------------------------------------------------------

static stream_ev_obj* basic_event_create            (const char* name);
static const char*    basic_event_get_name          (stream_ev_obj* ev);
static int64_t        basic_event_get_ts            (stream_ev_obj* ev);
static int            basic_event_set_ts            (stream_ev_obj* ev, int64_t ts);
static const void*    basic_event_get_context       (stream_ev_obj* ev);
static int            basic_event_set_context       (stream_ev_obj* ev, const void* context);
static int            basic_event_get_property      (stream_ev_obj* ev, const CHAR_T* name, void* value, size_t* size);
static int            basic_event_set_property      (stream_ev_obj* ev, const CHAR_T* name, void* value, size_t size);
static void           basic_event_destroy           (stream_ev_obj* ev);


static stream_ev_api_t _g_basic_event_api = {
    basic_event_create,
    basic_event_get_name,
    basic_event_get_ts,
    basic_event_set_ts,
    basic_event_get_context,
    basic_event_set_context,
    basic_event_get_property,
    basic_event_set_property
};


//-----------------------------------------------------------------------------
extern "C" stream_ev_api_t*      get_basic_event_api              ()
{
    return &_g_basic_event_api;
}



//-----------------------------------------------------------------------------
static stream_ev_obj* basic_event_create            (const char* name)
{
    basic_event_obj* res = (basic_event_obj*)malloc(sizeof(basic_event_obj));
    res->magic = BASIC_EVENT_MAGIC;
    res->refcount = 0;
    res->api = get_basic_event_api();
    res->context = NULL;
    res->destructor = basic_event_destroy;
    strncpy(res->name, name, kMaxNameSize);
    res->name[kMaxNameSize-1] = '\0';
    for (int i=0; i<kMaxProps; i++) {
        res->properties[i] = NULL;
    }
    return (stream_ev_obj*)res;
}

//-----------------------------------------------------------------------------
static const char*    basic_event_get_name          (stream_ev_obj* ev)
{
    DECLARE_EVENT(ev, event, NULL);
    return event->name;
}

//-----------------------------------------------------------------------------
static int64_t        basic_event_get_ts            (stream_ev_obj* ev)
{
    DECLARE_EVENT(ev, event, -1);
    return event->timestamp;
}

//-----------------------------------------------------------------------------
static int            basic_event_set_ts            (stream_ev_obj* ev, int64_t ts)
{
    DECLARE_EVENT(ev, event, -1);
    event->timestamp = ts;
    return 0;
}

//-----------------------------------------------------------------------------
static const void*    basic_event_get_context       (stream_ev_obj* ev)
{
    DECLARE_EVENT(ev, event, NULL);
    return event->context;
}

//-----------------------------------------------------------------------------
static int            basic_event_set_context       (stream_ev_obj* ev,
                                                    const void* context)
{
    DECLARE_EVENT(ev, event, -1);
    event->context = context;
    return 0;
}

//-----------------------------------------------------------------------------
static int            basic_event_get_property      (stream_ev_obj* ev,
                                                    const CHAR_T* name,
                                                    void* value,
                                                    size_t* size)
{
    DECLARE_EVENT(ev, event, -1);
    for (int i=0; i<kMaxProps; i++) {
        basic_prop_t* prop = event->properties[i];
        if ( prop!=NULL && !_stricmp(name, prop->name) ) {
            if (*size < prop->size) {
                *size = prop->size;
                return -1;
            }
            *size = prop->size;
            memcpy(value, prop->value, prop->size);
            return i;
        }
    }
    return -1;
}

//-----------------------------------------------------------------------------
static int            basic_event_set_property      (stream_ev_obj* ev,
                                                    const CHAR_T* name,
                                                    void* value,
                                                    size_t size)
{
    DECLARE_EVENT(ev, event, -1);
    if (strlen(name)>kMaxNameSize) {
        return -1;
    }

    for (int i=0; i<kMaxProps; i++) {
        basic_prop_t* prop = event->properties[i];
        if ( prop!=NULL &&
             !_stricmp(prop->name, prop->name) ) {
            free(prop);
            event->properties[i] = NULL;
            break;
        }
    }

    for (int j=0; j<kMaxProps; j++) {
        if ( event->properties[j]==NULL ) {
            basic_prop_t* prop = (basic_prop_t*)malloc(sizeof(basic_prop)+size);
            strcpy(prop->name, name);
            memcpy(prop->value, value, size);
            prop->size = size;
            event->properties[j] = prop;
            return 0;
        }
    }
    return -2;
}

//-----------------------------------------------------------------------------
static void           basic_event_destroy           (stream_ev_obj* ev)
{
    DECLARE_EVENT_V(ev, event);

    for (int i=0; i<kMaxProps; i++) {
        if ( event->properties[i]!=NULL ) {
            free(event->properties[i]);
            event->properties[i] = NULL;
        }
    }

    free(event);
}
