/*****************************************************************************
 *
 * localVideoServer.h
 *  Part of videoLib integration with USB/build-in camera for Mac
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

// RPC messages supported by the server
enum LVS_MESSAGES {
    LVS_TEST_CONNECTION,
    LVS_SET_VERBOSE,
    LVS_LIST_DEVICES,
    LVS_LIST_DEVICES_ONLY,
    LVS_GET_DEVICE_NAME,
    LVS_SETUP_DEVICE,
    LVS_IS_DEVICE_SETUP,
    LVS_STOP_DEVICE,
    LVS_GET_WIDTH,
    LVS_GET_HEIGHT,
    LVS_GET_SIZE,
    LVS_IS_FRAME_NEW,
    LVS_GET_PIXELS,
    LVS_SHOW_SETTINGS_WINDOW,
    LVS_MESSAGES_COUNT          // Total number of messages
};

// Names of messages, for error reporting
const char * LVS_MSG_NAMES[LVS_MESSAGES_COUNT] = {
    "LVS_TEST_CONNECTION",
    "LVS_SET_VERBOSE",
    "LVS_LIST_DEVICES",
    "LVS_LIST_DEVICES_ONLY",
    "LVS_GET_DEVICE_NAME",
    "LVS_SETUP_DEVICE",
    "LVS_IS_DEVICE_SETUP",
    "LVS_STOP_DEVICE",
    "LVS_GET_WIDTH",
    "LVS_GET_HEIGHT",
    "LVS_GET_SIZE",
    "LVS_IS_FRAME_NEW",
    "LVS_GET_PIXELS",
    "LVS_SHOW_SETTINGS_WINDOW",
};

// Message responses
const char* LVS_RESPONSE_SUCCESS = "OK";
const char* LVS_RESPONSE_ERROR = "ERR";


// Memory map constants
const int kMaxFrameWidth = 1920;
const int kMaxFrameHeight = 1080;
const int kParamDataOffset = 0;     // Location in map of message parameters
const int kVideoFrameOffset = 3072; // Location in map of pixel data
const int kPixelDataSize = kMaxFrameWidth*kMaxFrameHeight*2; // maximum data
                                                             // size for YUV2
const int kMmapSize = kVideoFrameOffset+kPixelDataSize;

// Other shared constants
const int kMaxCameras = 20;
const int kMaxResolutions = 30;  // Maximum number of supported resolutions

// Data structures for message parameters
typedef struct setVerboseParams {
    BOOL verbose;           // input parameter
} setVerboseParamsT;

typedef struct listDevicesParams {
    int numDevices;                                          // return value
    char deviceNames[kMaxCameras][256];                      // return value
    int numResolutionsPerDevice[kMaxCameras];                // return value
    int deviceResolutions[kMaxCameras][kMaxResolutions][2];  // return value
} listDevicesParamsT;

typedef struct getDeviceNameParams {
    int deviceID;           // input parameter
    char deviceName[256];   // return value
} getDeviceNameParamsT;

typedef struct setupDeviceParams {
    int deviceID;           // input parameter
    int width;              // input parameter
    int height;             // input parameter
    BOOL success;           // return value
} setupDeviceParamsT;

typedef struct isDeviceSetupParams {
    int deviceID;           // input parameter
    BOOL isSetup;           // return value
} isDeviceSetupParamsT;

typedef struct stopDeviceParams {
    int deviceID;           // input parameter
} stopDeviceParamsT;

typedef struct getWidthParams {
    int deviceID;           // input parameter
    int width;              // return value
} getWidthParamsT;

typedef struct getHeightParams {
    int deviceID;           // input parameter
    int height;             // return value
} getHeightParamsT;

typedef struct getSizeParams {
    int deviceID;           // input parameter
    int size;               // return value
} getSizeParamsT;

typedef struct isFrameNewParams {
    int deviceID;           // input parameter
    BOOL isNew;             // return value
} isFrameNewParamsT;

typedef struct getPixelsParams {
    int deviceID;                               // input parameter
    BOOL success;                               // return value
    int pixelDataSize;                          // return value
} getPixelsParamsT;

typedef struct showSettingsWindowParams {
    int deviceID;           // input parameter
} showSettingsWindowParamsT;

