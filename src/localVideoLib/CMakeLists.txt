cmake_minimum_required(VERSION 3.2)
project(localVideoLib)


set(LOCAL_INC "../../include")
set(INSTALL_LIB ${CMAKE_INSTALL_PREFIX}/lib)
set(INSTALL_INC ${CMAKE_INSTALL_PREFIX}/include)

if (NOT WEBCAM_SERVER_EXENAME)
    set(WEBCAM_SERVER_EXENAME "Sighthound USB")
endif()
add_definitions( -DWEBCAM_SERVER_EXENAME="${WEBCAM_SERVER_EXENAME}" )

set(LVL_LIBS "")

if (APPLE)
    set(LVL_SOURCES
        srcMac/localVideo.cpp
        srcMac/localCapture.mm
        srcMac/localCaptureWrapper.mm
        ../videoLib/logging.c
        )

    set(LVL_SRV_SOURCES
	    srcMac/localVideoServer.cpp
        srcMac/localCapture.mm
        srcMac/localCaptureWrapper.mm
        )

    set(LVL_LIBS
        "-framework Foundation"
        "-framework AVFoundation"
        "-framework CoreVideo"
        "-framework CoreMedia")
else()
    # add_definitions( -D__CRT_UUI_DECL  -DNO_DSHOW_STRSAFE -D__MIDL_user_allocate_free_DEFINED__ )
    set(LVL_SOURCES
        srcWin/localVideo.cpp
        srcWin/videoInput.cpp
        ../videoLib/logging.c
        )
    set(LVL_LIBS
        dxguid
        strmiids
        uuid
        ole32
        oleaut32)
endif()

set(LVL_HEADERS "")

set(LVL_INCLUDE
    ${LOCAL_INC}
    ${INSTALL_INC}
    )

if (WIN32 AND NOT MINGW)
    # MingGW supplies its own qedit.h, which contains deprecated APIs we use
    # On Appveyor, where a proper Windows SDK is installed, we have to rely on our
    # own copy of DirectX SDK. Since the binding is only via CLSIDs, this is valid,
    # but so dirty it makes me cringe.
    list(APPEND LVL_INCLUDE srcWin/DirectX90c/include)
endif()


add_definitions( -D__STDC_CONSTANT_MACROS -DSTRSAFE_NO_DEPRECATE )
include_directories(${LVL_INCLUDE})
link_directories(${INSTALL_LIB})
add_library(${PROJECT_NAME} SHARED "${LVL_HEADERS}" "${LVL_SOURCES}")
target_link_libraries(${PROJECT_NAME} PRIVATE ${LVL_LIBS})

if (APPLE)
    set(WEBCAM_SERVER_TARGET "SighthoundUSBTarget")
    add_executable("${WEBCAM_SERVER_TARGET}" "${LVL_SRV_SOURCES}")
    target_link_libraries(${WEBCAM_SERVER_TARGET} PRIVATE ${LVL_LIBS})
    set_target_properties(${WEBCAM_SERVER_TARGET} PROPERTIES
        OUTPUT_NAME "${WEBCAM_SERVER_EXENAME}"
    )

    install(TARGETS ${WEBCAM_SERVER_TARGET}
            RUNTIME DESTINATION bin
            LIBRARY DESTINATION lib
            ARCHIVE DESTINATION lib)
endif()


install(TARGETS ${PROJECT_NAME}
        RUNTIME DESTINATION bin
        LIBRARY DESTINATION lib
        ARCHIVE DESTINATION lib)
