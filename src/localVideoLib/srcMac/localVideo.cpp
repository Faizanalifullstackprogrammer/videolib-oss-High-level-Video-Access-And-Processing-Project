/*****************************************************************************
 *
 * localVideo.cpp
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
 * Mac implementation of localVideoLib
 */

#include <errno.h>
#include <fcntl.h>
#include <stddef.h>
#include <string.h>
#include <dlfcn.h>
#include <libgen.h>
#include <sys/mman.h>
#include <unistd.h>
#include <stdlib.h>
#include <stdio.h>

#define SVLVL_EXPORTS 1

#include "localVideo.h"
#include "localVideoServer.h"

// Since lvlSetVerbose() can be called before initialization,
// keep the verbosity state to send to the server upon creation.
static int gVerbose = FALSE;

// Keep track of whether the server is created
static int gServerPid = 0;

// File descriptor and address for the memory map
static int gMapFildes = -1;
static char* gMapAddr = 0;

// File descriptors for the communication pipes
static int gPipeInput = -1;
static int gPipeOutput = -1;

// Logging function
static log_fn_t gLogFn = NULL;

// Cached device names
static char deviceNames[kMaxCameras][256]={{0}};

// Cached number of supported resolutions per device
static int numResolutionsPerDevice[kMaxCameras];

// Cached supported resolutions per device
static int deviceResolutions[kMaxCameras][kMaxResolutions][2]={{{0}}};

// Cached supported resolution pair
static Dimensions dimensions;

/***********************************************************/

/*
Replacement for Standard C perror() function using logging

    @param  errstring   string prepend to the system error string
*/
static void printError(const char* errstring)
{
    log_err(gLogFn, "%s: %s", errstring, strerror(errno));
}

/*
Sends the specified message to the server

    @param   msg     Message to send
    @return  err     0 if no error
 */
static int sendMessage(int msg)
{
    int nwritten;
    if ((nwritten = write(gPipeOutput, &msg, sizeof(msg))) == -1) {
        log_err(gLogFn, "Error writing to server: %s",
                            strerror(errno));
        return nwritten;
    }
    return 0;
}

/*
Reads a response from the intput pipe

    @param  buf     buffer to read into
    @param  size    size of buffer, aka the maximum amount to read
    @return nread   The number of bytes read, or -1 if an error
 */
static int readResponse(char* buf, long size)
{
    int nread;

    if ((nread = read(gPipeInput, buf, size)) == -1) {
        printError("Error reading from server");
        return nread;
    }
    //fprintf(stderr, "CLIENT: read string \"%s\", %d bytes (strlen %ld), (max %ld)\n", buf, nread, strlen(buf), size);

    return nread;
}

/*
Sends a message to the server, expecting a simple acknowledgement response.
Verifies the response, which means that any return data is available in
the shared memory buffer.

Note: this function should only be used for atomic reads and writes.

    @param  msg     message to send
    @return err     0 if no error
 */
static int sendMessageAndVerifyAck(int msg)
{
    int err;
    int nread;
    char buf[100];
    memset(buf, 0, sizeof(buf));

    // Send the message to the server
    if ((err = sendMessage(msg))) {
        log_err(gLogFn, "Error sending message: %s", LVS_MSG_NAMES[msg]);
        return err;
    }

    // Read the response
    if ((nread = readResponse(buf, sizeof(buf))) == -1) {
        log_err(gLogFn, "Error reading response for: %s", LVS_MSG_NAMES[msg]);
        return nread;
    }
    if (nread == 0) {
        log_err(gLogFn, "Error: server closed communication pipe.");
        return -1;
    }

    // Validate that the call was successful
    if (strcmp(LVS_RESPONSE_SUCCESS, buf)) {
        log_err(gLogFn, "Error: response %s received for message %s",
                buf, LVS_MSG_NAMES[msg]);
        return err;
    }

    return 0;
}

/*
Starts the server which handles webcam requests on the Mac.

   @return  pid              Set to the server's process ID
   @param   dataDir          Directory in which to create the shared memory
                             file. If NULL, do not create shared memory.
   @return  err              0 if no error
*/
static int startServer(int* pid, const char* dataDir)
{
    int err = 0;
    int triedOnce = 0;
    int pfdToServer[2];  // From client (parent) to server (child)
    int pfdToClient[2];   // From server (child) to client (parent)

    // Create the pipes
    if ((err = pipe(pfdToServer)) == -1) {
        printError("Failed to create pipe");
        return err;
    }

    if ((err = pipe(pfdToClient)) == -1) {
        printError("Failed to create pipe");
        return err;
    }

    // Create the shared memory, if we are given a dataDir
    const char* mapFileName = "localVideoMmap.XXXXXX";
    char* mapPath = NULL;

    if (dataDir) {
        mapPath = (char*)malloc(strlen(dataDir) + strlen(mapFileName) + 1);
        strcpy(mapPath, dataDir);
        strcat(mapPath, mapFileName);

        if ((gMapFildes = mkstemp(mapPath)) == -1) {
            log_err(gLogFn, "Failed to create mmap temp file %s: %s", mapPath, strerror(errno));
            return -1;
        }

        if (ftruncate(gMapFildes, kMmapSize)) {
            perror("truncate failed");
            return -1;
        }

        if ((gMapAddr = (char*)mmap(0, kMmapSize, PROT_READ|PROT_WRITE,
                                MAP_SHARED,  gMapFildes, 0)) == (void*)-1) {
            printError("mmap failed");
            return -1;
        }
        memset(gMapAddr, 0, kMmapSize);
    }

    // Create the child process
    *pid = fork();
    if (*pid == 0) {
        // Child process:

        // Close pipe ends child doesn't use, and pass the others to exec
        char fdServerIn[10];
        char fdServerOut[10];
        snprintf(fdServerIn, sizeof(fdServerIn), "%d", pfdToServer[0]);
        snprintf(fdServerOut, sizeof(fdServerIn), "%d", pfdToClient[1]);
        close(pfdToClient[0]);
        close(pfdToServer[1]);

        const char* folderName = "/Applications/Sighthound Video.app/Contents/MacOS";
        const int kMaxPath = 512;
        Dl_info module_info;
        char fullExePath[kMaxPath], dllNamePath[kMaxPath];
        err=dladdr(reinterpret_cast<void*>(lvlGetWidth), &module_info);
        if (err != 0) {
            if ( strlen(module_info.dli_fname)+1 < kMaxPath ) {
                strcpy(dllNamePath, module_info.dli_fname);
                folderName = dirname(dllNamePath);
            }
        }
Retry:
        if (triedOnce) {
            sprintf(fullExePath, "%s/../bin/%s", folderName, WEBCAM_SERVER_EXENAME);
        } else {
            sprintf(fullExePath, "%s/%s", folderName, WEBCAM_SERVER_EXENAME);
        }

        // Execute the server program
        if ((execl(fullExePath,       // program to execute
                   fullExePath,       // argv[0]
                   gVerbose ? "1" : "0",  // verbose flag
                   fdServerIn,            // pipe 1 fd
                   fdServerOut,           // pipe 2 fd
                   mapPath,               // path to shared memory file
                   (char*)NULL)) == -1) {
            if (!triedOnce) {
                triedOnce=1;
                goto Retry;
            }
            printError("exec failed");
            return err;
        }
    }

    // Parent process:

    // Close the pipe ends we don't use and save the others
    gPipeInput = pfdToClient[0];
    gPipeOutput = pfdToServer[1];
    close(pfdToClient[1]);
    close(pfdToServer[0]);

    free(mapPath);

    return 0;
}

/*
Starts a server, gets the list of devices, and returns the count.
Does not use shared memory, so that an app can get the device list
without having to know the shared memory path, call lvlNew(), etc.

   @return      count     number of devices, or 0 if error
 */
static int listDevicesWithoutServer()
{
    LocalVideoHandle lvlH = 0;
    lvlH = lvlNew(NULL, NULL /* no shared memory */);

    // Send message and get response.  Response in this case can be
    // a non-atomic read.  The first is the number of cameras, followed
    // by one read for each camera.
    int err;
    int nread;
    int msg = LVS_LIST_DEVICES_ONLY;

    // Includes space for 8 chars and 1 return char to represent an integer value.
    const int maxStrLenForNums = 9;
    // Includes space for a string version of the count, plus a final NULL.
    int size = sizeof(deviceNames) + maxStrLenForNums + 1;
    // Includes space for a string version of each number of resolutions per device.
    size += sizeof(numResolutionsPerDevice) * maxStrLenForNums;
    // Includes space for a string version of each resolution pair per device.
    size += sizeof(deviceResolutions) * maxStrLenForNums;

    char buf[size];

    // Initialize to zero.
    for (int i = 0; i < size; i++) {
        buf[i] = 0;
    }

    char* p = buf;

    // Send the message to the server
    if ((err = sendMessage(msg))) {
        log_err(gLogFn, "Error sending message: %s", LVS_MSG_NAMES[msg]);
        lvlDelete(lvlH);
        return 0;
    }

    // Read the full response.  The response may come in multiple reads, so
    // we read until the writer closes its pipe.
    while (TRUE) {
        if ((nread = readResponse(p, size)) == -1) {
            log_err(gLogFn, "Error reading response for: %s", LVS_MSG_NAMES[msg]);
            lvlDelete(lvlH);
            return 0;
        }
        if (nread == 0) {
            break;
        }
        p += nread;
        size -= nread;
    }

    char* nextStr = NULL;
    p = buf;

    nextStr = strsep(&p, "\n");

    if (!nextStr) {
        // If we're here, it's because strsep returned NULL.
        // This can happen if the first argument to strsep
        // is NULL, or if strsep cannot find our chosen
        // delimiter in the first argument.
        lvlDelete(lvlH);
        return 0;
    }

    // Get the number of cameras.
    int count = atoi(nextStr);

    if (count < 0) {
        lvlDelete(lvlH);
        return 0;
    }

    // Get the camera names.
    for (int i = 0; i < count; i++) {

        nextStr = strsep(&p, "\n");

        if (!nextStr) {
            /* We got here because strsep returned NULL.
             We were able to get some of the names up to this
             point, so return the count of the number of names
             we have successfully acquired. NOTE: Because we are
             returning here, the supported resolutions for each
             device does not get recorded, so they will all
             appear to have "0" supported resolutions.
             */
            lvlDelete(lvlH);
            return i;
        }

        strncpy(deviceNames[i], nextStr, sizeof(deviceNames[i]));
    }

    // Get the number of supported resolutions for all devices.
    for (int i = 0; i < count; i++) {

        nextStr = strsep(&p, "\n");

        if (!nextStr) {
            /* We got here because strsep returned NULL.
             This means we won't be able to get the supported
             resolutions for any of the devices thus far. So
             just break here, because the next for-loop below
             will recognize the error, and rewrite all of the
             number of resolutions per device back to zero.
             */
            break;
        }

        numResolutionsPerDevice[i] = atoi(nextStr);
    }

    char* nextResWidth = NULL;
    char* nextResHeight = NULL;

    // Get all of the supported resolutions for all devices.
    for (int i = 0; i < count; i++) {
        for (int j = 0; j < numResolutionsPerDevice[i]; j++)
        {
            nextResWidth = strsep(&p, "\n");
            nextResHeight = strsep(&p, "\n");

            if (!nextResWidth || !nextResHeight) {
                /* We got here because strsep returned NULL.
                 This means one of two things:
                 1. The previous for-loop did not complete
                 successfully for some reason, or
                 2. We were able to record a couple of resolutions
                 for the device we're currently on, but encountered
                 an error.
                 A simple solution to handle both cases is to just
                 start overwriting the number of resolutions per
                 device back to zero, starting with the current
                 device. This will ensure that the device resolution
                 list will not be accessed for the rest of the devices,
                 including the one we're currently on.
                */
                numResolutionsPerDevice[i] = 0;
                continue;
            }

            deviceResolutions[i][j][0] = atoi(nextResWidth);
            deviceResolutions[i][j][1] = atoi(nextResHeight);
        }
    }

    lvlDelete(lvlH);

    return count;
}

/***********************************************************/

/*
Called to initialize the library for use.

    @param      logFn      Logging function.
    @param      dataDir    Location to store shared memory file, or NULL
                           to not use shared memory. NULL should only be
                           used internally, because most APIs depend on
                           the shared memory.
    @return     handle     Handle to the webcam instance, or NULL if error
 */
#define BUFFER_OFFSET(i) ((char*)NULL+(i))
SVLVL_API LocalVideoHandle lvlNew(log_fn_t logFn, const char* dataDir)
{
    int err = 0;
    gLogFn = logFn; // Save our log function

    // Uncomment the following lines to enable debugging
    //fprintf(stderr, "lvlNew paused. Connect to pid %i to debug...", getpid());
    //pause();

    // Start server
    if ((err = startServer(&gServerPid, dataDir))) {
        log_err(gLogFn, "Failed to start local video server");
        return NULL;
    }

    // Test connection with server
    err = sendMessageAndVerifyAck(LVS_TEST_CONNECTION);
    if (err) {
        log_err(gLogFn, "Error testing server connection.");
        return NULL;
    }

    // Return pid of server as the handle
    return (LocalVideoHandle)BUFFER_OFFSET(gServerPid);
}
#undef BUFFER_OFFSET

SVLVL_API void lvlDelete(LocalVideoHandle h)
{
    // Note: all cleanup necessary to be successfully
    // re-inited must be done here.

    // Tell the server to close by closing the write pipe
    if (gPipeOutput != -1 && (close(gPipeOutput)) == -1) {
        printError("close failed");
    }

    // Wait for our child (the server) to exit, otherwise
    // it will become a zombie process.
    if (gServerPid && (waitpid(gServerPid, NULL, 0)) == -1) {
        printError("wait failed");
    }

    // Closing our read pipe prevents file descriptor leaks.
    if (gPipeInput != -1 && (close(gPipeInput)) == -1) {
        printError("close failed");
    }

    // Unmap and close the shared memory file
    if (gMapAddr && (munmap(gMapAddr, kMmapSize)) == -1) {
        printError("munmap failed");
    }
    if (gMapFildes != -1 && (close(gMapFildes)) == -1) {
        printError("Failed to close memory mapped file");
    }

    // Re-initialize our globals
    gMapFildes = -1;
    gMapAddr = 0;
    gServerPid = 0;
    gLogFn = NULL;

    gPipeInput = -1;
    gPipeOutput = -1;
}

SVLVL_API void lvlSetVerbose(BOOL verbose)
{
    gVerbose = verbose;

    // If the server is already running, go ahead and send the message.
    // Otherwise we'll send the verbosity state upon server creation.
    if (gServerPid) {
        ((setVerboseParamsT*)gMapAddr)->verbose = verbose;
        sendMessageAndVerifyAck(LVS_SET_VERBOSE);
    }
}

// OSX doesn't need this, but streamreader.py calls this because
// of a bug in the Windows video library that makes lvlListDevices()
// crash.
SVLVL_API int lvlListDevicesWithoutResolutionList(log_fn_t logFn)
{
    return lvlListDevices(logFn);
}

SVLVL_API int lvlListDevices(log_fn_t logFn)
{
    listDevicesParamsT* data = (listDevicesParamsT*) gMapAddr;
    int numDevices;

    // If no server has been started, create one and call it directly,
    // so that we don't have to create shared memory.  The reason to avoid
    // that is that we don't want the caller to have to know the path, etc.
    if (!gServerPid) {
        return listDevicesWithoutServer();
    }

    if (sendMessageAndVerifyAck(LVS_LIST_DEVICES)) {
        log_err(gLogFn, "Error in %s", __func__);
        return 0;
    }

    numDevices = data->numDevices;

    memcpy(deviceNames, data->deviceNames, sizeof(deviceNames));
    memcpy(numResolutionsPerDevice, data->numResolutionsPerDevice, sizeof(numResolutionsPerDevice));
    memcpy(deviceResolutions, data->deviceResolutions, sizeof(deviceResolutions));

    return numDevices;
}

char* lvlGetDeviceName(int deviceID)
{
    return deviceNames[deviceID];
}

SVLVL_API int lvlGetNumSupportedResolutionsOfDevice(int deviceID)
{
    return numResolutionsPerDevice[deviceID];
}

SVLVL_API Dimensions* lvlGetSupportedResolutionPairOfDevice(int deviceID, int resPairIdx)
{
    dimensions.width = deviceResolutions[deviceID][resPairIdx][0];
    dimensions.height = deviceResolutions[deviceID][resPairIdx][1];
    return &dimensions;
}

SVLVL_API BOOL lvlSetupDevice(LocalVideoHandle h, int deviceID, int width, int height)
{
    setupDeviceParamsT* data = (setupDeviceParamsT*)gMapAddr;
    data->deviceID = deviceID;
    data->width = width;
    data->height = height;

    if (sendMessageAndVerifyAck(LVS_SETUP_DEVICE)) {
        log_err(gLogFn, "Error in %s", __func__);
        return FALSE;
    }

    return ((setupDeviceParamsT*) gMapAddr)->success;
}

SVLVL_API BOOL lvlIsDeviceSetup(LocalVideoHandle h, int deviceID)
{
    ((isDeviceSetupParamsT*)gMapAddr)->deviceID = deviceID;

    if (sendMessageAndVerifyAck(LVS_IS_DEVICE_SETUP)) {
        log_err(gLogFn, "Error in %s", __func__);
        return FALSE;
    }

    return ((isDeviceSetupParamsT*)gMapAddr)->isSetup;
}

SVLVL_API void lvlStopDevice(LocalVideoHandle h, int deviceID)
{
    ((stopDeviceParamsT*)gMapAddr)->deviceID = deviceID;

    if (sendMessageAndVerifyAck(LVS_STOP_DEVICE)) {
        log_err(gLogFn, "Error in %s", __func__);
    }
}

SVLVL_API int lvlGetWidth(LocalVideoHandle h, int deviceID)
{
    ((getWidthParamsT*)gMapAddr)->deviceID = deviceID;

    if (sendMessageAndVerifyAck(LVS_GET_WIDTH)) {
        log_err(gLogFn, "Error in %s", __func__);
        return 0;
    }

    return ((getWidthParamsT*)gMapAddr)->width;
}

SVLVL_API int lvlGetHeight(LocalVideoHandle h, int deviceID)
{
    ((getHeightParamsT*)gMapAddr)->deviceID = deviceID;

    if (sendMessageAndVerifyAck(LVS_GET_HEIGHT)) {
        log_err(gLogFn, "Error in %s", __func__);
        return 0;
    }

    return ((getHeightParamsT*)gMapAddr)->height;
}

SVLVL_API int lvlGetSize(LocalVideoHandle h, int deviceID)
{
    ((getSizeParamsT*)gMapAddr)->deviceID = deviceID;

    if (sendMessageAndVerifyAck(LVS_GET_SIZE)) {
        log_err(gLogFn, "Error in %s", __func__);
        return 0;
    }

    return ((getSizeParamsT*)gMapAddr)->size;
}

SVLVL_API int lvlGetPixelFormat()
{
    // Defined in ffMPEG's libavutil/pixfmt.h
    return PIXEL_FORMAT_YUYV422;
    //return PIX_FMT_UYVY422;
    //return PIX_FMT_RGB24;
}

SVLVL_API BOOL lvlIsFrameNew(LocalVideoHandle h, int deviceID)
{
    ((isFrameNewParamsT*)gMapAddr)->deviceID = deviceID;

    if (sendMessageAndVerifyAck(LVS_IS_FRAME_NEW)) {
        log_err(gLogFn, "Error in %s", __func__);
        return FALSE;
    }

    return ((isFrameNewParamsT*)gMapAddr)->isNew;
}

SVLVL_API BOOL lvlGetPixels (LocalVideoHandle h, int deviceID, unsigned char* pixels,
                   BOOL flipRedAndBlue, BOOL flipImage)
{
    getPixelsParamsT* data = (getPixelsParamsT*)gMapAddr;
    data->deviceID = deviceID;

    if (sendMessageAndVerifyAck(LVS_GET_PIXELS)) {
        log_err(gLogFn, "Error in %s", __func__);
        return FALSE;
    }

    if (data->success) {
        memcpy(pixels, gMapAddr+kVideoFrameOffset, data->pixelDataSize);
    }

    return data->success;
}

SVLVL_API void lvlShowSettingsWindow(LocalVideoHandle h, int deviceID)
{
    ((showSettingsWindowParamsT*)gMapAddr)->deviceID = deviceID;

    if (sendMessageAndVerifyAck(LVS_SHOW_SETTINGS_WINDOW)) {
        log_err(gLogFn, "Error in %s", __func__);
    }
}

