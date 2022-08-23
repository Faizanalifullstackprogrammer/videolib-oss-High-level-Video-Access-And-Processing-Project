/*****************************************************************************
 *
 * LocalCapture.hpp
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


#import <Foundation/Foundation.h>
#import <AVFoundation/AVFoundation.h>
#import <AppKit/AppKit.h>

#if !__has_feature(objc_arc)
#define RETAIN_OBJ(name) [name retain]
#define RELEASE_OBJ(name) if(name){[name release];name=nil;}
#define DEALLOC_OBJ(name) [name dealloc]
#define RETAIN_DISPATCH_OBJ(name) dispatch_retain(name)
#define RELEASE_DISPATCH_OBJ(name) if(name){dispatch_release(name);}
#else
#define RETAIN_OBJ(name)
#define RELEASE_OBJ(name) if(name){name=nil;}
#define DEALLOC_OBJ(name)
#define RETAIN_DISPATCH_OBJ(name)
#define RELEASE_DISPATCH_OBJ(name)
#endif

/**
 This class allows and establishes a connection to video devices on the local machine.
 Once a connection is established, video frames from the device can be accessed and copied.
 */
@interface LocalCapture : NSObject
{
    @private
    /**
     The |session| object controls the flow of data between inputs and outputs.
     */
    AVCaptureSession *_session;

    /**
     The current video device to pull frames from. Has to be added to |AVCaptureDeviceInput| once it's been chosen.
     */
    AVCaptureDevice *_selectedVideoDevice;

    /**
     Stores and manages the selected video device. Once set, this input object is added to the |session| object.
     */
    AVCaptureDeviceInput *_videoDeviceInput;

    /**
     Stores and manages the output. In this case, a video frame. Once set, this output object is added to the |session| object.
     */
    AVCaptureVideoDataOutput *_videoDeviceOutput;

    /**
     Stores an Objective-C Mutex that prevents the |framePixelBuffer| from being read from and written to at the same time.
     */
    NSLock *_nsLock;

    /**
     The serial dispatch queue that the capture device fills up with video frames.
     */
    dispatch_queue_t _frameQueue;
    /**
     Total size of the frame buffer in bytes. Usually, the calculation performed to get this value is ( width * height * numBytesPerPixel ).
     */
    size_t _frameBufferPixelDataSize;

    /**
     Address to the current frame data. Specifically, it's a pointer to the first byte data in |frameData| object.
     */
    unsigned char *_framePixelBuffer;

    /**
     Stores the ready state of the frame. 1 if the frame is ready to be copied/viewed, 0 otherwise.
     */
    int _isFrameUpdated;

    /**
     The resolution width of the frame.
     */
    int _frameWidth;

    /**
     The resolution height of the frame.
     */
    int _frameHeight;
}

@property (assign) int isFrameUpdated;

/**
 Retrieves the number of attached and builtin video devices on the local machine.

 @param n A pre-allocated pointer that is set to the number of available video capture devices on this machine.

 @return 0 if successful. -1 if an error occured.
 */
+ (OSErr)           getNumberOfInputs:(int*)n;


/**
 Retrieves the local name of the capture device.

 @param globalDeviceID Chosen device; a number from 0 to n, where n is the number of devices on the local machine.
 @param deviceName A pre-allocated pointer to copy the name to.
 @param maxLengthOfDeviceName A pre-allocated pointer that provides the maximum number of characters to be copied.

 @return 0 if successful. -1 if an error occured.
 */
+ (OSErr)           getDeviceName:(int)globalDeviceID destinationArray:(char*)deviceName lengthOfDestArray:(long*)maxLengthOfDeviceName;


/**
 Retrieves the number of native resolutions offered by the given video capture device.

 @param globalDeviceID Chosen device; a number in the set [0, n), where n is the number of devices on the local machine.
 @param n A pre-allocated pointer that will be set to the number of native resolutions offered by the capture device.

 @return 0 if successful. -1 if an error occured.
 */
+ (OSErr)           getNumberOfSupportedResolutionsFromDevice:(int)globalDeviceID num:(int*)n;


/**
 Retrieves a pair of supported resolutions offered by the given capture device.

 @param globalDeviceID Chosen device; a number in the set [0, n), where n is the number of devices on the local machine.
 @param pairIdx The index of the supported resolution that is desired.
 @param width A pre-allocated pointer that will be set to the width of the supported resolution.
 @param height A pre-allocated pointer that will be set to the height of the supported resolution.

 @return 0 if successful. -1 if an error occured.
 */
+ (OSErr)           getSupportedResolutionPairFromDevice:(int)globalDeviceID resPairIdx:(int)pairIdx width:(int*)width height:(int*)height;


/**
 Releases all retained objects.

 @warning This needs to be called last to free memory correctly.
 */
- (void)            lcDeallocate;


/**
 Sets up a device with the requested width and height.

 @param globalDeviceID A value from 0 to n, where n is the number of devices.
 @param width Desired width resolution for the chosen device.
 @param height Desired height resolution for the chosen device.

 @return 0 if no erros occured, -1 if setup was not possible.

 @warning This needs to be called after lcwNew() in order for the other functions to work properly.
 */
- (OSErr)           setupDevice:(int)globalDeviceID withWidth:(int)width withHeight:(int)height;


/**
 Empty function. This will be implemented in the future for retrieving resolution formats of a chosen capture device.

 @return 0 if no erros occured, -1 otherwise.

 @warning This function always returns 0, since it's not fully implemented yet.
 */
- (OSErr)           getSettings;


/**
 Empty function. This is here in case it's needed in the future.
 */
- (void)            showSettingsWindow;


/**
 Starts the device, and starts pulling frames.

 @return 0 if no erros occured, -1 if there was a problem starting the device.
 */
- (OSErr)           startGrabbing;


/**
 Stops the device, and stops pulling frames.

 @return 0 if no erros occured, -1 if there was a problem stopping the device.
 */
- (OSErr)           stopGrabbing;


/**
 Returns a pointer to the current video frame from the capture device.

 @return pointer to the first byte data in the video frame.
 */
- (unsigned char*)  getPixels;


/**
 Locks the current frame from the capture device for processing.

 @warning Without locking the frame, the capture device may overwrite
 the data in the frame, or it might release the memory location
 resulting in a dangling pointer or NULL pointer.
 */
- (int)            lockFrame;


/**
 Unlocks the current frame from the capture device.

 @see Refer to lockFrame for more details.
 */
- (void)            unLockFrame;


/**
 Returns the size of the frame currently captured by the device.

 @return Total size of the frame in bytes.
 */
- (long)            getPixelDataSize;


/**
 Sets |count| to the number of frames currently in the queue.

 @param count A pre-allocated pointer to set the number of frames to.

 @return 0 if successful. -1 if an error occured.

 @warning As of this writing, the LocalCapture object does not have a way to
 return the number of frames in the queue. So, |count| is actually
 set to 1 if the current frame is ready to be copyed, or 0
 if the frame is not ready. In the future, we may change this
 function to actually return the number of frames in the queue.
 */
- (OSErr)           queuedFrameCount:(UInt8*)count;


/**
 Gets the width of the current frame.

 @return The width of the frame.
 */
- (int)             getWidth;


/**
 Gets the height of the current frame.

 @return The height of the frame.
 */
- (int)             getHeight;


/**
 Currently an empty function.

 @warning It might be needed/implemented in the future
 if the we need to know the pixel format of the frame. Write now, the pixel
 format is hardcoded to be compatible with ffmpeg.
 */
- (void)            getPixelFormat;


/**
 Checks if the device is still running and pulling frames.

 @return 1 if the device is on and pulling frames, and 0 otherwise.
 */
- (int)             isGrabbing;


/**
 Checks if the frame is ready to be copied over.

 @param isUpdated A pre-allocated pointer that is set to 1 if the frame is ready, and 0 otherwise.

 @return 0 if successful. -1 if an error occured.
 */
- (OSErr)           idle:(int*)isUpdated;

@end
