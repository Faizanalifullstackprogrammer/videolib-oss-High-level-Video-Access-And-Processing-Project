/*****************************************************************************
 *
 * LocalCaptureWrapper.hpp
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


#ifndef LocalCaptureWrapper_hpp
#define LocalCaptureWrapper_hpp

#include <MacTypes.h>

typedef void* LocalCaptureHandler;

/**
 Creates a Local Capture Handler.

 @return A newly instantiated Local Capture Handler.
 */
LocalCaptureHandler  lcwNew();


/**
 Releases the Local Capture Handler.

 @warning This needs to be called last to release free correctly.
 */
void            lcwDelete(LocalCaptureHandler lch);


/**
 Retrieves the number of attached and builtin video devices on the local machine.

 @param n A pre-allocated pointer that is set to the number of available video capture devices on this machine.

 @return 0 if successful. -1 if an error occured.
 */
OSErr           lcwGetNumberOfInputs(int* n);


/**
 Retrieves the local name of the capture device.

 @param globalDeviceID Chosen device; a number from 0 to n, where n is the number of devices on the local machine.
 @param deviceName A pre-allocated pointer to copy the name to.
 @param maxLengthOfDeviceName A pre-allocated pointer that provides the maximum number of characters to be copied.

 @return 0 if successful. -1 if an error occured.
 */
OSErr           lcwGetDeviceName(int globalDeviceID, char* deviceName, long* maxLengthOfDeviceName);


/**
 Retrieves the number of supported resolutions offered by the given capture device.

 @param globalDeviceID Chosen device; a number in the set [0, n), where n is the number of devices on the local machine.
 @param n A pre-allocated pointer that will be set to the number of supported resolutions offered by the capture device.

 @return 0 if successful. -1 if an error occured.
 */
OSErr           lcwGetNumberOfSupportedResolutionsFromDevice(int globalDeviceID, int* n);


/**
 Retrieves a pair of supported resolutions offered by the given capture device.

 @param globalDeviceID Chosen device; a number in the set [0, n), where n is the number of devices on the local machine.
 @param pairIdx The index of the supported resolution that is desired.
 @param width A pre-allocated pointer that will be set to the width of the supported resolution.
 @param height A pre-allocated pointer that will be set to the height of the supported resolution.

 @return 0 if successful. -1 if an error occured.
 */
OSErr           lcwGetSupportedResolutionPairFromDevice(int globalDeviceID, int pairIdx, int* width, int* height);


/**
 Sets up a device with the requested globalDeviceID, width, and height.

 @param lch The Local Capture Handler.
 @param globalDeviceID A value from 0 to n, where n is the number of devices.
 @param width Desired width resolution for the chosen device.
 @param height Desired height resolution for the chosen device.

 @return 0 if no erros occured, -1 if setup was not possible.

 @warning This needs to be called after lcwNew() in order for the other functions to work properly.
 */
OSErr           lcwSetupDevice(LocalCaptureHandler lch, int globalDeviceID, int width, int height);


/**
 Empty function. This will be implemented in the future for retrieving resolution formats of a chosen capture device.

 @param lch The Local Capture Handler.

 @return 0 if no erros occured, -1 otherwise.

 @warning This function always returns 0, since it's not fully implemented yet.
 */
OSErr           lcwGetSettings(LocalCaptureHandler lch);


/**
 Empty function. This is here in case it's needed in the future.

 @param lch The Local Capture Handler.
 */
void            lcwShowSettingsWindow(LocalCaptureHandler lch);


/**
 Starts the device, and starts pulling frames.

 @param lch The Local Capture Handler.

 @return 0 if no erros occured, -1 if there was a problem starting the device.
 */
OSErr           lcwStartGrabbing(LocalCaptureHandler lch);


/**
 Stops the device, and stops pulling frames.

 @param lch The Local Capture Handler.

 @return 0 if no erros occured, -1 if there was a problem stopping the device.
 */
OSErr           lcwStopGrabbing(LocalCaptureHandler lch);


/**
 Returns a pointer to the current video frame from the capture device.

 @param lch The Local Capture Handler.

 @return pointer to the first byte data in the video frame.
 */
unsigned char*  lcwGetPixels(LocalCaptureHandler lch);


/**
 Tries to acquire the lock to the current frame of the capture device for processing.

 @param lch The Local Capture Handler.

 @return Returns 1 if acquiring the lock was successful, 0 otherwise.

 @warning Without locking the frame, the capture device may overwrite
          the data in the frame, or it might release the memory location
          resulting in a dangling pointer or NULL pointer. Also, if the lock
          is acquired successfully, |lcwUnLockFrame()| must be called to
          release the lock!
 */
int            lcwLockFrame(LocalCaptureHandler lch);


/**
 Unlocks the current frame from the capture device.

 @param lch The Local Capture Handler.

 @see Refer to lcwLockFrame() for more details.
 */
void            lcwUnLockFrame(LocalCaptureHandler lch);


/**
 Returns the size of the frame currently captured by the device.

 @param lch The Local Capture Handler.

 @return Total size of the frame in bytes.
 */
long            lcwGetPixelDataSize(LocalCaptureHandler lch);


/**
 Sets |count| to the number of frames currently in the queue.

 @param lch The Local Capture Handler.
 @param count A pre-allocated pointer to set the number of frames to.

 @return 0 if successful. -1 if an error occured.

 @warning As of this writing, the LocalCapture object does not have a way to
          return the number of frames in the queue. So, |count| is actually
          set to 1 if the current frame is ready to be copyed, or 0
          if the frame is not ready. In the future, we may change this
          function to actually return the number of frames in the queue.
 */
OSErr           lcwQueuedFrameCount(LocalCaptureHandler lch, UInt8* count);


/**
 Gets the width of the current frame.

 @param lch The Local Capture Handler.

 @return The width of the frame.
 */
int             lcwGetWidth(LocalCaptureHandler lch);


/**
 Gets the height of the current frame.

 @param lch The Local Capture Handler.

 @return The height of the frame.
 */
int             lcwGetHeight(LocalCaptureHandler lch);


/**
 Currently an empty function.

 @param lch The Local Capture Handler.

 @warning It might be needed/implemented in the future
          if the we need to know the pixel format of the frame. Write now, the pixel
          format is hardcoded to be compatible with ffmpeg.
 */
void            lcwGetPixelFormat(LocalCaptureHandler lch);


/**
 Checks if the device is still running and pulling frames.

 @param lch The Local Capture Handler.

 @return 1 if the device is on and pulling frames, and 0 otherwise.
 */
int             lcwIsGrabbing(LocalCaptureHandler lch);


/**
 Checks if the frame is ready to be copied over.

 @param lch The Local Capture Handler.
 @param isUpdated A pre-allocated pointer that is set to 1 if the frame is ready, and 0 otherwise.

 @return 0 if successful. -1 if an error occured.
 */
OSErr           lcwIdle(LocalCaptureHandler lch, int* isUpdated);


/**
 Changes the state of the frame from ready to stail (or used).

 @param lch The Local Capture Handler.
 */
void            lcwUnSetIsFrameUpdated(LocalCaptureHandler lch);

#endif
