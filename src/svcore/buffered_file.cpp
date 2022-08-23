/*****************************************************************************
 *
 * buffered_file.cpp
 *   Memory file buffer, used for recording in memory in context of SV.
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

#include "buffered_file.h"
#include <iostream>
#include <cstring>
#include <algorithm>

//------------------------------------------------------------------------------
using std::vector;
using std::min;
using std::max;
using std::ostream;
using std::ofstream;

#if BUFFERED_FILE_DEBUG
#define _LOG(a) mLog << a << std::endl
#else
#define _LOG(a)
#endif

#define BUFFERED_FILE_DEBUG 0

//------------------------------------------------------------------------------
class BufferedFile : public IBufferedFile
{
    typedef std::vector< char* > BufferContainer;
public:
    BufferedFile( const std::string& name = std::string(""),
                int bufferSize = 1024*1024,
                int maxSize = 512*1024*1024 );

    ~BufferedFile();

    // access to opaque value settable by the consumer
    void                        setOpaque(void* pOpaque);
    void*                       getOpaque() const;

    // get file name associated with this object
    const std::string&          getName() const;

    // seek to a position in the file, but not beyond EOF
    int64_t                     seek(int64_t pos, int dir);

    // write to the current position in the buffer
    bool                        write(char* data, size_t size);

    // save the contents to file
    bool                        save(const char* sFilename);

    // save the contents to stream
    bool                        save(std::ostream& os);
private:
    // save the current buffer and allocate a fresh one
    bool _allocBuffer();

    // write to the current buffer, return number of bytes written
    int _writeToBuffer(char* ptr, int size);

private:
    std::string                 mName;              // ID of the file object
#if BUFFERED_FILE_DEBUG
    std::ostream&               mLog;
#endif
    int                         mBufferSize;        // size of each chunk
    int                         mMaxSize;           // maximum allocation size
    int                         mLastWrittenPos;    // size of file
    int                         mWritePos;          // current writing position
    bool                        mInvalidState;      // error flag
    BufferContainer             mBuffers;           // actual buffers with data
    void*                       mOpaque;            // opaque value for the benefit of consumer
};

SVCORE_API IBufferedFile* _CreateBufferedFile( const std::string& name,
                int bufferSize,
                int maxSize)
{
    return new BufferedFile(name, bufferSize, maxSize);
}

//------------------------------------------------------------------------------
BufferedFile::BufferedFile(const std::string& name, int bufferSize, int maxSize )
    : mName ( name )
#if BUFFERED_FILE_DEBUG
    , mLog( std::cout ) // name + ".txt", ofstream::out|ofstream::app )
#endif
    , mBufferSize ( bufferSize )
    , mMaxSize ( maxSize )
    , mLastWrittenPos ( 0 )
    , mWritePos ( 0 )
    , mInvalidState( false )
    , mOpaque ( nullptr )
{
    _LOG ( "=====================" );
}

//------------------------------------------------------------------------------
BufferedFile::~BufferedFile()
{
    for ( auto ptr : mBuffers ) {
        delete [] ptr;
    }
}

//------------------------------------------------------------------------------
void                        BufferedFile::setOpaque(void* pOpaque)
{
    mOpaque = pOpaque;
}

//------------------------------------------------------------------------------
void*                       BufferedFile::getOpaque() const
{
    return mOpaque;
}

//------------------------------------------------------------------------------
const std::string&          BufferedFile::getName() const
{
    return mName;
}

//------------------------------------------------------------------------------
// seek to a position in the file, but not beyond EOF
int64_t                     BufferedFile::seek(int64_t relPos, int dir)
{
    _LOG ("Seeking to pos=" << relPos << " dir=" << dir );
    int64_t pos;
    switch (dir)
    {
    case SEEK_SET:       pos = relPos; break;
    case SEEK_CUR:       pos = mWritePos + relPos; break;
    case SEEK_END:       pos = mLastWrittenPos + relPos; break;
    default:
        return -1;
    }
    //printf("Seeking to pos %d\n", pos);
    if ( pos > mLastWrittenPos || pos < 0 || mInvalidState ) {
        return -1;
    }
    mWritePos = pos;
    return mWritePos;
}

//------------------------------------------------------------------------------
// write to the current position in the buffer
bool                        BufferedFile::write(char* data, size_t size)
{
    _LOG( "Writing at pos=" << mWritePos << " size=" << size );

    // we should use buffers larger than any possible single write
    if ( mInvalidState ) {
        return false;
    }
    if ( size == 0 ) {
        return true;
    }

    size_t totalWritten = 0;
    while ( totalWritten < size ) {
        size_t written = _writeToBuffer(&data[totalWritten], min(size - totalWritten, (size_t)mBufferSize));
        if ( written <= 0 ) {
            mInvalidState = true;
            return false;
        }
        totalWritten += written;
    }

    return true;
}

//------------------------------------------------------------------------------
// save the current buffer and allocate a fresh one
bool BufferedFile::_allocBuffer()
{
    if ( mBufferSize*mBuffers.size() >= mMaxSize ) {
        return false;
    }
    char* newBuffer = new char[mBufferSize];
    if ( newBuffer != nullptr ) {
        mBuffers.push_back(newBuffer);
        return true;
    }
    return false;
}

//------------------------------------------------------------------------------
// write to the current buffer, return number of bytes written
int BufferedFile::_writeToBuffer(char* ptr, int size)
{
    int currentBuffer  = mWritePos / mBufferSize;
    int offsetInBuffer = mWritePos % mBufferSize;

    _LOG ( "Writing to obj=" << (void*) this << " buffer=" << currentBuffer << ":" << offsetInBuffer << " buffers=" << mBuffers.size() <<  " pos=" << mWritePos << " bufSize=" << mBufferSize );

    // we should, at most, need one more buffer
    if ( currentBuffer > mBuffers.size() ) {
        return 0;
    } else if ( currentBuffer == mBuffers.size() ) {
        if ( !_allocBuffer() ) {
            return 0;
        }
    }

    char* buffer = mBuffers[currentBuffer];

    int written = 0;
    written = min(mBufferSize - offsetInBuffer, size);
    if ( written > 0 ) {
        _LOG( "About to memcpy " << written << " at offset " << offsetInBuffer << " buffer " << (void*) buffer << " size " << mBufferSize );
        memcpy(&buffer[offsetInBuffer], ptr, written);
        mWritePos += written;
        mLastWrittenPos = max( mWritePos, mLastWrittenPos );
    }
    return written;
}


//------------------------------------------------------------------------------
bool BufferedFile::save(const char* sFilename)
{
    if ( mInvalidState ) {
        return false;
    }
    ofstream os(sFilename, ofstream::out|ofstream::binary);
    bool res = save( os );
    os.close();
    return res;
}

//------------------------------------------------------------------------------
bool BufferedFile::save(ostream& os)
{
    auto size = mBuffers.size();
    for ( int nI=0; nI<size && os.good(); nI++ ) {
        int nToWrite = (nI != size-1) ? mBufferSize : mLastWrittenPos%mBufferSize;
        os.write( mBuffers[nI], nToWrite );
    }
    return os.good();
}
