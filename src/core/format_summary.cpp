#include "core/format_summary.h"

#include "core/supported_extensions.h"
#include "decoders/freeimage_decoder.h"
#include "decoders/libraw_decoder.h"
#include "decoders/wic_decoder.h"

#include <algorithm>
#include <array>

namespace nv {
namespace {

// Display order of the families. Highlights (the empty-state line) come first
// in exactly this order; the remaining families only appear in "all formats".
enum class FamilyKey {
  kJpeg = 0,
  kPng,
  kWebp,
  kBmp,
  kGif,
  kTiff,
  kPsd,
  kHdr,
  kHeif,
  kRaw,
  kAvif,
  kSvg,
  kIco,
  kTga,
  kPcx,
  kPnm,
  kXbm,
  kXpm,
  kJp2,
  kJxr,
  kWbmp,
  kMng,
  kCount,
};

struct FamilySpec {
  FamilyKey key;
  const wchar_t* name;
  ImageFormat format;  // representative
  bool highlight;
  bool needs_system_codec;
};

constexpr std::array<FamilySpec, static_cast<size_t>(FamilyKey::kCount)>
    kFamilies{{
        {FamilyKey::kJpeg, L"JPG", ImageFormat::kJpeg, true, false},
        {FamilyKey::kPng, L"PNG", ImageFormat::kPng, true, false},
        {FamilyKey::kWebp, L"WebP", ImageFormat::kWebp, true, false},
        {FamilyKey::kBmp, L"BMP", ImageFormat::kBmp, true, false},
        {FamilyKey::kGif, L"GIF", ImageFormat::kGif, true, false},
        {FamilyKey::kTiff, L"TIFF", ImageFormat::kTiff, true, false},
        {FamilyKey::kPsd, L"PSD", ImageFormat::kPsd, true, false},
        // Radiance/EXR/PFM share the FreeImage float path and the fixed
        // tone-mapping step (規劃/22/16 §3.3).
        {FamilyKey::kHdr, L"HDR/EXR", ImageFormat::kHdr, true, false},
        {FamilyKey::kHeif, L"HEIC/HEIF", ImageFormat::kHeif, true, true},
        {FamilyKey::kRaw, L"RAW", ImageFormat::kRaw, true, false},
        {FamilyKey::kAvif, L"AVIF", ImageFormat::kAvif, false, true},
        {FamilyKey::kSvg, L"SVG", ImageFormat::kSvg, false, false},
        {FamilyKey::kIco, L"ICO/CUR", ImageFormat::kIco, false, false},
        {FamilyKey::kTga, L"TGA", ImageFormat::kTga, false, false},
        {FamilyKey::kPcx, L"PCX", ImageFormat::kPcx, false, false},
        {FamilyKey::kPnm, L"PNM", ImageFormat::kPnm, false, false},
        {FamilyKey::kXbm, L"XBM", ImageFormat::kXbm, false, false},
        {FamilyKey::kXpm, L"XPM", ImageFormat::kXpm, false, false},
        {FamilyKey::kJp2, L"JP2", ImageFormat::kJp2, false, false},
        {FamilyKey::kJxr, L"JXR", ImageFormat::kJxr, false, false},
        {FamilyKey::kWbmp, L"WBMP", ImageFormat::kWbmp, false, false},
        {FamilyKey::kMng, L"MNG", ImageFormat::kMng, false, false},
    }};

size_t FamilyIndex(FamilyKey key) {
  return static_cast<size_t>(key);
}

FamilyKey KeyForFormat(ImageFormat format) {
  switch (format) {
    case ImageFormat::kJpeg:
      return FamilyKey::kJpeg;
    case ImageFormat::kPng:
    case ImageFormat::kApng:
      return FamilyKey::kPng;
    case ImageFormat::kWebp:
      return FamilyKey::kWebp;
    case ImageFormat::kBmp:
      return FamilyKey::kBmp;
    case ImageFormat::kGif:
      return FamilyKey::kGif;
    case ImageFormat::kTiff:
      return FamilyKey::kTiff;
    case ImageFormat::kPsd:
      return FamilyKey::kPsd;
    case ImageFormat::kHeif:
      return FamilyKey::kHeif;
    case ImageFormat::kRaw:
      return FamilyKey::kRaw;
    case ImageFormat::kAvif:
      return FamilyKey::kAvif;
    case ImageFormat::kSvg:
      return FamilyKey::kSvg;
    case ImageFormat::kIco:
      return FamilyKey::kIco;
    case ImageFormat::kTga:
      return FamilyKey::kTga;
    case ImageFormat::kPcx:
      return FamilyKey::kPcx;
    case ImageFormat::kPnm:
      return FamilyKey::kPnm;
    case ImageFormat::kXbm:
      return FamilyKey::kXbm;
    case ImageFormat::kXpm:
      return FamilyKey::kXpm;
    case ImageFormat::kHdr:
    case ImageFormat::kExr:
    case ImageFormat::kPfm:
      return FamilyKey::kHdr;
    case ImageFormat::kJp2:
      return FamilyKey::kJp2;
    case ImageFormat::kJxr:
      return FamilyKey::kJxr;
    case ImageFormat::kWbmp:
      return FamilyKey::kWbmp;
    case ImageFormat::kMng:
      return FamilyKey::kMng;
    case ImageFormat::kUnknown:
      break;
  }
  return FamilyKey::kCount;
}

bool FamilyAvailable(FamilyKey key, const FormatCapabilities& caps) {
  switch (key) {
    case FamilyKey::kJpeg:
    case FamilyKey::kPng:
    case FamilyKey::kWebp:
    case FamilyKey::kBmp:
    case FamilyKey::kGif:
    case FamilyKey::kTiff:
    case FamilyKey::kIco:
      return caps.wic;
    case FamilyKey::kPsd:
      return caps.psd;
    case FamilyKey::kSvg:
      return true;
    case FamilyKey::kHdr:
    case FamilyKey::kTga:
    case FamilyKey::kPcx:
    case FamilyKey::kPnm:
    case FamilyKey::kXbm:
    case FamilyKey::kXpm:
    case FamilyKey::kWbmp:
    case FamilyKey::kMng:
      return caps.freeimage;
    case FamilyKey::kJp2:
      return caps.freeimage || caps.jp2;
    case FamilyKey::kJxr:
      return caps.freeimage || caps.jxr;
    case FamilyKey::kRaw:
      // FreeImage falls back to its own RAW plugins when LibRaw is absent.
      return caps.libraw || caps.freeimage;
    case FamilyKey::kHeif:
      return caps.heif;
    case FamilyKey::kAvif:
      return caps.avif;
    case FamilyKey::kCount:
      break;
  }
  return false;
}

}  // namespace

FormatSummary BuildFormatSummary(const FormatCapabilities& caps) {
  return BuildFormatSummary(caps, SupportedExtensions());
}

FormatSummary BuildFormatSummary(const FormatCapabilities& caps,
                                 const std::vector<std::wstring>& extensions) {
  FormatSummary summary;
  summary.families.resize(kFamilies.size());
  for (size_t i = 0; i < kFamilies.size(); ++i) {
    FormatFamilyInfo& family = summary.families[i];
    family.format = kFamilies[i].format;
    family.name = kFamilies[i].name;
    family.highlight = kFamilies[i].highlight;
    family.needs_system_codec = kFamilies[i].needs_system_codec;
    family.available = FamilyAvailable(kFamilies[i].key, caps);
  }

  // Group every routed extension through the router itself. A family that has
  // no extensions is not a format this build can open and is dropped below.
  for (const std::wstring& extension : extensions) {
    const ImageFormat format = FormatRouter::FormatForExtension(extension);
    if (format == ImageFormat::kUnknown) continue;
    const FamilyKey key = KeyForFormat(format);
    if (key == FamilyKey::kCount) continue;
    FormatFamilyInfo& family = summary.families[FamilyIndex(key)];
    if (std::ranges::find(family.extensions, extension) ==
        family.extensions.end()) {
      family.extensions.push_back(extension);
    }
  }

  std::vector<FormatFamilyInfo> families;
  families.reserve(summary.families.size());
  for (FormatFamilyInfo& family : summary.families) {
    if (family.extensions.empty()) continue;
    summary.routed_extension_count += family.extensions.size();
    if (family.available) {
      summary.extension_count += family.extensions.size();
      if (family.highlight) summary.highlights.push_back(family.name);
    }
    families.push_back(std::move(family));
  }
  summary.families = std::move(families);
  return summary;
}

FormatCapabilities ProbeFormatCapabilities() {
  FormatCapabilities caps;
  caps.wic = true;
  caps.psd = true;
  caps.freeimage = freeimage::Available();
  caps.libraw = libraw::Available();
  caps.heif = wic::DecoderSupportsExtension(L".heic") ||
              wic::DecoderSupportsExtension(L".heif");
  caps.avif = wic::DecoderSupportsExtension(L".avif");
  caps.jp2 = wic::DecoderSupportsExtension(L".jp2");
  caps.jxr = wic::DecoderSupportsExtension(L".jxr") ||
             wic::DecoderSupportsExtension(L".wdp");
  return caps;
}

const FormatCapabilities& FormatCapabilitiesForThisBuild() {
  static const FormatCapabilities caps = ProbeFormatCapabilities();
  return caps;
}

const FormatSummary& GetFormatSummary() {
  static const FormatSummary summary =
      BuildFormatSummary(FormatCapabilitiesForThisBuild());
  return summary;
}

}  // namespace nv
