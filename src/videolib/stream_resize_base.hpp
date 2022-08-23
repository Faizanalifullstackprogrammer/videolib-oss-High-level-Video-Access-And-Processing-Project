/*****************************************************************************
 *
 * stream_resize_base.hpp
 *   Common elements of node objects performing color conversion and resize operations.
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

#include "streamprv.h"


typedef struct resize_dims {
    size_t      width;
    size_t      height;
    float       resizeFactor;
    int         keepAspectRatio;
} resize_dims;

//-----------------------------------------------------------------------------
typedef struct resize_base  : public stream_base  {
    resize_dims         dimSetting;
    resize_dims         dimActual;
    int                 allowUpsize;
    int                 updatePending;
    size_t              minHeight;
    int                 pixfmt;
    int                 colorSpace;
    int                 colorRange;
    size_t              inputHeight;
    size_t              inputWidth;
    int                 inputPixFmt;
    int                 retainSourceFrameInterval;
    INT64_T             prevFramePts;
} resize_base_obj;


//-----------------------------------------------------------------------------
void        resize_base_init           (resize_base_obj* r);
void        resize_base_flag_for_reopen(resize_base_obj* r);
int         resize_base_set_param      (resize_base_obj* r,
                                        const CHAR_T* name,
                                        const void* value);
void        resize_base_proxy_params   (const resize_base_obj* r,
                                        resize_base_obj* other);
int         resize_base_get_param      (resize_base_obj* r,
                                        const CHAR_T* name,
                                        void* value,
                                        size_t* size);
int         resize_base_open_in        (resize_base_obj* r);
size_t      resize_base_get_width      (resize_base_obj* r);
size_t      resize_base_get_height     (resize_base_obj* r);
int         resize_base_get_pixel_format(resize_base_obj* r);
frame_obj*  resize_base_pre_process    (resize_base_obj* r, frame_obj** frame, int* res);
