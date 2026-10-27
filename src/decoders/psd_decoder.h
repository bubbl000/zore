#pragma once

#include "core/format_router.h"

#include <filesystem>

namespace nv::psd {

// PSD (version 1) and PSB (version 2) merged-image decoding.
//
// Only the flattened composite stored at the end of the file is read; layer
// data is skipped. Layers are irrelevant for a viewer and the merged image is
// what Photoshop shows with "Maximize Compatibility" enabled.
ProbeResult Probe(const std::filesystem::path& path);
DecodeResult Decode(const std::filesystem::path& path,
                    const DecodeOptions& options);

}  // namespace nv::psd
