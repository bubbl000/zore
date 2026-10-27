cmake_minimum_required(VERSION 3.25)
project(JpegSpike LANGUAGES CXX)

# Standalone spike project. Deliberately NOT part of new/CMakeLists.txt: the
# libjpeg-turbo archive defines the same `jpeg_*` symbols as the libjpeg that
# FreeImage links into zoreview_core, so the two must never meet in one image.
#
# Configure with:
#   -DJPEG_TURBO_INCLUDE=<dir with jpeglib.h>
#   -DJPEG_TURBO_CONFIG_INCLUDE=<dir with jconfig.h/jversion.h>
#   -DJPEG_TURBO_LIB=<jpeg-static.lib>

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release CACHE STRING "" FORCE)
endif()

add_executable(jpeg_spike jpeg_spike.cpp)
target_include_directories(jpeg_spike PRIVATE
  "${JPEG_TURBO_CONFIG_INCLUDE}"
  "${JPEG_TURBO_INCLUDE}"
)
target_compile_definitions(jpeg_spike PRIVATE
  UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX _WIN32_WINNT=0x0A00
)
if(MSVC)
  target_compile_options(jpeg_spike PRIVATE /O2 /utf-8 /EHsc)
endif()
target_link_libraries(jpeg_spike PRIVATE
  "${JPEG_TURBO_LIB}" windowscodecs ole32
)
