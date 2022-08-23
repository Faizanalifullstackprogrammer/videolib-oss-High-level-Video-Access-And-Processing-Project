/*****************************************************************************
 *
 * bounding_boxes.hpp
 *   Utilities around parsing and serializing bounding box information used
 *   in context of Sighthound Video.
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

//-----------------------------------------------------------------------------

typedef struct  box {
    INT64_T     timestamp;
    uint8_t     color[3];
    int         thickness;
    rect_t      r;
    int         procH;
    int         procW;
    int         uid;
} box_t;

inline ostream& operator<<(ostream& os, const rect_t& r)
{
    os << "[" << r.x << "," << r.y << "," << r.x+r.w << "," << r.y+r.h << "]";
    return os;
}

inline ostream& operator<<(ostream& os, const box_t& b)
{
    os << b.uid << "@" << b.r << "@" << b.timestamp;
    return os;
}

typedef std::list<box_t> BoxList;
typedef BoxList::iterator BoxListIter;


//-----------------------------------------------------------------------------
typedef struct parsed_metadata {
    BoxList*            boxes;
    UINT64_T            timestamp;
    int                 duration;
} parsed_metadata;


//-----------------------------------------------------------------------------
typedef struct ff_filter_base  : public stream_base  {
    bool                allowInterpolation;
    bool                staticMetadata;
    parsed_metadata     currentMetadata;
    parsed_metadata     futureMetadata;
    BoxList*            interpolatedMetadata;
} ff_filter_base_obj;

//-----------------------------------------------------------------------------
static void        _ff_filter_clear_metadata   (parsed_metadata& pm)
{
    pm.boxes->clear();
    pm.duration = 0;
    pm.timestamp = INVALID_PTS;
}

//-----------------------------------------------------------------------------
static void    _ff_filter_activate_future_meta(ff_filter_base_obj* fffilter)
{
    TRACE(_FMT("Activating future metadata"));
    std::swap ( fffilter->currentMetadata, fffilter->futureMetadata );
    _ff_filter_clear_metadata(fffilter->futureMetadata);
}

//-----------------------------------------------------------------------------
static int        _ff_filter_init             (ff_filter_base_obj* fffilter)
{
    fffilter->currentMetadata.boxes = new BoxList;
    fffilter->currentMetadata.timestamp = INVALID_PTS;
    fffilter->currentMetadata.duration = 0;
    fffilter->futureMetadata.boxes = new BoxList;
    fffilter->futureMetadata.timestamp = INVALID_PTS;
    fffilter->futureMetadata.duration = 0;
    fffilter->interpolatedMetadata = new BoxList;
    fffilter->allowInterpolation = false;
    fffilter->staticMetadata = false;
    return 0;
}

//-----------------------------------------------------------------------------
static int        _ff_filter_clear             (ff_filter_base_obj* fffilter)
{
    TRACE(_FMT("Clearing the filters"));
    _ff_filter_clear_metadata(fffilter->currentMetadata);
    _ff_filter_clear_metadata(fffilter->futureMetadata);
    fffilter->interpolatedMetadata->clear();
    return 0;
}

//-----------------------------------------------------------------------------
static int        _ff_filter_destroy          (ff_filter_base_obj* fffilter)
{
    _ff_filter_clear(fffilter); // make sure all the internals had been freed
    delete fffilter->currentMetadata.boxes;
    delete fffilter->futureMetadata.boxes;
    delete fffilter->interpolatedMetadata;
    return 0;
}

//-----------------------------------------------------------------------------
static int        _ff_filter_parse_metadata    (ff_filter_base_obj* fffilter,
                                                const char* filterType,
                                                const char* str,
                                                INT64_T pts,
                                                parsed_metadata& pm)
{
    int count, elem;

    _ff_filter_clear_metadata(pm);
    if (!str || !*str) {
        return 0;
    }

    bool filterMatched = false;
    while (*str) {
        if ( _strnicmp("type=", str, 5) ) {
            // we're out of type listings
            break;
        }
        str += 5;

        const char*  cur = str;
        // advance until next semicolon or end of string
        while (*cur && *cur != ';') cur++;

        if (*cur == ';') {
            size_t len = cur - str;
            if ( !_strnicmp(filterType, str, len ) ) {
                filterMatched = true;
            }
            str = ++cur;
        }
    }

    if ( !filterMatched ) {
        TRACE(_FMT("No matching filters found ... filter type is " << filterType ));
        return -1;
    }

    TRACE(_FMT("Filter config for " << pts << ": " << str));

    elem = sscanf(str, "duration=%d%n", &pm.duration, &count );
    if ( elem != 1 ) {
        TRACE(_FMT("Duration mismatch (" << elem <<"): " << str ));
        return -1;
    }
    str += count;

    if ( _strnicmp(";", str, 1 ) ) {
        TRACE(_FMT("Delim mismatch: expected=; got=" << str ));
        return -1;
    }
    str += 1;

    while (*str) {
        box_t          b;
        char           color[16];
        count = 0;
        elem = sscanf(str, "%u:%u:%u:%u:%u:%u:%d:%d:%15[a-z]%n",
                        &b.r.x, &b.r.y,
                        &b.r.w, &b.r.h,
                        &b.procW, &b.procH,
                        &b.thickness, &b.uid,
                        color, &count );
        if ( elem != 9 ) {
            TRACE(_FMT("Bounding box parsing error (" << elem << "): " << str ));
            return -1;
        }

        b.color[0] = 0; b.color[1] = 0; b.color[2] = 0;
        if ( !_stricmp(color,"yellow") ) {
            b.color[0] = 255; b.color[1] = 255;
        } else
        if ( !_stricmp(color,"green") ) {
            b.color[1] = 255;
        } else
        if ( !_stricmp(color,"blue") ) {
            b.color[2] = 255;
        } else
        if ( !_stricmp(color,"orange") ) {
            b.color[0] = 255; b.color[1] = 165;
        } else
        if ( !_stricmp(color,"pink") ) {
            b.color[0] = 128; b.color[2] = 128;
        } else
        if ( !_stricmp(color,"red") ) {
            b.color[0] = 255;
        } else {
            b.color[2] = 255;
        }


        b.timestamp = pts;
        pm.boxes->push_back(b);
        str += count;
        if (*str==';') str ++;
        TRACE(_FMT("Adding bounding box(" <<
                    b.r.x << ":" <<
                    b.r.y << ":" <<
                    b.r.w << ":" <<
                    b.r.h << ":" <<
                    b.thickness<< ":" <<
                    color<< ") - " <<
                    ". Count=" << count << " Remaining: " << str));
    }

    // build the graph
    pm.timestamp = pts;

    return 0;
}



//-----------------------------------------------------------------------------
static box_t      _ff_filter_merge_box( ff_filter_base_obj* fffilter, box_t& b1,
                                        box_t& b2, INT64_T ts, int uid)
{
    int distance = b2.timestamp - b1.timestamp,
        d1 = ts - b1.timestamp,
        d2 = b2.timestamp - ts;
    if ( distance == 0 ) {
        return b1;
    }
    float w1 = d1 / (float) distance,
          w2 = d2 / (float) distance;

    box_t result = b1;
    result.timestamp = (INT64_T)(b1.timestamp + distance*w1);
    rect_t& rres = result.r;
    rect_t& b1r = b1.r;
    rect_t& b2r = b2.r;
    rres.x = (int)(b1r.x*w2 + b2r.x*w1);
    rres.y = (int)(b1r.y*w2 + b2r.y*w1);
    rres.h = (int)(b1r.h*w2 + b2r.h*w1);
    rres.w = (int)(b1r.w*w2 + b2r.w*w1);


    TRACE(_FMT("Interpolated " << b1 << " and " << b2 << " to " << result ));
    return result;
}

//-----------------------------------------------------------------------------
static BoxList*   _ff_filter_interpolate_metadata( ff_filter_base_obj* fffilter,
                                                    INT64_T ts )
{
    parsed_metadata& cur = fffilter->currentMetadata;
    parsed_metadata& fut = fffilter->futureMetadata;
    bool canUseCurrent = cur.timestamp != INVALID_PTS &&
                         cur.timestamp + cur.duration > ts;
    bool canUseFuture = fut.timestamp != INVALID_PTS &&
                         ts + fut.duration <= fut.timestamp;
    bool canInterpolate = fffilter->allowInterpolation &&
                        cur.timestamp != INVALID_PTS &&
                        fut.timestamp != INVALID_PTS;

    // if the current video frame is too far from either metadata, don't apply the filter
    TRACE(_FMT("canUseCurrent="<<canUseCurrent<<" canUseFuture="<<canUseFuture<<" canInterpolate="<<canInterpolate));
    if ( !canUseFuture && !canUseCurrent && !canInterpolate ) {
        return NULL;
    }

    // if either current or future metadata is too far, us the one that can be used
    if ( !canUseCurrent && !canInterpolate )
        return fut.boxes;
    if ( !canUseFuture && !canInterpolate )
        return cur.boxes;

    // interpolate
    fffilter->interpolatedMetadata->clear();
    // find all the boxes that exist in both lists, and create weighted interpolation,
    // based on the distance from the metadata
    for ( BoxListIter currIt = cur.boxes->begin(); currIt != cur.boxes->end(); currIt++ ) {
        for ( BoxListIter futIt = fut.boxes->begin(); futIt != fut.boxes->end(); futIt++ ) {
            if ( futIt->uid == currIt->uid ) {
                fffilter->interpolatedMetadata->push_back(_ff_filter_merge_box(fffilter, *currIt, *futIt, ts, futIt->uid));
                break;
            }
        }
    }
    return fffilter->interpolatedMetadata;
}

//-----------------------------------------------------------------------------
static BoxList*   _ff_filter_determine_metadata( ff_filter_base_obj* fffilter,
                                                    const char* filterType,
                                                    INT64_T ts )
{
    BoxList* boxes = NULL;

    if ( fffilter->futureMetadata.timestamp != INVALID_PTS &&
         ts >= fffilter->futureMetadata.timestamp ) {
        fffilter->logCb(logWarning, _FMT("Unexpected video timestamp " << ts << " after seeing metadata " << fffilter->currentMetadata.timestamp << ": expected to see meta for " << fffilter->futureMetadata.timestamp ));
        _ff_filter_activate_future_meta( fffilter );
    }

    if ( fffilter->staticMetadata ) {
        boxes = fffilter->currentMetadata.boxes;
    } else
    if ( fffilter->currentMetadata.timestamp == INVALID_PTS ) {
        TRACE(_FMT("No metadata yet"));
    } else if ( ts < fffilter->currentMetadata.timestamp ) {
        fffilter->logCb(logWarning, _FMT("Unexpected video timestamp " << ts << " after seeing metadata " << fffilter->currentMetadata.timestamp ));
        _ff_filter_clear_metadata(fffilter->currentMetadata);
        _ff_filter_clear_metadata(fffilter->futureMetadata);
    } else if ( ts == fffilter->currentMetadata.timestamp ) {
        TRACE(_FMT("Exact metadata match for ts=" << ts));
        boxes = fffilter->currentMetadata.boxes;
        if (_gTraceLevel>0 && boxes) {
            for (BoxListIter it=boxes->begin(); it!=boxes->end(); it++) {
                TRACE(_FMT("Exact match for bounding box " << *it ));
            }
        }
    } else {
        if ( fffilter->futureMetadata.timestamp == INVALID_PTS ) {
            frame_obj*      nextMetadata = NULL;
            size_t          size = sizeof(nextMetadata);
            if ( fffilter->sourceApi->get_param(fffilter->source, "nextMetadata", &nextMetadata, &size) >= 0 &&
                 nextMetadata != NULL ) {
                frame_api_t* fapi = frame_get_api(nextMetadata);
                INT64_T futPts = fapi->get_pts(nextMetadata);
                TRACE( _FMT("Got future meta: " << futPts));
                _ff_filter_parse_metadata( fffilter, filterType, (const char*)fapi->get_data(nextMetadata), futPts, fffilter->futureMetadata);
            }
        }

        boxes = _ff_filter_interpolate_metadata(fffilter, ts);
    }
    return boxes;
}

//-----------------------------------------------------------------------------
static int     _ff_filter_read_frame( ff_filter_base_obj* fffilter,
                                        const char* filterType,
                                        frame_obj** frame,
                                        BoxList**  boxes)
{
    int res = -1;
    *frame = NULL;
    *boxes = NULL;

    if (!fffilter->source) {
        fffilter->logCb(logError, _FMT("filter -- source isn't set"));
        return -1;
    }

    frame_obj* tmp = NULL;
    res = fffilter->sourceApi->read_frame(fffilter->source, &tmp);
    if ( res < 0 || tmp == NULL ) {
        if ( res < 0 ) {
            fffilter->logCb(logError, _FMT("filter failed to read from the source"));
        }
        return res;
    }

    frame_api_t*    tmpFrameAPI = frame_get_api(tmp);
    INT64_T         ts = tmpFrameAPI->get_pts(tmp);
    // See if this particular set of metadate contributes to our filter.
    // If it does, save this frame.
    if ( tmpFrameAPI->get_media_type(tmp) == mediaMetadata ) {
        if (!fffilter->staticMetadata) {
            TRACE( _FMT("Got Meta: " << ts));
            if ( ts == fffilter->futureMetadata.timestamp ) {
                _ff_filter_activate_future_meta( fffilter );
            } else {
                _ff_filter_parse_metadata( fffilter,
                                filterType,
                                (const char*)tmpFrameAPI->get_data(tmp),
                                ts,
                                fffilter->currentMetadata);
                _ff_filter_clear_metadata(fffilter->futureMetadata);
            }
        }

        // This metadata may be of interest to other filters
        *frame = tmp;
        return 0;
    }


    if ( tmpFrameAPI->get_media_type(tmp) != mediaVideo ) {
        // only video filters exist for now
        *frame = tmp;
        return 0;
    }

    TRACE(_FMT("Got Video: " << ts));

    // indicate further processing by the caller
    *boxes = _ff_filter_determine_metadata( fffilter, filterType, ts );
    *frame = tmp;
    return 1;
}

//-----------------------------------------------------------------------------
static void  _ff_rescale_box( const box_t& b, box_t& res, int w, int h )
{
    const rect_t& r1 = b.r;
    rect_t& r2 = res.r;

    res = b;

    float       ratioH =  h / (float)b.procH;
    float       ratioW =  w / (float)b.procW;

    r2.x = (int)(ratioW * r1.x);
    r2.w = (int)(ratioW * r1.w);
    r2.y = (int)(ratioH * r1.y);
    r2.h = (int)(ratioH * r1.h);

    if (res.thickness == 0) {
        // 1 pixel of box for each 1280 pixels of width
        res.thickness = w / 1280 + 1;
    }
}