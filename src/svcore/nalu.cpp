/*****************************************************************************
 *
 * nalu.cpp
 *   NAL units primitives - minimal bitstream parsing needed.
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
#include "stream.h"
#include "nalu.h"

//extern "C" {
//#include <libavutil/mem.h>
//}

////////////////////////////////////////////////////////////////////////////////////////////////
static char kAnnexBHeader[] = { 0,0,0,1 };
static const size_t kAnnexBHeaderSize = sizeof(kAnnexBHeader);


////////////////////////////////////////////////////////////////////////////////////////////////
/*
    Finds next NAL unit in the buffer.
    Parameters:
    data            - buffer pointer
    sizeInOut       - in: size of the buffer;
                      out: size of the remaining buffer, including discovered NALU and header, or 0
    nalHdsSize      - size of NALU header if found, or 0
    nalType         - type of NALU if found, or undetermined
    logCb           - log callback
    Returns         - pointer to the header of next NALU if found, or NULL otherwise
*/
extern "C"
uint8_t* videolibapi_find_next_nal       ( uint8_t* data,
                                          int* sizeInOut,
                                          size_t* nalHdrSize,
                                          uint8_t* nalType,
                                          fn_stream_log logCb )
{
    static const char mask = 0x1f;
    int size = *sizeInOut;

    while (size > 3) {
        if ( !data[0] && !data[1] ) {
            if ( !data[2] && size>4 && data[3]==1 ) {
                *nalHdrSize = 4;
                *nalType = data[4]&mask;
                *sizeInOut = size;
    //            logCb(logTrace, _FMT("Found NALU " << (int)*nalType ));
                return data;
            }
            if ( data[2] == 1 ) {
                *nalHdrSize = 3;
                *nalType = data[3]&mask;
                *sizeInOut = size;
    //            logCb(logTrace, _FMT("Found NALU " << (int)*nalType ));
                return data;
            }
        }
        data++;
        size--;
    }

//    logCb(logTrace, _FMT("No more NALU!" ));
    *sizeInOut = 0;
    *nalHdrSize = 0;
    return NULL;
}

////////////////////////////////////////////////////////////////////////////////////////////////
extern "C"
int videolibapi_extract_nalu(uint8_t* data, size_t size,
                        uint8_t nalTypeWanted, uint8_t** mem,
                        size_t* naluSize,
                        size_t* remainingSize,
                        fn_stream_log logCb)
{
    size_t      nalHdrSize = 0;
    uint8_t     nalType = 0;
    int         remaining = size, remainingAfterNAL;
    uint8_t*    nal = data;
    uint8_t*    nextNAL;

    while ( remaining > 0 ) {
        nal = videolibapi_find_next_nal(data, &remaining, &nalHdrSize, &nalType, logCb);
        if ( nal ) {
            if ( nalType == nalTypeWanted || nalTypeWanted == (uint8_t)-1 ) {
                remainingAfterNAL = remaining - nalHdrSize;
                nextNAL = videolibapi_find_next_nal(nal + nalHdrSize, &remainingAfterNAL, &nalHdrSize, &nalType, logCb);
                if ( nextNAL != NULL ) {
                    *naluSize = remaining - remainingAfterNAL;
                    if ( remainingSize ) *remainingSize = remainingAfterNAL;
                } else {
                    *naluSize = remaining;
                    if ( remainingSize ) *remainingSize = 0;
                }
                *mem = (uint8_t*)malloc( *naluSize );
                memcpy( *mem, nal, *naluSize );
                return 1;
            } if ( nalType == kNALIFrame || nalType == kNALCodedSlice ) {
                // won't find anything after that
                return 0;
            }
            data = nal + nalHdrSize;
            remaining -= nalHdrSize;
        } else {
            return 0;
        }
    }
    return 0;

}

////////////////////////////////////////////////////////////////////////////////////////////////
/*
    Determines if provided buffer contains an I-frame
    Parameters:
    data            - buffer
    size            - buffer size
    logCb           - log callback
    Returns:        - 1 if I-frame is found, 0 otherwise

*/
extern "C"
int     videolibapi_contains_idr_frame   ( uint8_t* data, size_t size,
                                            fn_stream_log logCb )
{
    size_t      nalHdrSize = 0;
    uint8_t     nalType = 0;
    int         remaining = size;
//    logCb(logTrace, _FMT("Checking frame for IDR NALU: size=" << size));
    while ( remaining > 0 ) {
        data = videolibapi_find_next_nal(data, &remaining, &nalHdrSize, &nalType, logCb);
        if ( data == NULL )  {
//            logCb(logTrace, _FMT("No more NALU in the frame!"));
            return 0;
        }
//        logCb(logTrace, _FMT("Found NALU of type " << (int)nalType << " at " << size - remaining));
        if ( nalType == kNALIFrame ) {
//            logCb(logTrace, _FMT("Found IDR frame!" ));
            return 1;
        }
        if ( nalType == kNALCodedSlice ) {
            // IDR cannot follow non-IDR slice, so save some time and exit
            return 0;
        }
        data += nalHdrSize;
        remaining -= nalHdrSize;
    }
    return 0;
}

////////////////////////////////////////////////////////////////////////////////////////////////
extern "C"
int     videolibapi_contained_nalu   ( uint8_t* data, size_t size,
                                            fn_stream_log logCb )
{
    size_t      nalHdrSize = 0;
    uint8_t     nalType = 0;
    int         remaining = size;
    int         result = 0;
    while ( remaining > 0 && data != NULL ) {
        data = videolibapi_find_next_nal(data, &remaining, &nalHdrSize, &nalType, logCb);
        if ( data != NULL )  {
            int nalBit = (1 << (nalType-1));
            result |= nalBit;
            data += nalHdrSize;
            remaining -= nalHdrSize;
        }
    }
    return result;
}


////////////////////////////////////////////////////////////////////////////////////////////////
extern "C"
uint8_t videolibapi_get_nalu_type       ( uint8_t* data, size_t size )
{
    static const char mask = 0x1f;
    // ascertain NAL header
    if (size>4 && !data[0] && !data[1] && !data[2] && data[3]==1)
        return data[4]&mask;
    // ascertain NAL header
    if (size>3 && !data[0] && !data[1] && data[2]==1 )
        return data[3]&mask;
    return 0;
}


////////////////////////////////////////////////////////////////////////////////////////////////
static unsigned int _read_bit(unsigned char* pStream, int& nCurrentBit)
{
    int nIndex = nCurrentBit / 8;
    int nOffset = nCurrentBit % 8 + 1;

    nCurrentBit ++;
    return (pStream[nIndex] >> (8-nOffset)) & 0x01;
}

static unsigned int _read_bits(int n, unsigned char* pStream, int& nCurrentBit)
{
    int r = 0;
    int i;
    for (i = 0; i < n; i++) {
        r |= ( _read_bit(pStream, nCurrentBit) << ( n - i - 1 ) );
    }
    return r;
}

static unsigned int _read_exponential_golomb_code(unsigned char* pStream, int& nCurrentBit)
{
    int r = 0;
    int i = 0;

    while( (_read_bit(pStream, nCurrentBit) == 0) && (i < 32) ) {
        i++;
    }

    r = _read_bits(i, pStream, nCurrentBit);
    r += (1 << i) - 1;
    return r;
}

static unsigned int _read_se(unsigned char* pStream, int& nCurrentBit)
{
    int r = _read_exponential_golomb_code(pStream, nCurrentBit);
    if (r & 0x01) {
        r = (r+1)/2;
    } else {
        r = -(r/2);
    }
    return r;
}

extern "C"
void videolibapi_parse_sps(unsigned char * pStart, unsigned short _nLen, int* w, int* h, int* profile, int* level)
{
#define ReadBits(a) _read_bits(a, pStart, nCurrentBit)
#define ReadBit() _read_bit(pStart, nCurrentBit)
#define ReadSE() _read_se(pStart, nCurrentBit)
#define ReadExponentialGolombCode() _read_exponential_golomb_code(pStart, nCurrentBit)

    if (_nLen >= 4 || memcmp(pStart, kAnnexBHeader, kAnnexBHeaderSize) == 0 ) {
        // not AnnexB, need to write a header
        _nLen -= kAnnexBHeaderSize;
        pStart += kAnnexBHeaderSize;
    }

    int nCurrentBit = 0;

    int frame_crop_left_offset=0;
    int frame_crop_right_offset=0;
    int frame_crop_top_offset=0;
    int frame_crop_bottom_offset=0;

    int skipBits = ReadBits(8); // forbidden_zero_bit; nal_ref_idc; nal_unit_type
    int profile_idc = ReadBits(8);
    int constraint_set0_flag = ReadBit();
    int constraint_set1_flag = ReadBit();
    int constraint_set2_flag = ReadBit();
    int constraint_set3_flag = ReadBit();
    int constraint_set4_flag = ReadBit();
    int constraint_set5_flag = ReadBit();
    int reserved_zero_2bits  = ReadBits(2);
    int level_idc = ReadBits(8);
    int seq_parameter_set_id = ReadExponentialGolombCode();


    if( profile_idc == 100 || profile_idc == 110 ||
        profile_idc == 122 || profile_idc == 244 ||
        profile_idc == 44 || profile_idc == 83 ||
        profile_idc == 86 || profile_idc == 118 ) {
        int chroma_format_idc = ReadExponentialGolombCode();

        if( chroma_format_idc == 3 ) {
            int residual_colour_transform_flag = ReadBit();
        }
        int bit_depth_luma_minus8 = ReadExponentialGolombCode();
        int bit_depth_chroma_minus8 = ReadExponentialGolombCode();
        int qpprime_y_zero_transform_bypass_flag = ReadBit();
        int seq_scaling_matrix_present_flag = ReadBit();

        if (seq_scaling_matrix_present_flag) {
            int i=0;
            for ( i = 0; i < 8; i++) {
                int seq_scaling_list_present_flag = ReadBit();
                if (seq_scaling_list_present_flag) {
                    int sizeOfScalingList = (i < 6) ? 16 : 64;
                    int lastScale = 8;
                    int nextScale = 8;
                    int j=0;
                    for ( j = 0; j < sizeOfScalingList; j++) {
                        if (nextScale != 0) {
                            int delta_scale = ReadSE();
                            nextScale = (lastScale + delta_scale + 256) % 256;
                        }
                        lastScale = (nextScale == 0) ? lastScale : nextScale;
                    }
                }
            }
        }
    }

    int log2_max_frame_num_minus4 = ReadExponentialGolombCode();
    int pic_order_cnt_type = ReadExponentialGolombCode();
    if( pic_order_cnt_type == 0 ){
        int log2_max_pic_order_cnt_lsb_minus4 = ReadExponentialGolombCode();
    } else if( pic_order_cnt_type == 1 ) {
        int delta_pic_order_always_zero_flag = ReadBit();
        int offset_for_non_ref_pic = ReadSE();
        int offset_for_top_to_bottom_field = ReadSE();
        int num_ref_frames_in_pic_order_cnt_cycle = ReadExponentialGolombCode();
        int i;
        for( i = 0; i < num_ref_frames_in_pic_order_cnt_cycle; i++ ) {
            ReadSE();
            //sps->offset_for_ref_frame[ i ] = ReadSE();
        }
    }
    int max_num_ref_frames = ReadExponentialGolombCode();
    int gaps_in_frame_num_value_allowed_flag = ReadBit();
    int pic_width_in_mbs_minus1 = ReadExponentialGolombCode();
    int pic_height_in_map_units_minus1 = ReadExponentialGolombCode();
    int frame_mbs_only_flag = ReadBit();
    if( !frame_mbs_only_flag ) {
        int mb_adaptive_frame_field_flag = ReadBit();
    }
    int direct_8x8_inference_flag = ReadBit();
    int frame_cropping_flag = ReadBit();
    if( frame_cropping_flag ) {
        frame_crop_left_offset = ReadExponentialGolombCode();
        frame_crop_right_offset = ReadExponentialGolombCode();
        frame_crop_top_offset = ReadExponentialGolombCode();
        frame_crop_bottom_offset = ReadExponentialGolombCode();
    }
    int vui_parameters_present_flag = ReadBit();

    *w = ((pic_width_in_mbs_minus1 +1)*16) - frame_crop_bottom_offset*2 - frame_crop_top_offset*2;
    *h = ((2 - frame_mbs_only_flag)* (pic_height_in_map_units_minus1 +1) * 16) - (frame_crop_right_offset * 2) - (frame_crop_left_offset * 2);
    *profile = profile_idc;
    *level = level_idc;

#undef ReadBits
#undef ReadBit
#undef ReadSE
#undef ReadExponentialGolombCode

}