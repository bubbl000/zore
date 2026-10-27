#pragma once

#include <cstddef>
#include <cstdint>
#include <string>

namespace nv {

// Minimal ICC v2/v4 header + tag-table reader. It is only used to decide
// whether an embedded profile is sRGB (in which case the decode path can keep
// the pixels untouched) and to put a readable name in the log. It does not
// interpret color transforms; WIC/ICM does that.
bool IsValidIccProfile(const uint8_t* data, size_t size);

// Human-readable 'desc' (v2) or 'mluc' (v4) text, empty when unavailable.
std::string IccProfileDescription(const uint8_t* data, size_t size);

// True when a valid profile's description identifies the sRGB / IEC 61966-2.1
// color space. Invalid or unrecognized profiles return false.
bool IsSrgbProfile(const uint8_t* data, size_t size);

}  // namespace nv
