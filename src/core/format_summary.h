#pragma once

#include "core/format_router.h"

#include <cstddef>
#include <string>
#include <vector>

namespace nv {

// Which families the current build/system can actually decode. Everything the
// empty state advertises is derived from this, never from a hand-written list
// (規劃/22/03 §3.3): a Debug build without FreeImage/LibRaw, or a machine
// without the HEIF/AV1 Store extensions, must not claim those formats.
struct FormatCapabilities {
  bool wic = true;  // JPEG/PNG/BMP/GIF/TIFF/WebP/ICO (always in this build)
  bool psd = true;  // the in-tree PSD/PSB decoder is always compiled in
  bool freeimage = false;
  bool libraw = false;
  bool heif = false;
  bool avif = false;
  // Optional Windows codecs, probed like heif/avif; FreeImage is the fallback.
  bool jp2 = false;
  bool jxr = false;
};

// One row of the format summary. Families group every extension that routes to
// the same decoder (PNG+APNG, PSD+PSB, ...).
struct FormatFamilyInfo {
  ImageFormat format = ImageFormat::kUnknown;  // representative format
  std::wstring name;                           // "JPG", "HEIC/HEIF", ...
  std::vector<std::wstring> extensions;        // ".jpg", ".jpeg", ...
  bool available = true;
  // Shown in the empty-state format line (the rest only in "all formats").
  bool highlight = false;
  // The decoder comes from a Windows optional component (HEIF/AVIF).
  bool needs_system_codec = false;
};

struct FormatSummary {
  std::vector<FormatFamilyInfo> families;  // display order
  // Available highlight names in display order; the empty state joins these.
  std::vector<std::wstring> highlights;
  // Number of extensions that are actually decodable in this build/system.
  size_t extension_count = 0;
  // Number of extensions the router knows (may exceed extension_count).
  size_t routed_extension_count = 0;
};

// Queries the build and the installed system codecs. Cached by GetFormatSummary;
// call it once during startup (it creates a WIC factory).
FormatCapabilities ProbeFormatCapabilities();

// The probed capabilities of this process (probed on first use, then cached).
const FormatCapabilities& FormatCapabilitiesForThisBuild();

// Pure derivation from the capability flags: unit-testable without a codec.
// The overload takes an explicit extension table so tests can prove that the
// summary follows the router table instead of a hard-coded line (規劃/22/03 §9).
FormatSummary BuildFormatSummary(const FormatCapabilities& caps);
FormatSummary BuildFormatSummary(const FormatCapabilities& caps,
                                 const std::vector<std::wstring>& extensions);

// Probe once, build once. Never reports a format that cannot be opened.
const FormatSummary& GetFormatSummary();

}  // namespace nv
