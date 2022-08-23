cmake_minimum_required(VERSION 3.2)
project(svpcap)

###############################################
# videolib components
###############################################

set(INSTALL_INC ${CMAKE_INSTALL_PREFIX}/include)
set(INSTALL_LIB ${CMAKE_INSTALL_PREFIX}/lib)

set(SVPCAP_HEADERS
    ../../include/sv_pcap.h
    )


set(SVPCAP_SOURCES
    sv_pcap.cpp
    )

set(SVPCAP_INCLUDE
    ${INSTALL_INC}
    )

set(SVPCAP_LIBS
    svcore
    )


add_definitions( -DSVPCAP_EXPORTS=1 )

if (WIN32 OR MINGW)
    set(SVPCAP_LIBSUBDIR "/x64")
    list(APPEND SVPCAP_LIB_DIRS ${CMAKE_CURRENT_SOURCE_DIR}/../../3rdparty/winpcap/Lib${SVPCAP_LIBSUBDIR} )
    list(APPEND SVPCAP_INCLUDE_DIRS ${CMAKE_CURRENT_SOURCE_DIR}/../../3rdparty/winpcap/Include)
    list(APPEND SVPCAP_LIBS wpcap packet ws2_32)
else()
    list(APPEND SVPCAP_LIBS pcap)
endif()


add_definitions( -D__STDC_CONSTANT_MACROS )
add_library(${PROJECT_NAME} SHARED "${SVPCAP_HEADERS}" "${SVPCAP_SOURCES}")
target_include_directories(${PROJECT_NAME} PRIVATE ${SVPCAP_INCLUDE_DIRS} ../../include)
target_link_directories(${PROJECT_NAME} PRIVATE ${SVPCAP_LIB_DIRS})
target_link_libraries(${PROJECT_NAME} PRIVATE ${SVPCAP_LIBS} svcore)
if (DEPS_PATH_FLAGS)
    set_target_properties(${PROJECT_NAME} PROPERTIES LINK_FLAGS ${DEPS_PATH_FLAGS})
endif()

install(TARGETS ${PROJECT_NAME}
        RUNTIME DESTINATION bin
        LIBRARY DESTINATION lib)
