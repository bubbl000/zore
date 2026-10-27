cmake_minimum_required(VERSION 3.25)
# RC supplies the VERSIONINFO resources shared by the exe, CLI and Shell DLL.
# The numeric version itself lives only in src/core/version_numbers.h.
project(ZoreView LANGUAGES C CXX RC)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release CACHE STRING "" FORCE)
endif()

# M1-4: AddressSanitizer for memory-error hunting (use-after-free, overflow).
# MSVC does not ship LeakSanitizer on Windows, so leaks are handled separately
# by the Debug CRT (see src/platform/leak_check.* and zoreview_leaktests).
option(ZOREVIEW_ASAN "Build with AddressSanitizer (MSVC only)" OFF)
if(ZOREVIEW_ASAN)
  if(NOT MSVC)
    message(FATAL_ERROR "ZOREVIEW_ASAN requires MSVC")
  endif()
  # /RTC1 is incompatible with /fsanitize=address in Debug builds.
  string(REPLACE "/RTC1" "" CMAKE_CXX_FLAGS_DEBUG "${CMAKE_CXX_FLAGS_DEBUG}")
  add_compile_options(/fsanitize=address)
  add_link_options(/INFERASANLIBS)
  add_compile_definitions(NV_ASAN=1)
  # The prebuilt FreeImage/LibRaw archives were compiled without ASan. MSVC's
  # default STL container annotations change a detect_mismatch value and pull in
  # stl_asan, which those archives do not agree with, so turn them off.
  add_compile_definitions(_DISABLE_STL_ANNOTATION)
  message(STATUS "ZoreView: AddressSanitizer enabled")
endif()

function(nv_set_warnings target)
  if(MSVC)
    target_compile_options(${target} PRIVATE
      /W4 /WX /permissive- /EHsc /utf-8 /Zc:__cplusplus
    )
  endif()
endfunction()

# The ASan runtime is a DLL that must sit next to the executable when it is
# launched directly (Visual Studio's debugger normally adds it to PATH).
function(nv_copy_asan_runtime target)
  if(NOT ZOREVIEW_ASAN)
    return()
  endif()
  get_filename_component(_nv_compiler_dir "${CMAKE_CXX_COMPILER}" DIRECTORY)
  foreach(_dll clang_rt.asan_dbg_dynamic-x86_64.dll
               clang_rt.asan_dynamic-x86_64.dll)
    if(EXISTS "${_nv_compiler_dir}/${_dll}")
      add_custom_command(TARGET ${target} POST_BUILD
        COMMAND "${CMAKE_COMMAND}" -E copy_if_different
                "${_nv_compiler_dir}/${_dll}" "$<TARGET_FILE_DIR:${target}>/"
        VERBATIM)
    endif()
  endforeach()
endfunction()

# FreeImage is optional for M0. If a previous build tree provides it,
# link it for a wider format matrix; otherwise the CLI still works through WIC.
set(ZOREVIEW_FREEIMAGE_INCLUDE "" CACHE PATH "FreeImage Source directory")
set(ZOREVIEW_FREEIMAGE_LIB "" CACHE FILEPATH "FreeImage static library")
set(ZOREVIEW_LIBRAW_INCLUDE "" CACHE PATH "LibRaw include root (contains libraw/libraw.h)")
set(ZOREVIEW_LIBRAW_LIB "" CACHE FILEPATH "LibRaw static library")
set(ZOREVIEW_JPEG_LIB "" CACHE FILEPATH "JPEG static library used by FreeImage")
# The prebuilt FreeImage/LibRaw archives use the Release CRT (/MD). Debug
# builds cannot link them, so Debug + CRT heap leak checking is built with
# FreeImage off (`-NoFreeImage` in tools/build.ps1); ASan uses RelWithDebInfo.
option(ZOREVIEW_USE_FREEIMAGE "Link the prebuilt FreeImage fallback" ON)

# M2: everything now lives in this project's third_party tree. The old
# A sibling build tree is only a fallback for a clone without the vendored
# binaries.
if(ZOREVIEW_USE_FREEIMAGE AND NOT ZOREVIEW_FREEIMAGE_INCLUDE)
  set(_nv_tp "${CMAKE_CURRENT_SOURCE_DIR}/third_party")
  if(EXISTS "${_nv_tp}/FreeImage/FreeImage.h")
    set(ZOREVIEW_FREEIMAGE_INCLUDE
        "${_nv_tp}/FreeImage" CACHE PATH "" FORCE)
    set(ZOREVIEW_FREEIMAGE_LIB
        "${_nv_tp}/lib/freeimage.lib" CACHE FILEPATH "" FORCE)
    set(ZOREVIEW_LIBRAW_INCLUDE
        "${_nv_tp}/LibRaw-0.21.5" CACHE PATH "" FORCE)
    set(ZOREVIEW_LIBRAW_LIB
        "${_nv_tp}/lib/libraw.lib" CACHE FILEPATH "" FORCE)
    set(ZOREVIEW_JPEG_LIB
        "${_nv_tp}/lib/libjpeg.lib" CACHE FILEPATH "" FORCE)
  else()
    message(FATAL_ERROR
        "third_party/ is missing its prebuilt libraries; restore the vendored "
        "FreeImage/LibRaw/libjpeg binaries (see third_party/README) or pass "
        "-DZOREVIEW_FREEIMAGE_LIB=... -DZOREVIEW_LIBRAW_LIB=... explicitly.")
  endif()
endif()

# The existing cache may already contain the FreeImage paths from an older
# configure, so fill the LibRaw include root independently.
if(ZOREVIEW_USE_FREEIMAGE AND NOT ZOREVIEW_LIBRAW_INCLUDE)
  set(_nv_tp_libraw "${CMAKE_CURRENT_SOURCE_DIR}/third_party")
  if(EXISTS "${_nv_tp_libraw}/LibRaw-0.21.5/libraw/libraw.h")
    set(ZOREVIEW_LIBRAW_INCLUDE
        "${_nv_tp_libraw}/LibRaw-0.21.5" CACHE PATH "" FORCE)
  else()
    message(FATAL_ERROR
        "third_party/LibRaw-0.21.5 is missing; pass -DZOREVIEW_LIBRAW_INCLUDE=... explicitly.")
  endif()
endif()

# zlib 1.2.11 (vendored next to FreeImage) is used by the PSD ZIP codec.
add_library(zoreview_zlib STATIC
  third_party/zlib/adler32.c
  third_party/zlib/crc32.c
  third_party/zlib/inffast.c
  third_party/zlib/inflate.c
  third_party/zlib/inftrees.c
  third_party/zlib/uncompr.c
  third_party/zlib/zutil.c
)
target_include_directories(zoreview_zlib PUBLIC third_party/zlib)
if(MSVC)
  target_compile_options(zoreview_zlib PRIVATE /w)
endif()


add_library(zoreview_core STATIC
  src/core/format_router.cpp
  src/core/animation_controller.cpp
  src/core/format_summary.cpp
  src/core/preview_plan.cpp
  src/core/supported_extensions.cpp
  src/core/ui_error.cpp
  src/core/folder_navigator.cpp
  src/core/folder_watcher.cpp
  src/core/icc_profile.cpp
  src/core/image_transform.cpp
  src/core/disk_cache.cpp
  src/core/disk_cache_writer.cpp
  src/core/display_tier.cpp
  src/core/tier_payload.cpp
  src/core/decode_service.cpp
  src/core/thumbnail_service.cpp
  src/core/exif.cpp
  src/core/exif_service.cpp
  src/platform/paths.cpp
  src/platform/dpi.cpp
  src/platform/i18n.cpp
  src/platform/theme.cpp
  src/platform/settings.cpp
  src/platform/single_instance.cpp
  src/platform/window_placement.cpp
  src/platform/dir_notify.cpp
  src/platform/log.cpp
  src/platform/leak_check.cpp
  src/decoders/wic_decoder.cpp
  src/decoders/freeimage_decoder.cpp
  src/decoders/libraw_decoder.cpp
  src/decoders/psd_decoder.cpp
  src/decoders/svg_decoder.cpp
  src/decoders/gif_decoder.cpp
  src/decoders/apng_decoder.cpp
  src/decoders/webp_anim.cpp
)

target_include_directories(zoreview_core PUBLIC src)
target_compile_definitions(zoreview_core PUBLIC
  UNICODE
  _UNICODE
  WIN32_LEAN_AND_MEAN
  NOMINMAX
  _WIN32_WINNT=0x0A00
  WINVER=0x0A00
)
target_link_libraries(zoreview_core PUBLIC windowscodecs ole32 zoreview_zlib d3d11 dxgi d2d1 shlwapi)
nv_set_warnings(zoreview_core)

if(ZOREVIEW_USE_FREEIMAGE AND
   ZOREVIEW_FREEIMAGE_INCLUDE AND ZOREVIEW_FREEIMAGE_LIB AND
   EXISTS "${ZOREVIEW_FREEIMAGE_INCLUDE}/FreeImage.h" AND
   EXISTS "${ZOREVIEW_FREEIMAGE_LIB}")
  message(STATUS "ZoreView: FreeImage enabled (${ZOREVIEW_FREEIMAGE_LIB})")
  target_compile_definitions(zoreview_core PUBLIC NV_WITH_FREEIMAGE FREEIMAGE_LIB)
  target_include_directories(zoreview_core PUBLIC "${ZOREVIEW_FREEIMAGE_INCLUDE}")
  # FreeImage 3.18's header contains a non-UTF-8 author byte; /utf-8 + /WX would fail.
  target_compile_options(zoreview_core PRIVATE /wd4828)
  target_link_libraries(zoreview_core PUBLIC "${ZOREVIEW_FREEIMAGE_LIB}")
  if(EXISTS "${ZOREVIEW_LIBRAW_LIB}" AND
     EXISTS "${ZOREVIEW_LIBRAW_INCLUDE}/libraw/libraw.h")
    message(STATUS "ZoreView: LibRaw enabled (${ZOREVIEW_LIBRAW_LIB})")
    target_compile_definitions(zoreview_core PUBLIC NV_WITH_LIBRAW LIBRAW_NODLL)
    target_include_directories(zoreview_core PUBLIC "${ZOREVIEW_LIBRAW_INCLUDE}")
    target_link_libraries(zoreview_core PUBLIC "${ZOREVIEW_LIBRAW_LIB}")
  endif()
  if(EXISTS "${ZOREVIEW_JPEG_LIB}")
    target_link_libraries(zoreview_core PUBLIC "${ZOREVIEW_JPEG_LIB}")
  endif()
else()
  message(STATUS "ZoreView: FreeImage disabled (WIC only)")
endif()

# v2.0 S0: custom-drawn UI skeleton (tokens, layout, hit stack, DWrite text,
# vector icons, title/bottom chrome). Separate from zoreview_core so the pure
# layout tests can link only what they need.
add_library(zoreview_ui STATIC
  src/ui/theme_tokens.cpp
  src/ui/layout.cpp
src/ui/minimap.cpp
  src/ui/command.cpp
  src/ui/menu.cpp
  src/ui/settings_layout.cpp
  src/ui/overlay_stack.cpp
  src/ui/window_state.cpp
  src/ui/ui_context.cpp
  src/ui/draw_util.cpp
  src/ui/icons.cpp
  src/ui/app_logo.cpp
  src/ui/chrome.cpp
  src/ui/info_panel.cpp
  src/ui/empty_state.cpp
  src/ui/error_card.cpp
  src/ui/loading_state.cpp
  src/ui/toast.cpp
  src/ui/strip_tooltip.cpp
  src/ui/anim_pill.cpp
  src/ui/preview_badge.cpp
  src/ui/ui_root.cpp
)
target_link_libraries(zoreview_ui PUBLIC
  zoreview_core
  d2d1 dwrite user32 gdi32
)
nv_set_warnings(zoreview_ui)

# M3-1/M3-2b: thumbnail registration lives in a static library so both the
# in-proc handler DLL and the installer/tests share one implementation without
# making the application depend on the DLL at load time.
add_library(zoreview_shellreg STATIC
  src/shell/thumbnail_registry.cpp
)
target_link_libraries(zoreview_shellreg PUBLIC
  zoreview_core
  advapi32 shell32 ole32
)
nv_set_warnings(zoreview_shellreg)

# File operations shared by the app and the automated file-op harness:
# atomic image save (WIC), recycle-bin delete/rename, clipboard DIB.
add_library(zoreview_platform_ops STATIC
  src/platform/image_io.cpp
  src/platform/gif_encoder.cpp
  src/platform/encoder_probe.cpp
  src/platform/save_service.cpp
  src/platform/file_ops.cpp
  src/platform/clipboard.cpp
  src/platform/drag_drop.cpp
  src/platform/file_data_object.cpp
  src/platform/file_drop_source.cpp
  src/platform/shell_integration.cpp
  src/platform/file_association.cpp
  src/platform/installer.cpp
)
target_include_directories(zoreview_platform_ops PUBLIC src)
target_compile_definitions(zoreview_platform_ops PUBLIC
  UNICODE
  _UNICODE
  WIN32_LEAN_AND_MEAN
  NOMINMAX
  _WIN32_WINNT=0x0A00
  WINVER=0x0A00
)
# FreeImage 3.18's header contains a non-UTF-8 author byte; /utf-8 + /WX would
# fail exactly like it does for zoreview_core.
target_compile_options(zoreview_platform_ops PRIVATE /wd4828)
target_link_libraries(zoreview_platform_ops PUBLIC
  zoreview_core
  zoreview_shellreg
  windowscodecs ole32 shell32 advapi32 uuid shlwapi
)
nv_set_warnings(zoreview_platform_ops)

add_executable(zoreview WIN32
  src/app/main.cpp
  src/render/renderer.cpp
  src/render/thumbnail_strip.cpp
  src/ui/settings_window.cpp
  src/ui/info_window.cpp
  src/platform/crash.cpp
  src/platform/screenshot.cpp
  src/platform/window_capture.cpp
  src/platform/input_dialog.cpp
  src/platform/save_dialog.cpp
  src/app/app.manifest
  src/app/version.rc
)
target_link_libraries(zoreview PRIVATE
  zoreview_core
  zoreview_ui
  zoreview_platform_ops
  d3d11 dxgi d2d1 dwmapi dbghelp psapi
  user32 gdi32 shell32 comdlg32 imm32 comctl32
)
nv_set_warnings(zoreview)
nv_copy_asan_runtime(zoreview)

if(MSVC)
  target_link_options(zoreview PRIVATE
    "/MANIFEST:EMBED"
    "/MANIFESTINPUT:${CMAKE_CURRENT_SOURCE_DIR}/src/app/app.manifest"
  )
endif()

add_executable(zoreview_cli src/cli/main.cpp src/cli/version.rc)
target_link_libraries(zoreview_cli PRIVATE zoreview_core)
nv_set_warnings(zoreview_cli)
nv_copy_asan_runtime(zoreview_cli)

# The test sources are kept out of the public repository (they are developed
# locally), so every test target is guarded: a clone without tests/ still
# configures and builds the application. Pass -DNV_BUILD_TESTS=OFF to skip
# them even when the directory is present.
option(NV_BUILD_TESTS "Build the unit/regression test executables" ON)
set(NV_HAVE_TESTS OFF)
if(NV_BUILD_TESTS AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/tests/core_cache_test.cpp")
  set(NV_HAVE_TESTS ON)
  message(STATUS "ZoreView: test targets enabled")
add_executable(zoreview_tests
  tests/strip_layout_test.cpp
  tests/color_tests.cpp
  tests/ui_tests.cpp
  tests/ui_layout_tests.cpp
  tests/exif_test.cpp
  tests/settings_test.cpp
  tests/window_session_tests.cpp
  tests/folder_watch_tests.cpp
  src/render/thumbnail_strip.cpp
)
target_link_libraries(zoreview_tests PRIVATE zoreview_core zoreview_ui d2d1)
target_include_directories(zoreview_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
nv_set_warnings(zoreview_tests)
nv_copy_asan_runtime(zoreview_tests)

# M1-4: leak / stability harness. Exercises the decode, thumbnail and disk
# caches the same way the UI does, then compares Debug CRT heap checkpoints so
# per-round growth is measured without a debugger.
add_executable(zoreview_leaktests tests/leak_test.cpp)
target_link_libraries(zoreview_leaktests PRIVATE zoreview_core)
nv_set_warnings(zoreview_leaktests)
nv_copy_asan_runtime(zoreview_leaktests)

# M1-6: save/rename/delete/clipboard checks, no GUI required.
add_executable(zoreview_filetests tests/file_ops_test.cpp)
target_link_libraries(zoreview_filetests PRIVATE zoreview_platform_ops shell32)
nv_set_warnings(zoreview_filetests)
nv_copy_asan_runtime(zoreview_filetests)

# V3-04: display-tier / disk-cache v3 checks (cache key format, tier ladder,
# upward tier lookup, header version rejection, JPEG/PNG payload round-trip,
# WIC downscale). Pure CPU apart from the WIC round-trip; no window is created.
add_executable(zoreview_coretests tests/core_cache_test.cpp)
target_link_libraries(zoreview_coretests PRIVATE zoreview_core)
nv_set_warnings(zoreview_coretests)
nv_copy_asan_runtime(zoreview_coretests)

# v2.0 S3 (22/19): drag-out data object / drop source / trigger policy.
add_executable(zoreview_dragtests tests/drag_out_test.cpp)
target_link_libraries(zoreview_dragtests PRIVATE zoreview_platform_ops)
nv_set_warnings(zoreview_dragtests)
nv_copy_asan_runtime(zoreview_dragtests)

# S1.5 (22/16): generate the extra-format samples with the same FreeImage build
# the application links. Formats whose plugin cannot write are reported as SKIP.
# The generator needs FreeImage headers, so it is not part of -NoFreeImage.
if(ZOREVIEW_USE_FREEIMAGE)
  add_executable(zoreview_samplegen tests/make_extra_samples.cpp)
  target_link_libraries(zoreview_samplegen PRIVATE zoreview_core)
  target_compile_options(zoreview_samplegen PRIVATE /wd4828)
  nv_set_warnings(zoreview_samplegen)
  nv_copy_asan_runtime(zoreview_samplegen)
endif()

# M2: PSD/PSB, HEIC/AVIF, multi-page TIFF and corrupt-sample checks.
add_executable(zoreview_formattests tests/format_extra_test.cpp)
target_link_libraries(zoreview_formattests PRIVATE zoreview_core)
nv_set_warnings(zoreview_formattests)
nv_copy_asan_runtime(zoreview_formattests)

# M3-1: Explorer thumbnail handler (ZoreViewThumb.dll).
include(cmake/shell_thumbnail.cmake)

# M3-1: direct COM + Shell (IShellItemImageFactory) thumbnail checks.
add_executable(zoreview_shelltests tests/shell_thumbnail_test.cpp)
target_link_libraries(zoreview_shelltests PRIVATE
  zoreview_core
  zoreview_thumbshell
  shell32 ole32 advapi32
)
nv_set_warnings(zoreview_shelltests)
nv_copy_asan_runtime(zoreview_shelltests)

# M3-1: Shell-only probe that does not link the handler DLL, so the handler is
# discovered/loaded from the registry exactly like Explorer does.
add_executable(zoreview_shellprobe tests/shell_probe.cpp)
target_link_libraries(zoreview_shellprobe PRIVATE shell32 ole32 gdi32)
nv_set_warnings(zoreview_shellprobe)
nv_copy_asan_runtime(zoreview_shellprobe)

# M3-2: context-menu verbs and their headless commands.
add_executable(zoreview_shellverbtests tests/shell_verbs_test.cpp)
target_link_libraries(zoreview_shellverbtests PRIVATE
  zoreview_platform_ops
  zoreview_core
  shell32 ole32 advapi32 user32
)
nv_set_warnings(zoreview_shellverbtests)
nv_copy_asan_runtime(zoreview_shellverbtests)

# M3-2b: portable mode, optional associations and the per-user installer.
add_executable(zoreview_installtests tests/install_test.cpp)
target_link_libraries(zoreview_installtests PRIVATE
  zoreview_platform_ops
  zoreview_shellreg
  zoreview_core
  shell32 ole32 advapi32 user32
)
nv_set_warnings(zoreview_installtests)
nv_copy_asan_runtime(zoreview_installtests)
endif()  # NV_HAVE_TESTS

