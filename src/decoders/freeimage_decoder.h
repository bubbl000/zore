#pragma once

#include "core/format_router.h"

#include <filesystem>

namespace nv::freeimage {

bool Available();

// `hint` lets the router pick the plugin for formats FreeImage cannot identify
// from the bytes alone (WBMP) or where the container is ambiguous. kUnknown
// keeps the original content-sniffing behaviour.
ProbeResult Probe(const std::filesystem::path& path,
                  ImageFormat hint = ImageFormat::kUnknown);
DecodeResult Decode(const std::filesystem::path& path,
                    const DecodeOptions& options,
                    ImageFormat hint = ImageFormat::kUnknown);

}  // namespace nv::freeimage
