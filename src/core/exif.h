#pragma once

// EXIF metadata for the info panel (規劃/22/06).
//
// The reader is deliberately self-contained:
//   * JPEG (APP1 "Exif\0\0"), PNG (eXIf chunk) and TIFF are parsed from the
//     TIFF IFD blocks directly. WIC's metadata query reader is not used there
//     because it does not expose the nested GPS IFD consistently and returns
//     rationals as several different PROPVARIANT shapes.
//   * HEIC/AVIF fall back to the WIC query reader (the system codec decides
//     which tags exist).
//   * RAW files are read by LibRaw (metadata only, no unpack); see
//     decoders/libraw_decoder.h.
//
// Everything here runs on the EXIF worker, never on the UI thread; the result
// carries pre-formatted strings so the panel only has to lay them out.

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace nv {

// Fixed field order of the "camera" group. The panel always shows all of them
// (missing values stay empty so the row can render an em dash instead of
// pretending the camera did not write anything).
enum class ExifId {
  kMake = 0,
  kModel,
  kLens,
  kDateTimeOriginal,
  kExposureTime,
  kFNumber,
  kIso,
  kFocalLength,
  kExposureBias,
  kFlash,
  kMeteringMode,
  kWhiteBalance,
  kColorSpace,
  kPixelX,
  kPixelY,
  kSoftware,
  kDateTime,
  kGpsLatitude,
  kGpsLongitude,
  kGpsAltitude,
  kCount,
};

inline constexpr size_t kExifFieldCount = static_cast<size_t>(ExifId::kCount);

const char* ExifIdName(ExifId id);

struct ExifField {
  ExifId id = ExifId::kMake;
  std::wstring value;  // Already formatted; empty = missing/unreadable.
};

struct ExifData {
  // Always kExifFieldCount entries in enum order.
  std::vector<ExifField> fields;
  // False when the file has no EXIF block, the container is unsupported or the
  // block was malformed. It is not an error the user needs to see.
  bool ok = false;

  ExifData();
  void Set(ExifId id, std::wstring value);
  // Empty string when missing.
  const std::wstring& Get(ExifId id) const;
  bool Has(ExifId id) const;
  // True when at least one field has a value.
  bool Any() const;
};

// Reads and formats EXIF for `path`. Synchronous: call it from the EXIF worker
// (core/exif_service) or the CLI, never from the UI thread.
ExifData ReadExif(const std::filesystem::path& path);

// --- Formatting helpers (pure, unit-tested) --------------------------------

struct ExifRational {
  int64_t numerator = 0;
  int64_t denominator = 0;
};

// "1/125" or "2.5". Empty when the denominator is zero / the value is invalid
// (規劃/22/06 坑 5: never render "1/0 秒").
std::wstring FormatExposureFraction(const ExifRational& value);
// "<fraction> 秒" / "<fraction> s"; Chinese unit from i18n.
std::wstring FormatExposureTime(const ExifRational& value);
// "f/2.8"; one decimal, empty when invalid.
std::wstring FormatAperture(const ExifRational& value);
// "ISO 400"; empty when <= 0.
std::wstring FormatIso(int value);
// "50 mm"; integer when whole, one decimal otherwise.
std::wstring FormatFocalLength(const ExifRational& value);
// "+0.3 EV" / "-0.7 EV" / "0 EV".
std::wstring FormatExposureBias(const ExifRational& value);
// EXIF Flash tag (bit 0 = fired).
std::wstring FormatFlash(int value);
// EXIF MeteringMode 0..6 (0/unknown -> empty).
std::wstring FormatMeteringMode(int value);
// EXIF WhiteBalance (0 auto, 1 manual, other -> empty).
std::wstring FormatWhiteBalance(int value);
// EXIF ColorSpace (1 = sRGB, 0xFFFF = uncalibrated).
std::wstring FormatColorSpace(int value);
// "2024:08:12 14:33:05" or "2024-08-12T14:33:05" -> "2024-08-12 14:33:05".
// Returns the trimmed input when it does not look like an EXIF timestamp.
std::wstring FormatExifDateTime(const std::wstring& raw);
// Decimal degrees with 6 decimals: "31.230416". Empty when all components are
// zero and no reference was written (common for "no GPS").
std::wstring FormatGpsDegrees(const ExifRational& degrees,
                              const ExifRational& minutes,
                              const ExifRational& seconds);
// Applies the N/S/E/W reference: S/W become negative.
std::wstring FormatGpsCoordinate(const ExifRational& degrees,
                                 const ExifRational& minutes,
                                 const ExifRational& seconds, wchar_t ref);
// Altitude with one decimal and a unit: "15.0 m". `below_sea_level` negates.
std::wstring FormatGpsAltitude(const ExifRational& value, bool below_sea_level);

// --- Low-level parsers (exposed for the unit tests) ------------------------

// Parses one TIFF/EXIF block (starting with "II*\0" or "MM\0*") and fills the
// subset of tags the panel shows, including the GPS sub-IFD. Bounds-checked;
// returns false for a malformed block (partial fields are still kept).
bool ParseExifTiffBlock(const uint8_t* data, size_t size, ExifData* out);

// Extracts the EXIF block from a JPEG byte stream (APP1) into `out`.
bool ExtractJpegExifBlock(const uint8_t* data, size_t size,
                          std::vector<uint8_t>* out);
// Extracts the eXIf chunk payload from a PNG byte stream.
bool ExtractPngExifBlock(const uint8_t* data, size_t size,
                         std::vector<uint8_t>* out);

}  // namespace nv
