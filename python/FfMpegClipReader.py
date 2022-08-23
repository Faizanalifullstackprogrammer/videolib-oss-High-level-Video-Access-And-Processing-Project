#! /usr/local/bin/python

#*****************************************************************************
#
# FfMpegClipReader.py
#   A more specific instance of ClipReader API.
#   Despite what the name suggests, it isn't specific to ffmpeg
#
#*****************************************************************************
#
# Copyright 2013-2022 Sighthound, Inc.
#
# Licensed under the GNU GPLv3 license found at
# https://www.gnu.org/licenses/gpl-3.0.txt
#
# Alternative licensing available from Sighthound, Inc.
# by emailing opensource@sighthound.com
#
# This file is part of the Sighthound Video project which can be found at
# https://github.url/thing
#
# This program is free software; you can redistribute it and/or modify
# it under the terms of the GNU General Public License as published by
# the Free Software Foundation; using version 3 of the License.
#
# This program is distributed in the hope that it will be useful,
# but WITHOUT ANY WARRANTY; without even the implied warranty of
# MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
# GNU General Public License for more details.
#
# You should have received a copy of the GNU General Public License
# along with this program; if not, write to the Free Software
# Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02111, USA.
#
#*****************************************************************************

"""
## @file
Contains an interface to the c videolib clips module.
"""

from ctypes import CFUNCTYPE, POINTER, Structure, c_void_p, c_int
from ctypes import c_char_p, c_longlong, byref, c_uint64, c_int64
import sys
import os
import traceback

from vitaToolbox.ctypesUtils.PtrFreer import PtrFreer, withByref

from vitaToolbox.image.ImageConversion import convertClipFrameToPIL
from vitaToolbox.image.ImageConversion import convertClipFrameToWxBitmap
from vitaToolbox.image.ImageConversion import convertClipFrameToNumpy
from vitaToolbox.image.ImageConversion import convertNumpyToPilNoNorm
from vitaToolbox.image.ImageConversion import convertClipFrameToBuffer
from vitaToolbox.ctypesUtils.LoadLibrary import LoadLibrary
from vitaToolbox.loggingUtils.LoggingUtils import getStderrLogCB, kLogLevelCritical
from vitaToolbox.process.ProcessUtils import getMemoryStats
from vitaToolbox.strUtils.EnsureUnicode import ensureUtf8

from videoLib2.python.ClipReader import ClipFrame
from videoLib2.python.VideoLibUtils import SetVideoLibDataPath, getTimestampFlags


_libName = 'videolib'

_lib = LoadLibrary(None, _libName)


##############################################################################
class BoxOverlayInfo(Structure):
    """Information describing a single box overlay"""
    _fields_ = [("ms", c_longlong),
                ("drawboxParams", c_char_p)]
    def _toString(self):
        return "[ms=" + str(self.ms) + " params=" + str(self.drawboxParams) + "]"
    def __repr__(self):
        return self._toString()
    def __str__(self):
        return self._toString()


##############################################################################
class FfMpegClipFrameStruct(Structure):
    """The frame data returned from the c library."""
    # TODO: This really needs to include the width and height. Buggy to track
    #       that separately here.
    _fields_ = [("dataBuffer", c_void_p),
                ("ms", c_longlong)]



LOGFUNC = CFUNCTYPE(None, c_int, c_char_p)

# Set function argument and return types
_lib.open_clip.argtypes = [c_char_p, c_int, c_int, c_uint64, c_int, c_int, c_int, c_int, c_int, c_int, POINTER(BoxOverlayInfo), c_int, POINTER(BoxOverlayInfo), LOGFUNC]
_lib.open_clip.restype = c_void_p
_lib.get_output_width.argtypes = [c_void_p]
_lib.get_output_width.restype = c_int
_lib.get_output_height.argtypes = [c_void_p]
_lib.get_output_height.restype = c_int
_lib.get_input_width.argtypes = [c_void_p]
_lib.get_input_width.restype = c_int
_lib.get_input_height.argtypes = [c_void_p]
_lib.get_input_height.restype = c_int
_lib.set_mute_audio.argtypes = [c_void_p, c_int]
_lib.set_mute_audio.restype = c_int
_lib.clip_has_audio.argtypes = [c_void_p]
_lib.clip_has_audio.restype = c_int
_lib.get_clip_length.argtypes = [c_void_p]
_lib.get_clip_length.restype = c_longlong
_lib.free_clip_stream.argtypes = [c_void_p]
_lib.get_next_frame.argtypes = [c_void_p]
_lib.get_next_frame.restype = POINTER(FfMpegClipFrameStruct)
_lib.get_prev_frame.argtypes = [c_void_p]
_lib.get_prev_frame.restype = POINTER(FfMpegClipFrameStruct)
_lib.get_next_frame_offset.argtypes = [c_void_p]
_lib.get_next_frame_offset.restype = c_longlong
_lib.get_frame_at.argtypes = [c_void_p, c_longlong]
_lib.get_frame_at.restype = POINTER(FfMpegClipFrameStruct)
_lib.free_clip_frame.argtypes = [POINTER(POINTER(FfMpegClipFrameStruct))]
_lib.get_ms_list.argtypes = [c_void_p]
_lib.get_ms_list.restype = POINTER(c_longlong)
_lib.get_ms_list2.argtypes = [c_char_p, LOGFUNC]
_lib.get_ms_list2.restype = POINTER(c_longlong)
_lib.get_duration.argtypes = [c_char_p, LOGFUNC]
_lib.get_duration.restype = c_int64
_lib.free_ms_list.argtypes = [POINTER(POINTER(c_longlong))]
_lib.set_output_size.argtypes = [c_void_p, c_int, c_int]
_lib.set_output_size.restype = c_int

SetVideoLibDataPath()


##############################################################################
class FfMpegClipFrame(ClipFrame):
    """A class for accessing video frames."""
    ###########################################################
    def __init__(self, structPtr, width, height):
        """FfMpegClipFrame constructor.

        @param  structPtr  A FfMpegClipFrameStruct pointer
        @param  width      The width of the frame.
        @param  height     The height of the frame.
        """
        # Add a freer to the structure so that when there are no more
        # references to it, it will be freed automatically...
        structPtr.__freer = PtrFreer(structPtr, withByref(_lib.free_clip_frame))

        # Save the struct parameters; made public so that convertClipFrameToIpl
        # can add references to it...
        self.structPtr = structPtr

        # This is public...
        self.ms = self.structPtr.contents.ms

        # Make the pointer contents accessible, which is used by
        # ImageConversion...
        self.width = width
        self.height = height
        self.buffer = c_void_p(self.structPtr.contents.dataBuffer)
        self.pilFrame = None
        self.numpyFrame = None
        self.rawBuffer = None
        self.wxBuffer = None

        # Add a direct reference to the structPtr.  That way memory will be
        # alive as long as this pointer is alive...
        self.buffer.__refToStructPtr = structPtr


    ###########################################################
    def asNumpy(self):
        """Return a numpy version of our data.

        @return img  A numpy version of our data.
        """

        if self.numpyFrame is None:
            self.numpyFrame = convertClipFrameToNumpy(self)

        return self.numpyFrame


    ###########################################################
    def asPil(self):
        """Return a PIL version of our data.

        @return img  A PIL version of our data.
        """

        if self.pilFrame is None:
            self.pilFrame = convertClipFrameToPIL(self)

        return self.pilFrame


    ###########################################################
    def asRawBuffer( self ):
        """Return a raw version of our data.

        @return img  A raw version of our data.
        """
        if self.rawBuffer is None:
            self.rawBuffer = convertClipFrameToBuffer( self )

        return self.rawBuffer

    ###########################################################
    def asWxBuffer( self ):
        """Return a raw version of our data.

        @return img  A raw version of our data.
        """
        if self.wxBuffer is None:
            self.wxBuffer = convertClipFrameToWxBitmap( self )

        return self.wxBuffer

##############################################################################
def getDuration(filename, logFn=None):
    """ A light(er)-weight utility to retrieve clip duration
    """
    if logFn is None:
        logFn = LOGFUNC(getStderrLogCB())

    return _lib.get_duration(ensureUtf8(filename), logFn)

##############################################################################
def getMsList(filename, logFn=None):
    """ A light(er)-weight utility to retrieve frame timestamps of a file
    """
    if logFn is None:
        logFn = LOGFUNC(getStderrLogCB())

    msListPtr = _lib.get_ms_list2(ensureUtf8(filename), logFn)
    if msListPtr:
        msList = [msListPtr[i] for i in xrange(1, msListPtr[0]+1)]
        _lib.free_ms_list(byref(msListPtr))
        return msList
    else:
        return []



##############################################################################
class FfMpegClipReader(object):
    """A class for accessing video clips."""
    ###########################################################
    def __init__(self, logFn=None):
        """FfMpegClipReader constructor.

        @param  logFn       Function for logging; should be suitable for
                            passing to ctypes.
        """
        if logFn is None:
            logFn = LOGFUNC(getStderrLogCB())
        self._logFn = logFn

        self._clip = None
        self._mute = False
        self._width = None
        self._height = None


    ###########################################################
    def __del__(self):
        """Free resources used by FfMpegClipReader"""
        self.close()


    ###########################################################
    def open( self, filename, width, height, firstMs, extras ):
        """Open a video clip for reading

        @param  filename  The clip to open
        @param  width     The desired width of retrieved frames
        @param  height    The desired height of retrieved frames
        @return success   True if the clip was opened
        """
        if self._clip:
            self.close()


        boxes = None
        zones = None
        numBoxes = 0
        numZones = 0
        enableAudio = 0
        enableThread = 0
        enableDebug = 0
        enableTimestamp = 0
        keyframeOnly = 0
        if not extras is None:
            boxList = extras.get('boxList', [])
            zonesList = extras.get('zonesList', [])
            numBoxes = len(boxList)
            boxes = (BoxOverlayInfo*numBoxes)()
            numZones = len(zonesList)
            zones = (BoxOverlayInfo*numZones)()
            enableAudio = extras.get('enableAudio', 0)
            enableDebug = extras.get('enableDebug', 0)
            enableTimestamp = getTimestampFlags(extras)
            enableThread = extras.get('asyncRead', 0)
            keyframeOnly = extras.get('keyframeOnly', 0)
            self._mute = extras.get('audioMute', self._mute)


        for i in range(numBoxes):
            ms, text = boxList[i]
            boxes[i] = BoxOverlayInfo(ms, text)

        for i in range(numZones):
            ms, text = zonesList[i]
            zones[i] = BoxOverlayInfo(ms, text)

        try:
            self._clip = c_void_p(_lib.open_clip( ensureUtf8(filename),
                                                  width,
                                                  height,
                                                  firstMs,
                                                  enableAudio,
                                                  enableThread,
                                                  enableDebug,
                                                  enableTimestamp,
                                                  keyframeOnly,
                                                  numBoxes,
                                                  boxes,
                                                  numZones,
                                                  zones,
                                                  self._logFn ))
            if self._clip:
                self._width = _lib.get_output_width(self._clip)
                self._height = _lib.get_output_height(self._clip)
                self.setMute(self._mute)
                return True
        except:
            try:
                fileExists = "exists" if os.path.isfile(filename) else "does not exist"
                self._logFn(kLogLevelCritical, "Exception opening a clip at " + ensureUtf8(filename) + "("+ fileExists + ")" )
                self._logFn(kLogLevelCritical, "w=" + str(width) + " h=" + str(height) + \
                                            " firstMs=" + str(firstMs) + " audio=" + str(enableAudio) + \
                                            " thread=" + str(enableThread) + " debug=" + str(enableDebug) + \
                                            " timestamp=" + str(enableTimestamp) + \
                                            " boxesCount=" + str(numBoxes) + \
                                            " boxes=" + str(boxList) + \
                                            " zonesCount=" + str(numZones) + \
                                            " zones=" + str(zonesList) + \
                                            traceback.format_exc() )
                self._logFn(kLogLevelCritical, "Memory: " + str(getMemoryStats(os.getpid())))
            except:
                self._logFn(kLogLevelCritical, "Critical exception opening a clip at " + ensureUtf8(filename) )



        return False


    ###########################################################
    def close(self):
        """Close the currently opened clip"""
        if self._clip:
            _lib.free_clip_stream(byref(self._clip))

        self._clip = None
        self._width = 0
        self._height = 0


    ###########################################################
    def getInputSize(self):
        """Return the (width, height) of the input file.

        This isn't useful for too much, except to know how we've skewed the
        file to keep its width divisible.

        @return width   The width of the input file.
        @return height  The height of the input file.
        """
        return (_lib.get_input_width(self._clip),
                _lib.get_input_height(self._clip))

    ###########################################################
    def hasAudio(self):
        """Returns True if the clip is valid and has audio track

        @return hasAudio
        """
        if not self._clip:
            return False

        return _lib.clip_has_audio(self._clip) != 0


    ###########################################################
    def getDuration(self):
        """Get the duration of the clip in ms

        @return duration  The duration of the clip in ms
        """
        if not self._clip:
            return -1

        return _lib.get_clip_length(self._clip)


    ###########################################################
    def getMsList(self):
        """Get the ms offset of each frame in the clip

        @return msList  A list of offsets of each frame in the clip
        """
        if not self._clip:
            return []

        msListPtr = _lib.get_ms_list(self._clip)
        if msListPtr:
            msList = [msListPtr[i] for i in xrange(1, msListPtr[0]+1)]
            _lib.free_ms_list(byref(msListPtr))
            return msList
        else:
            return []


    ###########################################################
    def getNextFrame(self):
        """Get the next frame in the current clip

        @return frame  A FfMpegClipFrame of the next frame in the clip or None
                       on error or when all frames have been read
        """
        if not self._clip:
            return None

        result = _lib.get_next_frame(self._clip)
        if not result:
            return None

        clipFrame = FfMpegClipFrame(result, self._width, self._height)

        return clipFrame


    ###########################################################
    def getPrevFrame(self):
        """Get the previous frame in the current clip

        @return frame  A FfMpegClipFrame of the next frame in the clip or None
                       on error or when all frames have been read
        """
        if not self._clip:
            return None

        result = _lib.get_prev_frame(self._clip)
        if not result:
            return None

        clipFrame = FfMpegClipFrame(result, self._width, self._height)

        return clipFrame


    ###########################################################
    def setMute(self, mute):
        """Set the status of audio playback
        """
        self._mute = mute

        if not self._clip:
            return -1

        value = 1 if mute else 0
        return _lib.set_mute_audio(self._clip, value)

    ###########################################################
    def getNextFrameOffset(self):
        """Get the offset in ms of the next frame in the current clip.

        @return ms  The ms offset of the next frame, or -1 if no more.
        """
        if not self._clip:
            return -1

        return _lib.get_next_frame_offset(self._clip)


    ###########################################################
    def seek(self, msOffset):
        """Move the sequential file pointer to retrieve a frame at some time.

        @param  msoffset  The millisecond offset of the desired frame
        @return frame     A ClipFrame of the requested frame, None on error
        """
        return self.getFrameAt(msOffset)


    ###########################################################
    def getFrameAt(self, msOffset):
        """Retrieve the frame in the current clip closest to the given offset

        IMPORTANT NOTE: You can't mix this with getNextFrame(), since this
        function will leave the "next frame" in the file as some semi-arbitrary
        location.

        @param  msoffset  The millisecond offset of the desired frame
        @return frame     A FfMpegClipFrame of the requested frame, None on error
        """
        if not self._clip:
            return None

        result = _lib.get_frame_at(self._clip, int(msOffset))
        if not result:
            return None

        return FfMpegClipFrame(result, self._width, self._height)


    ###########################################################
    def setOutputSize(self, resolution):
        """Set the size retrieved frames shoudl be.

        @param  resolution  The resolution new frames should be returned at."""
        _lib.set_output_size(self._clip, resolution[0], resolution[1])
        self._width = _lib.get_output_width(self._clip)
        self._height = _lib.get_output_height(self._clip)

        return self._width, self._height

