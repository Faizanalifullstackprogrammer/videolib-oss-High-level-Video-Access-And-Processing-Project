# videoLib

## Description

videoLib is used as a high level video access and processing library in Sighthound Video.
Sighthound Video integrates with it using Python bindings:
* `ClipReader.py` - provides access to locally recorded clips for playback and individual frame access.
  * Initially allowed multiple implementations. As it stands, it uses `FfMpegClipReader.py`, which integrates with videoLib's C API.
* `ClipUtils.py` - allows creation of modified clips (fragments, overlays, etc) using recorded clips as the source material.
* `StreamReader.py` - SV's integration with a live camera stream. Provides a flow of frames from the live stream, along with information about the location of these frames on disk (file/offset)
* `VideoLibUtils.py` and `ffmpegLog.py` provide miscellaneous housekeeping utilities for library's control and configuration.

Python bindings utilize C API specific to Sighthound Video, and defined exclusively in `include/videolib.h`

## Implementation

videoLib streams are implemented by assembling graphs of components modifying images/audio frames as they propagate through the graph. Individual nodes of the graph implement the API defined in `stream.h`

## Building

* MacOS
    * mkdir .build
    * cd .build
    * cmake \
    -DFFMPEG_INCLUDE_DIRS=/path/to/ffmpeg/include/ \
    -DFFMPEG_LIB_DIRS=/path/to/ffmpeg/lib \
    -DPORTAUDIO_INCLUDE_DIRS=/path/to/portaudio/include \
    -DPORTAUDIO_LIB_DIRS=/path/to/portaudio/lib \
    -DLIVE555_INCLUDE_DIRS="/path/to/live555/include/svlive555;/path/to/live555/include/BasicUsageEnvironment;/path/to/live555/include/UsageEnvironment;/path/to/live555/include/groupsock;/path/to/live555/include/liveMedia" \
    -DLIVE555_LIB_DIRS=/path/to/live555/lib \
    -DOPENSSL_INCLUDE_DIRS=/path/to/openssl/include \
    -DOPENSSL_LIB_DIRS=/path/to/openssl/lib \
    -DIPP_PATH=/path/to/ipp \
    -DSVPCAP_INCLUDE_DIRS=/path/to/libpcap/include \
    -DSVPCAP_LIB_DIRS=/path/to/libpcap/lib \
    -DCMAKE_INSTALL_PREFIX=/path/to/install/videoLib \
    ..
    * make install

* Windows
    * Open an MSVC shell (x64 Native Tools Command Prompt for VS2017)
    * Set up additional environment
        * set PATH=u:\cmake\bin;u:\git\bin;u:\git\usr\bin;%PATH%;u:\llvm10\bin;u:\path\to\ninja;u:\path\to\make
        * set CC=clang-cl
        * set CXX=clang-cl
    * mkdir .build
    * cd .build
    * cmake ^\
    -DFFMPEG_INCLUDE_DIRS=/path/to/ffmpeg/include ^\
    -DFFMPEG_LIB_DIRS=/path/to/ffmpeg/bin ^\
    -DPORTAUDIO_INCLUDE_DIRS=/path/to/portaudio/include ^\
    -DPORTAUDIO_LIB_DIRS=/path/to/portaudio/lib ^\
    -DLIVE555_INCLUDE_DIRS="/path/to/live555/include/svlive555;/path/to/live555/include/BasicUsageEnvironment;/path/to/live555/include/UsageEnvironment;/path/to/live555/include/groupsock;/path/to/live555/include/liveMedia" ^\
    -DLIVE555_LIB_DIRS=/path/to/live555/lib ^\
    -DOPENSSL_INCLUDE_DIRS=/path/to/openssl/include ^\
    -DOPENSSL_LIB_DIRS=/path/to/openssl/lib ^\
    -DIPP_PATH=/path/to/ipp ^\
    -DCMAKE_INSTALL_PREFIX=/path/to/install/videoLib ^\
    -G "Ninja" ..
    * ninja

* While you're welcome to use your own binary dependencies, a full set of these is available as part of Sighthound Video open source package.