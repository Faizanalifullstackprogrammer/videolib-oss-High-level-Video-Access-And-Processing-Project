cmake_minimum_required(VERSION 3.14)
project(videolib)


###############################################
# videolib components
###############################################

set(LOCAL_INC ../../include)
set(INSTALL_INC ${CMAKE_INSTALL_PREFIX}/include)
set(INSTALL_LIB ${CMAKE_INSTALL_PREFIX}/lib)

# Export factory methods
add_definitions( -DSVVIDEOLIB_EXPORTS=1 )

set(VIDEOLIB_HEADERS
    ${LOCAL_INC}/sv_ffmpeg.h
    ${LOCAL_INC}/logging.h
    ${LOCAL_INC}/videolib.h
    ${LOCAL_INC}/videolibUtils.h
    )


set(VIDEOLIB_SOURCES
    logging.c
    frame_cloned.cpp
    frame_ffframe.cpp
    frame_ffpacket.cpp
    stream_audio_resample.cpp
    stream_fffilter.cpp
    stream_ffmpeg_decoder.cpp
    stream_ffmpeg_demux.cpp
    stream_ffmpeg_encoder.cpp
    stream_ffmpeg_recorder.cpp
    stream_ffmpeg_resize_filter.cpp
    stream_input_iterator.cpp
    stream_jitter_buffer.cpp
    stream_limiter.cpp
    stream_live555_demux.cpp
    stream_metadata_injector.cpp
    stream_mmap.cpp
    stream_pixelate.cpp
    stream_recorder_sync.cpp
    stream_resize_base.cpp
    stream_resize_factory.cpp
    stream_splitter.cpp
    stream_thread_connector.cpp
    sv_ffmpeg.cpp
    sv_pixfmt.cpp
    timestamp_creator.cpp
    videolibUtils.cpp
    )

if (NOT WITH_SV)
    list(APPEND VIDEOLIB_SOURCES
    stream_debug.cpp
    stream_ffmpeg.cpp
    stream_filter_inserter.cpp
    stream_frame_injector.cpp
    stream_rev_reader.cpp
    )
endif()

set(VIDEOLIB_INCLUDE
    ${LOCAL_INC}
    ${INSTALL_INC}
    ${IPP_INCLUDE}
    )

set(VIDEOLIB_LIBS
    svcore
    ${IPP_LIBRARIES}
    )

if (WITH_PORTAUDIO)
    list(APPEND VIDEOLIB_SOURCES stream_audio_renderer_portaudio.cpp)
endif()

if (WITH_IPP)
    list(APPEND VIDEOLIB_SOURCES stream_ipp_resize_filter.cpp)
    list(APPEND VIDEOLIB_SOURCES stream_ipp_colorconv.cpp)
endif()

if (WITH_SV)
    list(APPEND VIDEOLIB_SOURCES videolib.c)
    list(APPEND VIDEOLIB_SOURCES videolib_hls.cpp)
endif()

if (WIN32 OR MINGW)
    list(APPEND VIDEOLIB_SOURCES stream_mediafoundation_decoder.cpp)
endif()

if (WITH_LOCALVIDEOLIB)
    list(APPEND VIDEOLIB_SOURCES stream_local.cpp)
    list(APPEND VIDEOLIB_LIBS localVideoLib)
    add_definitions( -DLOCAL_CAMERA_SUPPORT=1 )
endif()

if (WIN32 OR MINGW)
    list(APPEND VIDEOLIB_LIBS ws2_32 mfplat mfuuid mf )
endif()


add_definitions( -D__STDC_CONSTANT_MACROS )

add_library(${PROJECT_NAME} SHARED "${VIDEOLIB_HEADERS}" "${VIDEOLIB_SOURCES}")


if (APPLE)
    if (CMAKE_FRAMEWORK)
        string(REPLACE "_" "-" SIO_BUNDLE_NAME ${PROJECT_NAME})
        set_target_properties(${PROJECT_NAME} PROPERTIES MACOSX_FRAMEWORK_IDENTIFIER com.sighthound.${PROJECT_NAME})
    endif()

    find_library(AUDIO_TOOLBOX AudioToolbox)
    find_library(CORE_AUDIO CoreAudio)
    find_library(CORE_FOUNDATION CoreFoundation)
    find_library(CORE_SERVICES CoreServices)
    list(APPEND VIDEOLIB_LIBS ${AUDIO_TOOLBOX} ${AUDIO_UNIT} ${CORE_AUDIO} ${CORE_FOUNDATION} ${CORE_SERVICES})
endif()


target_include_directories(${PROJECT_NAME} PRIVATE ${VIDEOLIB_INCLUDE} ${DEPS_INCLUDE_DIRS})
target_link_directories(${PROJECT_NAME} PRIVATE ${DEPS_LIB_DIRS})
target_link_libraries(${PROJECT_NAME} PRIVATE ${VIDEOLIB_LIBS} ${DEPS_LIBS})
if (DEPS_PATH_FLAGS)
    set_target_properties(${PROJECT_NAME} PROPERTIES LINK_FLAGS ${DEPS_PATH_FLAGS})
endif()


file(COPY "${CMAKE_CURRENT_LIST_DIR}/../../fonts/Inconsolata.otf" DESTINATION ${CMAKE_INSTALL_PREFIX}/data/fonts)

install(TARGETS ${PROJECT_NAME}
        RUNTIME DESTINATION bin
        BUNDLE DESTINATION bin
        LIBRARY DESTINATION lib
        FRAMEWORK DESTINATION lib
        ARCHIVE DESTINATION lib)
