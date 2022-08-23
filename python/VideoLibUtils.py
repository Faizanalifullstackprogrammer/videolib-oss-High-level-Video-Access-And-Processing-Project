#! /usr/local/bin/python

#*****************************************************************************
#
# VideoLibUtils.py
#   Python bindings for videoLib configuration APIs that do not fit anywhere else
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


# Python imports...
import sys
import os
from ctypes import c_int, c_char_p, c_void_p, POINTER
from vitaToolbox.ctypesUtils.LoadLibrary import LoadLibrary
from vitaToolbox.loggingUtils.LoggingUtils import setLogParams

# Common 3rd-party imports...
# None, at the moment...

# Toolbox imports...
# None, at the moment...

# Local imports...
# None, at the moment...


_dataDir = None

if hasattr(sys, 'frozen'):
    if 'darwin' in sys.platform:
        _dataDir = os.path.abspath(
            os.path.join(
                os.path.dirname(sys.executable), '..', 'Resources',
            )
        )
    elif 'win32' in sys.platform:
        _dataDir = os.path.abspath(
            os.path.join(
                os.path.dirname(sys.executable),
            )
        )


_videoLib = LoadLibrary(None, 'videolib')


# Defined in sv_os.h
_kDataPath = 0
_kTempPath = 1

##############################################################################
class TSOption(object):
    ENABLE                    = 0x001
    USE_12HR_TIME             = 0x002
    USE_US_DATE               = 0x004



##############################################################################
def SetVideoLibDataPath(dataPath=_dataDir):
    """Sets the data path where resources needed by our video library will be
       located.

    @param dataPath  The path to the directory where the resources needed by
                     VideoLib will be located.
                     If None then this function behaves as a 'no-op'.
    """

    if dataPath is not None:
        _videoLib.videolib_set_path.argtypes = [c_int, c_char_p]
        _videoLib.videolib_set_path(_kDataPath, dataPath)

##############################################################################
_kEnableFFmpegLogging = "Enable FFmpeg Logging"
_kKeepManyLargeLogFilesAround = "Keep Many Large Log Files Around"
_kLogLevelTrace = "Trace"
_kLogLevelDebug = "Debug"
_kLogLevelInfo = "Info"
_kLogLevelWarning = "Warning"
_kLogLevelError = "Error"

##############################################################################
def GetVideoLibDebugConfigItems():
    result = [ _kEnableFFmpegLogging,
        _kKeepManyLargeLogFilesAround ]
    return result

##############################################################################
def GetVideoLibLogLevels():
    result = [
        (_kLogLevelTrace, False),
        (_kLogLevelDebug, False),
        (_kLogLevelInfo, True),
        (_kLogLevelWarning, False),
        (_kLogLevelError, False)
    ]
    return result



##############################################################################
def GetVideolibModulesList():
    _videoLib.get_module_names.argtypes = []
    _videoLib.get_module_names.restype = POINTER(c_char_p)

    reslist = []
    index = 0
    modules = _videoLib.get_module_names()
    while modules[index] != None:
        print modules[index]
        reslist.append(modules[index])
        index = index+1

    return reslist

##############################################################################
def SetVideoLibDebugConfig(dict):
    _videoLib.set_module_trace_level.argtypes = [c_char_p, c_int]
    _videoLib.ffmpeg_log_pause.argtypes = []
    _videoLib.ffmpeg_log_resume.argtypes = []

    logLevel = _kLogLevelInfo
    logCount = -1
    logSize  = -1

    enableAllTracing = False

    for key in dict:
        val = dict.get(key, '')
        if key == _kEnableFFmpegLogging:
            if val:
                _videoLib.ffmpeg_log_resume()
            else:
                _videoLib.ffmpeg_log_pause()
        elif key == _kKeepManyLargeLogFilesAround:
            logCount = 20
            logSize = 20*1024*1024
        elif key == _kLogLevelTrace and val:
            logLevel = key
            enableAllTracing = True
        elif key in (_kLogLevelInfo, _kLogLevelError, _kLogLevelWarning, _kLogLevelDebug) and val:
            logLevel = key
        elif key != '' and not enableAllTracing:
            level = 100 if val else 0
            _videoLib.set_module_trace_level(key, level)

    if enableAllTracing:
        for item in GetVideolibModulesList():
            _videoLib.set_module_trace_level(item, 100)

    setLogParams(logLevel, logSize, logCount)

##############################################################################
def SetVideoLibDebugConfigSimple(enableFfmpegLogging, modules=None):
    dict={ _kEnableFFmpegLogging: enableFfmpegLogging,
           _kLogLevelTrace: modules is None,
           _kLogLevelDebug: modules is not None,
           _kKeepManyLargeLogFilesAround: True
         }

    if modules is not None:
        modules = modules.lower()
        modulesList = modules.split(',')
        existingModules=[x.lower() for x in GetVideolibModulesList()]
        for item in modulesList:
            item = item.strip()
            if item in existingModules:
                dict[item] = True

    SetVideoLibDebugConfig(dict)


##############################################################################
def getTimestampFlags(extras):
    """ Convert dictionary values for timestamp options into a bitmask understood by videoLib
    """
    if extras is None:
        return 0

    enableTimestamps = TSOption.ENABLE if extras.get('enableTimestamps', False) else 0
    if enableTimestamps != 0:
        if extras.get('use12HrTime', False):
            enableTimestamps |= TSOption.USE_12HR_TIME
        if extras.get('useUSDate', False):
            enableTimestamps |= TSOption.USE_US_DATE
    return enableTimestamps