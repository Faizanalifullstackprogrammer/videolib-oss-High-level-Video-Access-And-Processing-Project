#! /usr/local/bin/python


#*****************************************************************************
#
# ClipReader.py
#   Python bindings for access to recorded video clips
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


##############################################################################
class ClipReader(object):
    """This is the master clip reader that's built on top of other ones.

    Clip readers using certain libraries are minimal.  This is the main reader
    and handles things like:
    - Choosing which actual library to use.
    - Caching.
    - Convenience functions for the client.
    """

    ###########################################################
    def __init__(self, logFn=None):
        """ClipReader constructor.

        @param  logFn       Function for logging; should be suitable for
                            passing to ctypes.
        """
        self._logFn = logFn
        self._reset()


    ###########################################################
    def _reset(self, forClose=False):
        """Reset our state.

        This is called by init and also by close.

        @param  forClose  If True, we're being called from "close" so we know
                          that certain variables already exist.
        """
        # Force a close right away for any readers that we have.  This probably
        # isn't required (their descructors should be called when we lose
        # references to them), but it's safer.
        if forClose:
            for rdr in (self._firstReader, self._randReader, self._seqReader):
                if rdr is not None:
                    rdr.close()

        # We open a different reader for sequential vs. random access.  This
        # makes it efficient to effectively have two file pointers...
        # ...we'll open them on-demand...
        self._seqReader = None
        self._randReader = None

        # When the clip is first opened and we don't know whether we're going
        # to be used in sequential or random access (or both), we'll
        # temporarily store the clipReader here...
        self._firstReader = None

        # Semi public (read-only) variables...

        # ...the parameters that were passed to open...
        # Note that width and height are the *requested* width and height, and
        # may not match the resolution of returned images.
        self.path = None
        self.width = 0
        self.height = 0
        self.firstMs = -1
        self.extras = None

        # ...the millisecond value that came from the last getNextFrame()
        self.prevMs = -1

        # ...the millisecond value that the sequential reader most recently
        # returned...
        self._readerPrevMs = -1

        # ...set to True once getNextFrame() hits the end
        self.isDone = False

        # Cached values
        self._duration = None
        self._msList = None




    ###########################################################
    def __del__(self):
        """Free resources used by ClipReader"""
        self.close()


    ###########################################################
    def open( self, filename, width, height, firstMs, extras ):
        """Open a video clip for reading.

        @param  filename  The clip to open
        @param  width     The desired width of retrieved frames
        @param  height    The desired height of retrieved frames
        @return success   True if the clip was opened
        """
        self.close()

        self.path = filename
        self.width = width
        self.height = height
        self.firstMs = firstMs
        self.extras = extras

        self._firstReader = self._allocReader()

        return self._firstReader is not None


    ###########################################################
    def close(self):
        """Close the currently opened clip"""
        self._reset(True)

    ###########################################################
    def hasAudio(self):
        for rdr in (self._firstReader, self._randReader, self._seqReader):
            if rdr is not None:
                return rdr.hasAudio()
        return False

    ###########################################################
    def getInputSize(self):
        """Return the (width, height) of the input file.

        This isn't useful for too much, except to know how we've skewed the
        file to keep its width divisible.

        @return width   The width of the input file.
        @return height  The height of the input file.
        """
        reader = self._getAnyReader()
        if reader is None:
            return (0, 0)

        return reader.getInputSize()


    ###########################################################
    def _allocReader(self):
        """Allocate a child reader.

        @return reader  A newly created reader; None upon error.
        """
        from FfMpegClipReader import FfMpegClipReader
        reader = FfMpegClipReader(self._logFn)
        success = reader.open( self.path, self.width, self.height, self.firstMs, self.extras )
        if not success:
            reader = None

        return reader


    ###########################################################
    def _getAnyReader(self):
        """Return a reader for the clip.

        We don't care if it's the sequential one, the random one, or the
        first one.

        @return reader  One of our readers.
        """
        for rdr in (self._firstReader, self._randReader, self._seqReader):
            if rdr is not None:
                return rdr

        return None


    ###########################################################
    def _getSeqReader(self):
        """Return the sequential reader.

        This will handle opening the reader if necessary.

        @return reader  The sequential reader.
        """
        if self._seqReader is None:
            if self._firstReader is not None:
                # First becomes sequential...
                self._seqReader = self._firstReader
                self._firstReader = None
            else:
                # First was already used, allocate new...
                self._seqReader = self._allocReader()

        return self._seqReader


    ###########################################################
    def _getRandReader(self):
        """Return the random reader.

        This will handle opening the reader if necessary.

        @return reader  The random reader.
        """
        if self._randReader is None:
            if self._firstReader is not None:
                # First becomes random...
                self._randReader = self._firstReader
                self._firstReader = None
            else:
                # First was already used, allocate new...
                self._randReader = self._allocReader()

        return self._randReader


    ###########################################################
    def getDuration(self):
        """Get the duration of the clip in ms

        @return duration  The duration of the clip in ms
        """
        if self._duration is None:
            reader = self._getAnyReader()
            if reader is None:
                return -1

            self._duration = reader.getDuration()

        return self._duration


    ###########################################################
    def getMsList(self):
        """Get the ms offset of each frame in the clip

        @return msList  A list of offsets of each frame in the clip
        """
        if not self._msList:
            # Need to use random reader, since this will mess up the location...
            reader = self._getRandReader()
            if reader is None:
                return []

            self._msList = reader.getMsList()

        return self._msList


    ###########################################################
    def getNextFrame(self):
        """Get the next frame in the current clip

        @return frame  A ClipFrame of the next frame in the clip or None on
                       error or when all frames have been read
        """
        if self._readerPrevMs != self.prevMs:
            # We're off in the weeds--just use seek...
            self.seek(self.prevMs)

        reader = self._getSeqReader()
        if reader is None:
            return None

        clipFrame = reader.getNextFrame()
        if clipFrame is None:
            self._readerPrevMs = -1
            self.prevMs = -1
            self.isDone = True
            return None
        self._readerPrevMs = clipFrame.ms

        self.prevMs = clipFrame.ms

        return clipFrame


    ###########################################################
    def getPrevFrame(self):
        """Get the next frame in the current clip

        @return frame  A ClipFrame of the previous frame in the clip or None on
                       error or when all frames have been read
        """
        if self._readerPrevMs != self.prevMs:
            self.seek(self.prevMs)

        reader = self._getSeqReader()
        if reader is None:
            return None

        clipFrame = reader.getPrevFrame()
        if clipFrame is None:
            self._readerPrevMs = -1
            self.prevMs = -1
            return None
        self._readerPrevMs = clipFrame.ms

        self.isDone = False
        self.prevMs = clipFrame.ms

        return clipFrame


    ###########################################################
    def getNextFrameOffset(self):
        """Get the offset in ms of the next frame in the current clip.

        @return ms  The ms offset of the next frame, or -1 if no more.
        """
        reader = self._getSeqReader()
        if reader is None:
            return -1
        return reader.getNextFrameOffset()

    ###########################################################
    def setMute(self, _mute):
        if self._seqReader is not None:
            self._seqReader.setMute(_mute)
        if self._randReader is not None:
            self._randReader.setMute(_mute)

    ###########################################################
    def seek(self, msOffset):
        """Move the sequential file pointer to retrieve a frame at some time.

        @param  msoffset  The millisecond offset of the desired frame.
        @return frame     A ClipFrame of the requested frame, None on error.
        """
        reader = self._getSeqReader()
        if reader is None:
            return None

        clipFrame = reader.seek(msOffset)

        if clipFrame is None:
            self._readerPrevMs = -1
            self.prevMs = -1
            return None
        self._readerPrevMs = clipFrame.ms
        self.prevMs = clipFrame.ms

        return clipFrame


    ###########################################################
    def getFrameAt(self, msOffset):
        """Retrieve the frame in the current clip closest to the given offset

        IMPORTANT NOTE: You can't mix this with getNextFrame(), since this
        function will leave the "next frame" in the file as some semi-arbitrary
        location.

        @param  msoffset  The millisecond offset of the desired frame
        @return frame     A ClipFrame of the requested frame, None on error
        """
        # Need to use random reader, since this will mess up the location...
        reader = self._getRandReader()
        if reader is None:
            return None

        clipFrame = reader.getFrameAt(msOffset)
        if clipFrame is None:
            return None

        return clipFrame


    ###########################################################
    def markDone(self):
        """Arbitrarily marks the sequential reader as "done"."""
        self.isDone = True


    ###########################################################
    def getPrevMs(self):
        """Return the ms that were returned with the previous getNextFrame().

        This is just a convenience for clients.

        @return prevMs  The milliseconds from the previous getNextFrame(), or
                        -1 if getNextFrame() hasn't been called yet.
        """
        return self.prevMs


    ###########################################################
    def setOutputSize(self, resolution):
        """Set the size retrieved frames shoudl be.

        @param  resolution  The resolution new frames should be returned at."""
        self.width = resolution[0]
        self.height = resolution[1]

        for rdr in (self._firstReader, self._randReader, self._seqReader):
            if rdr is not None:
                rdr.setOutputSize(resolution)

##############################################################################
class ClipFrame(object):
    """Interface to the ClipFrames returned returned by ClipReader."""
    ###########################################################
    def asNumpy(self):
        """Return a numpy version of our data.

        @return img  A numpy version of our data.
        """
        raise NotImplementedError


    ###########################################################
    def asPil(self):
        """Return a PIL version of our data.

        @return img  A PIL version of our data.
        """
        raise NotImplementedError


##############################################################################
def getMsList(filename, logFn=None):
    """ A light(er)-weight utility to retrieve frame timestamps of a file
    """
    from FfMpegClipReader import getMsList
    return getMsList(filename, logFn)

##############################################################################
def getDuration(filename, logFn=None):
    """ A light(er)-weight utility to retrieve frame timestamps of a file
    """
    from FfMpegClipReader import getDuration
    return getDuration(filename, logFn)
