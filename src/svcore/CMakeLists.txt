cmake_minimum_required(VERSION 3.2)
project(svcore)

# Set up exports
set(SVCORE_LINKAGE STATIC)

###############################################
# Components shared across modules
###############################################

message( "CMAKE_CXX_FLAGS=${CMAKE_CXX_FLAGS}" )
message( "CMAKE_CPP_FLAGS=${CMAKE_CPP_FLAGS}" )
message( "CMAKE_C_FLAGS=${CMAKE_C_FLAGS}" )

set(SHARED_HEADERS
    ../../include/event_basic.h
    ../../include/frame_basic.h
    ../../include/stream.h
    ../../include/streamprv.h
    ../../include/sv_os.h
    )

set(SHARED_SOURCES
    buffered_file.cpp
    event_basic.cpp
    frame_allocator.cpp
    frame_basic.cpp
    nalu.cpp
    stream_api.cpp
    sv_os.cpp
    )



add_library(${PROJECT_NAME} ${SVCORE_LINKAGE} "${SHARED_HEADERS}" "${SHARED_SOURCES}")
target_include_directories(${PROJECT_NAME} PRIVATE ${DEPS_INCLUDE_DIRS} ../../include)
if (WIN32 OR MINGW)
    target_link_libraries(${PROJECT_NAME} PRIVATE shlwapi)
elseif(${CMAKE_SYSTEM_NAME} MATCHES "Linux")
    target_link_libraries(${PROJECT_NAME} PRIVATE pthread dl)
endif()


