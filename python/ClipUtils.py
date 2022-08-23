#! /usr/local/bin/python

#*****************************************************************************
#
# ClipUtils.py
#   Python bindings for various clip information and alteration utilities
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
Contains an interface to clip manipulation functions from the videolib module.
"""


from ctypes import CFUNCTYPE, POINTER, pointer, cdll, Structure, c_int
from ctypes import c_char_p, c_uint64, c_longlong
from ctypes.util import find_library
import os
import sys

from videoLib2.python.StreamReader import CodecConfig, getCodecConfig
from videoLib2.python.VideoLibUtils import SetVideoLibDataPath, getTimestampFlags

from vitaToolbox.ctypesUtils.LoadLibrary import LoadLibrary

from backEnd.BackEndPrefs import kClipQualityProfile

_libName = 'videolib'

_videolib = LoadLibrary(None, _libName)

_kCreateClipError = c_uint64(-1).value

##############################################################################
class BoxOverlayInfo(Structure):
    """Information describing a single box overlay"""
    _fields_ = [("ms", c_longlong),
                ("drawboxParams", c_char_p)]



LOGFUNC = CFUNCTYPE(None, c_int, c_char_p)
PROGFUNC = CFUNCTYPE(c_int, c_int)

# Set function argument and return types
_videolib.create_clip.argtypes = [c_int, POINTER(c_char_p), POINTER(c_uint64),
        c_uint64, c_uint64, c_char_p, POINTER(CodecConfig),
        c_int, c_int, POINTER(BoxOverlayInfo), c_char_p, c_int, LOGFUNC, PROGFUNC]
_videolib.create_clip.restype = c_uint64
_videolib.preserve_aspect_ratio.argtypes = [c_int, c_int, POINTER(c_int),
        POINTER(c_int), c_int]
_videolib.preserve_aspect_ratio.restype = None

_videolib.fast_create_clip.argtypes = [c_int, POINTER(c_char_p), POINTER(c_uint64),
        c_uint64, c_uint64, c_char_p, c_char_p, LOGFUNC, PROGFUNC]
_videolib.fast_create_clip.restype = c_uint64

SetVideoLibDataPath()

###########################################################
def defaultProggressCb(pct):
    return 0

###########################################################
def remuxClip(origFiles, newFile, firstMs, lastMs, configDir, extras,
        logFn=None, progFn=None):
    """Create a clip from one or more existing video files by remuxing.

    @param  origFiles  A list of (filename, offset) where filename is the path
                       to the original file and offset is the time in ms
                       between the last frame of the previous file.
    @param  newFile    The path where the resulting file should be created.
    @param  firstMs    The offset into the first file to start copying.
    @param  lastMs     The offset into the last file at which to stop copying.
    @param  configDir  Unused
    @param  extras     Unused.
    @param  logFn      Function for logging; should be suitable for
                       passing to ctypes.
    @param  progFn     Function for reporting progress/aborting the call
    @return success    Actual offset of the clip, or -1 for error
    """
    if logFn is None:
        logFn = LOGFUNC(lambda lvl, s: sys.stderr.write("%d: %s" % (lvl,s)))
    if progFn is None:
        progFn = PROGFUNC(defaultProggressCb)

    maxSize = extras.get('maxSize', None)
    if len(extras.get('boxList', [])) > 0 \
       or extras.get('enableTimestamps', False) \
       or extras.get('format', 'mp4') != 'mp4' \
       or extras.get(kClipQualityProfile, 0) != 0 \
       or extras.get('fps',0) > 0 \
       or (maxSize is not None and maxSize != (0,0)):
        # if quality setting isn't explicitly set, default to the a very high value
        # (we were prepared to remux after all)
        if not kClipQualityProfile in extras:
            extras[kClipQualityProfile] = 5 # svvpVeryHigh
        return createClip(origFiles, newFile, firstMs, lastMs, configDir, extras, logFn, progFn)

    fmt = extras.get('format', "mp4")
    numFiles = len(origFiles)
    filenames = (c_char_p*numFiles)()
    offsets = (c_uint64*numFiles)()

    for i in range(numFiles):
        filename, offset = origFiles[i]
        filenames[i] = filename.encode('utf-8')
        offsets[i] = offset

    res = _videolib.fast_create_clip(len(origFiles), filenames, offsets,
            firstMs, lastMs, newFile.encode('utf-8'), fmt, logFn, progFn)
    if res == _kCreateClipError:
        return -1
    return int(res)

###########################################################
def remuxSubClip(origFile, newFile, firstMs, lastMs, configDir, logFn=None, progFn=None):
    """Create a clip from an existing video file by remuxing.

    @param  origFile  The path to the original file.
    @param  newFile   The path where the resulting file should be created.
    @param  firstMs   The offset into origFile from which to begin copying.
    @param  lastMs    The offset into origFile at which to stop copying.
    @param  configDir Unused
    @param  logFn     Function for logging; should be suitable for
                      passing to ctypes.
    @param  progFn     Function for reporting progress/aborting the call
    @return success   Actual offset of the clip, or -1 for error
    """
    return remuxClip([(origFile, 0)], newFile, firstMs, lastMs, configDir,
            {}, logFn, progFn)



###########################################################
def createSubClip(origFile, newFile, firstMs, lastMs, configDir, logFn=None, progFn=None):
    """Create a clip from an existing video file by transcoding.

    @param  origFile  The path to the original file.
    @param  newFile   The path where the resulting file should be created.
    @param  firstMs   The offset into origFile from which to begin copying.
    @param  lastMs    The offset into origFile at which to stop copying.
    @param  configDir Directory to search for config files.
    @param  logFn     Function for logging; should be suitable for
                      passing to ctypes.
    @param  progFn     Function for reporting progress/aborting the call
    @return success   Actual offset of the clip, or -1 for error
    """
    return createClip([(origFile, 0)], newFile, firstMs, lastMs, configDir,
            {}, logFn, progFn)


###########################################################
def createClip(origFiles, newFile, firstMs, lastMs, configDir,
        extras, logFn=None, progFn=None):
    """Create a clip from one or more existing video files by transcoding.

    @param  origFiles  A list of (filename, offset) where filename is the path
                       to the original file and offset is the time in ms
                       between the last frame of the previous file.
    @param  newFile    The path where the resulting file should be created.
    @param  firstMs    The offset into the first file to start copying.
    @param  lastMs     The offset into the last file at which to stop copying.
    @param  configDir  Directory to search for config files.
    @param  extras     A dictionary of optional parameters:
                       'boxList' - a list of (ms, drawboxParams) to overlay
                       'max_bit_rate' - a requested max bitrate for the clip
                       'maxSize' - a requested max bitrate for the clip
    @param  logFn      Function for logging; should be suitable for
                       passing to ctypes.
    @param  progFn     Function for reporting progress/aborting the call
    @return success    Actual offset of the clip, or -1 for error
    """
    if logFn is None:
        logFn = LOGFUNC(lambda lvl, s: sys.stderr.write("%d: %s" % (lvl,s)))
    if progFn is None:
        progFn = PROGFUNC(defaultProggressCb)

    boxList = extras.get('boxList', [])
    fps = extras.get('fps', 0)
    enableTimestamps = getTimestampFlags(extras)

    config = getCodecConfig(configDir)
    config.sv_profile = int(extras.get(kClipQualityProfile, config.sv_profile))
    if "maxSize" in extras:
        config.max_width = extras['maxSize'][0]
        config.max_height = extras['maxSize'][1]

    fmt = extras.get('format', "mp4")
    numFiles = len(origFiles)
    numBoxes = len(boxList)
    filenames = (c_char_p*numFiles)()
    offsets = (c_uint64*numFiles)()
    boxes = (BoxOverlayInfo*numBoxes)()

    for i in range(numFiles):
        filename, offset = origFiles[i]
        filenames[i] = filename.encode('utf-8')
        offsets[i] = offset

    for i in range(len(boxList)):
        ms, text = boxList[i]
        boxes[i] = BoxOverlayInfo(ms, text)

    res = _videolib.create_clip(len(origFiles), filenames, offsets, firstMs,
            lastMs, newFile.encode('utf-8'), config, enableTimestamps,
            numBoxes, boxes, fmt, fps, logFn, progFn)
    if res == _kCreateClipError:
        return -1
    return int(res)


###########################################################
def getRealClipSize(sourceWidth, sourceHeight, requestedWidth, requestedHeight):
    """Retrieve the actual dimensions a clip will be created as.

    @param  sourceWidth    The input width.
    @param  sourceHeight   The input height.
    @param  requestedWidth The requested output width.
    @param  requesteHeight The requested output height.
    @return outputWidth    The actual output width.
    @return outputHeight   The actual output height.
    """
    pointerWidth = pointer(c_int(requestedWidth))
    pointerHeight = pointer(c_int(requestedHeight))
    _videolib.preserve_aspect_ratio(sourceWidth, sourceHeight, pointerWidth,
            pointerHeight, 1)
    return pointerWidth.contents.value, pointerHeight.contents.value
