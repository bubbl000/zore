# M3-1: Shell thumbnail provider. A separate in-proc COM DLL so Explorer can
# load only the decode stack (no UI/render/settings). D3D/D2D are delay-loaded:
# only SVG thumbnails touch Direct2D, and loading d3d11 into every Explorer
# process would otherwise slow the common JPEG/PNG path.
add_library(zoreview_thumbshell SHARED
  src/shell/thumbnail_provider.cpp
  src/shell/dllmain.cpp
  src/shell/ZoreViewThumb.def
  src/shell/version.rc
)
set_target_properties(zoreview_thumbshell PROPERTIES
  OUTPUT_NAME ZoreViewThumb
)
target_compile_definitions(zoreview_thumbshell PRIVATE
  ZOREVIEW_THUMBSHELL_EXPORTS
)
target_link_libraries(zoreview_thumbshell PRIVATE
  zoreview_core
  zoreview_shellreg
  advapi32 delayimp
)
if(MSVC)
  target_link_options(zoreview_thumbshell PRIVATE
    /DELAYLOAD:d3d11.dll
    /DELAYLOAD:d2d1.dll
  )
endif()
nv_set_warnings(zoreview_thumbshell)
nv_copy_asan_runtime(zoreview_thumbshell)
