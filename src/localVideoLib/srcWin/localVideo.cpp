/*****************************************************************************
 *
 * localVideo.cpp
 *  Part of videoLib integration with USB/build-in camera for Windows
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

/*
 * Windows implementation of localVideoLib
 */
 #define SVLVL_EXPORTS 1

#include "localVideo.h"
#include "videoInput.h"

// Cached supported resolution pair
static Dimensions dimensions;

SVLVL_API LocalVideoHandle lvlNew(log_fn_t logFn, const char* unused)
{
    videoInput* v = new videoInput(logFn);
    return (LocalVideoHandle)v;
}

SVLVL_API void lvlDelete(LocalVideoHandle lvlH)
{
    videoInput* v = (videoInput*)lvlH;
    delete v;
}

SVLVL_API void lvlSetVerbose(BOOL _verbose)
{
    videoInput::setVerbose((bool)_verbose);
}

SVLVL_API int lvlListDevices(log_fn_t logFn)
{
    // We pass in "TRUE" to silence the function.
    return videoInput::listDevices(TRUE, logFn);
}

SVLVL_API int lvlListDevicesWithoutResolutionList(log_fn_t logFn)
{
    // We pass in "TRUE" to silence the function.
    return videoInput::listDevicesWithoutResolutionList(TRUE, logFn);
}

SVLVL_API char* lvlGetDeviceName(int deviceID)
{
    return videoInput::getDeviceName(deviceID);
}

SVLVL_API int lvlGetNumSupportedResolutionsOfDevice(int deviceID)
{
	int numSupportedResolutions = 0;
	videoInput::getNumSupportedResolutionsOfDevice(deviceID, &numSupportedResolutions);
    return numSupportedResolutions;
}

SVLVL_API Dimensions* lvlGetSupportedResolutionPairOfDevice(int deviceID, int resPairIdx)
{
	videoInput::getSupportedResolutionPairOfDevice(deviceID, resPairIdx, &dimensions.width, &dimensions.height);
    return &dimensions;
}

SVLVL_API BOOL lvlSetupDevice(LocalVideoHandle lvlH, int deviceID, int w, int h)
{
    videoInput* v = (videoInput*)lvlH;
    return (BOOL)v->setupDevice(deviceID, w, h);
}

SVLVL_API BOOL lvlIsDeviceSetup(LocalVideoHandle lvlH, int deviceID)
{
    videoInput* v = (videoInput*)lvlH;
    return (BOOL)v->isDeviceSetup(deviceID);
}

SVLVL_API int lvlGetWidth(LocalVideoHandle lvlH, int deviceID)
{
    videoInput* v = (videoInput*)lvlH;
    return v->getWidth(deviceID);
}

SVLVL_API int lvlGetHeight(LocalVideoHandle lvlH, int deviceID)
{
    videoInput* v = (videoInput*)lvlH;
    return v->getHeight(deviceID);
}

SVLVL_API int lvlGetSize(LocalVideoHandle lvlH, int deviceID)
{
    videoInput* v = (videoInput*)lvlH;
    return v->getSize(deviceID);
}

SVLVL_API int lvlGetPixelFormat()
{
    // Defined in ffMPEG's avutil.h
    return PIXEL_FORMAT_RGB24;
}

SVLVL_API BOOL lvlIsFrameNew(LocalVideoHandle lvlH, int deviceID)
{
    videoInput* v = (videoInput*)lvlH;
    return (BOOL)v->isFrameNew(deviceID);
}

SVLVL_API BOOL lvlGetPixels (LocalVideoHandle lvlH, int deviceID, unsigned char* pixels,
                   BOOL flipRedAndBlue, BOOL flipImage)
{
    videoInput* v = (videoInput*)lvlH;
    return (BOOL)v->getPixels(deviceID, pixels, flipRedAndBlue, flipImage);
}

SVLVL_API void lvlShowSettingsWindow(LocalVideoHandle lvlH, int deviceID)
{
    videoInput* v = (videoInput*)lvlH;
    v->showSettingsWindow(deviceID);
}

SVLVL_API void lvlStopDevice(LocalVideoHandle lvlH, int deviceID)
{
    videoInput* v = (videoInput*)lvlH;
    v->stopDevice(deviceID);
}
