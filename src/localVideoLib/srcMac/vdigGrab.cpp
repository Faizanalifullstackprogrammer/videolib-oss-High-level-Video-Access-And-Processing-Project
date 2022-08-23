/*
 *  vdigGrab.c
 *  seeSaw
 *
 *  Created by Daniel Heckenberg.
 *  Copyright (c) 2004 Daniel Heckenberg. All rights reserved.
 *  (danielh.seeSaw<at>cse<dot>unsw<dot>edu<dot>au)
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, including without limitation
 * the right to use, copy, modify, merge, publish, communicate, sublicence,
 * and/or sell copies of the Software, and to permit persons to whom the
 * Software is furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included
 * in all copies or substantial portions of the Software.
 *
 * TO THE EXTENT PERMITTED BY APPLICABLE LAW, THIS SOFTWARE IS PROVIDED
 * "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR IMPLIED, INCLUDING BUT
 * NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY, FITNESS FOR A PARTICULAR
 * PURPOSE AND NON-INFRINGEMENT.  IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT
 * HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN
 * ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN
 * CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
 *
 */

#include "vdigGrab.h"

// Controls output of verbose debug info to stderr
static int verbose = false;

struct tagVdigGrab
{
    // State
    int isPreflighted;
    int isGrabbing;
    int isRecording;
    
    // QT Components
    SeqGrabComponent seqGrab;
    SGChannel sgchanVideo;
    ComponentInstance vdCompInst;
    
    // Device (source) settings
    ImageDescriptionHandle vdImageDesc;     // Source image information
    Rect vdDigitizerRect;                   // Video source rectangle
    
    // Destination Settings
    CGrafPtr dstPort;
    ImageSequence dstImageSeq;
    
    // Compression settings
    short cpDepth;
    CompressorComponent cpCompressor;
    CodecQ cpSpatialQuality;
    CodecQ cpTemporalQuality;
    long cpKeyFrameRate;
    Fixed cpFrameRate;
};

Boolean MySGModalFilterProc (
            DialogPtr            theDialog,
            const EventRecord    *theEvent,
            short                *itemHit,
            long                 refCon );

SeqGrabComponent
MakeSequenceGrabber(WindowRef pWindow);

OSErr
MakeSequenceGrabChannel(SeqGrabComponent seqGrab, SGChannel *sgchanVideo);

OSErr 
InitTimeBaseRate(VdigGrab* pVdg);

void
vdgSetVerbose(int _verbose)
{
    verbose = _verbose;
}

VdigGrab*
vdgNew()
{
    VdigGrab* pVdg;
    pVdg = (VdigGrab*)malloc(sizeof(VdigGrab));
    memset(pVdg, 0, sizeof(VdigGrab));
    return pVdg;
}


// Lets you enumerate through digitizer components, opening them for you.
//
// Never actually returns any errors--any digitizer components that fail to
// open are just skipped...
//
// @param  pState       Allocate a "Component" variable and init it to 0, then
//                      pass a pointer to it into this function.  This will let
//                      you enumerate all of the digitizer components.
// @param  pvdCompInst  We'll return an opened digitizer component here, or
//                      set it to NULL if we didn't open one.  You should call
//                      releaseDigitizerComponent() on this.
void
getNextDigitizerComponent (Component* pState, ComponentInstance* pvdCompInst)
{
    ComponentDescription desc;
    
    memset(&desc, 0, sizeof(ComponentDescription));
    desc.componentType = videoDigitizerComponentType;
    
    *pvdCompInst = NULL;
    
    while (1) 
    {
        *pState = FindNextComponent(*pState, &desc);
        if (*pState == 0) 
        {
            return;
        }
        
        if (verbose) {
            ComponentDescription descToPrint = { 0 };
            GetComponentInfo(*pState, &descToPrint, NULL, NULL, NULL);
            
            fprintf(stderr, "OpenComponent: vdig, %c%c%c%c, %c%c%c%c\n",
                    ((char*)&descToPrint.componentSubType)[3],
                    ((char*)&descToPrint.componentSubType)[2],
                    ((char*)&descToPrint.componentSubType)[1],
                    ((char*)&descToPrint.componentSubType)[0],
                    ((char*)&descToPrint.componentManufacturer)[3],
                    ((char*)&descToPrint.componentManufacturer)[2],
                    ((char*)&descToPrint.componentManufacturer)[1],
                    ((char*)&descToPrint.componentManufacturer)[0]);
        }
        
        *pvdCompInst = OpenComponent(*pState);
        if (*pvdCompInst != NULL) 
        {
            return;
        }
        else if (verbose)
        {
            // Happens if no cameras...
            fprintf(stderr, "...failed to open!\n");
        }
    }
}

OSErr
releaseDigitizerComponent (ComponentInstance vdCompInst)
{
    OSErr err;
    
    if (!vdCompInst)
        return 0;
    
    if ((err = CloseComponent(vdCompInst))) {
        if (err == 704)  // USB only???
            fprintf(stderr, "Error %d: Device not connected\n", err);
        else
            fprintf(stderr, "CloseComponent err=%d\n", err);
    }
    
    return err;
}


// Get / open the digitizer component with the given device ID.
//
// @param  pDeviceID    A device ID, which is an index into the list of
//                      devices.  Assumes that FindNextComponent() and the
//                      enumeration of devices within a component are
//                      consistent.  NOTE: This will be modified so that upon
//                      return it will be an index into the devices in the
//                      opened component.
// @param  pvdCompInst  We'll return an opened digitizer component here, or
//                      set it to NULL if we didn't open one.  You should call
//                      releaseDigitizerComponent() on this.
// @return err          We'll return an error here, or 0 if no error.
OSErr
getDigitizerComponent(int* pDeviceID, ComponentInstance* pvdCompInst)
{
    OSErr err;
    Component enumState = 0;
    int globalDeviceID = *pDeviceID;
    
    // Just to be sure...
    *pvdCompInst = NULL;
    
    if (verbose)
        fprintf(stderr, "Getting digitizer component %d\n", globalDeviceID);
    
    // Set the digitizer and input
    while (1)
    {
        getNextDigitizerComponent(&enumState, pvdCompInst);
        
        // If we had no error, but we didn't get back any digitizer components,
        // then we've enumerated through all the components and haven't found
        // enough.  The device ID must have been too large.
        if (*pvdCompInst == NULL) 
        {
            err = qtParamErr;
            fprintf(stderr, "Device ID %d too large\n", globalDeviceID);
            return err;
        }
        
        short n;
        if ((err = VDGetNumberOfInputs(*pvdCompInst, &n)))
        {
            fprintf(stderr, "VDGetNumberOfInputs err=%d\n", err);
            // Don't return--just go onto the next digitizer component...
            continue;
        }
        else
        {
            n += 1;  // VDGetNumberOfInputs returns a count that is zero-based!
        }
        
        if (verbose)
            fprintf(stderr, "...found %d video digitizer input(s)\n", n);
        
        if (n-1 < *pDeviceID)
        {
            // We haven't found it yet, so release this component so we can
            // loop around and go onto the next one...
            if ((err = releaseDigitizerComponent(*pvdCompInst)))
            {
                fprintf(stderr, "releaseDigitizerComponent err=%d\n", err);
                // Don't return--just go onto the next digitizer component...
            }
            *pvdCompInst = NULL;
            
            // Subtract devices from this already-enumerated component...
            *pDeviceID -= n;
        }
        else
        {
            // We found it.  Component has already been opened and deviceID
            // has already been adjusted, so just return that there were no
            // errors...
            if (verbose)
                fprintf(stderr, "...got digitizer component %d\n", globalDeviceID);
            return 0;
        }
    }
}

OSErr
vdgInit(VdigGrab* pVdg, const int globalDeviceID)
{
    OSErr err;
    int deviceID;
    
    memset(pVdg, 0, sizeof(VdigGrab));
    
    // Set the digitizer and input; need to do this early for VideoGlide
    // driver to work, for some reason (otherwise it fails to open)...
    deviceID = globalDeviceID;
    if ((err = getDigitizerComponent(&deviceID, &pVdg->vdCompInst)))
    {
        fprintf(stderr, "getDigitizerComponent err=%d\n", err);
        return err;
    }
        
    if ((err = EnterMovies()))
    {
        fprintf(stderr, "EnterMovies err=%d\n", err);
        return err;
    }
    
    if (!(pVdg->seqGrab = MakeSequenceGrabber(NULL)))
    {
        fprintf(stderr, "MakeSequenceGrabber error\n");
        return -1;
    }
    
    if ((err = MakeSequenceGrabChannel(pVdg->seqGrab, &pVdg->sgchanVideo)))
    {
        if ( err != couldntGetRequiredComponent )
            fprintf(stderr, "MakeSequenceGrabChannel err=%d\n", err);
        else
            fprintf(stderr, "Device busy or missing\n");
        return err;
    }
    
    if ((err = SGSetVideoDigitizerComponent(pVdg->sgchanVideo, 
                                            pVdg->vdCompInst)))
    {
        fprintf(stderr, "SGSetVideoDigitizerComponent err=%d\n", err);
        return err;
    }
    
    if ((err = VDSetInput(pVdg->vdCompInst, deviceID)))
    {
        fprintf(stderr, "VDSetInput err=%d\n", err);
        return err;
    }
    
    if (verbose) fprintf(stderr, "SETUP: Successfully initialized\n");

    return 0;
}

OSErr
vdgGetNumberOfInputs(short* n)
{
    OSErr err;
    ComponentInstance vdCompInst = NULL;
    Component enumState = 0;
    
    *n = 0;
    
    while (1)
    {
        short thisN = 0;
        
        getNextDigitizerComponent(&enumState, &vdCompInst);
        
        // If we got to the end of the enumeration, break out of the loop...
        if (vdCompInst == NULL) {
            break;
        }
        
        if ((err = VDGetNumberOfInputs(vdCompInst, &thisN)))
            fprintf(stderr, "VDGetNumberOfInputs err=%d\n", err);
        else
            // VDGetNumberOfInputs returns a count that is zero-based!
            *n += (thisN + 1);
        
        if ((err = releaseDigitizerComponent(vdCompInst)))
        {
            fprintf(stderr, "releaseDigitizerComponent err=%d\n", err);
            // Don't return--just go onto the next digitizer component...
        }
        vdCompInst = NULL;
    }
    
    // TODO: Return the error if some components behaving badly?
    return 0;
}

OSErr
vdgGetDeviceNameAndFlags(int globalDeviceID, char* szName, long* pBuffSize, 
                         UInt32* pVdFlags)
{
    OSErr err;
    Str255 vdName;
    UInt32 vdFlags;
    ComponentInstance vdCompInst = 0;
    int deviceID;
    
    deviceID = globalDeviceID;
    if ((err = getDigitizerComponent(&deviceID, &vdCompInst)))
    {
        fprintf(stderr, "getDigitizerComponent err=%d\n", err);
        return err;
    }
    
    if (!pBuffSize)
    {
        fprintf(stderr, "vdgGetDeviceName: NULL pointer error\n");
        err = qtParamErr;
        goto endFunc;
    }
    
    if ((err = VDGetDeviceNameAndFlags(vdCompInst, vdName, &vdFlags)))
    {
        // To make the VideoGlide driver work, we need to ignore this err (and 
        // assume the vdDeviceFlagShowInputsAsDevices flag is set).
        vdFlags = vdDeviceFlagShowInputsAsDevices;
        
        //fprintf(stderr, "VDGetDeviceNameAndFlags err=%d\n", err);
        
        //*pBuffSize = 0;
        //goto endFunc;
    }
    
    if (vdFlags & vdDeviceFlagShowInputsAsDevices)
    {
        if ((err = VDGetInputName(vdCompInst, deviceID, vdName)))
        {
            fprintf(stderr, "VDGetInputName err=%i\n", err);
            goto endFunc;
        }
    }
    
    if (szName)
    {
        int copyLen = *pBuffSize-1 < vdName[0] ?
        *pBuffSize - 1 : vdName[0];
        
        strncpy(szName, (char *)vdName+1, copyLen);
        szName[copyLen] = '\0';
        
        *pBuffSize = copyLen + 1;
    } else
    {
        *pBuffSize = vdName[0] + 1;
    }
    
    if (pVdFlags)
        *pVdFlags = vdFlags;

endFunc:
    if ((err = releaseDigitizerComponent(vdCompInst)))
    {
        fprintf(stderr, "releaseDigitizerComponent err=%d\n", err);
        return err;
    }
    
    return err;
}

OSErr
vdgSetDestination(VdigGrab* pVdg, CGrafPtr dstPort)
{
    pVdg->dstPort = dstPort;
    return noErr;
}

OSErr
vdgPreflightGrabbing(VdigGrab* pVdg, int w, int h)
{
    /* from Steve Sisak (on quicktime-api list):
     A much more optimal case, if you're doing it yourself is:
     
     VDGetDigitizerInfo()     // make sure this is a compressed source only
     VDGetCompressionTypes()  // tells you the supported types
     VDGetMaxSourceRect()     // returns full-size rectangle (sensor size)
     VDSetDigitizerRect()     // determines cropping
     
     VDSetCompressionOnOff(true)
     
     VDSetFrameRate()         // set to 0 for default
     VDSetCompression()       // compresstype=0 means default
     VDGetImageDescription()  // find out image format
     VDGetDigitizerRect()     // find out if vdig is cropping for you
     VDResetCompressSequence()
     
     (grab frames here)
     
     VDSetCompressionOnOff(false)
     */
    OSErr err;
    Rect maxRect;
    
    DigitizerInfo info;
    
    // make sure this is a compressed source only
    if ((err = VDGetDigitizerInfo(pVdg->vdCompInst, &info)))
    {
        if (!(info.outputCapabilityFlags & digiOutDoesCompress))
        {
            fprintf(stderr, "VDGetDigitizerInfo: not a compressed source device.\n");
            goto endFunc;
        }
    }
    
    if ((err = VDGetMaxSrcRect(  pVdg->vdCompInst, currentIn, &maxRect)))
    {
        fprintf(stderr, "VDGetMaxSrcRect err=%d\n", err);
        //goto endFunc;
    }
    
    // Crop the source here if you want to.  This is different than the 
    // destination scaling, which is done below
    if ((err = VDSetDigitizerRect( pVdg->vdCompInst, &maxRect)))
    {
        fprintf(stderr, "VDSetDigitizerRect err=%d\n", err);
    }
    
    if ((err = VDSetCompressionOnOff( pVdg->vdCompInst, 1)))
    {
        fprintf(stderr, "VDSetCompressionOnOff err=%d\n", err);
    }
    
    // We could try to force the frame rate here... necessary for ASC softvdig
    if ((err = VDSetFrameRate( pVdg->vdCompInst, 0)))
    {
        fprintf(stderr, "VDSetFrameRate err=%d\n", err);
    }
    
    // Try to set a format that matches our target. Note that
    // USB cameras appear to only have the YUV compression type. 
    maxRect.top = 0; maxRect.bottom = h;
    maxRect.left = 0; maxRect.right = w;
    if ((err = VDSetCompression(pVdg->vdCompInst,
                                0, //'yuv2'
                                0,
                                &maxRect,
                                0, //codecNormalQuality,
                                0, //codecNormalQuality,
                                0)))
    {
        fprintf(stderr, "VDSetCompression err=%d\n", err);
    }
        
    pVdg->vdImageDesc = (ImageDescriptionHandle)NewHandle(0);
    if ((err = VDGetImageDescription( pVdg->vdCompInst, pVdg->vdImageDesc)))
    {
        fprintf(stderr, "VDGetImageDescription err=%d\n", err);
    }
    
    // Check whether the digitizer is cropping for you.  Note that this 
    // is the video source rectangle, not the output rectange. 
    if ((err = VDGetDigitizerRect( pVdg->vdCompInst, &pVdg->vdDigitizerRect)))
    {
        fprintf(stderr, "VDGetDigitizerRect err=%d\n", err);
    }

    if ((err = VDResetCompressSequence( pVdg->vdCompInst )))
    {
        if (err == digiUnimpErr)
        {
            if (verbose)
                fprintf(stderr, "SETUP: VDResetCompressSequence err " \
                        "\"digiUnimpErr\" (expected with webcams)\n");
            err = 0; 
        }
        else
        {
            fprintf(stderr, "VDResetCompressSequence err=%d\n", err);
        }
    }
    
    pVdg->isPreflighted = 1;
    
    if (verbose) fprintf(stderr, "SETUP: Successfully preflighted\n");
    
endFunc:
    return err;
}

OSErr
vdgStartGrabbing(VdigGrab* pVdg)
{
    OSErr err;
    
    Rect dstBounds; 
    PixMapHandle pm; 
    
    if (!pVdg->isPreflighted)
    {
        fprintf(stderr, "vdgStartGrabbing called without previous successful vdgPreflightGrabbing()\n");
        err = badCallOrderErr;
        goto endFunc;
    }
    
    // Start the digitizing process
    if ((err = VDCompressOneFrameAsync( pVdg->vdCompInst )))
    {
        fprintf(stderr, "VDCompressOneFrameAsync err=%d\n", err);
        goto endFunc;
    }
    
    // Start the decompression process
    pm = GetGWorldPixMap(pVdg->dstPort);
    GetPixBounds(pm, &dstBounds);
    if ((err = vdgDecompressionSequenceBegin( pVdg, pVdg->dstPort, &dstBounds)))
    {
        fprintf(stderr, "vdgDecompressionSequenceBegin err=%d\n", err);
        goto endFunc;
    }
    
    // If we don't do this, some drivers will not return any frames
    err = InitTimeBaseRate(pVdg);
    
    pVdg->isGrabbing = 1;
    
    if (verbose) fprintf(stderr, "SETUP: Successfully started grabbing\n");
    
endFunc:
    return err;
}

OSErr
vdgGetDataRate( VdigGrab*   pVdg,
               long*       pMilliSecPerFrame,
               Fixed*      pFramesPerSecond,
               long*       pBytesPerSecond)
{
    OSErr err;
    
    if ((err = VDGetDataRate( pVdg->vdCompInst,
                             pMilliSecPerFrame,
                             pFramesPerSecond,
                             pBytesPerSecond)))
        fprintf(stderr, "VDGetDataRate err=%d\n", err);
    
    return err;
}

OSErr
vdgGetImageDescription( VdigGrab* pVdg,
                       ImageDescriptionHandle vdImageDesc )
{
    OSErr err;
    
    if ((err = VDGetImageDescription( pVdg->vdCompInst, vdImageDesc)))
        fprintf(stderr, "VDGetImageDescription err=%d\n", err);
    
    return err;
}

OSErr
vdgGetSrcWidth( VdigGrab* pVdg, int* width )
{
    OSErr err = 0;
    
    if (!pVdg->isPreflighted)
    {
        fprintf(stderr, "vdgGetSrcWidth called without previous successful "\
                            "vdgPreflightGrabbing()\n");
        err = badCallOrderErr;
        return err;
    }
    
    *width = (*pVdg->vdImageDesc)->width;
    
    return err;
}

OSErr
vdgGetSrcHeight( VdigGrab* pVdg, int* height )
{
    OSErr err = 0;
    
    if (!pVdg->isPreflighted)
    {
        fprintf(stderr, "vdgGetSrcHeight called without previous successful "\
                            "vdgPreflightGrabbing()\n");
        err = badCallOrderErr;
        return err;
    }
    
    *height = (*pVdg->vdImageDesc)->height;
    
    return err;
}

OSErr
vdgDecompressionSequenceBegin(VdigGrab* pVdg,
                              CGrafPtr dstPort,
                              Rect* pDstRect)
{
    OSErr err;
        
    Rect sourceRect;
    MatrixRecord scaleMatrix;
    MatrixRecordPtr scaleMatrixP = NULL;
    
    // Make a scaling matrix for the sequence.  If the video digitizer can 
    // handle resizing, our source and destination will be equal.  If not,
    // this will give us the destination size we want. 
    if (pDstRect) {
        sourceRect.left = 0;
        sourceRect.top = 0;
        sourceRect.right = (*pVdg->vdImageDesc)->width;
        sourceRect.bottom = (*pVdg->vdImageDesc)->height;
        RectMatrix(&scaleMatrix, &sourceRect, pDstRect);
        scaleMatrixP = &scaleMatrix;
    }
    
    // !HACK! Different conversions are used for these two equivalent types
    // so we force the cType so that the more efficient path is used
    if ((*pVdg->vdImageDesc)->cType == FOUR_CHAR_CODE('yuv2'))
        (*pVdg->vdImageDesc)->cType = FOUR_CHAR_CODE('yuvu'); // kYUVUPixelFormat
    
    // begin the process of decompressing a sequence of frames
    // this is a set-up call and is only called once for the sequence
    // - the ICM will interrogate different codecs and construct a
    // suitable decompression chain, as this is a time consuming
    // process we don't want to do this once per frame (eg. by using
    // DecompressImage) for more information see Ice Floe #8
    // http://developer.apple.com/quicktime/icefloe/dispatch008.html
    // the destination is specified as the GWorld
    if ((err = DecompressSequenceBeginS(
                                        // pointer to field to receive unique
                                        // ID for sequence
                                        &pVdg->dstImageSeq,
                                        // handle to image description structure
                                        pVdg->vdImageDesc,
                                        0,
                                        0,
                                        // port for the DESTINATION image
                                        dstPort,
                                        // graphics device handle, if port is
                                        // set, set to NULL
                                        NULL,
                                        // source rectangle defining the
                                        // portion of the image to decompress
                                        //&sourceRect,
                                        NULL,
                                        // transformation matrix
                                        scaleMatrixP,
                                        // transfer mode specifier
                                        srcCopy,
                                        // clipping region in dest. coordinate
                                        // system to use as a mask
                                        (RgnHandle)NULL,
                                        0, // flags
                                        // accuracy in decompression
                                        codecHighQuality,
                                        // compressor identifier or special
                                        // identifiers ie. bestSpeedCodec
                                        bestSpeedCodec)))
    {
        fprintf(stderr, "DecompressSequenceBeginS err=%d\n", err);
    }
    
    return err;
}

OSErr
vdgDecompressionSequenceWhen(VdigGrab* pVdg,
                             Ptr theData,
                             long dataSize)
{
    OSErr err;
    CodecFlags  ignore = 0;
    
    if ((err = DecompressSequenceFrameWhen(
                                           pVdg->dstImageSeq, // sequence ID returned by DecompressSequenceBegin
                                           theData,  // pointer to compressed image data
                                           dataSize, // size of the buffer
                                           0,        // in flags
                                           &ignore,  // out flags
                                           NULL,     // async completion proc
                                           NULL )))
        fprintf(stderr, "DecompressSequenceFrameWhen err=%d\n", err);
    
    return err;
}

OSErr
vdgDecompressionSequenceEnd( VdigGrab* pVdg )
{
    OSErr err;
    
    if (!pVdg->dstImageSeq)
    {
        fprintf(stderr, "vdgDecompressionSequenceEnd NULL sequence\n");
        err = qtParamErr;
        goto endFunc;
    }
    
    if ((err = CDSequenceEnd(pVdg->dstImageSeq)))
    {
        fprintf(stderr, "CDSequenceEnd err=%d\n", err);
        goto endFunc;
    }
    
    pVdg->dstImageSeq = 0;
    
endFunc:
    return err;
}

OSErr
vdgStopGrabbing(VdigGrab* pVdg)
{
    OSErr err;
    
    if ( !pVdg->vdCompInst )
        return 0;
    
    if ((err = VDSetCompressionOnOff( pVdg->vdCompInst, 0)))
    {
        fprintf(stderr, "VDSetCompressionOnOff err=%d\n", err);
    }
    
    if ((err = vdgDecompressionSequenceEnd(pVdg)))
    {
        fprintf(stderr, "vdgDecompressionSequenceEnd err=%d\n", err);
    }
    
    pVdg->isGrabbing = 0;
    
    return err;
}

bool
vdgIsGrabbing(VdigGrab* pVdg)
{
    return pVdg->isGrabbing;
}

// Checks if a frame is available, and if it is, decompresses it into our 
// buffer and releases the compressed frame. As a side effect, kicks off 
// compression of the next frame. 
// If pIsUpdated is set to true, a new decompressed frame is available.  
OSErr
vdgIdle(VdigGrab* pVdg, int*  pIsUpdated)
{
    OSErr err;
    
    UInt8       queuedFrameCount;
    Ptr         theData;
    long        dataSize;
    UInt8       similarity;
    TimeRecord  time;
    
    *pIsUpdated = 0;

    // If we poll and one or more frames is available, decompress into our 
    // offscreen GWorld buffer and release the compressed buffer (I believe). 
    if ( !(err = vdgPoll( pVdg,
                         &queuedFrameCount,
                         &theData,
                         &dataSize,
                         &similarity,
                         &time))
        && queuedFrameCount)
    {
        *pIsUpdated = 1;
        
        // Decompress the sequence
        if ((err = vdgDecompressionSequenceWhen(pVdg, theData, dataSize)))
        {
            fprintf(stderr, "vdgDecompressionSequenceWhen err=%d\n", err);
            //goto endFunc;
        }
                
        // Free the buffer returned by vdgPoll
        if ((err = vdgReleaseBuffer(pVdg, theData)))
        {
            fprintf(stderr, "vdgReleaseBuffer err=%d\n", err);
            //goto endFunc;
        }
    }
    
    if (err)
    {
        fprintf(stderr, "vdgPoll err=%d\n", err);
        goto endFunc;
    }
    
endFunc:
    return err;
}

// Checks whether a new frame is available, and if so, returns the data as well.
// If pQueuedFrameCount > 0, one or more new frames is available.  
// If the frame is available, this function also kicks off compression of the 
// next frame. 
OSErr
vdgPoll(VdigGrab* pVdg,
        UInt8*            pQueuedFrameCount,
        Ptr*        pTheData,
        long*       pDataSize,
        UInt8*            pSimilarity,
        TimeRecord* pTime )
{
    OSErr err;
    
    if (!pVdg->isGrabbing)
    {
        fprintf(stderr, "vdgPoll error: not grabbing\n");
        err = qtParamErr;
        goto endFunc;
    }
    
    // Check if a new frame is available
    if ((err = VDCompressDone(pVdg->vdCompInst,
                              pQueuedFrameCount,
                              pTheData,
                              pDataSize,
                              pSimilarity,
                              pTime)))
    {
        fprintf(stderr, "vdgPoll error: not grabbing\n");
        goto endFunc;
    }
    
    // Overlapped grabbing
    if (*pQueuedFrameCount)
    {
        if ((err = VDCompressOneFrameAsync(pVdg->vdCompInst)))
        {
            fprintf(stderr, "VDCompressOneFrameAsync err=%d\n", err);
            goto endFunc;
        }
    }
    
endFunc:
    return err;
}

OSErr
vdgReleaseBuffer(VdigGrab*   pVdg, Ptr theData)
{
    OSErr err;
    
    if ((err = VDReleaseCompressBuffer(pVdg->vdCompInst, theData)))
        fprintf(stderr, "VDReleaseCompressBuffer err=%d\n", err);
    
    return err;
}

OSErr
vdgUninit(VdigGrab* pVdg)
{
    OSErr err = noErr;
    
    if (pVdg->vdImageDesc)
    {
        DisposeHandle((Handle)pVdg->vdImageDesc);
        pVdg->vdImageDesc = nil;
    }
    
    if (pVdg->vdCompInst)
    {
        if ((err = releaseDigitizerComponent(pVdg->vdCompInst)))
            fprintf(stderr, "releaseDigitizerComponent err=%d\n", err);
        pVdg->vdCompInst = nil;
    }
    
    if (pVdg->sgchanVideo)
    {
        if ((err = SGDisposeChannel(pVdg->seqGrab, pVdg->sgchanVideo)))
            fprintf(stderr, "SGDisposeChannel err=%d\n", err);
        pVdg->sgchanVideo = nil;
    }
    
    if (pVdg->seqGrab)
    {
        if ((err = CloseComponent(pVdg->seqGrab)))
            fprintf(stderr, "CloseComponent err=%d\n", err);
        pVdg->seqGrab = nil;
    }
    
    ExitMovies();
    return err;
}

void
vdgDelete(VdigGrab* pVdg)
{
    if (!pVdg)
    {
        fprintf(stderr, "vdgDelete NULL pointer\n");
        return;
    }
    
    free(pVdg);
}

OSErr
vdgGetSettings(VdigGrab* pVdg)
{
    OSErr err;
    
    // Extract information from the SG
    if ((err = SGGetVideoCompressor(pVdg->sgchanVideo,
                                    &pVdg->cpDepth,
                                    &pVdg->cpCompressor,
                                    &pVdg->cpSpatialQuality,
                                    &pVdg->cpTemporalQuality,
                                    &pVdg->cpKeyFrameRate)))
    {
        fprintf(stderr, "SGGetVideoCompressor err=%d\n", err);
        goto endFunc;
    }
    
    if ((err = SGGetFrameRate(pVdg->sgchanVideo, &pVdg->cpFrameRate)))
    {
        fprintf(stderr, "SGGetFrameRate err=%d\n", err);
        goto endFunc;
    }
    
endFunc:
    return err;
}

OSErr
vdgSetFrameRate(VdigGrab* pVdg, float frameRate)
{
    OSErr err;
    Fixed rate;
    
    rate = FloatToFixed(frameRate);
    if ((err = VDSetFrameRate( pVdg->vdCompInst, rate)))
    {
        fprintf(stderr, "VDSetFrameRate err=%d\n", err);
    }
    
    return err;
}

void
vdgShowSettingsWindow(VdigGrab* pVdg)
{
    ComponentResult	err;
    SGModalFilterUPP	seqGragModalFilterUPP;

    err = SGPause (pVdg->seqGrab, true);

    seqGragModalFilterUPP = (SGModalFilterUPP)NewSGModalFilterUPP(MySGModalFilterProc);
	err = SGSettingsDialog(pVdg->seqGrab, pVdg->sgchanVideo, 0, 
                           NULL, 0L, seqGragModalFilterUPP, 0);
	DisposeSGModalFilterUPP(seqGragModalFilterUPP);
    
    err = SGPause (pVdg->seqGrab, false);
}

// --------------------
// MakeSequenceGrabber  (adapted from Apple mung sample)
//
SeqGrabComponent
MakeSequenceGrabber(WindowRef pWindow)
{
    SeqGrabComponent seqGrab = NULL;
    OSErr err = noErr;
    
    // open the default sequence grabber
    if (!(seqGrab = OpenDefaultComponent(SeqGrabComponentType, 0)))
    {
        fprintf(stderr, "OpenDefaultComponent failed to open the default sequence grabber.\n");
        goto endFunc;
    }
    
    // initialize the default sequence grabber component
    if ((err = SGInitialize(seqGrab)))
    {
        fprintf(stderr, "SGInitialize err=%d\n", err);
        goto endFunc;
    }
    
    // The sequence grabber wants a port even if it is not drawing to it. 
    // In that case, pass it a null pWindow. 
    if ((err = SGSetGWorld(seqGrab, GetWindowPort(pWindow), NULL)))
    {
        fprintf(stderr, "SGSetGWorld err=%d\n", err);
        goto endFunc;
    }
    
    // specify the destination data reference for a record operation
    // tell it we're not making a movie
    // if the flag seqGrabDontMakeMovie is used, the sequence grabber still calls
    // your data function, but does not write any data to the movie file
    // writeType will always be set to seqGrabWriteAppend
    if ((err = SGSetDataRef(seqGrab, 0, 0, seqGrabDontMakeMovie)))
    {
        fprintf(stderr, "SGSetGWorld err=%d\n", err);
        goto endFunc;
    }
    
endFunc:
    if (err && (seqGrab != NULL))
    { // clean up on failure
        CloseComponent(seqGrab);
        seqGrab = NULL;
    }
    
    return seqGrab;
}


// --------------------
// MakeSequenceGrabChannel (adapted from Apple mung sample)
//
OSErr
MakeSequenceGrabChannel(SeqGrabComponent seqGrab, SGChannel* psgchanVideo)
{
    long  flags = 0;
    OSErr err = noErr;
    
    if ((err = SGNewChannel(seqGrab, VideoMediaType, psgchanVideo)))
    {
        if ( err != couldntGetRequiredComponent )
            fprintf(stderr, "SGNewChannel err=%d\n", err);
        goto endFunc;
    }
    
    //err = SGSetChannelBounds(*sgchanVideo, rect);
    // set usage for new video channel to avoid playthrough
    // note we don't set seqGrabPlayDuringRecord
    if ((err = SGSetChannelUsage(*psgchanVideo, flags | seqGrabRecord)))
    {
        fprintf(stderr, "SGSetChannelUsage err=%d\n", err);
        goto endFunc;
    }
    
endFunc:
    if ((err != noErr) && psgchanVideo)
    {
        // clean up on failure
        SGDisposeChannel(seqGrab, *psgchanVideo);
        *psgchanVideo = NULL;
    }
    
    return err;
}

// From QT sample code
// Declaration of a typical application-defined function
Boolean MySGModalFilterProc (
            DialogPtr            theDialog,
            const EventRecord    *theEvent,
            short                *itemHit,
            long                 refCon )
{
      // Ordinarily, if we had multiple windows we cared about, we'd handle
      // updating them in here, but since we don't, we'll just clear out
      // any update events meant for us
      Boolean handled = false;

      if ((theEvent->what == updateEvt) &&
                  ((WindowPtr) theEvent->message == (WindowPtr) refCon))
      {
            BeginUpdate ((WindowPtr) refCon);
            EndUpdate ((WindowPtr) refCon);
            handled = true;
      }
      return handled;
}

// Gets the time base, sets the rate to the default of 1.0x,
// and sets it back.  A time base rate is the rate at which the 
// video is interpreted relative to real time. Required by some vdig's.
OSErr 
InitTimeBaseRate(VdigGrab* pVdg)
{
    OSErr err; 
    TimeBase timeBase;
    
    if ((err = SGGetTimeBase(pVdg->seqGrab, &timeBase)))
    {
        fprintf(stderr, "SGGetTimeBase err=%d\n", err);
        return err;
    }
    SetTimeBaseRate(timeBase, fixed1);   // 1x rate
    if ((err = CDSequenceSetTimeBase( pVdg->dstImageSeq, timeBase)))
    {
        fprintf(stderr, "CDSequenceSetTimeBase err=%d\n", err);
        return err;
    }
    
    return err;
}

OSErr
createOffscreenGWorld(GWorldPtr* pGWorldPtr,
                      OSType pixelFormat,
                      Rect* pBounds)
{
    OSErr err;
    CGrafPtr theOldPort;
    GDHandle theOldDevice;
    
    // create an offscreen GWorld
    if ((err = QTNewGWorld(pGWorldPtr,           // returned GWorld
                           pixelFormat, // pixel format
                           pBounds,     // bounds
                           0,           // color table
                           NULL,        // GDHandle
                           0)))         // flags
    {
        fprintf(stderr, "QTNewGWorld: err=%d\n", err);
        goto endFunc;
    }
    
    // lock the pixmap and make sure it's locked because
    // we can't decompress into an unlocked PixMap
    if (!LockPixels(GetGWorldPixMap(*pGWorldPtr)))
        fprintf(stderr, "createOffscreenGWorld: Can't lock pixels!\n");
    
    GetGWorld(&theOldPort, &theOldDevice);
    SetGWorld(*pGWorldPtr, NULL);
    EraseRect(pBounds);
    SetGWorld(theOldPort, theOldDevice);
    
endFunc:
    return err;
}

void
disposeOffscreenGWorld(GWorldPtr gworld)
{
    UnlockPixels(GetGWorldPixMap(gworld));
    DisposeGWorld(gworld);
}
