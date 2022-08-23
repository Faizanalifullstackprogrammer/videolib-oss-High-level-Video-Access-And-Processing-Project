/*****************************************************************************
 *
 * localVideoServer.cpp
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

/*
 * Mac implementation of localVideoLib - server portion
 */

#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <stddef.h>
#include <sys/mman.h>
#include <unistd.h>
#include <errno.h>
#include <fcntl.h>

#include "localVideo.h"
#include "localVideoServer.h"

#include "localCaptureWrapper.hpp"

/*
 For reference, some common errors and their meaning on OSX:
   -32767 => badComponentInstance
   704 => "no camera connected"
 */

/***********************************************************/
// Cached device names
static char deviceNames[kMaxCameras][256]={{0}};

// Cached number of supported resolutions per device
static int numResolutionsPerDevice[kMaxCameras];

// Cached supported resolutions per device
static int deviceResolutions[kMaxCameras][kMaxResolutions][2]={{{0}}};

// Period between checking for frames
static const int kMicroSecPerTimer = 66000;

// Number of times to try getting a frame before giving up
static const int kMaxGrabAttempts = 30;

// Local copy of the verbosity flag
static int gVerbose = FALSE;

struct LocalVideoData {
    LocalCaptureHandler lch;
};

LocalVideoHandle LVS_new()
{
    LocalVideoData* data = (LocalVideoData*) malloc (sizeof(LocalVideoData));
    data->lch = lcwNew();
    return (LocalVideoHandle)data;
}

void LVS_delete(LocalVideoHandle lvlH)
{
    LocalVideoData* data = (LocalVideoData*) lvlH;

    // Check for NULL
    if (!data) {
        return;
    }

    // Check for NULL
    if (data->lch) {
        lcwDelete(data->lch);
        data->lch = NULL;
    }

    free(data);
}

void LVS_setVerbose(BOOL verbose)
{
    gVerbose = verbose;
}

int LVS_listDevices()
{
    OSErr err;
    int n = 0;

    if ((err = lcwGetNumberOfInputs(&n))) {
        fprintf(stderr, "LVS_listDevices err=%i\n", err);
        return 0;
    }

    int i, j;
    long len;
    for (i=0; i<n; i++) {

        len = 255;

        // Cache all of the device names, so that we can return them when
        // the user calls getDeviceName().
        if ((err = lcwGetDeviceName(i, deviceNames[i], &len))) {
            fprintf(stderr, "lcwGetDeviceName err=%i\n", err);
        }

        // Cache the number of supported resolutions per device.
        if ((err = lcwGetNumberOfSupportedResolutionsFromDevice(i, &numResolutionsPerDevice[i]))) {
            fprintf(stderr, "lcwGetNumberOfSupportedResolutionsFromDevice err=%i\n", err);
        }

        // Cache the list of supported resolutions per device.
        for (j = 0; j < numResolutionsPerDevice[i]; j++) {
            int width = 0;
            int height = 0;
            if ((err = lcwGetSupportedResolutionPairFromDevice(i, j, &width, &height))) {
                fprintf(stderr, "lcwGetSupportedResolutionPairFromDevice err=%i\n", err);
            }
            deviceResolutions[i][j][0] = width;
            deviceResolutions[i][j][1] = height;
        }
    }

    return n;
}

char* LVS_getDeviceName(int deviceID)
{
    return deviceNames[deviceID];
}

BOOL LVS_setupDevice(LocalVideoHandle lvlH, int deviceID, int w, int h)
{
    LocalVideoData* data = (LocalVideoData*) lvlH;
    OSErr err;

    // Check to see if the device is already pulling frames.
    if (lcwIsGrabbing(data->lch)) {
        fprintf(stderr, "Device %i already set up\n", deviceID);
        return FALSE;
    }

    if (w > kMaxFrameWidth || h > kMaxFrameHeight) {
        fprintf(stderr, "Requested width or height too large\n");
        return FALSE;
    }
    // Set-up the device with the requested width and height.
    if ((err = lcwSetupDevice(data->lch, deviceID, w, h))) {
        fprintf(stderr, "lcwSetupDevice failed\n");
        return FALSE;
    }

    if ((err = lcwGetSettings(data->lch))) {
        fprintf(stderr, "lcwGetSettings failed\n");
        return FALSE;
    }
    // Tell the device to start pulling frames.
    if ((err = lcwStartGrabbing(data->lch))) {
        fprintf(stderr, "lcwStartGrabbing failed\n");
        return FALSE;
    }

    return TRUE;
}

BOOL LVS_isDeviceSetup(LocalVideoHandle lvlH, int deviceID)
{
    LocalVideoData* data = (LocalVideoData*) lvlH;
    if (lcwIsGrabbing(data->lch)) {
        return TRUE;
    }
    return FALSE;
}

int LVS_getWidth(LocalVideoHandle lvlH, int deviceID)
{
    LocalVideoData* data = (LocalVideoData*) lvlH;
    return lcwGetWidth(data->lch);
}

int LVS_getHeight(LocalVideoHandle lvlH, int deviceID)
{
    LocalVideoData* data = (LocalVideoData*) lvlH;
    return lcwGetHeight(data->lch);
}

int LVS_getSize(LocalVideoHandle lvlH, int deviceID)
{
    LocalVideoData* data = (LocalVideoData*) lvlH;
    return lcwGetPixelDataSize(data->lch);
}

// Note: calling this twice will return 0 the second time, so you can use
// this function *or* LVS_getPixels (which calls this for you), but not both
// in series.  To get a frame after isFrameNew, we'd need to change this
// behavior or add an option to getPixels to get without polling first.
BOOL LVS_isFrameNew(LocalVideoHandle lvlH, int deviceID)
{
    OSErr err;
    UInt8 queuedFrameCount;
    LocalVideoData* data = (LocalVideoData*) lvlH;

    if ((err = lcwQueuedFrameCount(data->lch, &queuedFrameCount))) {
        fprintf(stderr, "lcwQueuedFrameCount err=%i\n", err);
        return FALSE;
    }

    if (queuedFrameCount > 0)
        return TRUE;

    return FALSE;
}

// Note: on Mac, flipRedAndBlue and flipImage are ignored.
BOOL LVS_getPixels (LocalVideoHandle lvlH, int deviceID, unsigned char* pixels,
                   BOOL flipRedAndBlue, BOOL flipImage)
{
    LocalVideoData* data = (LocalVideoData*) lvlH;
    int isUpdated = 0;
    int attempts = 0;
    int err;

    fprintf(stderr, "SERVER: Entering LVS_getPixels.\n");

    while (!isUpdated) {
        if (lcwIsGrabbing(data->lch)) {
            if ((err = lcwIdle(data->lch, &isUpdated))) {
                // Frame not ready yet.  We assume that if there is an error,
                // we just have to wait longer. If we see other errors here, we
                // may have to fix this.
                fprintf(stderr, "LVS_getPixels err: lcwIdle returned %d\n", err);
                continue;
            }
            if (isUpdated && lcwLockFrame(data->lch)) {
                memcpy(pixels, lcwGetPixels(data->lch), lcwGetPixelDataSize(data->lch));
                lcwUnSetIsFrameUpdated(data->lch);
                lcwUnLockFrame(data->lch);
                return TRUE;
            }
            else {
                attempts += 1;
                if (attempts > kMaxGrabAttempts) {
                    fprintf(stderr,
                        "LVS_getPixels err: too many attempts, giving up\n");
                    return FALSE;
                }
            }
        }
        else {
            fprintf(stderr, "LVS_getPixels err: lcwIsGrabbing returned false\n");
            return FALSE;
        }

        usleep(kMicroSecPerTimer);
    }

    fprintf(stderr, "SERVER: Leaving LVS_getPixels.\n");
    return FALSE;
}

void LVS_showSettingsWindow(LocalVideoHandle lvlH, int deviceID)
{
    LocalVideoData* data = (LocalVideoData*) lvlH;
    lcwShowSettingsWindow(data->lch);
}

void LVS_stopDevice(LocalVideoHandle lvlH, int deviceID)
{
    LocalVideoData* data = (LocalVideoData*) lvlH;
    OSErr err;
    // Stop pulling frames.
    if ((err = lcwStopGrabbing(data->lch))) {
        fprintf(stderr, "lcwStopGrabbing failed\n");
    }
}

/***********************************************************/
static int gPipeInput = -1;
static int gPipeOutput = -1;

/*
Writes a buffer containing a null-terminated string to the output pipe

    @param  buf         a pointer to a null-terminated string
    @return err         0 if no error
 */
int sendResponseString(const char* buf)
{
    int err;
    //fprintf(stderr, "SERVER: Writing \"%s\", %ld bytes\n", buf, strlen(buf)+1);
    if ((err = write(gPipeOutput, buf, strlen(buf)+1)) == -1) {
        perror("SERVER: Error writing to client");
        return err;
    }
    return 0;
}

/*
Sends the count of devices followed by the device names. The format
of the response is the count of devices, '\n', then each device name
separated by '\n', then the number of supported resolutions per device
separated by '\n', and lastly, all of the supported resolutions per
device, as one long null-terminated string.

    @return     err             0 if no error
 */
//#define DEBUG_GENERATED_CAMERAS TRUE
int sendDeviceCountAndNames()
{
    int err;
    int count;
    int len;

    // Includes space for 8 chars and 1 return char to represent an integer value.
    const int maxStrLenForNums = 9;
    // Includes space for a string version of the count, plus a final NULL.
    int size = sizeof(deviceNames) + maxStrLenForNums + 1;
    // Includes space for a string version of each number of resolutions per device.
    size += sizeof(numResolutionsPerDevice) * maxStrLenForNums;
    // Includes space for a string version of each resolution pair per device.
    size += sizeof(deviceResolutions) * maxStrLenForNums;

    char buf[size];
    char* p = buf;

    count = LVS_listDevices();
    //if (DEBUG_GENERATED_CAMERAS) count = kMaxCameras;
    len = snprintf(p, maxStrLenForNums, "%d\n", count);
    p += len;
    size -= len;

    int i, j;

    /*if (DEBUG_GENERATED_CAMERAS) {
        for (i = 0; i < count; i++) {
            int j;
            for (j = 0; j < 255; j++) {
                deviceNames[i][j] = 'A';
            }
            deviceNames[i][j] = '\0';
        }
    }*/

    // Send the names.
    for (i = 0; i < count; i++) {
        len = snprintf(p, size, "%s\n", deviceNames[i]);
        p += len;
        size -= len;
    }
    // Send the number of supported resolutions.
    for (i = 0; i < count; i++) {
        len = snprintf(p, maxStrLenForNums, "%d\n", numResolutionsPerDevice[i]);
        p += len;
        size -= len;
    }
    // Send the list of supported resolutions.
    for (i = 0; i < count; i++) {
        for (j = 0; j < numResolutionsPerDevice[i]; j++) {
            len = snprintf(p, 2 * maxStrLenForNums, "%d\n%d\n", deviceResolutions[i][j][0], deviceResolutions[i][j][1]);
            p += len;
            size -= len;
        }
    }

    *p = '\0';

    if ((err = sendResponseString(buf))) {
        return err;
    }

    return err;
}

/*
 The server's main loop, which processes and responds to messages from
 the client.
 */
int main(int argc, char** argv)
{
    if (argc < 4 || argc > 5) {
        exit(2);
    }

    // Set up logging of stderr (disabled by default)
   int err = 0;
   FILE* logFile = NULL;
   if (FALSE) {
       int fdLog;
       if ((logFile = fopen("localVideoServer.log", "a")) == NULL) {
            perror("Failed to open log file");
       }
       fdLog = fileno(logFile);
       if ((dup2(fdLog, STDERR_FILENO)) == -1) {
            perror("Failed to duplicate log file descriptor");
       }
    }

    // Set verbosity of log messages
    LVS_setVerbose(atoi(argv[1]));
    if (gVerbose) {
        fprintf(stderr, "SERVER: Started localVideoServer process...\n");
    }


    //create a videoInput object
    LocalVideoHandle lvlH = NULL;
    lvlH = LVS_new();

    // Save our pipe ends
    gPipeInput = atoi(argv[2]);
    gPipeOutput = atoi(argv[3]);

    // If we have a valid path to the shared memory file, create it.
    // Otherwise, assume the caller will not use shared memory for
    // any messages sent.
    int fd = -1;
    char* data = NULL;
    char* mapPath;

    if (argc == 5) {
        mapPath = argv[4];
        if ((fd = open(mapPath, O_RDWR)) == -1) {
            perror("SERVER: Failed to open memory mapped file");
            return -1;
        }
        if ((data = (char*)mmap(0, kMmapSize, PROT_READ|PROT_WRITE, MAP_SHARED,
                                    fd, 0)) == (void*)-1) {
            perror("SERVER: mmap failed");
            return -1;
        }
        // Delete the file, which will free it when the processes exit
        if ((err = unlink(mapPath)) == -1) {
            perror("Failed to unlink mmap temp file");
            return -1;
        }
    }

    // ***********************************************************
    // MAIN SERVICE LOOP
    int nread;
    int msg;
    char request[100];
    memset(request, 0, sizeof(request));

    // Uncomment the following lines to enable debugging
    //fprintf(stderr, "Server paused. Connect to pid %i to debug...", getpid());
    //pause();

    while (!err) {
        // Read the next message, blocking if there is none
        if ((nread = read(gPipeInput, request, sizeof(request))) == -1) {
            perror("SERVER: Error reading from client");
            if (gVerbose) {
                fprintf(stderr, "SERVER: Error reading from client.\n");
            }
            break;
        }

        // If the writer closed its pipe, we'll clean up and exit.
        if (nread == 0) {
            if (gVerbose) {
                fprintf(stderr, "SERVER: client shut down.  Exiting.\n");
            }
            break;
        }

        // Decode and handle the message
        msg = (int)*request;
        //if (msg != LVS_GET_PIXELS)
        //    fprintf(stderr, "SERVER: Received message %s\n", LVS_MSG_NAMES[msg]);
        if (gVerbose) {
            fprintf(stderr, "SERVER: Message received - %s.\n", LVS_MSG_NAMES[msg]);
        }
        switch (msg) {
            case LVS_TEST_CONNECTION:
            {
                err = sendResponseString(LVS_RESPONSE_SUCCESS);
                break;
            }

            case LVS_SET_VERBOSE:
            {
                LVS_setVerbose(((setVerboseParamsT*)data)->verbose);
                err = sendResponseString(LVS_RESPONSE_SUCCESS);
                break;
            }

            case LVS_LIST_DEVICES:
            {
                listDevicesParamsT* gndData = (listDevicesParamsT*)data;
                int n = LVS_listDevices();
                gndData->numDevices = n;
                memcpy(gndData->deviceNames, deviceNames, sizeof(deviceNames));
                memcpy(gndData->numResolutionsPerDevice, numResolutionsPerDevice, sizeof(numResolutionsPerDevice));
                memcpy(gndData->deviceResolutions, deviceResolutions, sizeof(deviceResolutions));
                err = sendResponseString(LVS_RESPONSE_SUCCESS);
                break;
            }

            case LVS_LIST_DEVICES_ONLY:
            {
                // Special command to bypass shared memory usage
                err = sendDeviceCountAndNames();

                // Return an error so that the server shuts down
                if (!err) {
                    err = 99;
                }
                break;
            }

            case LVS_SETUP_DEVICE:
            {
                setupDeviceParamsT* sdData =  (setupDeviceParamsT*)data;
                sdData->success = LVS_setupDevice(lvlH, sdData->deviceID,
                                                 sdData->width,
                                                 sdData->height);
                err = sendResponseString(LVS_RESPONSE_SUCCESS);
                break;
            }

            case LVS_IS_DEVICE_SETUP:
            {
                isDeviceSetupParamsT* idsData = (isDeviceSetupParamsT*)data;
                idsData->isSetup = LVS_isDeviceSetup(lvlH, idsData->deviceID);
                err = sendResponseString(LVS_RESPONSE_SUCCESS);
                break;
            }

            case LVS_STOP_DEVICE:
            {
                LVS_stopDevice(lvlH, ((stopDeviceParamsT*)data)->deviceID);
                err = sendResponseString(LVS_RESPONSE_SUCCESS);
                break;
            }

            case LVS_GET_WIDTH:
            {
                ((getWidthParamsT*)data)->width =
                        LVS_getWidth(lvlH, ((getWidthParamsT*)data)->deviceID);
                err = sendResponseString(LVS_RESPONSE_SUCCESS);
                break;
            }

            case LVS_GET_HEIGHT:
            {
                ((getHeightParamsT*)data)->height =
                    LVS_getHeight(lvlH, ((getHeightParamsT*)data)->deviceID);
                err = sendResponseString(LVS_RESPONSE_SUCCESS);
                break;
            }

            case LVS_GET_SIZE:
            {
                ((getSizeParamsT*)data)->size =
                    LVS_getSize(lvlH, ((getSizeParamsT*)data)->deviceID);
                err = sendResponseString(LVS_RESPONSE_SUCCESS);
                break;
            }

            case LVS_IS_FRAME_NEW:
            {
                ((isFrameNewParamsT*)data)->isNew =
                    LVS_isFrameNew(lvlH, ((isFrameNewParamsT*)data)->deviceID);
                err = sendResponseString(LVS_RESPONSE_SUCCESS);
                break;
            }

            case LVS_GET_PIXELS:
            {
                getPixelsParamsT* gpData = (getPixelsParamsT*)data;

                gpData->success = LVS_getPixels(lvlH, gpData->deviceID,
                                               (unsigned char*)(data+kVideoFrameOffset),
                                               FALSE, FALSE);
                gpData->pixelDataSize = LVS_getSize(lvlH, gpData->deviceID);

                err = sendResponseString(LVS_RESPONSE_SUCCESS);
                break;
            }

            case LVS_SHOW_SETTINGS_WINDOW:
            {
                LVS_showSettingsWindow(lvlH,
                    ((showSettingsWindowParamsT*)data)->deviceID);
                err = sendResponseString(LVS_RESPONSE_SUCCESS);
                break;
            }

            default:
            {
                fprintf(stderr, "SERVER: Error unsupported message %s\n",
                        LVS_MSG_NAMES[msg]);
                err = sendResponseString(LVS_RESPONSE_ERROR);
                break;
            }
        }
        if (gVerbose) {
            fprintf(stderr, "SERVER: %s with error code %d.\n", LVS_MSG_NAMES[msg], err);
        }
    }
    // ***********************************************************

    // Close our write communication pipe
    close(gPipeOutput);

    // Unmap and close the shared memory file
    if (data && (munmap(data, kMmapSize)) == -1) {
        perror("SERVER: munmap failed");
    }
    if (fd != -1 && (close(fd)) == -1) {
        perror("SERVER: Failed to close memory mapped file");
    }

    // Close the log file
    if (logFile) {
        fclose(logFile);
    }

    // Clean up our state and exit
    if (lvlH) {
        LVS_delete(lvlH);
    }

    if (gVerbose) {
        fprintf(stderr, "SERVER: Server shutting down.\n");
    }

    return 0;
}
