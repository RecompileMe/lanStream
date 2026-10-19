cmake_minimum_required(VERSION 3.20)
project(LanStream CXX)
set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# DYLIBS on Mac need to have the right search path set before linking
if(APPLE)
    enable_language(OBJCXX)
    find_library(VIDEOTOOLBOX_FRAMEWORK VideoToolbox REQUIRED)
    add_library(libavcodec SHARED IMPORTED)
    set_target_properties(libavcodec PROPERTIES
            IMPORTED_LOCATION "${PROJECT_SOURCE_DIR}/lib/mac_intel/libavcodec.dylib"
            INTERFACE_INCLUDE_DIRECTORIES "${PROJECT_SOURCE_DIR}/inc"
            )

    add_library(libavutil SHARED IMPORTED)
    set_target_properties(libavutil PROPERTIES
            IMPORTED_LOCATION "${PROJECT_SOURCE_DIR}/lib/mac_intel/libavutil.dylib"
            INTERFACE_INCLUDE_DIRECTORIES "${PROJECT_SOURCE_DIR}/inc"
            )

    add_library(libswscale SHARED IMPORTED)
    set_target_properties(libswscale PROPERTIES
            IMPORTED_LOCATION "${PROJECT_SOURCE_DIR}/lib/mac_intel/libswscale.dylib"
            INTERFACE_INCLUDE_DIRECTORIES "${PROJECT_SOURCE_DIR}/inc"
            )

    # Set variables so the rest of your script works seamlessly
    set(FFMPEG_INCS "${PROJECT_SOURCE_DIR}/inc")
    set(FFMPEG_LIBS libavcodec libavutil libswscale ${VIDEOTOOLBOX_FRAMEWORK})
elseif (UNIX AND NOT APPLE)
    add_library(libavcodec SHARED IMPORTED)
    set_target_properties(libavcodec PROPERTIES
            IMPORTED_LOCATION "${PROJECT_SOURCE_DIR}/lib/linux/libavcodec.so"
            INTERFACE_INCLUDE_DIRECTORIES "${PROJECT_SOURCE_DIR}/inc"
            )

    add_library(libavutil SHARED IMPORTED)
    set_target_properties(libavutil PROPERTIES
            IMPORTED_LOCATION "${PROJECT_SOURCE_DIR}/lib/linux/libavutil.so"
            INTERFACE_INCLUDE_DIRECTORIES "${PROJECT_SOURCE_DIR}/inc"
            )

    add_library(libswscale SHARED IMPORTED)
    set_target_properties(libswscale PROPERTIES
            IMPORTED_LOCATION "${PROJECT_SOURCE_DIR}/lib/linux/libswscale.so"
            INTERFACE_INCLUDE_DIRECTORIES "${PROJECT_SOURCE_DIR}/inc"
            )
    add_library(libva SHARED IMPORTED)
    set_target_properties(libva PROPERTIES
            IMPORTED_LOCATION "${PROJECT_SOURCE_DIR}/lib/linux/libva.so"
            INTERFACE_INCLUDE_DIRECTORIES "${PROJECT_SOURCE_DIR}/inc"
            )

    # Set variables so the rest of your script works seamlessly
    set(FFMPEG_INCS "${PROJECT_SOURCE_DIR}/inc")
    set(FFMPEG_LIBS libavcodec libavutil libswscale libva)
endif()

find_package(PkgConfig REQUIRED)
find_package(Threads REQUIRED)
find_package(SDL2 REQUIRED)



#if(UNIX AND NOT APPLE)
#    pkg_check_modules(AVCODEC  REQUIRED libavcodec)
#    pkg_check_modules(AVUTIL   REQUIRED libavutil)
#    pkg_check_modules(SWSCALE  REQUIRED libswscale)
#    set(FFMPEG_INCS ${AVCODEC_INCLUDE_DIRS} ${AVUTIL_INCLUDE_DIRS} ${SWSCALE_INCLUDE_DIRS})
#    set(FFMPEG_LIBS ${AVCODEC_LIBRARIES}    ${AVUTIL_LIBRARIES}    ${SWSCALE_LIBRARIES})
#endif()

if(AVCODEC_LIBRARY_DIRS)
    link_directories(${AVCODEC_LIBRARY_DIRS})
endif()
if(AVUTIL_LIBRARY_DIRS)
    link_directories(${AVUTIL_LIBRARY_DIRS})
endif()
if(SWSCALE_LIBRARY_DIRS)
    link_directories(${SWSCALE_LIBRARY_DIRS})
endif()

set(COMMON_SOURCES
        src/encoder/encoder_ffmpeg.cpp
        src/encoder/gpu_detect.cpp
        src/transport/udp_sender.cpp
        src/transport/udp_receiver.cpp
        src/decoder/decoder_ffmpeg.cpp
        src/render/renderer_sdl.cpp
        )

if(WIN32)
    set(PLATFORM_CAPTURE src/capture/capture_dxgi.cpp)
    set(PLATFORM_LIBS    d3d11 dxgi dxguid ws2_32)
    add_compile_definitions(PLATFORM_WINDOWS)
elseif(APPLE)
    set(PLATFORM_CAPTURE src/capture/capture_cg.mm)
    find_library(FW_FOUNDATION    Foundation     REQUIRED)
    find_library(FW_COREGRAPHICS  CoreGraphics   REQUIRED)
    find_library(FW_COREVIDEO     CoreVideo      REQUIRED)
    find_library(FW_COREMEDIA     CoreMedia      REQUIRED)
    find_library(FW_SCREENCAPTUREKIT ScreenCaptureKit REQUIRED)
    find_library(FW_IOSURFACE     IOSurface      REQUIRED)
    find_library(FW_APPKIT        AppKit         REQUIRED)
    set(PLATFORM_LIBS ${FW_FOUNDATION} ${FW_COREGRAPHICS}
            ${FW_COREVIDEO} ${FW_COREMEDIA} ${FW_SCREENCAPTUREKIT}
            ${FW_IOSURFACE} ${FW_APPKIT})
    add_compile_definitions(PLATFORM_MACOS)
else()
    set(PLATFORM_CAPTURE src/capture/capture_x11.cpp)
    pkg_check_modules(X11  REQUIRED x11)
    pkg_check_modules(XEXT REQUIRED xext)
    set(PLATFORM_LIBS ${X11_LIBRARIES} ${XEXT_LIBRARIES})
    include_directories(${X11_INCLUDE_DIRS} ${XEXT_INCLUDE_DIRS})
    add_compile_definitions(PLATFORM_LINUX)
endif()

macro(make_target name main_file)
    add_executable(${name} ${main_file} ${COMMON_SOURCES} ${PLATFORM_CAPTURE})
    target_include_directories(${name} PRIVATE src ${FFMPEG_INCS} ${SDL2_INCLUDE_DIRS})
    target_link_libraries(${name}
            ${FFMPEG_LIBS} ${SDL2_LIBRARIES} ${PLATFORM_LIBS} Threads::Threads)
endmacro()

make_target(sender   src/sender_main.cpp)
make_target(receiver src/receiver_main.cpp)