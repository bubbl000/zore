#pragma once

#include <cstdint>
#include <filesystem>

namespace nv::platform {

// Writes a top-down BGRA8 buffer as a PNG. Used by the --screenshot test mode
// so UI verification does not depend on desktop capture.
bool SavePng(const std::filesystem::path& path, const uint8_t* bgra,
             uint32_t width, uint32_t height, uint32_t stride);

}  // namespace nv::platform
