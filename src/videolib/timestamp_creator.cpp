/*****************************************************************************
 *
 * timestamp_creator.cpp
 *   Generates timestamps for streams, where inconsistency in timestamps
 *   provided by the stream requies falling back to locally generated.
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

#include <svlive555.h>

#include <algorithm>
#include <math.h>

#include "streamprv.h"

//============================================================================================
class TimestampCreator : public sio::live555::ITimestampCreator
{
    typedef struct frameTime {
       int64_t  time;   // wall clock on receipt
       int64_t  received;   // timestamp from protocol
       int64_t  assigned;   // timestamp we've assigned
    } frameTime;


    fn_stream_log         logCb;
    frameTime             first;
    frameTime             previous;
    int                   framesSeen;
    int64_t               debtCounter;
    const char*           id;

    int64_t               debtCounterIncrement;          // how much did debtCounter change on last assignment
    int64_t               diff;                          // how much did last timestamp increase compared to the previous one, including adjustment
    int64_t               adjustment;                    // how much was the last timestamp adjusted to decrease debt
    int64_t               tsBase;                        // what was the last pts sent before switching to local ts

    fps_limiter*          fpsLimit;

    static const int64_t kDefaultTsIncrement = 40;       // assume 25fps, if we have not calculated fps yet
    static const int     kFramesRequiredForFps = 200;    // do not use the calculated fps until we've seen that many frames
    static const int     kMaxTimeRetreat = 100;          // NTP may set the time back, but if its too much, it's better to disconnect and start over
    static const int     kMaxTimeJump = 5000;            // 5 seconds without a frame will cause us to drop and reconnect
    static const int     kResetTimeJump = 1000;          // 1 second time jump will cause us to abandon calculations and start over (TBD: do we need it?)

public:
    TimestampCreator(fn_stream_log logCb, const char* id);
    ~TimestampCreator();
    void setLogCb(fn_stream_log log);
    int assignTs(int64_t receivedTs, int64_t& assignedTs, bool rtcpSyncOccurred);
    std::string stats() const;
    void setTsBase(int64_t base) { tsBase = base; }

private:
    void _reset();
    int _assignTs(const frameTime& prevTs, frameTime& currTs, int rtcpSyncOccurred);
    void _log(int logLevel, const char* msg);
    void _logCondition(int level, const char* msg,
                    const frameTime& prevTs, frameTime& currTs);
};

//============================================================================================
sio::live555::ITimestampCreator* _CreateTimestampCreator(fn_stream_log logCb, const char* id)
{
    return new TimestampCreator(logCb, id);
}

//============================================================================================
TimestampCreator::TimestampCreator(fn_stream_log _logCb, const char* _id)
    : fpsLimit( NULL )
    , logCb( _logCb )
    , id ( _id )
{
    _reset();
}

//============================================================================================
TimestampCreator::~TimestampCreator()
{
    fps_limiter_destroy(&fpsLimit);
}

//============================================================================================
void TimestampCreator::setLogCb(fn_stream_log log)
{
    logCb = log;
}

//============================================================================================
int TimestampCreator::assignTs(int64_t receivedTs, int64_t& assignedTs, bool rtcpSyncOccurred)
{
    frameTime current;
    current.time = sv_time_get_current_epoch_time();
    current.received = receivedTs;
    current.assigned = INVALID_PTS;

    if ( _assignTs(previous, current, rtcpSyncOccurred) < 0 ||
         current.assigned == INVALID_PTS ) {
        _reset();
        return -1;
    }

    if ( framesSeen == 0 ) {
        first = current;
    }

    framesSeen++;


    previous = current;
    assignedTs = current.assigned;

    return 0;
}


//============================================================================================
std::string TimestampCreator::stats() const
{
    int64_t relativeTime = previous.time - first.time;
    int64_t relativePts = previous.assigned - first.assigned;
    int64_t jitter = relativePts - relativeTime;

    return _STR("Frame #" << framesSeen << ": " <<
                " diff=" << diff <<
                " fps=" << fps_limiter_get_fps(fpsLimit) <<
                " time=" << relativeTime <<
                " ts=" << relativePts <<
                " jitter=" << jitter <<
                " adj=" << adjustment <<
                " debt=" << debtCounter <<
                " lastDebtIncrement=" << debtCounterIncrement );
}

//============================================================================================
void TimestampCreator::_reset()
{
    fps_limiter_destroy(&fpsLimit);
    fpsLimit = fps_limiter_create(kFramesRequiredForFps, 0);
    previous.time = previous.received = previous.assigned = INVALID_PTS;
    framesSeen = 0;
    debtCounter = 0;
};

//============================================================================================
void TimestampCreator::_log(int logLevel, const char* msg)
{
    logCb(logLevel, _FMT(id << ": " << msg));
}

//============================================================================================
void TimestampCreator::_logCondition(int level, const char* msg,
                                    const frameTime& prevTs, frameTime& currTs)
{
    _log(level, _STR(msg <<
                        " assignedDelta=" << currTs.assigned - prevTs.assigned <<
                        " receivedDelta=" << currTs.received - prevTs.received <<
                        " timeDelta=" << currTs.time - prevTs.time ));
}

//============================================================================================
int TimestampCreator::_assignTs(const frameTime& prevTs, frameTime& currTs, int rtcpSyncOccurred)
{
    int     result = 0;                  // assume success
    int64_t receivedDelta = currTs.received - prevTs.received;
    int64_t timeDelta = currTs.time - prevTs.time;

    float   estimatedFps;
    if ( currTs.received != prevTs.received ) {
        fps_limiter_report_frame(fpsLimit, &estimatedFps, currTs.time);
    }
    int     canUseFps = (framesSeen > kFramesRequiredForFps);
    int     expectedTsIncrement = canUseFps ? floor(1000/estimatedFps) : kDefaultTsIncrement;
    int64_t maxAdjustment = expectedTsIncrement / 2;

    debtCounterIncrement = 0;

    if (framesSeen == 0) {
        // assign current time as timestamp
        currTs.assigned = currTs.time;
        if ( tsBase != INVALID_PTS ) {
            int64_t delta = currTs.assigned - tsBase;
            if ( delta < 0 ) {
                currTs.assigned = tsBase + kDefaultTsIncrement;
                debtCounter = delta;
            } else if ( delta > kDefaultTsIncrement ) {
                debtCounter = delta - kDefaultTsIncrement;
            }
            if ( debtCounter != 0 ) {
                _log(logInfo, _STR("time=" << currTs.time << " base=" << tsBase <<
                                " debt=" << debtCounter << " assigned=" << currTs.assigned));
            }
        }
    } else
    if (receivedDelta == 0) {
        // timestamp did not change, which means we've received something that's part of the same frame
        currTs.assigned = prevTs.assigned;
    } else
    if ( timeDelta < 0 ) {
        // clock went backwards
        if ( std::abs(timeDelta) < kMaxTimeRetreat ) {
            // recoverable
            currTs.assigned = prevTs.assigned + expectedTsIncrement;
            debtCounterIncrement = timeDelta; // note: negative in this case
        } else {
            // too much, error out
            _logCondition(logError, _STR("Clock moved back by " << timeDelta << "ms. Reconnecting."),
                        prevTs, currTs);
            result = -1;
        }
    } else
    if ( timeDelta > kResetTimeJump ) {
        // a long time without a frame
        if ( timeDelta > kMaxTimeJump ) {
            // too much, error out and start over
            _logCondition(logError, _STR("Clock jumped forward by " << timeDelta << "ms. Reconnecting."),
                        prevTs, currTs);
            return -1;
        } else {
            // reset and recover
            currTs.assigned = std::max(currTs.time, prevTs.assigned + kDefaultTsIncrement);
            _logCondition(logInfo, _STR("Clock jumped forward by " << timeDelta << "ms."),
                        prevTs, currTs);
            _reset();
        }
    } else {
        currTs.assigned = prevTs.assigned + expectedTsIncrement;
        debtCounterIncrement = expectedTsIncrement - timeDelta;
    }

    debtCounter += debtCounterIncrement;

    if ( receivedDelta != 0 ) {
        if ( debtCounter > 0 ) {
            // positive debtCounter means the timestamp is ahead of the clock ... we'll drop it if we can
            adjustment = std::min( debtCounter, maxAdjustment );
        } else {
            // negative debtCounter: timestamp is behind the clock ... make it go forward
            adjustment = std::max( debtCounter, -maxAdjustment );
        }
    } else {
        adjustment = 0;
    }

    debtCounter -= adjustment;
    currTs.assigned -= adjustment;
    diff = currTs.assigned - prevTs.assigned;

    return 0;
};

//============================================================================================
std::ostream& operator<<(std::ostream& os, const sio::live555::ITimestampCreator& tc)
{
    os << tc.stats();
    return os;
}
