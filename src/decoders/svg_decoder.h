#pragma once

#include "core/format_router.h"

#include <filesystem>

namespace nv::svg {

// SVG is rasterized with Direct2D's SVG document API. The decoder keeps a
// per-thread D3D11/D2D device so directory thumbnails do not pay the ~200 ms
// device creation cost for every file.
ProbeResult Probe(const std::filesystem::path& path);
DecodeResult Decode(const std::filesystem::path& path,
                    const DecodeOptions& options);

// Releases the calling thread's cached Direct2D/D3D device. Worker loops call
// this before exiting: destroying D2D objects from a thread_local destructor
// can deadlock during DLL thread detach.
void ReleaseThreadContext();

}  // namespace nv::svg
