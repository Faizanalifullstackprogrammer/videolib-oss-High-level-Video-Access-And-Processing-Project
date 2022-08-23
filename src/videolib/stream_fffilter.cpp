/*****************************************************************************
 *
 * stream_fffilter.cpp
 *   Node allowing a number of ffmpeg filters to be applied to the frame.
 *   Examples include timestamp overlay, frame bluring, frame rotation etc.
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
#define SV_MODULE_VAR fffilter
#define SV_MODULE_ID "FILTER"
#include "sv_module_def.hpp"

#include "streamprv.h"


#include "frame_basic.h"
#include "sv_ffmpeg.h"
#include "sv_pixfmt.h"

#include "videolibUtils.h"

#include <algorithm>
#include <list>

#define FF_FILTER_MAGIC 0x1249

using namespace std;

#include "bounding_boxes.hpp"

//-----------------------------------------------------------------------------
typedef struct fs_filter  : public ff_filter_base_obj  {
    AVFilterContext*    bufsrc;
    AVFilterContext*    bufsink;
    AVFilterGraph*      graph;
    AVFilterInOut*      inputs;
    AVFilterInOut*      outputs;

    int                 width;
    int                 height;
    enum AVPixelFormat  pixfmt;

    AVFrame*            srcFrame;
    AVFrame*            dstFrame;

    INT64_T             timestampOffset;
    int                 showDate;
    char*               font;
    int                 fontsize;

    int                 rotation;
    int                 transposeHW;

    int                 blurRepeat;

    int                 rebuildGraph;

    char*               filterType;
    char*               strftime;
    int                 staticFilter;

    frame_allocator*    fa;
} ff_filter_obj;

//-----------------------------------------------------------------------------
// Stream API
//-----------------------------------------------------------------------------

//-----------------------------------------------------------------------------
//  Forward declarations
//-----------------------------------------------------------------------------
static stream_obj* ff_filter_create             (const char* name);
static int         ff_filter_set_param          (stream_obj* stream,
                                                const CHAR_T* name,
                                                const void* value);
static int         ff_filter_seek               (stream_obj* stream,
                                                       INT64_T offset,
                                                        int flags);
static size_t      ff_filter_get_width          (stream_obj* stream);
static size_t      ff_filter_get_height         (stream_obj* stream);
static int         ff_filter_read_frame         (stream_obj* stream, frame_obj** frame);
static int         ff_filter_close              (stream_obj* stream);
static void        ff_filter_destroy            (stream_obj* stream);

extern "C" frame_api_t*     get_ffframe_frame_api   ( );
extern frame_obj*           alloc_avframe_frame     (int ownerTag, frame_allocator* fa,
                                                    fn_stream_log logCb);


//-----------------------------------------------------------------------------
stream_api_t _g_ff_filter_provider = {
    ff_filter_create,
    get_default_stream_api()->set_source,
    get_default_stream_api()->set_log_cb,
    get_default_stream_api()->get_name,
    get_default_stream_api()->find_element,
    get_default_stream_api()->remove_element,
    get_default_stream_api()->insert_element,
    ff_filter_set_param,
    get_default_stream_api()->get_param,
    get_default_stream_api()->open_in,
    ff_filter_seek,
    ff_filter_get_width,
    ff_filter_get_height,
    get_default_stream_api()->get_pixel_format,
    ff_filter_read_frame,
    get_default_stream_api()->print_pipeline,
    ff_filter_close,
    _set_module_trace_level
};


//-----------------------------------------------------------------------------
#define DECLARE_FF_FILTER(stream, name) \
    DECLARE_OBJ(ff_filter_obj, name,  stream, FF_FILTER_MAGIC, -1)

#define DECLARE_FF_FILTER_V(stream, name) \
    DECLARE_OBJ_V(ff_filter_obj, name,  stream, FF_FILTER_MAGIC)

static stream_obj*   ff_filter_create                (const char* name)
{
    ff_filter_obj* res = (ff_filter_obj*)stream_init(sizeof(ff_filter_obj),
                FF_FILTER_MAGIC,
                &_g_ff_filter_provider,
                name,
                ff_filter_destroy );

    _ff_filter_init(res);

    res->bufsrc = NULL;
    res->bufsink = NULL;
    res->graph = NULL;
    res->inputs = NULL;
    res->outputs = NULL;
    res->srcFrame = NULL;
    res->dstFrame = NULL;
    res->blurRepeat = 50;
    res->timestampOffset = 0;
    res->showDate = 1;
    res->font = NULL;
    res->fontsize = 0;
    res->filterType = NULL;
    res->staticFilter = 0;
    res->rotation = 0;
    res->transposeHW = 0;
    res->strftime = NULL;
    res->width = 0;
    res->height = 0;

    res->fa = create_frame_allocator(_STR("fffilter_"<<name));
    return (stream_obj*)res;
}

//-----------------------------------------------------------------------------
static int         ff_filter_set_param             (stream_obj* stream,
                                            const CHAR_T* name,
                                            const void* value)
{
    DECLARE_FF_FILTER(stream, fffilter);
    name = stream_param_name_apply_scope(stream, name);
    if ( !_stricmp(name, "filterType")) {
        if ( fffilter->graph == NULL ) {
            const char* ft = (const char*)value;

            if ( !_stricmp(ft, "timestamp") ||
                 !_stricmp(ft, "timer") ||
                 !_stricmp(ft, "rotate") ) {
                fffilter->staticFilter = 1;
            } else
            if ( !_stricmp(ft, "blurbox") ||
                 !_stricmp(ft, "boundingbox") ||
                 !_stricmp(ft, "bboxid") ) {
                fffilter->staticFilter = 0;
            } else {
                fffilter->logCb(logError, _FMT("Filter type '" << ft << "' hasn't been defined"));
                return -1;
            }

            fffilter->filterType = strdup(ft);
            return 0;
        }
        fffilter->logCb(logError, _FMT("Can't change filter type once initialized"));
        return -1;
    }
    if ( !_stricmp(name, "timestampOffset")) {
        fffilter->timestampOffset = *(INT64_T*)value;
        TRACE(_FMT("Setting timestamp offset to " << fffilter->timestampOffset));
        return 0;
    }
    if ( !_stricmp(name, "font") && value ) {
        fffilter->font = strdup((const char*)value);
        return 0;
    }
    if ( !_stricmp(name, "rotation") && value ) {
        int rot = *(int*)value;
        if ( abs(rot)!=90 && abs(rot)!=180 ) {
            fffilter->logCb(logError, _FMT("Invalid rotation value: " <<rot));
            return -1;
        }
        if ( abs(rot) == 90 ) {
            fffilter->transposeHW = 1;
        }
        fffilter->rotation = rot;
        return 0;
    }
    if ( !_stricmp(name, "blurRepeat") && value ) {
        fffilter->blurRepeat = *(int*)value;
        return 0;
    }
    if ( !_stricmp(name, "showDate") ) {
        fffilter->showDate = *(int*)value;
        return 0;
    }
    if ( !_stricmp(name, "modifyInPlace") ) {
        // not something we can do, but support for compatibility with our drawing filter
        return 0;
    }
    SET_STR_PARAM_IF(stream, name, "strftime", fffilter->strftime);
    return default_set_param(stream, name, value);
}

//-----------------------------------------------------------------------------
static void        _ff_filter_free                (ff_filter_obj* fffilter)
{
    avfilter_graph_free(&fffilter->graph);
    if (fffilter->inputs) {
        av_freep(&fffilter->inputs->name);
        avfilter_inout_free(&fffilter->inputs);
    }
    if (fffilter->outputs) {
        av_freep(&fffilter->outputs->name);
        avfilter_inout_free(&fffilter->outputs);
    }

    av_frame_free(&fffilter->srcFrame);
    av_frame_free(&fffilter->dstFrame);
}

//-----------------------------------------------------------------------------
static char*      _ff_update_storage               (char* buffer, int& argsSize, int& argsAlloc)
{
    static const int minBuffer = 512;
    static const int allocDelta = 8096;

    argsSize += strlen(&buffer[argsSize]);
    if ( argsAlloc - argsSize < minBuffer ) {
        argsAlloc += allocDelta;
        buffer = (char*)realloc(buffer, argsAlloc);
    }
    return buffer;
}

//-----------------------------------------------------------------------------
static int  _ff_filter_check_font          (ff_filter_obj* fffilter)
{
    if (fffilter->fontsize > 0)
        return 0;

    int fontsize;
    if (fffilter->height <= 300)
        fontsize = 14;
    else if (fffilter->height <= 500)
        fontsize = 24;
    else if (fffilter->height <= 1000)
        fontsize = 26;
    else
        fontsize = (int)(fffilter->height / 40);

    if (!fffilter->font) {
        char* path = sv_get_path(DataPath);
        if (path != NULL) {
            fffilter->font = (char*)malloc(4*strlen(path)+64);
            sprintf( fffilter->font, "%s%cfonts%cInconsolata.otf", path, PATH_SEPA, PATH_SEPA );
            sv_freep(&path);
        }
    }
    for (char* c=fffilter->font; c && *c; ) {
        if (*c == '\\' || *c == ':' ) {
            memmove(c+3, c, strlen(c)+1);
            *c = '\\';
            *(c+1) = '\\';
            *(c+2) = '\\';
            c += 4;
        } else {
            c++;
        }
    }
    fffilter->fontsize = fontsize;
    return 0;
}

//-----------------------------------------------------------------------------
static int         _ff_filter_build_graph          (ff_filter_obj* fffilter,
                                                    BoxList* boxes)
{
    const AVFilter *bufsrc, *bufsink;
    stream_obj* stream = (stream_obj*)fffilter;
    char* args = NULL;
    int   argsSize = 0, argsAlloc = 0;
    int   res = -1;
    int maxExpectedSize = 8096;
    rational_t timebase;
    int forcePixFmt = 0;

    if ( fffilter->filterType == NULL ) {
        fffilter->logCb(logError, _FMT("Filter type hasn't been set"));
        return -1;
    }

    size_t size = sizeof(rational_t);
    if (default_get_param(stream, "timebase", &timebase, &size) < 0 ) {
        fffilter->logCb(logError, _FMT("Couldn't determine timebase"));
        return -1;
    }

    fffilter->srcFrame = av_frame_alloc();
    if (!fffilter->srcFrame) {
        fffilter->logCb(logError, _FMT("Couldn't allocate frame object"));
        goto Error;
    }

    fffilter->width = default_get_width(stream);
    fffilter->height = default_get_height(stream);
    fffilter->pixfmt = svpfmt_to_ffpfmt(default_get_pixel_format(stream), NULL);

    // Get input and output filters
    bufsrc = avfilter_get_by_name("buffer");
    if (!bufsrc) {
        fffilter->logCb(logError, "Couldn't find 'buffer'");
        goto Error;
    }
    bufsink = avfilter_get_by_name("buffersink");
    if (!bufsink) {
        fffilter->logCb(logError, "Couldn't find 'buffersink'");
        goto Error;
    }

    // Allocate space for the filtergraph
    fffilter->graph = avfilter_graph_alloc();
    if (!fffilter->graph) {
        fffilter->logCb(logError, "Couldn't allocate the graph");
        goto Error;
    }

    args = (char*)malloc(maxExpectedSize);
    args[0] = '\0';
    argsAlloc = maxExpectedSize;
    // don't update argsSize, since we only need it once for this iteration

    // Set arguments for buffer source and create filtergraph
    snprintf(args, argsAlloc,
            "video_size=%dx%d:pix_fmt=%d:time_base=%d/%d",
            fffilter->width,
            fffilter->height,
            fffilter->pixfmt,
            timebase.num,
            timebase.denum);

    if (avfilter_graph_create_filter(&fffilter->bufsrc, bufsrc, "in", args, NULL,
                fffilter->graph) < 0 ||
        avfilter_graph_create_filter(&fffilter->bufsink, bufsink, "out", NULL, NULL,
                fffilter->graph) < 0) {
        fffilter->logCb(logError, _FMT("Couldn't create the filter input/output"));
        goto Error;
    }

    // Set endpoints for filtergraph
    fffilter->outputs = avfilter_inout_alloc();
    fffilter->outputs->name = av_strdup("in");
    fffilter->outputs->filter_ctx = fffilter->bufsrc;
    fffilter->outputs->pad_idx = 0;
    fffilter->outputs->next = NULL;

    fffilter->inputs = avfilter_inout_alloc();
    fffilter->inputs->name = av_strdup("out");
    fffilter->inputs->filter_ctx = fffilter->bufsink;
    fffilter->inputs->pad_idx = 0;
    fffilter->inputs->next = NULL;

    if ( !_stricmp(fffilter->filterType, "rotate") ) {
        static const char* clock = "clock";
        static const char* cclock = "cclock";


        snprintf(&args[argsSize],
                argsAlloc-argsSize,
                "%stranspose=%s",
                argsSize > 0 ? "," : "",
                (fffilter->rotation>0 ? clock : cclock) );
        args = _ff_update_storage(args, argsSize, argsAlloc);
        if ( abs(fffilter->rotation) == 180 ) {
            snprintf(&args[argsSize],
                    argsAlloc-argsSize,
                    ",transpose=%s",
                    (fffilter->rotation>0 ? clock : cclock) );
            args = _ff_update_storage(args, argsSize, argsAlloc);
        }
    } else
    if ( !_stricmp(fffilter->filterType, "timestamp") ||
         !_stricmp(fffilter->filterType, "timer")) {
        char offsetBuffer[64] = "";
        int  offsetBufferLen = 0;
        char strftimeBuffer[64] = "";
        int  strftimeBufferLen = 0;
        static const char* sParamPrefix = " \\\\: ";
        static int         sParamPrefixLen = strlen(sParamPrefix);

        _ff_filter_check_font(fffilter);

        strcpy(&offsetBuffer[offsetBufferLen], sParamPrefix);
        offsetBufferLen+=sParamPrefixLen;
        sprintf(&offsetBuffer[offsetBufferLen], I64FMT "." I64FMT, fffilter->timestampOffset/1000, (fffilter->timestampOffset%1000)*1000 );
        offsetBufferLen+=strlen(&offsetBuffer[offsetBufferLen]);

        const char* fmt = "flt";
        if (!_stricmp(fffilter->filterType, "timestamp")) {
            fmt = fffilter->showDate ? "localtime" : "hms";
            if (fffilter->strftime &&
                strlen(fffilter->strftime)+sParamPrefixLen < sizeof(strftimeBuffer)) {
                strcpy(&strftimeBuffer[strftimeBufferLen], sParamPrefix);
                strftimeBufferLen+=sParamPrefixLen;
                strcat(&strftimeBuffer[strftimeBufferLen], fffilter->strftime );
                strftimeBufferLen+=strlen(fffilter->strftime);
            }
        }
        snprintf(&args[argsSize],
                 argsAlloc-argsSize,
                 "%sdrawtext="
                 "fontfile=%s:"
                 "text=%%{pts \\\\: %s%s%s}:x=0:y=0:fontsize=%d:fontcolor=white@0.75:"
                 "box=1:boxcolor=black@0.35",
                 argsSize > 0 ? "," : "",
                 fffilter->font,
                 fmt,
                 offsetBuffer,
                 strftimeBuffer,
                 fffilter->fontsize);
        args = _ff_update_storage(args, argsSize, argsAlloc);
        forcePixFmt = 1;
    } else
    if ( !_stricmp(fffilter->filterType, "blurbox")  && boxes ) {
        static const char* unsharpMask = "unsharp=13:13:-2:13:13:-2";
        int          unsharpIter = fffilter->blurRepeat;
        BoxListIter  it;
        int          nI;
        int          count = boxes->size();

        snprintf(&args[argsSize],
                    argsAlloc-argsSize,
                    "[in]split=%d[v0sum]", count+1);
        args = _ff_update_storage(args, argsSize, argsAlloc);
        for (nI=0; nI<count; nI++) {
            snprintf(&args[argsSize], argsAlloc-argsSize, "[v%d]", nI );
            args = _ff_update_storage(args, argsSize, argsAlloc);
        }
        snprintf(&args[argsSize], argsAlloc-argsSize, ";" );
        args = _ff_update_storage(args, argsSize, argsAlloc);
        for ( it = boxes->begin(), nI=0; it != boxes->end(); it++, nI++ ) {
            // [v0]crop=$W:$H:$X1:$Y1,$FILTER1[v0c];
            box_t currBox;

            _ff_rescale_box( *it, currBox, fffilter->width, fffilter->height );

            snprintf(&args[argsSize], argsAlloc-argsSize, "[v%d]crop=%d:%d:%d:%d,",
                        nI, currBox.r.w, currBox.r.h,
                        currBox.r.x, currBox.r.y );
            args = _ff_update_storage(args, argsSize, argsAlloc);
            for ( int nJ=0; nJ<unsharpIter; nJ++ ) {
                snprintf(&args[argsSize], argsAlloc-argsSize, "%s%s", unsharpMask, nJ!=unsharpIter-1?",":"" );
                args = _ff_update_storage(args, argsSize, argsAlloc);
            }

            snprintf(&args[argsSize], argsAlloc-argsSize, "[v%dc];", nI );
            args = _ff_update_storage(args, argsSize, argsAlloc);
        }

        for ( it = boxes->begin(), nI = 0; it != boxes->end(); it++, nI++ ) {
            // [v1c]overlay=$X1:$Y1[v1sum];
            box_t currBox;

            _ff_rescale_box( *it, currBox, fffilter->width, fffilter->height );

            if (nI != boxes->size()-1) {
                snprintf(&args[argsSize], argsAlloc-argsSize, "[v%dsum][v%dc]overlay=%d:%d[v%dsum];",
                    nI, nI, currBox.r.x, currBox.r.y, nI+1 );
            } else {
                snprintf(&args[argsSize], argsAlloc-argsSize, "[v%dsum][v%dc]overlay=%d:%d",
                    nI, nI, currBox.r.x, currBox.r.y );
            }
            args = _ff_update_storage(args, argsSize, argsAlloc);
        }

        // overlay filter causes pixfmt to change at the output, which we do not want to happen
        forcePixFmt = 1;
    } else
    if ( !_stricmp(fffilter->filterType, "boundingbox") && boxes ) {
        // "drawbox=1024:2048:1000:2000:purple:t=8:enable='between(t,1444773630863,1444773630863)',"
        BoxListIter     it;

        for ( it = boxes->begin(); it != boxes->end(); it++ ) {
            box_t currBox = *it;
            _ff_rescale_box( *it, currBox, fffilter->width, fffilter->height );

            snprintf(&args[argsSize],
                    argsAlloc-argsSize,
                    "%sdrawbox=%d:%d:%d:%d:#%.02x%.02x%.02x:t=",
                    argsSize > 0 ? "," : "",
                    currBox.r.x,
                    currBox.r.y,
                    currBox.r.w,
                    currBox.r.h,
                    currBox.color[0], currBox.color[1], currBox.color[2] );
            args = _ff_update_storage(args, argsSize, argsAlloc);
            if ( currBox.thickness < 0 ) {
                snprintf(&args[argsSize], argsAlloc-argsSize, "max" );
            } else {
                snprintf(&args[argsSize], argsAlloc-argsSize, "%d", currBox.thickness );
            }
            args = _ff_update_storage(args, argsSize, argsAlloc);
        }

        // drawbox only operates on YUV variants ...
        forcePixFmt = 1;
    } else
    if ( !_stricmp(fffilter->filterType, "bboxid") && boxes ) {
        // "drawbox=1024:2048:1000:2000:purple:t=8:enable='between(t,1444773630863,1444773630863)',"
        BoxListIter     it;

        _ff_filter_check_font(fffilter);

        for ( it = boxes->begin(); it != boxes->end(); it++ ) {
            box_t currBox;
            _ff_rescale_box( *it, currBox, fffilter->width, fffilter->height );

            int fontsize = fffilter->fontsize-4;
            int fontmargin = 2;
            int textX, textY;
            if ( currBox.r.h > fontsize + fontmargin ) {
                textY = currBox.r.y + fontmargin;
            } else if ( currBox.r.y > fontsize + fontmargin ) {
                textY = currBox.r.y - fontsize - fontmargin;
            } else {
                textY = currBox.r.y + currBox.r.h + fontmargin;
            }
            textX = currBox.r.x + fontmargin;
            snprintf(&args[argsSize],
                    argsAlloc-argsSize,
                    "%sdrawtext="
                    "fontfile=%s:"
                    "text=#%d:x=%d:y=%d:fontsize=%d:fontcolor=#%.02x%.02x%.02x@0.75",
                    argsSize > 0 ? "," : "",
                    fffilter->font,
                    currBox.uid,
                    textX,
                    textY,
                    fontsize,
                    currBox.color[0], currBox.color[1], currBox.color[2] );
            args = _ff_update_storage(args, argsSize, argsAlloc);
        }
    } else if ( !_stricmp(fffilter->filterType, "ffmpeg") ) {
        // TBD
    }

    if ( forcePixFmt ) {
        snprintf(&args[argsSize],
                argsAlloc-argsSize,
                "%sformat=pix_fmts=%d",
                argsSize > 0 ? "," : "",
                fffilter->pixfmt );
        args = _ff_update_storage(args, argsSize, argsAlloc);
    }

    TRACE(_FMT("Filter: size=" << argsSize << " config=" << args));

    if (avfilter_graph_parse_ptr(fffilter->graph, args,
            &fffilter->inputs, &fffilter->outputs, NULL) < 0) {
        fffilter->logCb(logError, _FMT("Couldn't parse the filter description: " << args));
        if (fffilter->font) {
            fffilter->logCb(logError, _FMT("Does " << fffilter->font << " exist?"));
        }
        goto Error;
    }

    // Configure the filtergraph
    if (avfilter_graph_config(fffilter->graph, NULL) < 0) {
        fffilter->logCb(logError, _FMT("Couldn't configure the filter graph"));
        goto Error;
    }

    res = 0;

Error:
    if (res != 0) {
        _ff_filter_free(fffilter);
    }
    free(args);
    return res;
}

//-----------------------------------------------------------------------------
static int         ff_filter_seek               (stream_obj* stream,
                                                       INT64_T offset,
                                                        int flags)
{
    DECLARE_FF_FILTER(stream, fffilter);
    int res = default_seek(stream, offset, flags);
    _ff_filter_free(fffilter);
    return res;
}


//-----------------------------------------------------------------------------
static size_t      ff_filter_get_width          (stream_obj* stream)
{
    DECLARE_FF_FILTER(stream, fffilter);
    if (fffilter->transposeHW)
        return default_get_height(stream);
    return default_get_width(stream);
}

//-----------------------------------------------------------------------------
static size_t      ff_filter_get_height         (stream_obj* stream)
{
    DECLARE_FF_FILTER(stream, fffilter);
    if (fffilter->transposeHW)
        return default_get_width(stream);
    return default_get_height(stream);
}


//-----------------------------------------------------------------------------
static int         ff_filter_read_frame        (stream_obj* stream,
                                                    frame_obj** frame)
{
    DECLARE_FF_FILTER(stream, fffilter);

    frame_obj* tmp = NULL;
    int        res = -1;
    BoxList*   boxes = NULL;

    frame_api_t*    api;
    frame_obj*      newFrame;

    res = _ff_filter_read_frame( fffilter, fffilter->filterType, &tmp, &boxes );
    if ( res <= 0 ) {
        *frame = tmp;
        return res;
    }

    frame_api_t* tmpFrameAPI = frame_get_api(tmp);
    INT64_T ts = tmpFrameAPI->get_pts(tmp);

    // If size had changed, free the filter, and let it be recreated
    int f_width = tmpFrameAPI->get_width(tmp);
    int f_height = tmpFrameAPI->get_height(tmp);
    if ( f_width != fffilter->width || f_height != fffilter->height ) {
        if ( fffilter->width > 0 && fffilter->height > 0 ) {
            TRACE(_FMT("Output size had changed: "
                            << fffilter->width << "x" << fffilter->height
                            << " --> " << f_width << "x" << f_height));
        }
        fffilter->width = f_width;
        fffilter->height = f_height;
        _ff_filter_free(fffilter);
    } else if ( !fffilter->staticFilter ) {
        _ff_filter_free(fffilter);
    }

    if ( (fffilter->staticFilter && fffilter->graph == NULL) ||
         (!fffilter->staticFilter && boxes != NULL ) ) {
        if ( _ff_filter_build_graph( fffilter, boxes ) < 0 ) {
            return -1;
        }
    }

    if ( !fffilter->graph ) {
        TRACE( _FMT("filter doesn't apply to frame with pts=" << ts));
        *frame = tmp;
        return 0;
    }


    int newW = fffilter->transposeHW ? fffilter->height : fffilter->width;
    int newH = fffilter->transposeHW ? fffilter->width  : fffilter->height;

    AVFrame* srcFrame = (AVFrame*)tmpFrameAPI->get_backing_obj(tmp, "avframe");
    if ( srcFrame == NULL ) {
        av_image_fill_arrays(fffilter->srcFrame->data,
                       fffilter->srcFrame->linesize,
                       (const uint8_t*)tmpFrameAPI->get_data(tmp),
                       fffilter->pixfmt,
                       fffilter->width,
                       fffilter->height,
                       _kDefAlign );
        fffilter->srcFrame->width = fffilter->width;
        fffilter->srcFrame->height = fffilter->height;
        fffilter->srcFrame->format = fffilter->pixfmt;
        fffilter->srcFrame->pts = ts;
        srcFrame = fffilter->srcFrame;
    }

    AVFrame* dstFrame = av_frame_alloc();


    TRACE(_FMT("Applying a filter: pts=" << ts));
    if ((res = av_buffersrc_add_frame_flags(fffilter->bufsrc, srcFrame,
                                            AV_BUFFERSRC_FLAG_KEEP_REF)) < 0) {
        fffilter->logCb(logError, _FMT("filter failed to accept a frame " << res));
        goto Error;
    }
    if ((res = av_buffersink_get_frame(fffilter->bufsink, dstFrame)) < 0) {
        fffilter->logCb(logError, _FMT("filter failed to process frame " << res));
        goto Error;
    }

    if ( srcFrame->format != dstFrame->format ) {
        fffilter->logCb(logWarning, _FMT("Filter modified pixfmt from " << srcFrame->format <<
                                        " to " << dstFrame->format ));
    }

    api = get_ffframe_frame_api();
    newFrame = alloc_avframe_frame(FF_FILTER_MAGIC, fffilter->fa, fffilter->logCb);
    api->set_media_type(newFrame, mediaVideo);
    api->set_backing_obj(newFrame, "avframe", dstFrame);
    dstFrame = NULL;

    res = 0;
Error:
    frame_unref(&tmp);
    if (res!=0) {
        frame_unref(&newFrame);
    } else {
        *frame = newFrame;
    }
    if ( dstFrame ) {
        av_frame_free(&dstFrame);
    }
    return res;
}

//-----------------------------------------------------------------------------
static int         ff_filter_close             (stream_obj* stream)
{
    DECLARE_FF_FILTER(stream, fffilter);
    _ff_filter_free(fffilter);
    _ff_filter_clear(fffilter);
    return 0;
}


//-----------------------------------------------------------------------------
static void ff_filter_destroy         (stream_obj* stream)
{
    DECLARE_FF_FILTER_V(stream, fffilter);
    fffilter->logCb(logTrace, _FMT("Destroying stream object " << (void*)stream));
    ff_filter_close(stream); // make sure all the internals had been freed
    sv_freep(&fffilter->filterType);
    sv_freep(&fffilter->font);
    sv_freep(&fffilter->strftime);
    _ff_filter_destroy(fffilter);
    destroy_frame_allocator( &fffilter->fa, fffilter->logCb );
    stream_destroy( stream );
}

//-----------------------------------------------------------------------------
SVVIDEOLIB_API stream_api_t*     get_ff_filter_api                    ()
{
    ffmpeg_init();
    return &_g_ff_filter_provider;
}

