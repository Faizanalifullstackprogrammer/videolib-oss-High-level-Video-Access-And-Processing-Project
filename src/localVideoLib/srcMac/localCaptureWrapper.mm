/*****************************************************************************
 *
 * LocalCaptureWrapper.mm
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

#import "localCaptureWrapper.hpp"
#import "localCapture.hpp"

class LocalCaptureData {
public:
    LocalCapture* localCapture;
};

LocalCaptureHandler lcwNew()

{
    LocalCaptureData* data = new LocalCaptureData();

    // Create and store a new video handler.
    data->localCapture = [[LocalCapture alloc] init];

    return (LocalCaptureHandler) data;
}

void lcwDelete(LocalCaptureHandler lch)

{
    // Check if NULL;
    if (!lch) {
        return;
    }

    // Release all memory used by the video handler.
    LocalCaptureData* data = (LocalCaptureData*)lch;

    if (data->localCapture) {
        [data->localCapture lcDeallocate];
        DEALLOC_OBJ(data->localCapture);
        data->localCapture = nil;
    }

    free(data);
}

OSErr lcwGetNumberOfInputs(int* n)

{
    return [LocalCapture getNumberOfInputs:n];
}

OSErr lcwGetDeviceName(int globalDeviceID, char* deviceName, long* maxLengthOfDeviceName)

{
    return [LocalCapture getDeviceName:globalDeviceID destinationArray:deviceName lengthOfDestArray:maxLengthOfDeviceName];
}

OSErr lcwGetNumberOfSupportedResolutionsFromDevice(int globalDeviceID, int* n)

{
    return [LocalCapture getNumberOfSupportedResolutionsFromDevice:globalDeviceID num:n];
}

OSErr lcwGetSupportedResolutionPairFromDevice(int globalDeviceID, int pairIdx, int* width, int* height)

{
    return [LocalCapture getSupportedResolutionPairFromDevice:globalDeviceID resPairIdx:pairIdx width:width height:height];
}

OSErr lcwSetupDevice(LocalCaptureHandler lch, int globalDeviceID, int width, int height)

{
    return [((LocalCaptureData*)lch)->localCapture setupDevice:globalDeviceID withWidth:width withHeight:height];
}

OSErr lcwGetSettings(LocalCaptureHandler lch)

{
    return [((LocalCaptureData*)lch)->localCapture getSettings];
}

void lcwShowSettingsWindow(LocalCaptureHandler lch)

{
    [((LocalCaptureData*)lch)->localCapture showSettingsWindow];
}

OSErr lcwStartGrabbing(LocalCaptureHandler lch)

{
    return [((LocalCaptureData*)lch)->localCapture startGrabbing];
}

OSErr lcwStopGrabbing(LocalCaptureHandler lch)

{
    return [((LocalCaptureData*)lch)->localCapture stopGrabbing];
}

unsigned char* lcwGetPixels(LocalCaptureHandler lch)

{
    return [((LocalCaptureData*)lch)->localCapture getPixels];
}

int lcwLockFrame(LocalCaptureHandler lch)

{
    return [((LocalCaptureData*)lch)->localCapture lockFrame];
}

void lcwUnLockFrame(LocalCaptureHandler lch)

{
    [((LocalCaptureData*)lch)->localCapture unLockFrame];
}

long lcwGetPixelDataSize(LocalCaptureHandler lch)

{
    return [((LocalCaptureData*)lch)->localCapture getPixelDataSize];
}

OSErr lcwQueuedFrameCount(LocalCaptureHandler lch, UInt8* count)

{
    return [((LocalCaptureData*)lch)->localCapture queuedFrameCount:count];
}

int lcwGetWidth(LocalCaptureHandler lch)

{
    return [((LocalCaptureData*)lch)->localCapture getWidth];
}

int lcwGetHeight(LocalCaptureHandler lch)

{
    return [((LocalCaptureData*)lch)->localCapture getHeight];
}

void lcwGetPixelFormat(LocalCaptureHandler lch)

{
    [((LocalCaptureData*)lch)->localCapture getPixelFormat];
}

int lcwIsGrabbing(LocalCaptureHandler lch)

{
    return [((LocalCaptureData*)lch)->localCapture isGrabbing];
}

OSErr lcwIdle(LocalCaptureHandler lch, int* isUpdated)

{
    return [((LocalCaptureData*)lch)->localCapture idle:isUpdated];
}

void lcwUnSetIsFrameUpdated(LocalCaptureHandler lch)

{
    [((LocalCaptureData*)lch)->localCapture setIsFrameUpdated:0];
}
