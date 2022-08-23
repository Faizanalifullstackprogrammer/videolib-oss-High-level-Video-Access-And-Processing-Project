#*****************************************************************************
#
# ffmpegLog.py
#   Python bindings for control over ffmpeg logging
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

# Imports...
import ctypes

from vitaToolbox.ctypesUtils.LoadLibrary import LoadLibrary



##############################################################################
class FFmpegLog(object):
    """ Surfaces FFmpeg log messages. To be instantiated once per process. The
    messages get buffered, hence flushing on a regular basis is a must.
    """

    ##########################################################
    def __init__(self, libName, prefix):
        """ Constructor.

        @param  libName  Name of the native library containing the
                         ffmpeg_close_xxx functions. E.g. "videolib".
        @param  prefix   Prefix to put before every log message.
        """
        super(FFmpegLog, self).__init__()

        self._lib = LoadLibrary(None, libName)
        logFunc = ctypes.CFUNCTYPE(None, ctypes.c_int, ctypes.c_char_p)
        self._lib.ffmpeg_log_open.argtypes = [logFunc, ctypes.c_int]
        self._lib.ffmpeg_log_open.restype = ctypes.c_int
        self._lib.ffmpeg_log_close.argtypes = []
        self._lib.ffmpeg_log_pause.argtypes = []
        self._lib.ffmpeg_log_resume.argtypes = []
        self._lib.ffmpeg_log_close.restype = ctypes.c_int
        self._prefix = prefix
        self._cLogFun = None


    ##########################################################
    def open(self, logFn, bufLen):
        """ Opens logging. From this point on FFmpeg log messages are collected
        in a buffer. They can be emitted from any thread, even non-Python ones.

        @param  logFn   Log function to call when flushing is done. This is NOT
                        the ctypes function, but something resembling the
                        logging.Logger.log() method. Otherwise it will crash.
        @param  bufLen  Maximum number of messages to buffer. Each up to 1K.
        @return         Zero on success, error code otherwise.
        """
        def _prefixer(level, msg, *args, **kwargs):
            logFn(level, self._prefix + msg, *args, **kwargs)
        LOGFUNC = ctypes.CFUNCTYPE(None, ctypes.c_int, ctypes.c_char_p)
        self._cLogFun = LOGFUNC(_prefixer)
        return self._lib.ffmpeg_log_open(self._cLogFun, bufLen)


    ##########################################################
    def flush(self):
        """ No-op. Deprecated, but left for compatibility reasons

        @return  Number of dropped messages since the last flush.
        """
        return 0


    ##########################################################
    def close(self):
        """ Closes the log link. Afterwards FFmpeg will emit to stderr again.
        """
        self._lib.ffmpeg_log_close()

    ##########################################################
    def pause(self):
        """ Pause ffmpeg logging without closing it

        @return  Number of dropped messages since the last flush.
        """
        self._lib.ffmpeg_log_pause()


    ##########################################################
    def resume(self):
        """ Resume ffmpeg logging
        """
        self._lib.ffmpeg_log_resume()
