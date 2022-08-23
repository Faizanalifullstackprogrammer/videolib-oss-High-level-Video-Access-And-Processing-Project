/*****************************************************************************
 *
 * LocalCapture.mm
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


#import "localCapture.hpp"

#include <cmath>
using namespace std;

@interface LocalCapture () <AVCaptureVideoDataOutputSampleBufferDelegate>

/**
 The |retain| property is used for objects that must persist once they're create, and are automatically released when set to "nil" or other instances.
 The |assign| property is used for variables that only have numeric values assigned to them.
 */
@property (retain) AVCaptureSession *session;
@property (retain) AVCaptureDevice *selectedVideoDevice;
@property (retain) AVCaptureDeviceInput *videoDeviceInput;
@property (retain) AVCaptureVideoDataOutput *videoDeviceOutput;
@property (retain) NSLock *nsLock;
@property (assign) dispatch_queue_t frameQueue;
@property (assign) size_t frameBufferPixelDataSize;
@property (assign) unsigned char *framePixelBuffer;
@property (assign) int frameWidth;
@property (assign) int frameHeight;

@end

@implementation LocalCapture

@synthesize session = _session;
@synthesize selectedVideoDevice = _selectedVideoDevice;
@synthesize videoDeviceInput = _videoDeviceInput;
@synthesize videoDeviceOutput = _videoDeviceOutput;
@synthesize nsLock = _nsLock;
@synthesize frameQueue = _frameQueue;
@synthesize frameBufferPixelDataSize = _frameBufferPixelDataSize;
@synthesize framePixelBuffer = _framePixelBuffer;
@synthesize isFrameUpdated = _isFrameUpdated;
@synthesize frameWidth = _frameWidth;
@synthesize frameHeight = _frameHeight;

- (void) lcDeallocate

{
    [self stopGrabbing];
    [self setSession:nil];
    RELEASE_DISPATCH_OBJ([self frameQueue]);
    [self setFrameQueue:NULL];
    [self setSelectedVideoDevice:nil];
    [self setVideoDeviceInput:nil];
    [self setVideoDeviceOutput:nil];
    [self setNsLock:nil];
    if ([self framePixelBuffer]) {
        free([self framePixelBuffer]);
    }
    [self setFramePixelBuffer:NULL];
    [self setIsFrameUpdated:0];
    [self setFrameWidth:0];
    [self setFrameHeight:0];
}

+ (OSErr) getNumberOfInputs:(int*)n

{
    OSErr err = 0;

    NSArray *videoDevices = RETAIN_OBJ([AVCaptureDevice devicesWithMediaType:AVMediaTypeVideo]);

    if(videoDevices && n){
        (*n) = (int)[videoDevices count];
    }
    else
    {
        err = -1;
    }

    RELEASE_OBJ(videoDevices);

    return err;
}

+ (OSErr) getDeviceName:(int)globalDeviceID destinationArray:(char*)deviceName lengthOfDestArray:(long*)maxLengthOfDeviceName

{
    OSErr err = 0;
    NSString* localName = nil;

    NSArray *videoDevices = RETAIN_OBJ([AVCaptureDevice devicesWithMediaType:AVMediaTypeVideo]);

    if ((videoDevices) and (globalDeviceID < [videoDevices count]))
    {
        // Get the name of the device.
        localName = RETAIN_OBJ([[videoDevices objectAtIndex:globalDeviceID] localizedName]);

        // Convert it to C type for copying.
        const char* localNameUTF8 = [localName UTF8String];

        // Copy the name over to |deviceName|.
        if ([localName length] < *maxLengthOfDeviceName)
        {
            unsigned long i = 0;
            while (localNameUTF8[i] != '\0')
            {
                deviceName[i] = localNameUTF8[i];
                ++i;
            }
            deviceName[i] = localNameUTF8[i];
        }
        else
        {
            // If the length of the localName is longer than the given maximum
            // length, we just copy over that maximum amount.
            for (unsigned long i = 0; i < (*maxLengthOfDeviceName)-1; ++i) {
                deviceName[i] = localNameUTF8[i];
            }
            deviceName[(*maxLengthOfDeviceName)-1] = '\0';
        }
    }
    else
    {
        err = -1;
    }

    RELEASE_OBJ(localName);
    RELEASE_OBJ(videoDevices);

    return err;
}

+ (OSErr) getNumberOfSupportedResolutionsFromDevice:(int)globalDeviceID num:(int*)n

{
    OSErr err = 0;

    NSArray *videoDevices = RETAIN_OBJ([AVCaptureDevice devicesWithMediaType:AVMediaTypeVideo]);

    if ((videoDevices) and (globalDeviceID < [videoDevices count]))
    {
        *n = [[[videoDevices objectAtIndex:globalDeviceID] formats] count];
    }
    else
    {
        err = -1;
    }

    RELEASE_OBJ(videoDevices);

    return err;
}

+ (OSErr) getSupportedResolutionPairFromDevice:(int)globalDeviceID resPairIdx:(int)pairIdx width:(int*)width height:(int*)height

{
    OSErr err = 0;
    AVCaptureDeviceFormat *format = nil;

    NSArray *videoDevices = RETAIN_OBJ([AVCaptureDevice devicesWithMediaType:AVMediaTypeVideo]);

    if ((videoDevices) and (globalDeviceID < [videoDevices count]))
    {
        format = RETAIN_OBJ([[[videoDevices objectAtIndex:globalDeviceID] formats] objectAtIndex:pairIdx]);
        CMVideoDimensions dimensions = CMVideoFormatDescriptionGetDimensions((CMVideoFormatDescriptionRef)[format formatDescription]);
        (*width) = dimensions.width;
        (*height) = dimensions.height;
    }
    else
    {
        err = -1;
    }

    RELEASE_OBJ(format);
    RELEASE_OBJ(videoDevices);

    return err;
}

- (OSErr) setupDevice:(int)globalDeviceID withWidth:(int)width withHeight:(int)height

{
    [self lcDeallocate];

    // Return code. -1 means error. 0 means completed successfully.
    // This will be set to 0 on completion of device setup.
    OSErr retCode = -1;

    NSError *error = nil;
    NSArray *videoDevices = nil;
    AVCaptureDeviceFormat *chosenFormat = nil;
    AVFrameRateRange *chosenFrameRateRange = nil;

    // Temporary variables used to help with object initialization.
    AVCaptureSession *capture = nil;
    NSLock *lock = nil;
    AVCaptureVideoDataOutput *captureDevice = nil;

    // Set up the mutex.
    lock = [[NSLock alloc] init];
    [self setNsLock:lock];
    RELEASE_OBJ(lock);

    // Return, if our lock is nil.
    if (![self nsLock]) {
        goto endSetupDeviceLabel;
    }

    // Create a capture session.
    capture = [[AVCaptureSession alloc] init];
    [self setSession:capture];
    RELEASE_OBJ(capture);

    // Get list of video devices.
    videoDevices = RETAIN_OBJ([AVCaptureDevice devicesWithMediaType:AVMediaTypeVideo]);

    // Check if the session is ready, and if the device exists.
    if ( ([self session]) and (videoDevices) and (globalDeviceID < [videoDevices count]) )
    {

        // *** CONFIGURE THE SESSION *** //


        // Begin configuration to make changes to the current session.
        [[self session] beginConfiguration];

        @try {


            // *** CONFIGURE THE INPUT *** //


            // Get the device.
            [self setSelectedVideoDevice:[videoDevices objectAtIndex:globalDeviceID]];

            // Set the selected video device as input.
            if ([self selectedVideoDevice]) {
                [self setVideoDeviceInput:[AVCaptureDeviceInput deviceInputWithDevice:[self selectedVideoDevice] error:&error]];
            }

            // Check if something horrible happened.
            if (error or ![self selectedVideoDevice] or ![self videoDeviceInput]) {
                // Something went wrong. Release everything, and return.
                // First close the session configuration.
                [[self session] commitConfiguration];
                goto endSetupDeviceLabel;
            }

            // Add the input to the session.
            if ( [[self session] canAddInput:[self videoDeviceInput]] )
                [[self session] addInput:[self videoDeviceInput]];
            else {
                // Can't add the input. Release everything, and return.
                // First close the session configuration.
                [[self session] commitConfiguration];
                goto endSetupDeviceLabel;
            }


            // *** CONFIGURE THE OUTPUT *** //


            // Set the output to video data output.
            captureDevice = [[AVCaptureVideoDataOutput alloc] init];
            [self setVideoDeviceOutput:captureDevice];
            RELEASE_OBJ(captureDevice);

            if ([self videoDeviceOutput]) {

                // Discard late video frames.
                [[self videoDeviceOutput] setAlwaysDiscardsLateVideoFrames:YES];

                // Set the video settings.
                // AVVideoScalingModeKey is not documented as a supported ***FINISH COMMENTING HERE.***
                [[self videoDeviceOutput] setVideoSettings:@{ (NSString *)kCVPixelBufferPixelFormatTypeKey : @(kCVPixelFormatType_422YpCbCr8_yuvs),
                                                              (NSString *)AVVideoScalingModeKey : AVVideoScalingModeFit}];

                // Set up the dispatch queue that holds the video frames.
                dispatch_queue_t outputQueue = dispatch_queue_create ("outputQueue", DISPATCH_QUEUE_SERIAL);
                [self setFrameQueue:outputQueue];

                // Set the frame output delegate and frame queue.
                [[self videoDeviceOutput] setSampleBufferDelegate:self queue:outputQueue];
            }
            else {

                // Couldn't configure the output. Release everything, and return.
                // First close the session configuration.
                [[self session] commitConfiguration];
                goto endSetupDeviceLabel;
            }

            // Add the output to the session.
            if ( [[self session] canAddOutput:[self videoDeviceOutput]] ) {
                [[self session] addOutput:[self videoDeviceOutput]];
            }
            else {
                // Can't add the output. Release everything, and return.
                // First close the session configuration.
                [[self session] commitConfiguration];
                goto endSetupDeviceLabel;
            }

        }
        @catch (id exception) {
            // Something went horribly wrong. Release everything, and return.
            // First close the session configuration.
            [[self session] commitConfiguration];
            goto endSetupDeviceLabel;
        }

        // Apply all of our settings.
        [[self session] commitConfiguration];


        // *** CONFIGURE THE DEVICE CAPTURE RESOLUTION *** //


        // Prepare to configure the device capture resolution.
        chosenFormat = RETAIN_OBJ([[[self selectedVideoDevice] formats] objectAtIndex:0]);
        CMVideoDimensions dimensions = CMVideoFormatDescriptionGetDimensions((CMVideoFormatDescriptionRef)[chosenFormat formatDescription]);
        int smallestDifferenceInResolution = abs(width - dimensions.width);

        // Find a format that matches the desired width and height.
        // If the desired format does not exist, choose the one that's closest.
        for (AVCaptureDeviceFormat *format in [[self selectedVideoDevice] formats])
        {
            dimensions = CMVideoFormatDescriptionGetDimensions((CMVideoFormatDescriptionRef)[format formatDescription]);

            if ( (width == dimensions.width) && (height == dimensions.height) ) {
                // If we find the desired format, set it, and break the loop.
                RELEASE_OBJ(chosenFormat);
                chosenFormat = RETAIN_OBJ(format);
                break;
            }
            else {
                // While we're searching for the desired format, keep track of
                // which one is closest to what is wanted.
                if (abs(width - dimensions.width) < smallestDifferenceInResolution) {
                    smallestDifferenceInResolution = abs(width - dimensions.width);
                    RELEASE_OBJ(chosenFormat);
                    chosenFormat = RETAIN_OBJ(format);
                }
            }
        }

        // Cache the width and height. We use this information to make sure frames are comming in at the right size.
        dimensions = CMVideoFormatDescriptionGetDimensions((CMVideoFormatDescriptionRef)[chosenFormat formatDescription]);
        [self setFrameWidth:dimensions.width];
        [self setFrameHeight:dimensions.height];

        // Session has to start running BEFORE the device format is set, otherwise the session preset will take over.
        [[self session] startRunning];

        // Check if the frame rate we want is supported by this device.
        AVFrameRateRange *chosenFrameRateRange = RETAIN_OBJ([chosenFormat.videoSupportedFrameRateRanges objectAtIndex:0]);
        float desiredMaxFrameRate = 20.0;
        float smallestDifference = std::abs(desiredMaxFrameRate - chosenFrameRateRange.maxFrameRate);

        for ( AVFrameRateRange *range in chosenFormat.videoSupportedFrameRateRanges ) {
            if ( range.maxFrameRate == desiredMaxFrameRate ) {
                // If we find the frame rate, break the loop.
                RELEASE_OBJ(chosenFrameRateRange);
                chosenFrameRateRange = RETAIN_OBJ(range);
                break;
            }
            else {
                // While we're searching for the desired frame rate, keep track of
                // which one is closest to what we want.
                if (std::abs(desiredMaxFrameRate - range.maxFrameRate) < smallestDifference) {
                    smallestDifference = std::abs(desiredMaxFrameRate - range.maxFrameRate);
                    RELEASE_OBJ(chosenFrameRateRange);
                    chosenFrameRateRange = RETAIN_OBJ(range);
                }
            }
        }

        // Set the device format, and configure the frame rate.
        if ([[self selectedVideoDevice] lockForConfiguration:&error])
        {

            [[self selectedVideoDevice] setActiveFormat:chosenFormat];

            /* Set the frame rate. If the frame rate cannot be set, it will be the camera default.
             This was also tested with nonsense values (Max = 1, Min = 900), and it still works fine.
             Even though those values do get set, the program will just ignore impossible values, and
             instead use the device's default values. Below we have set the max frame rate to 20, and the
             min to 10. Notice that the minimum frame duration is the reciprocal of the maximum frame rate.
             Below, the CMTimeMake(1,20) corresponds to 1/20. So the max frame rate is 20.
             */
            [[self selectedVideoDevice] setActiveVideoMinFrameDuration:chosenFrameRateRange.minFrameDuration];

            /*
             max frame duration is only available on 10.9 and up.
             [[self selectedVideoDevice] setActiveVideoMaxFrameDuration:chosenFrameRateRange.maxFrameDuration];
             }*/

            [[self selectedVideoDevice] unlockForConfiguration];
        }
        else {
            // Couldn't lock for configuration. Release everything, and return.
            goto endSetupDeviceLabel;
        }

        // We completed setup without errors.
        retCode = 0;

    }

    // This label is here so that when
    // an error occurs, we can jump here,
    // cleanup, and return quickly.
    endSetupDeviceLabel:

    if (retCode == -1) {
        [self lcDeallocate];
    }

    error = nil;
    RELEASE_OBJ(videoDevices);
    RELEASE_OBJ(chosenFormat);
    RELEASE_OBJ(chosenFrameRateRange);
    return retCode;
}

- (OSErr) getSettings

{
    OSErr err = 0;
    // Not implemented.
    return err;
}

- (void) showSettingsWindow

{
    // Not implemented.
}

- (OSErr) startGrabbing

{
    OSErr err = -1;

    if ([self session])
    {

        [[self session] startRunning];

        if ([[self session] isRunning])
        {
            err = 0;
        }
    }

    return err;
}

- (OSErr) stopGrabbing

{
    OSErr err = 0;

    if ([self session])
    {

        [[self session] stopRunning];

        if ([[self session] isRunning])
        {
            err = -1;
        }
    }

    return err;
}

- (unsigned char*) getPixels

{
    return [self framePixelBuffer];
}

- (int) lockFrame

{
    return [[self nsLock] tryLock];
}

- (void) unLockFrame

{
    [[self nsLock] unlock];
}

- (long) getPixelDataSize

{
    return (long)[self frameBufferPixelDataSize];
}

- (OSErr) queuedFrameCount:(UInt8*)count

{
    OSErr err = 0;

    if (count) {
        *count = [self isFrameUpdated];
    }

    else {
        err = -1;
    }

    return err;
}

- (int) getWidth

{
    if ([self selectedVideoDevice]) {
        return [self frameWidth];
    }
    return 0;
}

- (int) getHeight

{
    if ([self selectedVideoDevice]) {
        return [self frameHeight];
    }
    return 0;
}

- (void) getPixelFormat

{
    // Not implemented.
}

- (int) isGrabbing

{
    return [[self session] isRunning];
}

- (OSErr) idle:(int*)isUpdated

{
    OSErr err = 0;

    if (isUpdated) {
        *isUpdated = [self isFrameUpdated];
    }

    else {
        err = -1;
    }

    return err;
}

- (void)captureOutput:(AVCaptureOutput *)captureOutput

        didOutputSampleBuffer:(CMSampleBufferRef)sampleBuffer

        fromConnection:(AVCaptureConnection *)connection
{
    // Cast the |sampleBuffer| to a |CVImageBufferRef| to retrieve video frame info.
    CVImageBufferRef imageBuffer = CMSampleBufferGetImageBuffer(sampleBuffer);

    // Take ownership of sampleBuffer and imageBuffer, in case their original owners don't want them anymore.
    CFRetain(sampleBuffer);
    CFRetain(imageBuffer);

    // Lock the base address of the pixel buffer.
    CVPixelBufferLockBaseAddress(imageBuffer,0);

    // Get the number of bytes per row for the pixel buffer. This value is ( width * numBytesPerPixel ) with padding.
    size_t bytesPerRow = CVPixelBufferGetBytesPerRow(imageBuffer);

    // Get the pixel buffer width and height.
    size_t width = CVPixelBufferGetWidth(imageBuffer);
    size_t height = CVPixelBufferGetHeight(imageBuffer);

    // Get the base address of the pixel buffer.
    void *baseAddress = CVPixelBufferGetBaseAddress(imageBuffer);

    // If the frame dimensions don't match the desired dimensions, return.
    // That means the capture device is not set to the right dimensions yet. After a few
    // frame captures, however, it will be set to the correct dimensions.  This happens
    // because the session has to start running at the default capture resolution before
    // it can be changed. Therefore, the first 2 or 3 frames are usually the default size.
    // After that, they should be the right size.
    if (([self frameWidth] != width) || ([self frameHeight] != height)) {
        goto endCaptureOutput;
    }

    // Get the data size for contiguous planes of the pixel buffer.
    //size_t bufferSize = CVPixelBufferGetDataSize(imageBuffer);

    // Cache the frame buffer data size.
    [self setFrameBufferPixelDataSize:(bytesPerRow*height)];

    // If |_framePixelBuffer| is NULL, then allocate the space needed to store the frame.
    if (![self framePixelBuffer]) {
        [self setFramePixelBuffer: (unsigned char*) malloc (bytesPerRow*height)];
        // If |malloc| returns NULL, return, since the frame won't be ready.
        if (![self framePixelBuffer]) {
            goto endCaptureOutput;
        }
    }

    if ([self lockFrame]) {
        // Copy the buffer into |_framePixelBuffer|.
        memcpy([self framePixelBuffer], baseAddress, bytesPerRow*height);
        // Set |isFrameUpdated| so that the frame can be accessed.
        [self setIsFrameUpdated:1];
        [self unLockFrame];
    }

    // This label is here so that when
    // an error occurs, we can jump here,
    // cleanup, and return quickly.
    endCaptureOutput:

    // Unlock the buffer.
    CVPixelBufferUnlockBaseAddress(imageBuffer, 0);

    // Release sampleBuffer and imageBuffer.
    CFRelease(imageBuffer);
    CFRelease(sampleBuffer);
}

@end