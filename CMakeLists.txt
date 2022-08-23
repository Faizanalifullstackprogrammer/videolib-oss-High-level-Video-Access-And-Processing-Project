cmake_minimum_required(VERSION 3.9.0)
project(videolib2)

if(CMAKE_COMPILER_IS_GNUCC)
set(CMAKE_SHARED_LINKER_FLAGS "${CMAKE_SHARED_LINKER_FLAGS} -t")
endif()

if (APPLE OR WIN32)
    SET(WITH_LOCALVIDEOLIB_DEFAULT ON)
    add_definitions( -DNOMINMAX )
else()
    SET(WITH_LOCALVIDEOLIB_DEFAULT OFF)
endif()

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# portaudio performs much better than OpenAL at this point, but if for some reason
# we need to go back, set this flag
option(WITH_LIVE555 "Enable Live555 integration" ON)
option(WITH_IPP "Enable IPP integration" ON)
option(WITH_PCAP "Enable libpcap integration" ON)
option(WITH_PORTAUDIO "Enable Portaudio integration" ON)
option(WITH_SV "Build videoLib in context of Sighthound Video build" ON)
option(WITH_LOCALVIDEOLIB "Build localVideoLib for Webcam integration" ${WITH_LOCALVIDEOLIB_DEFAULT})


message("CMAKE_BINARY_DIR is ${CMAKE_BINARY_DIR}")
message("PROJECT_BINARY_DIR is ${PROJECT_BINARY_DIR}")
message("CMAKE_CURRENT_BINARY_DIR is ${CMAKE_CURRENT_BINARY_DIR}")
message("CMAKE_SOURCE_DIR is ${CMAKE_SOURCE_DIR}")
message("PROJECT_SOURCE_DIR is ${PROJECT_SOURCE_DIR}")
message("CMAKE_CURRENT_SOURCE_DIR is ${CMAKE_CURRENT_SOURCE_DIR}")



if (UNIX)
    add_compile_options( -fPIC )
endif(UNIX)

if (WITH_SV)
    add_definitions( -DSIGHTHOUND_VIDEO=1 )
endif()


set(DEPS_INCLUDE_DIRS ${FFMPEG_INCLUDE_DIRS})
set(DEPS_LIB_DIRS ${FFMPEG_LIB_DIRS})
set(DEPS_LIBS avcodec;avdevice;swresample;avfilter;avformat;avutil;swscale )

if (WITH_PORTAUDIO)
    add_definitions( -DAUDIO_RENDER_SUPPORT=1 )
    list(APPEND DEPS_INCLUDE_DIRS ${PORTAUDIO_INCLUDE_DIRS} )
    list(APPEND DEPS_LIB_DIRS ${PORTAUDIO_LIB_DIRS}  )
    if (APPLE)
        list(APPEND DEPS_LIBS "portaudio_static" )
    else()
        list(APPEND DEPS_LIBS "portaudio_static_x64" )
    endif()
    if (APPLE)
        list(APPEND DEPS_LIBS "-framework AudioUnit" )
    endif()
endif()

if (WITH_LIVE555)
    list(APPEND DEPS_INCLUDE_DIRS ${LIVE555_INCLUDE_DIRS};${OPENSSL_INCLUDE_DIRS} )
    list(APPEND DEPS_LIB_DIRS ${LIVE555_LIB_DIRS};${OPENSSL_LIB_DIRS}  )
    list(APPEND DEPS_LIBS svlive555 )
endif()

if (WITH_IPP)
    set(IPP_STATIC "ON")

    foreach(IPPLIB cc i s vm core)
        if (WIN32 OR MSVC)
            list(APPEND DEPS_LIBS ${IPP_PATH}/lib/intel64/ipp${IPPLIB}mt.lib )
        elseif(APPLE)
            list(APPEND DEPS_LIBS ${IPP_PATH}/lib/libipp${IPPLIB}.a)
        else()
            list(APPEND DEPS_LIBS ${IPP_PATH}/lib/intel64/libipp${IPPLIB}.a)
        endif()
    endforeach()
    list(APPEND DEPS_INCLUDE_DIRS ${IPP_PATH}/include)
    add_definitions( -DWITH_IPP=1 )
endif()

set(DEPS_PATH_FLAGS "")
if(CMAKE_SYSTEM_NAME MATCHES iOS)
    list(TRANSFORM DEPS_LIBS PREPEND "-framework ")
    foreach(DEP IN LISTS DEPS_LIB_DIRS)
        set(DEPS_PATH_FLAGS "${DEPS_PATH_FLAGS} -Wl,-F${DEP}")
    endforeach()
endif()


add_definitions( -DSVCORE_STATIC=1 -DSVCORE_EXPORTS=1 )
add_subdirectory(src/svcore)

add_subdirectory(src/videolib)
if (WITH_PCAP)
    add_subdirectory(src/svpcap)
endif()
if (WITH_LOCALVIDEOLIB)
    add_subdirectory(src/localVideoLib)
endif()
if( NOT ${SV_ARCH} STREQUAL "arm" AND NOT WITH_SV )
    add_subdirectory(src/tests)
endif()
add_subdirectory(include)

if (WITH_SV)
    file(GLOB ALL_PYTHON "python/*.py")
    install(FILES ${ALL_PYTHON} DESTINATION "lib/python2.7/site-packages/videoLib2/python")
    install(FILES "python/__init__.py" DESTINATION "lib/python2.7/site-packages/videoLib2")
endif()