#! /usr/local/bin/python

#*****************************************************************************
#
# StreamReader.py
#   Python bindings integration with live video stream
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
Contains an interface to the c videolib module.
"""


from ctypes import CFUNCTYPE, POINTER, Structure, c_int, c_float, c_void_p
from ctypes import c_longlong, c_char_p, string_at, byref
import ConfigParser
import os
import shutil
import sys
import urlparse
import traceback
import threading
import time

from vitaToolbox.loggingUtils.LoggingUtils import kLogLevelError, kLogLevelInfo
from vitaToolbox.loggingUtils.LoggingUtils import getStderrLogCB
from vitaToolbox.ctypesUtils.PtrFreer import PtrFreer, withByref
from vitaToolbox.networking.SanitizeUrl import processUrlAuth
from vitaToolbox.path.PathUtils import abspathU
from vitaToolbox.strUtils.EnsureUnicode import ensureUtf8, ensureUnicode
from vitaToolbox.strUtils.EnsureUnicode import simplifyString
from vitaToolbox.ctypesUtils.LoadLibrary import LoadLibrary
from vitaToolbox.profiling.StatItem import StatItem
from vitaToolbox.profiling.MarkTime import TimerLogger

from videoLib2.python.VideoLibUtils import SetVideoLibDataPath

from backEnd.BackEndPrefs import kLiveEnableFastStart, kLiveEnableFastStartDefault, kRecordInMemory, kRecordInMemoryDefault, kHardwareAccelerationDevice


_libName = 'videolib'

_videolib = LoadLibrary(None, _libName)

kCodecConfigFilename = "output_config"
kCodecDefaults = {
            'bit_rate_multiplier':0,
            'max_bit_rate':0,
            'gop_size':42,
            'keyint_min':10,
            'preset':'ultrafast',
            'max_width':0,
            'max_height':0,
            'sv_profile':-1
}
kSectionVideo = "video"

kPacketCaptureErrCodes = {
    -1:"Insufficient priveledges!!",
    -2:"Driver is not installed or inactive!!",
    -3:"Capture is already enabled!!",
}


##############################################################################
class DimensionsStruct(Structure):
    """The frame data returned by the c library."""
    _fields_ = [("width", c_int),
                ("height", c_int)]


##############################################################################
class StreamFrameStruct(Structure):
    """The frame data returned by the c library."""
    _fields_ = [("procBuffer", c_void_p),
                ("procWidth", c_int),
                ("procHeight", c_int),
                ("wasResized", c_int),
                ("ms", c_longlong),
                ("filename", c_char_p),
                ("isRunning", c_int)]


##############################################################################
class CodecConfig(Structure):
    """The codec configuration passed to the c library."""
    _fields_ = [("bit_rate_multiplier", c_float),
                ("max_bit_rate", c_int),
                ("gop_size", c_int),
                ("keyint_min", c_int),
                ("preset", c_char_p),
                ("max_width", c_int),
                ("max_height", c_int),
                # -1=Not specified, 0=Original, 10=High, 20=Medium, 30=Low
                ("sv_profile", c_int)]



LOGFUNC = CFUNCTYPE(None, c_int, c_char_p)

# Set function argument and return types
_videolib.enable_packet_capture.argtypes = [c_char_p, c_char_p, LOGFUNC]
_videolib.enable_packet_capture.restype = c_int
_videolib.open_stream.argtypes = [c_char_p, c_char_p, c_int, c_int, c_int,
                                  c_char_p, POINTER(CodecConfig),
                                  c_int, c_int, LOGFUNC]
_videolib.open_stream.restype = c_void_p
_videolib.free_stream_data.argtypes = [c_void_p]
_videolib.flush_output.argtypes = [c_void_p]
_videolib.get_new_frame.argtypes = [c_void_p, c_int]
_videolib.get_new_frame.restype = POINTER(StreamFrameStruct)
_videolib.get_large_frame.argtypes = [c_void_p]
_videolib.get_large_frame.restype = POINTER(StreamFrameStruct)
_videolib.free_frame_data.argtypes = [POINTER(POINTER(StreamFrameStruct))]
_videolib.get_local_camera_count.argtypes = [LOGFUNC]
_videolib.get_local_camera_count.restype = c_int
_videolib.get_local_camera_name.argtypes = [c_int]
_videolib.get_local_camera_name.restype = c_char_p
_videolib.get_local_camera_count_without_resolution_cache.argtypes = [LOGFUNC]
_videolib.get_local_camera_count_without_resolution_cache.restype = c_int
_videolib.get_number_of_supported_resolutions_of_local_camera.argtypes = [c_int]
_videolib.get_number_of_supported_resolutions_of_local_camera.restype = c_int
_videolib.get_supported_resolution_pair_of_device.argtypes = [c_int, c_int]
_videolib.get_supported_resolution_pair_of_device.restype = POINTER(DimensionsStruct)
_videolib.get_fps_info.argtypes = [c_void_p, POINTER(c_float), POINTER(c_float)]
_videolib.get_proc_width.argtypes = [c_void_p]
_videolib.get_proc_width.restype = c_int
_videolib.get_proc_height.argtypes = [c_void_p]
_videolib.get_proc_height.restype = c_int
_videolib.open_mmap.argtypes = [c_void_p, c_char_p]
_videolib.open_mmap.restype = c_int
_videolib.is_running.argtypes = [c_void_p]
_videolib.is_running.restype = c_int
_videolib.close_mmap.argtypes = [c_void_p]
_videolib.set_mmap_params.argtypes = [c_void_p, c_int, c_int, c_int, c_int]
_videolib.set_audio_volume.argtypes = [c_void_p, c_int]
_videolib.set_live_stream_limits.argtypes = [c_void_p, c_int, c_int]
_videolib.set_live_stream_limits.restype = c_int
_videolib.enable_live_stream.argtypes = [c_void_p, c_int, c_char_p, c_int, c_longlong]
_videolib.enable_live_stream.restype = c_int
_videolib.disable_live_stream.argtypes = [c_void_p, c_int]
_videolib.get_newest_frame_as_jpeg.argtypes = [c_void_p, c_int, c_int,
                                               POINTER(c_int)]
_videolib.get_newest_frame_as_jpeg.restype = c_void_p
_videolib.free_newest_frame.argtypes = [c_void_p]
_videolib.get_initial_frame_buffer_size.argtypes = [c_void_p]
_videolib.get_initial_frame_buffer_size.restype = c_int
_videolib.move_recorded_file.argtypes = [LOGFUNC, c_char_p, c_char_p]
_videolib.move_recorded_file.restype = c_int
_videolib.videolib_get_hw_devices.argtypes = [ POINTER(c_char_p), c_int ]
_videolib.videolib_get_hw_devices.restype = c_int
_videolib.videolib_set_hw_device.argtypes = [ c_char_p ]
_videolib.videolib_set_hw_device.restype = c_int



_k_oifWantTCP              = 0x0001
_k_oifShouldRecord         = 0x0002
_k_oifLiveStream           = 0x0004
_k_oifEnableTimestamp      = 0x0008
_k_oifEdgeThread           = 0x0010
_k_oifRenderAudio          = 0x0020
_k_oifDebugClips           = 0x0040
_k_oifLimitFps             = 0x0080
_k_oifDisableAudio         = 0x0100
_k_oifShowRegions          = 0x0200
_k_oifFastStart            = 0x0400
_k_oifRecordInMemory       = 0x0800
_k_oifSimulation           = 0x1000

SetVideoLibDataPath()


##############################################################################
class StreamFrame(object):
    """A class for accessing video frames."""
    ###########################################################
    def __init__(self, structPtr):
        """StreamFrame constructor.

        @param  structPtr  A StreamFrameStruct pointer
        """
        # Add a freer to the structure so that when there are no more
        # references to it, it will be freed automatically...
        structPtr.__freer = PtrFreer(structPtr,
                                     withByref(_videolib.free_frame_data))

        # Save the struct parameters; made public so that convertClipFrameToIpl
        # can add references to it...
        self.structPtr = structPtr

        # Make the pointer contents accessible
        self.buffer = c_void_p(self.structPtr.contents.procBuffer)
        self.width = self.structPtr.contents.procWidth
        self.height = self.structPtr.contents.procHeight
        self.wasResized = self.structPtr.contents.wasResized > 0
        self.ms = self.structPtr.contents.ms
        self.filename = self.structPtr.contents.filename
        self.dummy = self.structPtr.contents.procBuffer is None

        # Add a direct reference to the structPtr.  That way memory will be
        # alive as long as this pointer is alive...
        self.buffer.__refToStructPtr = structPtr

    ###########################################################
    """ Procure non-resized frame which this object is based on, if available
    """
    def getLargeFrame(self):
        result = _videolib.get_large_frame(self.structPtr)
        if result:
            return StreamFrame(result)
        return None

##############################################################################
class StreamReader(object):
    """A class for accessing video streams."""
    ###########################################################
    def __init__(self, name='', clipManager=None, clipManagerLock=None, recordDir=u'.',
                 storageDir=u'.', configDir=u'.', logFn=None, record=True,
                 moveFailedCallback=None,
                 initFrameBufferSize=0,
                 statsInterval=0):
        """StreamReader constructor.

        @param  name                A name for the stream being recorded
        @param  clipManager         An optional ClipManager for tracking files
        @param  recordDir           Directory to record to.
        @param  storageDir          Directory to store to.
        @param  configDir           Directory to search for config files.
        @param  logFn               Function for logging; should be suitable for
                                    passing to ctypes.
        @param  record              If False, video will not be recorded.
        @param  moveFailedCallback  A callback function that will be called when
                                    a file can't be successfully moved.  The
                                    callback takes one parameter, the target
                                    relative path of the file.
        @param initFrameBufferSize  Storage size to use for this camera, or 0
                                    if unknown
        """
        if logFn is None:
            logFn = LOGFUNC(getStderrLogCB())

        # Save parameters
        self.locationName = name
        self._clipManager = clipManager
        self._record=record


        recordDir = ensureUnicode(recordDir)
        storageDir = ensureUnicode(storageDir)

        self._recordDir = abspathU(os.path.join(recordDir, name))+os.sep
        self._storageDir = abspathU(storageDir)+os.sep
        self._configDir = abspathU(configDir)
        self._recordInMemory = False

        self._logFn = logFn
        self._initFrameBufferSize = initFrameBufferSize

        self._moveFailedFn = moveFailedCallback
        self._cachedProcSize = (0,0)

        self._statsInterval = statsInterval
        self._lastStatsTime = time.time()
        self._timeToMoveClipStat = StatItem("timeToMoveClip", None, "%.1f", "%.1f")
        self._timeToMoveClipStat.setLimit((0,0.2))
        self._timeToAddClipStat  = StatItem("timeToAddClip", None, "%.1f", "%.1f")
        self._timeToAddClipStat.setLimit((0,0.2))
        self._timeToGetFrameStat = StatItem("timeToGetFrame", None, "%.1f", "%.1f")
        self._timeToGetFrameStat.setLimit((0,0.2))

        self._moverThread = None

        try:
            os.makedirs(self._recordDir)
        except Exception:
            pass

        self._initVariables()


    ###########################################################
    def __del__(self):
        """Free resources used by StreamReader"""
        self.close()


    ###########################################################
    def _initVariables(self):
        """Set variables to their initial state"""
        self._stream = None
        self.isRunning = False
        self._prevFilename = ''
        self._prevDirPath = ''
        self._curFilePath = ''
        self._curFilename = ''
        self._curDirPath = ''
        self._curStartMs = 0
        self._curLastMs = 0
        self._isMmapLargeView = False
        self._mmapViewWidth = 0
        self._mmapViewHeight = 0
        self._mmapViewFps = 0


    ###########################################################
    @staticmethod
    def _avoidBlankPasswordBug(url):
        """Avoid an FFMPEG bug related to blank passwords.

        Our current verison of FFMPEG (as of 2/25/2011) has a bug in it related
        to blank passwords.  It will never try to auth properly in that case.
        Rather than patch FFMPEG (which is a hassle), we'll detect this case
        and work around it.  FFMPEG is happy as long as there is a ":" in the
        string indicating that a blank password is really desired.  We'll add
        that.

        # We should make sure that these URLs don't fail...  We shouldnt' be
        # touching these URLs.
        >>> StreamReader._avoidBlankPasswordBug(u'device:9999:My w@cky web:cam')
        u'device:9999:My w@cky web:cam'
        >>> StreamReader._avoidBlankPasswordBug(u'http://www.yahoo.com/')
        u'http://www.yahoo.com/'
        >>> StreamReader._avoidBlankPasswordBug(u'http://myuser:mypass@www.yahoo.com/')
        u'http://myuser:mypass@www.yahoo.com/'
        >>> StreamReader._avoidBlankPasswordBug(u'http://myuser:@www.yahoo.com/')
        u'http://myuser:@www.yahoo.com/'
        >>> StreamReader._avoidBlankPasswordBug(u'completely@Bogus')
        u'completely@Bogus'

        # We purposely don't touch this one, mostly because I haven't thought
        # it through.  I'm pretty sure that a blank username/password is not
        # a valid case.  I'm about 99.9% sure that our camera wizard won't
        # let the user do this, anyway...
        >>> StreamReader._avoidBlankPasswordBug(u'http://@www.yahoo.com/')
        u'http://@www.yahoo.com/'

        # This is the one we care about...
        >>> StreamReader._avoidBlankPasswordBug(u'http://myuser@www.yahoo.com/')
        u'http://myuser:@www.yahoo.com/'
        >>> StreamReader._avoidBlankPasswordBug(u'HTTP://myuser@www.yahoo.com/')
        u'http://myuser:@www.yahoo.com/'

        # Let's not change rtsp.  We haven't heard any reports of problems, and
        # the RTSP code is very different than the HTTP code in FFMPEG.
        >>> StreamReader._avoidBlankPasswordBug(u'rtsp://www.yahoo.com/')
        u'rtsp://www.yahoo.com/'
        >>> StreamReader._avoidBlankPasswordBug(u'rtsp://myuser:mypass@www.yahoo.com/')
        u'rtsp://myuser:mypass@www.yahoo.com/'
        >>> StreamReader._avoidBlankPasswordBug(u'rtsp://myuser:@www.yahoo.com/')
        u'rtsp://myuser:@www.yahoo.com/'
        >>> StreamReader._avoidBlankPasswordBug(u'rtsp://myuser@www.yahoo.com/')
        u'rtsp://myuser@www.yahoo.com/'

        @param  url  The stream url to look at.  Could be ASCII str or unicode.
        @return url  The fixed up stream url.  Will be of the same type as
                     passed in url (str or unicode).
        """
        # We won't ever do anything to "device:" URLs.  NOTE: In actuality,
        # we don't need this check (the code below will be benign for all
        # valid "device:" URLs that I can think of), but it seems wise since
        # I'm not sure the behavior for urlsplit() is guaranteed for unknown URL
        # schemes.
        if not url.startswith("device:"):
            # Split the URL using normal mechanisms...
            scheme, netloc, path, query, fragment = urlparse.urlsplit(url)

            # We only do our magic if HTTP and there is auth info...
            if (scheme in ('http', 'https')) and ('@' in netloc):
                # Split on the first '@'.  Note that if the user/password have
                # '@' characters in them, they should have been escaped out.
                authPart, hostPart = netloc.split('@', 1)

                # If there is no password (signified by no ':'), add a ':' to
                # the auth part let FFMPEG know it should use a blank password.
                # ...then, recreate the full location.
                if authPart and (':' not in authPart):
                    netloc = '%s:@%s' % (authPart, hostPart)
                    url = urlparse.urlunsplit((scheme, netloc, path,
                                               query, fragment))

        return url


    ###########################################################
    def enablePacketCapture(self, captureLocation, cameraUri, logFn=None):
        """Enables packet capture

        @param   captureLocation    Directory for saving the packets.
        @param   cameraUri          Unsanitized camera URI.

        @return  result             0 on success,
                                    -1 on insufficient priveledges,
                                    -2 if driver isn't installed or inactive,
                                    -3 if the capture is already enabled.
        """
        if logFn is None:
            logFn = LOGFUNC(getStderrLogCB())

        return _videolib.enable_packet_capture(
                captureLocation, cameraUri, logFn
        )


    ###########################################################
    def open(self, path, extras={}):
        """Open a video stream for reading

        NOTE: Currently a 320x240 image will always be generated as well in
              addition to one at the requested resolution.

        @param  path     Path to the stream to open
        @param  extras   A dictionary containing optional configuration values
        @return success  True if the stream was opened
        """
        if self._stream:
            self.close()

        flags = 0

        decoderDevice = extras.get(kHardwareAccelerationDevice,'')
        if len(decoderDevice):
            _videolib.videolib_set_hw_device(ensureUtf8(decoderDevice))

        if self._record:
            flags |= _k_oifShouldRecord

        width, height = extras.get('recordSize', (320, 240))
        fps = extras.get('fpsLimit', 10)

        if extras.get('forceTCP', False):
            flags |= _k_oifWantTCP
        if extras.get('recordAudio', True) == False:
            flags |= _k_oifDisableAudio
        if extras.get(kLiveEnableFastStart, kLiveEnableFastStartDefault):
            flags |= _k_oifFastStart
        if extras.get('simulation', 0) > 0:
            flags |= _k_oifSimulation
        self._recordInMemory = extras.get(kRecordInMemory, kRecordInMemoryDefault)
        if self._recordInMemory:
            flags |= _k_oifRecordInMemory


        path = self._avoidBlankPasswordBug(path)
        path, sanitizedPath = processUrlAuth(path)

        if extras.get('addTimestamps', False):
            flags |= _k_oifEnableTimestamp

        self._stream = c_void_p(_videolib.open_stream(ensureUtf8(path),
            ensureUtf8(sanitizedPath), width, height, fps,
            ensureUtf8(self._recordDir),
            getCodecConfig(self._configDir),
            self._initFrameBufferSize, flags, self._logFn))
        if self._stream:
            self.isRunning = True
            _videolib.set_mmap_params(self._stream, self._isMmapLargeView,
                self._mmapViewWidth, self._mmapViewHeight, self._mmapViewFps)

        return self.isRunning


    ###########################################################
    def getProcSize(self):
        """Return the processing size being used for the current stream.

        Note: This will return (0, 0) if the stream is not open or not running.

        @return  procSize  Processing size as a 2-tuple, (width, height)
        """
        if self.isRunning and self._stream:
            self._cachedProcSize = (
                _videolib.get_proc_width(self._stream),
                _videolib.get_proc_height(self._stream)
            )

        return self._cachedProcSize


    ###########################################################
    def close(self, killableCallback=None):
        """Close the currently opened clip

        @param  killableCallback  A callback function that will be called
                                  after the video has been flushed and it is
                                  'safe' to kill this process.
        """

        # Ensure that we always complete the current file and close it (else
        # we can't move it).  Something could hang in free_stream_data.  If
        # we're forced to terminate we don't want to lose the current video.
        self.flush()

        # We're no longer running; need to do this after the flush, else the
        # flush won't do anything...
        self.isRunning = False

        if self._stream:
            _videolib.free_stream_data(byref(self._stream))

        # Add the file to the database...
        if self._curFilename:
            self._addFileToDb()

        self._terminateMoverThread()

        if killableCallback:
            killableCallback()

        self._initVariables()


    ###########################################################
    class ThreadedFileMover(threading.Thread):
        ''' Internal class for offloading large(ish) files copying to a separate
            thread, so to not delay incoming stream processing.
        '''
        def __init__(self, owner):
            threading.Thread.__init__(self)
            self._owner = owner

            # copy everything accessed in run() from the owner

            # these things may change while the thread is executing, so we must keep a copy
            self._curFilename = owner._curFilename
            self._storageDir  = owner._storageDir
            self._curDirPath  = owner._curDirPath
            self._recordDir   = owner._recordDir
            self._prevDirPath = owner._prevDirPath
            self._prevFilename= owner._prevFilename
            self._procSize    = owner.getProcSize()
            self._curStartMs  = owner._curStartMs
            self._curLastMs   = owner._curLastMs
            self._recordInMemory = owner._recordInMemory

            # these things should remain the same in the owner, but we'll still keep a pointer
            self._clipManager = owner._clipManager
            self._logFn       = owner._logFn
            self._moveFailedFn= owner._moveFailedFn
            self.locationName = owner.locationName
            self._timeToMoveClipStat = owner._timeToMoveClipStat
            self._timeToAddClipStat  = owner._timeToAddClipStat



        ###########################################################
        def run(self):
            if not self._curFilename or not self._clipManager:
                return

            timer = TimerLogger("moveFile")
            targetFolder = os.path.join(self._storageDir, self._curDirPath)
            try:
                if not os.path.isdir(targetFolder):
                    os.makedirs(targetFolder)
            except Exception, e:
                self._logFn(kLogLevelError, "Failed to create folder " + ensureUtf8(targetFolder) + ": " + str(e))

            src = os.path.join(self._recordDir, self._curFilename)
            dst = os.path.join(targetFolder, self._curFilename.lower())
            try:
                if self._recordInMemory:
                    if _videolib.move_recorded_file(self._logFn, src, dst) < 0:
                        raise Exception("Failed to move file!")
                else:
                    # Even if we've failed to create a folder, we try to move the file,
                    # so a `moveFailed' callback is triggered
                    shutil.move(src, dst)
            except Exception, e:
                self._logFn(kLogLevelError, "Failed to move file (" + ensureUtf8(src) + "->" + ensureUtf8(dst) + "): " + str(e))
                self._moveFailedFn(os.path.join(self._curDirPath,
                                                self._curFilename))
            self._timeToMoveClipStat.report(timer.diff_sec())

        ###########################################################
        def addFileToDb(self):
            timer = TimerLogger("addFile")
            prevFile = ''
            if self._prevFilename:
                prevFile = os.path.join(self._prevDirPath, self._prevFilename)

            size = self._procSize
            self._clipManager.addClip(os.path.join(self._curDirPath,
                                                self._curFilename.lower()),
                                    self.locationName, self._curStartMs,
                                    self._curLastMs, prevFile, "", 1,
                                    size[0],
                                    size[1])
            self._timeToAddClipStat.report(timer.diff_sec())

    ###########################################################
    def _terminateMoverThread(self):
        self._finalizeMoverThread(True)

    ###########################################################
    def _finalizeMoverThread(self, blockIfRunning):
        if self._moverThread is not None:
            if self._moverThread.isAlive():
                if not blockIfRunning:
                    # The thread hasn't finished copying the file yet; let it run
                    return

                # Definitely not normal, we expect 2 min files, and copying should not take this long
                self._logFn(kLogLevelInfo, "Blocking copy operation, since previous one is outstanding ...")
                self._moverThread.join()
                self._logFn(kLogLevelInfo, "... done")
            self._moverThread.addFileToDb()
            self._moverThread = None

    ###########################################################
    def _addFileToDb(self):
        """Move a clip to a perm location and add it to the database."""
        if not self._curFilename or not self._clipManager:
            return
        # ensure a previously started mover thread completes, and the new file
        # is added to database, before commencing
        self._terminateMoverThread()
        self._moverThread = self.ThreadedFileMover(self)
        self._moverThread.start()

    ###########################################################
    def getNewFrame(self, isLive=False):
        """Get and write the most recently obtained frame from a stream

        @param  isLive True if the frame should be copied to the memory map.
        @return frame  A StreamFrame or None if no new frame has been read
                       since the previous getNewFrame call or on error. To
                       determine which of the latter is true, check isRunning.
        """
        if not self._stream or not self.isRunning:
            return None

        timer = TimerLogger("getFrame")
        result = _videolib.get_new_frame(self._stream, isLive)
        if not result:
            isRunning = _videolib.is_running(self._stream)
        else:
            isRunning = result.contents.isRunning
        self._timeToGetFrameStat.report(timer.diff_sec())

        if isRunning == 0:
            # If the stream just stopped unexpectedly, close ourselves (which
            # will properly flush and add the file to the database), then return
            # None.
            self.close()
            if result:
                _videolib.free_frame_data(byref(result))

        if not result:
            return None

        if self._clipManager and result.contents.filename != self._curFilePath:
            self._curFilePath = result.contents.filename
            self._addFileToDb()
            self._prevFilename = self._curFilename
            self._prevDirPath = self._curDirPath
            self._curFilename = os.path.split(self._curFilePath)[1]
            self._curDirPath = os.path.join(self.locationName,
                                            self._curFilename[:5]).lower()
            self._curStartMs = result.contents.ms

        self._curLastMs = result.contents.ms

        # check if the mover thread had finished, and finalize/add to db if so
        self._finalizeMoverThread(False)

        if self._statsInterval > 0 and \
           self._lastStatsTime + self._statsInterval <= time.time():
           self._lastStatsTime = time.time()
           self._logFn(kLogLevelInfo, "TimingStats: %s %s %s" % (
                       self._timeToGetFrameStat.reset(),
                       self._timeToMoveClipStat.reset(),
                       self._timeToAddClipStat.reset() ) )

        return StreamFrame(result)

    ###########################################################
    def getInitialFrameBufferSize(self):
        """Get the maximum buffer required to accommodate a compressed frame seen so far on this stream
        @return size  Integer value, or 0 if not supported
        """
        if not self._stream or not self.isRunning:
            return 0

        return _videolib.get_initial_frame_buffer_size(self._stream)


    ###########################################################
    def getFpsInfo(self):
        """Get info on the frames per second of the stream.

        @return requestFps  The average # of times per second that getNewFrame()
                            has been called.  Note: this can be higher than
                            captureFps, since getNewFrame() effectively polls
                            for a new frame.
        @return captureFps  The average # of times per second that a new frame
                            came in from the camera.
        """
        requestFps = c_float()
        captureFps = c_float()

        _videolib.get_fps_info(self._stream,
                               byref(requestFps), byref(captureFps))

        return requestFps.value, captureFps.value


    ###########################################################
    def flush(self, msNeeded=None):
        """Ensure all data written to this point is readable.

        @param  msNeeded  If non-None, we'll only flush if the given msNeeded
                          is greater than the start of the clip being currently
                          recorded.
        """
        if self._stream and self.isRunning and \
           ((msNeeded is None) or (msNeeded >= self._curStartMs)):

            _videolib.flush_output(self._stream)


    ###########################################################
    def open_mmap(self, filename):
        """Open a memory map for sharing live frames with other processes.

        @param  filename  The filename for the memory map.  On windows this
                          is simply a shared memory name.
        """
        if self._stream:
            return bool(_videolib.open_mmap(self._stream,
                                            ensureUtf8(filename)))
        return False


    ###########################################################
    def close_mmap(self):
        """Close a previously opened memory map."""
        if self._stream:
            _videolib.close_mmap(self._stream)


    ###########################################################
    def setMmapParams(self, isLargeView=True, width=320, height=240, fps=0):
        """Enable sharing of higher resolution frames.

        @param  isLargeView  True to enable large frames.
        @param  width        Width of the large view.
        @param  height       Height of the large view.
        @param  fps          Desired frame rate.
        """
        self._isMmapLargeView = 1 if isLargeView else 0
        self._mmapViewWidth = width
        self._mmapViewHeight = height
        self._mmapViewFps = fps
        if self._stream:
            _videolib.set_mmap_params(self._stream,
                    self._isMmapLargeView, width,
                    height, fps)

    ###########################################################
    def setAudioVolume(self, audioVolume):
        """Enable sharing of higher resolution frames.

        @param  audioVolume                 volume in 0..100 range
        """
        if self._stream:
            _videolib.set_audio_volume(self._stream, audioVolume)

    ###########################################################
    def setLiveStreamLimits(self, maxRes, maxBitrate):
        if self._stream:
            return _videolib.set_live_stream_limits(self._stream, maxRes, maxBitrate)
        return -1

    ###########################################################
    def enableLiveStream(self, profileId, path, tsOption, startIndex):
        if self._stream:
            return _videolib.enable_live_stream(self._stream, profileId, ensureUtf8(path),
                        tsOption, startIndex)
        return -1

    ###########################################################
    def disableLiveStream(self, profileId):
        if self._stream:
            _videolib.disable_live_stream(self._stream, profileId)

    ###########################################################
    def getNewestFrameAsJpeg(self, width, height):
        """Gets the very latest frame as a JPEG

        @param width Width the JPEG should have.
        @param height Height the JPEG should have.
        @return The JPEG data or None if no data is available.
        """
        sizeOfJpeg = c_int()
        jpeg = _videolib.get_newest_frame_as_jpeg(self._stream, width, height,
                                                  byref(sizeOfJpeg))
        if jpeg is None:
            return None
        result = string_at(jpeg, sizeOfJpeg.value)
        _videolib.free_newest_frame(jpeg)
        return result

###########################################################
def getHardwareDevicesList(logFn = None):
    maxLen = 32
    arr = (c_char_p * maxLen)()
    count = _videolib.videolib_get_hw_devices(arr, maxLen)
    res = []
    if count > 0:
        for i in range(count):
            res.append(arr[i])
    return res



###########################################################
def getLocalCameraNames(logFn = None):
    """Returns a list of 2-tuples with the names of the system's local cameras,
        and their associated supported resolutions.

        @param  logFn        Function for logging; should be suitable for
                             passing to ctypes.

        @return cameraNames  A list where the first item in the tuple is the name,
                             and the second item is a list of supported resolutions
                             for that particular camera. Names are as *unicode* strings.
                             The index into the list is the device ID
                             of the camera.

    Note that the cameras may or may not be in use by other programs.
    """
    if logFn is None:
        logFn = LOGFUNC(getStderrLogCB())

    cameraNames = []

    try:
        n = _videolib.get_local_camera_count(logFn)
    except:
        logFn(kLogLevelError, "Failed to get number of local cameras. Going "
                              "to try to get just the names, without caching "
                              "resolution list. Exception info: "
                              "%s" % traceback.format_exc())
        try:
            n = _videolib.get_local_camera_count_without_resolution_cache(logFn)
        except:
            logFn(kLogLevelError, "Failed to get number of local cameras, even "
                                  "without caching the resolution list. "
                                  "Exception info: %s" % traceback.format_exc())
            return cameraNames

    for i in xrange(0, n):

        try:
            name = ensureUnicode(str(_videolib.get_local_camera_name(i)))
        except:
            logFn(kLogLevelError, "Failed to get local camera name.")
            name = "Unknown Device %s" % str(i)

        try:
            numResPairs = _videolib.get_number_of_supported_resolutions_of_local_camera(i)
        except:
            logFn(kLogLevelError, "Failed to get number of supported resolutions "
                                  "for this device: %s. Exception info: "
                                  "%s" % (name, traceback.format_exc()))
            numResPairs = 0

        resPairs = []

        for j in xrange(0, numResPairs):

            try:
                resPair = _videolib.get_supported_resolution_pair_of_device(i, j)
            except:
                logFn(kLogLevelError, "Failed to get the resolution pair for "
                                      "this device: %s. Exception info: "
                                      "%s" % (name, traceback.format_exc()))
                resPairs = []
                break

            try:
                if (resPair.contents.width >= 320) and (resPair.contents.height >= 240):
                    resPairs.append( (resPair.contents.width, resPair.contents.height) )
            except:
                logFn(kLogLevelError, "Failed to access resolution pair values "
                                      "in c-struct for this device: %s. "
                                      "Exception info: "
                                      "%s" % (name, traceback.format_exc()))
                resPairs = []
                break

        resPairs = sorted(list(set(resPairs)))
        cameraNames.append( (name, resPairs) )

    return cameraNames


###########################################################
def getCodecConfig(configDir):
    """Returns the codec configuration data to use for video creation

        @return codecConfig  A CodecConfig object.
    """
    codecConfig = CodecConfig()
    parser = ConfigParser.RawConfigParser(kCodecDefaults)
    parser.read(os.path.join(configDir, kCodecConfigFilename))

    if not parser.has_section(kSectionVideo):
        parser.add_section(kSectionVideo)

    codecConfig.bit_rate_multiplier = parser.getfloat(kSectionVideo, "bit_rate_multiplier")
    codecConfig.max_bit_rate = parser.getint(kSectionVideo, "max_bit_rate")
    codecConfig.gop_size = parser.getint(kSectionVideo, "gop_size")
    codecConfig.keyint_min = parser.getint(kSectionVideo, "keyint_min")
    codecConfig.preset = parser.get(kSectionVideo, "preset")
    codecConfig.max_width = parser.get(kSectionVideo, "max_width")
    codecConfig.max_height= parser.get(kSectionVideo, "max_height")
    codecConfig.sv_profile= parser.get(kSectionVideo, "sv_profile")

    return codecConfig


##############################################################################
def test_main():
    """Contains various self-test code."""
    import doctest
    doctest.testmod(verbose=True)


##############################################################################
if __name__ == '__main__':
    if len(sys.argv) > 1 and sys.argv[1] == "test":
        test_main()
    else:
        print "Try calling with 'test' as the argument."
