#include "core/format_router.h"

#include "decoders/apng_decoder.h"
#include "decoders/freeimage_decoder.h"
#include "decoders/gif_decoder.h"
#include "decoders/libraw_decoder.h"
#include "decoders/psd_decoder.h"
#include "decoders/svg_decoder.h"
#include "decoders/webp_anim.h"
#include "decoders/wic_decoder.h"
#include "platform/log.h"

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <cwctype>
#include <fstream>
#include <string>

namespace nv {
namespace {

constexpr int kRouteErrorBase = 1000;
constexpr size_t kHeaderBytes = 4096;

// WINCODEC_ERR_COMPONENTNOTFOUND: no WIC decoder is installed for the file.
constexpr HRESULT kCodecNotFound = static_cast<HRESULT>(0x88982F50L);

std::wstring LowerExtension(const std::filesystem::path& path) {
  std::wstring extension = path.extension().wstring();
  std::ranges::transform(extension, extension.begin(),
                         [](wchar_t c) { return static_cast<wchar_t>(std::towlower(c)); });
  return extension;
}

ImageFormat FormatFromExtension(const std::wstring& extension) {
  if (extension == L".jpg" || extension == L".jpeg" || extension == L".jpe" ||
      extension == L".jfif") {
    return ImageFormat::kJpeg;
  }
  if (extension == L".png") return ImageFormat::kPng;
  if (extension == L".apng") return ImageFormat::kPng;  // acTL is checked by magic.
  if (extension == L".bmp" || extension == L".dib") return ImageFormat::kBmp;
  if (extension == L".gif") return ImageFormat::kGif;
  if (extension == L".webp") return ImageFormat::kWebp;
  if (extension == L".tif" || extension == L".tiff") return ImageFormat::kTiff;
  if (extension == L".ico" || extension == L".cur") return ImageFormat::kIco;
  if (extension == L".psd" || extension == L".psb") return ImageFormat::kPsd;
  if (extension == L".heic" || extension == L".heif" || extension == L".hif") {
    return ImageFormat::kHeif;
  }
  if (extension == L".avif" || extension == L".avifs") return ImageFormat::kAvif;
  if (extension == L".svg") return ImageFormat::kSvg;
  if (extension == L".tga" || extension == L".targa") return ImageFormat::kTga;
  if (extension == L".pcx") return ImageFormat::kPcx;
  if (extension == L".pbm" || extension == L".pgm" || extension == L".ppm" ||
      extension == L".pnm") {
    return ImageFormat::kPnm;
  }
  if (extension == L".xbm") return ImageFormat::kXbm;
  if (extension == L".xpm") return ImageFormat::kXpm;
  if (extension == L".wbmp") return ImageFormat::kWbmp;
  if (extension == L".hdr") return ImageFormat::kHdr;
  if (extension == L".exr") return ImageFormat::kExr;
  if (extension == L".pfm") return ImageFormat::kPfm;
  if (extension == L".jp2" || extension == L".j2k" || extension == L".jpc") {
    return ImageFormat::kJp2;
  }
  if (extension == L".jxr" || extension == L".wdp") return ImageFormat::kJxr;
  if (extension == L".mng" || extension == L".jng") return ImageFormat::kMng;
  if (extension == L".cr2" || extension == L".cr3" || extension == L".nef" ||
      extension == L".arw" || extension == L".raf" || extension == L".orf" ||
      extension == L".rw2" || extension == L".pef" || extension == L".srw" ||
      extension == L".dng") {
    return ImageFormat::kRaw;
  }
  return ImageFormat::kUnknown;
}

bool IsRawExtension(const std::wstring& extension) {
  return FormatFromExtension(extension) == ImageFormat::kRaw;
}

// Strong signatures are unambiguous containers whose magic must win even when
// the extension claims a RAW format (規劃/25 §5). TIFF is deliberately absent:
// CR2/NEF/ARW/DNG are TIFF-based, so the extension still owns that family.
bool IsStrongMagic(ImageFormat format) {
  switch (format) {
    case ImageFormat::kJpeg:
    case ImageFormat::kPng:
    case ImageFormat::kGif:
    case ImageFormat::kBmp:
    case ImageFormat::kWebp:
    case ImageFormat::kHeif:
    case ImageFormat::kAvif:
    case ImageFormat::kJp2:
      return true;
    default:
      return false;
  }
}

bool IsHeifBrand(const std::array<uint8_t, 4>& brand) {
  static constexpr std::array<std::array<uint8_t, 4>, 10> kBrands{{
      {{'h', 'e', 'i', 'c'}}, {{'h', 'e', 'i', 'x'}},
      {{'h', 'e', 'v', 'c'}}, {{'h', 'e', 'i', 'm'}},
      {{'h', 'e', 'i', 's'}}, {{'h', 'e', 'v', 'm'}},
      {{'h', 'e', 'v', 's'}}, {{'m', 'i', 'f', '1'}},
      {{'m', 'i', 'f', '2'}}, {{'m', 's', 'f', '1'}},
  }};
  return std::ranges::any_of(kBrands, [&](const auto& candidate) {
    return candidate == brand;
  });
}

bool IsAvifBrand(const std::array<uint8_t, 4>& brand) {
  return brand == std::array<uint8_t, 4>{{'a', 'v', 'i', 'f'}} ||
         brand == std::array<uint8_t, 4>{{'a', 'v', 'i', 's'}};
}

ImageFormat DetectIsoBmff(const std::array<uint8_t, kHeaderBytes>& header,
                         size_t size) {
  if (size < 12) return ImageFormat::kUnknown;
  if (std::memcmp(header.data() + 4, "ftyp", 4) != 0) return ImageFormat::kUnknown;

  uint32_t box_size = (static_cast<uint32_t>(header[0]) << 24) |
                      (static_cast<uint32_t>(header[1]) << 16) |
                      (static_cast<uint32_t>(header[2]) << 8) |
                      static_cast<uint32_t>(header[3]);
  if (box_size < 16) box_size = 16;
  const size_t brand_bytes = std::min<size_t>(
      size, std::min<size_t>(box_size, kHeaderBytes));

  bool heif = false;
  for (size_t offset = 8; offset + 4 <= brand_bytes; offset += 4) {
    std::array<uint8_t, 4> brand{};
    std::memcpy(brand.data(), header.data() + offset, 4);
    if (IsAvifBrand(brand)) return ImageFormat::kAvif;
    if (IsHeifBrand(brand)) heif = true;
  }
  // QuickTime-style ftyp boxes can have a 64-bit size ("ftyp" after a
  // 4-byte size of 1); the major brand still sits at offset 8, so this loop
  // already covers it.
  return heif ? ImageFormat::kHeif : ImageFormat::kUnknown;
}

bool LooksLikeSvg(const std::array<uint8_t, kHeaderBytes>& header, size_t size) {  size_t start = 0;
  if (size >= 3 && header[0] == 0xEF && header[1] == 0xBB && header[2] == 0xBF) {
    start = 3;
  }
  while (start < size && (header[start] == ' ' || header[start] == '\t' ||
                          header[start] == '\r' || header[start] == '\n')) {
    ++start;
  }
  if (start >= size || header[start] != '<') return false;
  for (size_t i = start; i + 4 <= size; ++i) {
    if (header[i] == '<' && (header[i + 1] == 's' || header[i + 1] == 'S') &&
        (header[i + 2] == 'v' || header[i + 2] == 'V') &&
        (header[i + 3] == 'g' || header[i + 3] == 'G')) {
      const size_t after = i + 4;
      if (after >= size || header[after] == '>' || header[after] == '/' ||
          header[after] == ' ' || header[after] == '\t' ||
          header[after] == '\r' || header[after] == '\n') {
        return true;
      }
    }
  }
  return false;
}

bool LooksLikePcx(const std::array<uint8_t, kHeaderBytes>& header, size_t size) {
  return size >= 128 && header[0] == 0x0A && header[2] == 1;
}

bool LooksLikePnm(const std::array<uint8_t, kHeaderBytes>& header, size_t size) {
  if (size < 3 || header[0] != 'P' || header[1] < '1' || header[1] > '6') {
    return false;
  }
  return header[2] == ' ' || header[2] == '\t' || header[2] == '\r' ||
         header[2] == '\n';
}

bool ContainsAscii(const std::array<uint8_t, kHeaderBytes>& header, size_t size,
                   const char* needle, size_t offset = 0) {
  const size_t length = std::strlen(needle);
  if (length == 0 || size < length) return false;
  for (size_t i = offset; i + length <= size; ++i) {
    if (std::memcmp(header.data() + i, needle, length) == 0) return true;
  }
  return false;
}

bool LooksLikeXbm(const std::array<uint8_t, kHeaderBytes>& header, size_t size) {
  return ContainsAscii(header, size, "#define") &&
         ContainsAscii(header, size, "_width");
}

bool LooksLikeXpm(const std::array<uint8_t, kHeaderBytes>& header, size_t size) {
  return ContainsAscii(header, size, "/* XPM */");
}

ImageFormat DetectByMagic(const std::array<uint8_t, kHeaderBytes>& header,
                          size_t size) {
  // Float/HDR families first: EXR starts with binary bytes, HDR with '#' (which
  // would otherwise be inspected by XBM), PFM with 'P' (PNM is P1-P6 only).
  if (size >= 4 && header[0] == 0x76 && header[1] == 0x2F &&
      header[2] == 0x31 && header[3] == 0x01) {
    return ImageFormat::kExr;
  }
  if (size >= 6 &&
      ((size >= 10 && std::memcmp(header.data(), "#?RADIANCE", 10) == 0) ||
       std::memcmp(header.data(), "#?RGBE", 6) == 0)) {
    return ImageFormat::kHdr;
  }
  if (size >= 3 && header[0] == 'P' &&
      (header[1] == 'F' || header[1] == 'f') &&
      (header[2] == '\n' || header[2] == '\r' || header[2] == ' ' ||
       header[2] == '\t')) {
    return ImageFormat::kPfm;
  }
  if (size >= 12 && header[0] == 0x00 && header[1] == 0x00 &&
      header[2] == 0x00 && header[3] == 0x0C && header[4] == 0x6A &&
      header[5] == 0x50 && header[6] == 0x20 && header[7] == 0x20 &&
      header[8] == 0x0D && header[9] == 0x0A && header[10] == 0x87 &&
      header[11] == 0x0A) {
    return ImageFormat::kJp2;
  }
  if (size >= 4 && header[0] == 0xFF && header[1] == 0x4F &&
      header[2] == 0xFF && header[3] == 0x51) {
    return ImageFormat::kJp2;
  }
  if (size >= 4 &&
      ((header[0] == 0x49 && header[1] == 0x49 && header[2] == 0xBC &&
        header[3] == 0x01) ||
       (header[0] == 0x4D && header[1] == 0x4D && header[2] == 0x01 &&
        header[3] == 0xBC))) {
    return ImageFormat::kJxr;
  }
  if (size >= 8 && ((header[0] == 0x8A || header[0] == 0x8B) &&
                    std::memcmp(header.data() + 1, "MNG\r\n\x1a\n", 7) == 0)) {
    return ImageFormat::kMng;
  }
  if (size >= 3 && header[0] == 0xFF && header[1] == 0xD8 && header[2] == 0xFF) {
    return ImageFormat::kJpeg;
  }
  if (size >= 8 && header[0] == 0x89 && header[1] == 'P' && header[2] == 'N' &&
      header[3] == 'G' && header[4] == 0x0D && header[5] == 0x0A &&
      header[6] == 0x1A && header[7] == 0x0A) {
    return ImageFormat::kPng;
  }
  if (size >= 2 && header[0] == 'B' && header[1] == 'M') {
    return ImageFormat::kBmp;
  }
  if (size >= 6 && header[0] == 'G' && header[1] == 'I' && header[2] == 'F' &&
      header[3] == '8' && (header[4] == '7' || header[4] == '9') && header[5] == 'a') {
    return ImageFormat::kGif;
  }
  if (size >= 12 && header[0] == 'R' && header[1] == 'I' && header[2] == 'F' &&
      header[3] == 'F' && header[8] == 'W' && header[9] == 'E' && header[10] == 'B' &&
      header[11] == 'P') {
    return ImageFormat::kWebp;
  }
  if (size >= 4 && ((header[0] == 'I' && header[1] == 'I' && header[2] == 0x2A &&
                     header[3] == 0x00) ||
                    (header[0] == 'M' && header[1] == 'M' && header[2] == 0x00 &&
                     header[3] == 0x2A))) {
    return ImageFormat::kTiff;
  }
  if (size >= 4 && header[0] == 0x00 && header[1] == 0x00 &&
      (header[2] == 0x01 || header[2] == 0x02) && header[3] == 0x00 &&
      (header[4] != 0 || header[5] != 0)) {
    return ImageFormat::kIco;
  }
  if (size >= 6 && std::memcmp(header.data(), "8BPS", 4) == 0 &&
      ((header[4] == 0 && header[5] == 1) ||
       (header[4] == 0 && header[5] == 2))) {
    return ImageFormat::kPsd;
  }
  if (LooksLikeXpm(header, size)) return ImageFormat::kXpm;
  if (LooksLikeXbm(header, size)) return ImageFormat::kXbm;
  if (LooksLikePnm(header, size)) return ImageFormat::kPnm;
  if (LooksLikePcx(header, size)) return ImageFormat::kPcx;
  if (LooksLikeSvg(header, size)) return ImageFormat::kSvg;
  const ImageFormat bmff = DetectIsoBmff(header, size);
  if (bmff != ImageFormat::kUnknown) return bmff;
  return ImageFormat::kUnknown;
}

Error MissingCodecError(ImageFormat format) {
  if (format == ImageFormat::kJp2) {
    return Error::Make(
        kRouteErrorBase,
        "JPEG 2000: no system codec is installed and the FreeImage fallback "
        "is not built.",
        kCodecNotFound);
  }
  if (format == ImageFormat::kAvif) {
    return Error::Make(
        kRouteErrorBase,
        "AVIF: the system AV1 image extension is missing. Install "
        "\"AV1 Video Extension\" from the Microsoft Store and try again.",
        kCodecNotFound);
  }
  return Error::Make(
      kRouteErrorBase,
      "HEIC/HEIF: the system HEIF codec is missing. Install "
      "\"HEIF Image Extensions\" (and \"HEVC Video Extensions\") from the "
      "Microsoft Store and try again.",
      kCodecNotFound);
}

Error CodecAwareError(ImageFormat format, const Error& wic_error) {
  if (format == ImageFormat::kJxr && wic_error.hr == kCodecNotFound) {
    return Error::Make(kRouteErrorBase,
                       "JPEG XR: no system codec and the FreeImage fallback is "
                       "not built",
                       kCodecNotFound);
  }
  if (wic_error.hr == kCodecNotFound) return MissingCodecError(format);
  return wic_error;
}

bool FreeImageOnlyFormat(ImageFormat format) {
  return format == ImageFormat::kHdr || format == ImageFormat::kExr ||
         format == ImageFormat::kPfm || format == ImageFormat::kWbmp ||
         format == ImageFormat::kMng;
}

bool WicFirstFormat(ImageFormat format) {
  return format == ImageFormat::kJp2 || format == ImageFormat::kJxr;
}

void FirstFrameOnly(ProbeResult* probe) {
  probe->frame_count = 1;
  probe->animated = false;
}

void FirstFrameOnly(Image* image) {
  if (image == nullptr) return;
  image->info.frame_count = 1;
  image->info.animated = false;
}

// Probe fallbacks only contribute metadata; the caller keeps the routing format
// it resolved (or adopts the decoder's format when the extension was unknown).
void CopyProbeMetadata(const ProbeResult& from, ProbeResult* to) {
  to->width = from.width;
  to->height = from.height;
  to->has_alpha = from.has_alpha;
  to->orientation = from.orientation;
  to->orientation_applied = from.orientation_applied;
  to->has_icc = from.has_icc;
  to->icc_is_srgb = from.icc_is_srgb;
  to->frame_count = from.frame_count;
  to->animated = from.animated;
  to->decoder = from.decoder;
}

}  // namespace

const wchar_t* FormatName(ImageFormat format) {
  switch (format) {
    case ImageFormat::kJpeg: return L"JPEG";
    case ImageFormat::kPng: return L"PNG";
    case ImageFormat::kApng: return L"APNG";
    case ImageFormat::kBmp: return L"BMP";
    case ImageFormat::kGif: return L"GIF";
    case ImageFormat::kWebp: return L"WebP";
    case ImageFormat::kTiff: return L"TIFF";
    case ImageFormat::kIco: return L"ICO/CUR";
    case ImageFormat::kPsd: return L"PSD/PSB";
    case ImageFormat::kHeif: return L"HEIC/HEIF";
    case ImageFormat::kAvif: return L"AVIF";
    case ImageFormat::kSvg: return L"SVG";
    case ImageFormat::kTga: return L"TGA";
    case ImageFormat::kPcx: return L"PCX";
    case ImageFormat::kPnm: return L"PNM";
    case ImageFormat::kXbm: return L"XBM";
    case ImageFormat::kXpm: return L"XPM";
    case ImageFormat::kWbmp: return L"WBMP";
    case ImageFormat::kHdr: return L"HDR";
    case ImageFormat::kExr: return L"EXR";
    case ImageFormat::kPfm: return L"PFM";
    case ImageFormat::kJp2: return L"JP2";
    case ImageFormat::kJxr: return L"JXR";
    case ImageFormat::kMng: return L"MNG";
    case ImageFormat::kRaw: return L"RAW";
    default: return L"Unknown";
  }
}

const char* FormatNameUtf8(ImageFormat format) {
  switch (format) {
    case ImageFormat::kJpeg: return "JPEG";
    case ImageFormat::kPng: return "PNG";
    case ImageFormat::kApng: return "APNG";
    case ImageFormat::kBmp: return "BMP";
    case ImageFormat::kGif: return "GIF";
    case ImageFormat::kWebp: return "WebP";
    case ImageFormat::kTiff: return "TIFF";
    case ImageFormat::kIco: return "ICO";
    case ImageFormat::kPsd: return "PSD/PSB";
    case ImageFormat::kHeif: return "HEIC/HEIF";
    case ImageFormat::kAvif: return "AVIF";
    case ImageFormat::kSvg: return "SVG";
    case ImageFormat::kTga: return "TGA";
    case ImageFormat::kPcx: return "PCX";
    case ImageFormat::kPnm: return "PNM";
    case ImageFormat::kXbm: return "XBM";
    case ImageFormat::kXpm: return "XPM";
    case ImageFormat::kWbmp: return "WBMP";
    case ImageFormat::kHdr: return "HDR";
    case ImageFormat::kExr: return "EXR";
    case ImageFormat::kPfm: return "PFM";
    case ImageFormat::kJp2: return "JP2";
    case ImageFormat::kJxr: return "JXR";
    case ImageFormat::kMng: return "MNG";
    case ImageFormat::kRaw: return "RAW";
    default: return "Unknown";
  }
}

ImageFormat FormatRouter::Detect(const std::filesystem::path& path) {
  std::ifstream file(path, std::ios::binary);
  if (!file) return ImageFormat::kUnknown;

  std::array<uint8_t, kHeaderBytes> header{};
  file.read(reinterpret_cast<char*>(header.data()),
            static_cast<std::streamsize>(header.size()));
  const size_t size = static_cast<size_t>(file.gcount());

  // A strong signature wins over an extension that claims RAW (規劃/25 §5):
  // a JPEG renamed to .cr2 must be opened as JPEG instead of being handed to
  // LibRaw. TIFF is not strong, so a real CR2/NEF/ARW still routes to RAW.
  const ImageFormat by_magic = DetectByMagic(header, size);
  if (by_magic == ImageFormat::kPng && apng::Inspect(path).is_apng) {
    return ImageFormat::kApng;
  }
  if (IsStrongMagic(by_magic)) return by_magic;

  // TIFF-based RAW (CR2/NEF/ARW/DNG/...) must not be routed to the TIFF codec.
  const std::wstring extension = LowerExtension(path);
  if (IsRawExtension(extension)) return ImageFormat::kRaw;

  if (by_magic != ImageFormat::kUnknown) return by_magic;
  // TGA has no reliable magic; its header can collide with ICO/CUR. Only fall
  // back to TGA when the magic pass found nothing (ICO now requires count != 0).
  if (extension == L".tga" || extension == L".targa") return ImageFormat::kTga;
  return FormatFromExtension(extension);
}

ProbeResult FormatRouter::Probe(const std::filesystem::path& path) {
  return Probe(path, ImageFormat::kUnknown);
}

ProbeResult FormatRouter::Probe(const std::filesystem::path& path,
                               ImageFormat known_format) {
  ProbeResult result;
  std::error_code ec;
  if (!std::filesystem::is_regular_file(path, ec)) {
    result.error = Error::Make(kRouteErrorBase, "file does not exist or is not a regular file");
    return result;
  }

  result.format = known_format != ImageFormat::kUnknown ? known_format
                                                        : Detect(path);
  if (result.format == ImageFormat::kPsd) {
    return psd::Probe(path);
  }
  if (result.format == ImageFormat::kSvg) {
    return svg::Probe(path);
  }
  if (result.format == ImageFormat::kRaw) {
    if (libraw::Available()) {
      ProbeResult raw = libraw::Probe(path);
      if (raw.ok()) {
        raw.format = ImageFormat::kRaw;
        return raw;
      }
      result.error = raw.error;
    } else if (freeimage::Available()) {
      ProbeResult fi = freeimage::Probe(path);
      if (fi.ok()) {
        fi.format = ImageFormat::kRaw;
        return fi;
      }
      result.error = fi.error;
    } else {
      result.error = Error::Make(kRouteErrorBase,
                                 "RAW support is not built (LibRaw missing)");
    }
    return result;
  }
  if (result.format == ImageFormat::kUnknown) {
    // No magic and no known extension (e.g. a TGA without a suffix): do not
    // reject immediately. WIC is tried first, then FreeImage's own type
    // detection; both probes are the cheap form of the Decode fallback below.
    ProbeResult wic_probe = wic::Probe(path);
    if (wic_probe.ok()) {
      CopyProbeMetadata(wic_probe, &result);
      result.format = wic_probe.format;
      return result;
    }
    if (freeimage::Available()) {
      ProbeResult fi_probe = freeimage::Probe(path);
      if (fi_probe.ok()) {
        CopyProbeMetadata(fi_probe, &result);
        result.format = fi_probe.format;
        return result;
      }
    }
    result.error = Error::Make(kRouteErrorBase, "unsupported or unknown image format");
    return result;
  }

  if (FreeImageOnlyFormat(result.format)) {
    if (!freeimage::Available()) {
      result.error = Error::Make(
          kRouteErrorBase,
          std::string(FormatNameUtf8(result.format)) +
              " support is not built (FreeImage missing)");
      return result;
    }
    ProbeResult fi = freeimage::Probe(path, result.format);
    if (fi.ok()) {
      fi.format = result.format;
      if (result.format == ImageFormat::kMng) FirstFrameOnly(&fi);
      return fi;
    }
    result.error = fi.error;
    return result;
  }

  const bool iso_bmff = result.format == ImageFormat::kHeif ||
                        result.format == ImageFormat::kAvif;
  ProbeResult wic_probe = wic::Probe(path);
  if (wic_probe.ok()) {
    result.width = wic_probe.width;
    result.height = wic_probe.height;
    result.has_alpha = wic_probe.has_alpha;
    result.orientation = wic_probe.orientation;
    result.orientation_applied = wic_probe.orientation_applied;
    result.has_icc = wic_probe.has_icc;
    result.icc_is_srgb = wic_probe.icc_is_srgb;
    result.frame_count = wic_probe.frame_count;
    result.animated = wic_probe.animated;
    if (result.format == ImageFormat::kApng) {
      const apng::Info info = apng::Inspect(path);
      result.frame_count = info.frames;
      result.animated = info.frames > 1;
    }
    result.decoder = "wic";
    return result;
  }
  if (iso_bmff) {
    // HEIC/AVIF have no FreeImage fallback; report a codec hint when the
    // system extension is missing instead of a bare WIC HRESULT.
    result.error = CodecAwareError(result.format, wic_probe.error);
    return result;
  }

  if (freeimage::Available()) {
    ProbeResult fi_probe = freeimage::Probe(path, result.format);
    if (fi_probe.ok()) {
      result.width = fi_probe.width;
      result.height = fi_probe.height;
      result.has_alpha = fi_probe.has_alpha;
      result.orientation = fi_probe.orientation;
      result.orientation_applied = fi_probe.orientation_applied;
      result.decoder = "freeimage";
      if (result.format == ImageFormat::kMng) FirstFrameOnly(&result);
      return result;
    }
  }

  if (WicFirstFormat(result.format)) {
    result.error = CodecAwareError(result.format, wic_probe.error);
    return result;
  }

  result.error = wic_probe.error;
  return result;
}

DecodeResult FormatRouter::Decode(const std::filesystem::path& path,
                                  const DecodeOptions& options) {
  const ImageFormat format = Detect(path);
  if (format == ImageFormat::kPsd) {
    return psd::Decode(path, options);
  }
  if (format == ImageFormat::kSvg) {
    return svg::Decode(path, options);
  }
  if (format == ImageFormat::kGif) {
    DecodeResult gif_result = gif::Decode(path, options);
    if (gif_result.ok() || options.max_edge > 0) return gif_result;
    // Fall back to the normal WIC first-frame path if animation decoding
    // failed (e.g. a damaged frame late in the file).
    DecodeResult fallback = wic::Decode(path, options);
    if (fallback.ok()) {
      // wic::Decode now reports animated=false for every container (it only
      // decodes frame 0). A damaged animation keeps its metadata so the pill
      // can still show the degraded "first frame only" state (22/12 §3.4).
      fallback.image->info.animated = fallback.image->info.frame_count > 1;
      return fallback;
    }
    return gif_result;
  }
  if (format == ImageFormat::kApng) {
    DecodeResult apng_result = apng::Decode(path, options);
    if (apng_result.ok() || options.max_edge > 0) return apng_result;
    // Interlaced or damaged APNG: show the default image instead of failing.
    DecodeResult fallback = wic::Decode(path, options);
    if (fallback.ok()) {
      const apng::Info info = apng::Inspect(path);
      fallback.image->info.frame_count = info.frames;
      fallback.image->info.animated = info.frames > 1;
      nv::platform::LogInfo("apng: playback failed (" + apng_result.error.message +
                            "); showing the default image");
      return fallback;
    }
    return apng_result;
  }
  if (format == ImageFormat::kWebp) {
    // Animated WebP (規劃/22/12 §3.4): WIC enumerates the frames, the RIFF
    // container supplies delay/dispose/blend and the loop count. A static WebP
    // and every thumbnail/prefetch request fall through to the normal path.
    DecodeResult webp_result = webp::Decode(path, options);
    if (webp_result.ok() || options.max_edge > 0) return webp_result;
    DecodeResult fallback = wic::Decode(path, options);
    if (fallback.ok()) {
      fallback.image->info.animated = fallback.image->info.frame_count > 1;
      nv::platform::LogInfo("webp: playback failed (" +
                            webp_result.error.message +
                            "); showing the first frame");
      return fallback;
    }
    return webp_result;
  }
  if (format == ImageFormat::kRaw) {    DecodeResult raw_result;
    if (libraw::Available()) {
      raw_result = libraw::Decode(path, options);
      if (raw_result.ok()) return raw_result;
    }
    if (freeimage::Available()) {
      DecodeResult fi_result = freeimage::Decode(path, options);
      if (fi_result.ok()) return fi_result;
    }
    if (raw_result.error.ok()) {
      raw_result.error = Error::Make(kRouteErrorBase,
                                     "RAW support is not built (LibRaw missing)");
    }
    return raw_result;
  }

  if (FreeImageOnlyFormat(format)) {
    if (!freeimage::Available()) {
      DecodeResult missing;
      missing.error = Error::Make(
          kRouteErrorBase,
          std::string(FormatNameUtf8(format)) +
              " support is not built (FreeImage missing)");
      return missing;
    }
    DecodeResult fi_result = freeimage::Decode(path, options, format);
    if (fi_result.ok() && format == ImageFormat::kMng) {
      FirstFrameOnly(fi_result.image.get());
    }
    return fi_result;
  }

  if (format == ImageFormat::kHeif || format == ImageFormat::kAvif) {
    DecodeResult iso_result = wic::Decode(path, options);
    if (!iso_result.ok()) {
      iso_result.error = CodecAwareError(format, iso_result.error);
    }
    return iso_result;
  }

  DecodeResult wic_result = wic::Decode(path, options);
  if (wic_result.ok()) {
    return wic_result;
  }

  if (freeimage::Available()) {
    DecodeResult fi_result = freeimage::Decode(path, options, format);
    if (fi_result.ok()) {
      if (format == ImageFormat::kMng) FirstFrameOnly(fi_result.image.get());
      return fi_result;
    }
    // The fallback ran and failed: its error describes the file better than
    // "system codec missing" from the WIC attempt (規劃/22/16).
    if (WicFirstFormat(format)) return fi_result;
  }
  if (WicFirstFormat(format)) {
    wic_result.error = CodecAwareError(format, wic_result.error);
  }
  return wic_result;
}

bool FormatRouter::IsSupportedExtension(const std::wstring& extension_lower) {
  const ImageFormat format = FormatFromExtension(extension_lower);
  return format != ImageFormat::kUnknown;
}

ImageFormat FormatRouter::FormatForExtension(
    const std::wstring& extension_lower) {
  return FormatFromExtension(extension_lower);
}

}  // namespace nv
