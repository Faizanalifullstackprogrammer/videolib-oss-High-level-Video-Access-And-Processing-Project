/*****************************************************************************
 *
 * sv_pixfmt.cpp
 *   Conversion between ffmpeg and videoLib pixel formats.
 *   Only small subset of those is covered, will be added as need.
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
#include "sv_pixfmt.h"

static const int unknownFmtMask = 0xf000;

extern "C"
enum AVPixelFormat   svpfmt_to_ffpfmt                    ( int svpfmt,
                                                    enum AVColorRange* colorRange )
{
    if ( colorRange ) {
        *colorRange = AVCOL_RANGE_UNSPECIFIED;
    }

    switch (svpfmt)
    {
    case pfmtRGB24      : return AV_PIX_FMT_RGB24;
    case pfmtBGR24      : return AV_PIX_FMT_BGR24;
    case pfmtRGB8       : return AV_PIX_FMT_RGB8;
    case pfmtYUV420P    : return AV_PIX_FMT_YUV420P;
    case pfmtYUV422P    : return AV_PIX_FMT_YUV422P;
    case pfmtYUV444P    : return AV_PIX_FMT_YUV444P;
    case pfmtYUYV422    : return AV_PIX_FMT_YUYV422;
    case pfmtNV12       : return AV_PIX_FMT_NV12;
    case pfmtNV16       : return AV_PIX_FMT_NV16;
    case pfmtNV20       : return AV_PIX_FMT_NV20;
    case pfmtNV21       : return AV_PIX_FMT_NV21;
    case pfmtUndefined  : return AV_PIX_FMT_NONE;
    case pfmtYUVJ420P   : if (colorRange) *colorRange = AVCOL_RANGE_JPEG; return AV_PIX_FMT_YUV420P;
    case pfmtYUVJ422P   : if (colorRange) *colorRange = AVCOL_RANGE_JPEG; return AV_PIX_FMT_YUV422P;
    case pfmtYUVJ444P   : if (colorRange) *colorRange = AVCOL_RANGE_JPEG; return AV_PIX_FMT_YUV444P;
    case pfmtRGBA       : return AV_PIX_FMT_RGBA;
    case pfmtARGB       : return AV_PIX_FMT_ARGB;
    default             : return (enum AVPixelFormat)((~unknownFmtMask)&svpfmt);
    }
}

extern "C"
enum AVPixelFormat   svpfmt_to_ffpfmt_ext                ( int svpfmt,
                                                    enum AVColorRange* colorRange,
                                                    int dstCodec )
{
    enum AVPixelFormat res;

    if ( dstCodec == streamMJPEG || dstCodec == streamJPG ) {
        switch (svpfmt) {
        case pfmtYUVJ420P   : if (colorRange) *colorRange = AVCOL_RANGE_JPEG; return AV_PIX_FMT_YUVJ420P;
        case pfmtYUVJ422P   : if (colorRange) *colorRange = AVCOL_RANGE_JPEG; return AV_PIX_FMT_YUVJ422P;
        case pfmtYUVJ444P   : if (colorRange) *colorRange = AVCOL_RANGE_JPEG; return AV_PIX_FMT_YUVJ444P;
        default             : res = svpfmt_to_ffpfmt( svpfmt, colorRange );
        }
    } else {
        res = svpfmt_to_ffpfmt( svpfmt, colorRange );
    }
    return res;
}


extern "C"
int         ffpfmt_to_svpfmt                    ( enum AVPixelFormat pfpfmt,
                                                  enum AVColorRange colorRange )
{
    switch (pfpfmt)
    {

    case AV_PIX_FMT_RGB24   : return pfmtRGB24;
    case AV_PIX_FMT_BGR24   : return pfmtBGR24;
    case AV_PIX_FMT_RGB8    : return pfmtRGB8;
    case AV_PIX_FMT_YUV420P : return (colorRange==AVCOL_RANGE_JPEG)?pfmtYUVJ420P:pfmtYUV420P;
    case AV_PIX_FMT_YUV422P : return (colorRange==AVCOL_RANGE_JPEG)?pfmtYUVJ422P:pfmtYUV422P;
    case AV_PIX_FMT_YUV444P : return (colorRange==AVCOL_RANGE_JPEG)?pfmtYUVJ444P:pfmtYUV444P;
    case AV_PIX_FMT_YUYV422 : return pfmtYUYV422;
    case AV_PIX_FMT_NV12    : return pfmtNV12;
    case AV_PIX_FMT_NV16    : return pfmtNV16;
    case AV_PIX_FMT_NV20    : return pfmtNV20;
    case AV_PIX_FMT_NV21    : return pfmtNV21;
    case AV_PIX_FMT_NONE    : return pfmtUndefined;
    case AV_PIX_FMT_YUVJ420P: return pfmtYUVJ420P;
    case AV_PIX_FMT_YUVJ422P: return pfmtYUVJ422P;
    case AV_PIX_FMT_YUVJ444P: return pfmtYUVJ444P;
    case AV_PIX_FMT_RGBA    : return pfmtRGBA;
    case AV_PIX_FMT_ARGB    : return pfmtARGB;
    default                 : return (unknownFmtMask|pfpfmt);
    }
}

extern "C"
int                        ffsfmt_to_svsfmt     ( enum AVSampleFormat ffsfmt,
                                                  int* interleaved,
                                                  int* sampleSize )
{
    int fmt = (unknownFmtMask|ffsfmt);
    int inter = 1;
    int size = 1;

    switch (ffsfmt)
    {
    case AV_SAMPLE_FMT_U8P: fmt = sfmtInt8;   inter = 0; size = 1; break;
    case AV_SAMPLE_FMT_U8 : fmt = sfmtInt8;   inter = 1; size = 1; break;
    case AV_SAMPLE_FMT_S16P:fmt = sfmtInt16;  inter = 0; size = 2; break;
    case AV_SAMPLE_FMT_S16: fmt = sfmtInt16;  inter = 1; size = 2; break;
    case AV_SAMPLE_FMT_S32P:fmt = sfmtInt32;  inter = 0; size = 4; break;
    case AV_SAMPLE_FMT_S32: fmt = sfmtInt32;  inter = 1; size = 4; break;
    case AV_SAMPLE_FMT_FLTP:fmt = sfmtFloat;  inter = 0; size = 4; break;
    case AV_SAMPLE_FMT_FLT: fmt = sfmtFloat;  inter = 1; size = 4; break;
    case AV_SAMPLE_FMT_DBLP:fmt = sfmtDouble; inter = 0; size = 8; break;
    case AV_SAMPLE_FMT_DBL: fmt = sfmtDouble; inter = 1; size = 8; break;
    case AV_SAMPLE_FMT_NONE:
    case AV_SAMPLE_FMT_S64:
    case AV_SAMPLE_FMT_S64P:
    default:
        break; // we won't handle some formats we don't expect to see with AAC/linear
    }
    if ( interleaved ) {
        *interleaved = inter;
    }
    if ( sampleSize ) {
        *sampleSize = size;
    }
    return fmt;
}


extern "C"
int                        svsfmt_to_ffsfmt     ( int format,
                                                  int interleaved )
{
    int fmt = AV_SAMPLE_FMT_NONE;

    switch (format)
    {
    case sfmtInt8:   return (interleaved ? AV_SAMPLE_FMT_U8  : AV_SAMPLE_FMT_U8P);
    case sfmtInt16:  return (interleaved ? AV_SAMPLE_FMT_S16 : AV_SAMPLE_FMT_S16P);
    case sfmtInt32:  return (interleaved ? AV_SAMPLE_FMT_S32 : AV_SAMPLE_FMT_S32P);
    case sfmtFloat:  return (interleaved ? AV_SAMPLE_FMT_FLT : AV_SAMPLE_FMT_FLTP);
    case sfmtDouble: return (interleaved ? AV_SAMPLE_FMT_DBL : AV_SAMPLE_FMT_DBLP);
    }

    return fmt;
}

