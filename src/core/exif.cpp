#include "core/exif.h"

#include "core/format_router.h"
#include "decoders/libraw_decoder.h"
#include "platform/i18n.h"

#include <windows.h>
#include <wincodec.h>
#include <wrl/client.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <cwchar>
#include <fstream>
#include <map>
#include <utility>

namespace nv {
namespace {

constexpr size_t kMaxExifFileBytes = 32ull * 1024 * 1024;
constexpr size_t kMaxExifBlockBytes = 8ull * 1024 * 1024;
constexpr uint32_t kMaxIfdEntries = 512;
constexpr size_t kMaxTextChars = 128;

// Little helper for the TIFF type sizes (EXIF subset).
size_t TypeSize(uint16_t type) {
  switch (type) {
    case 1:  // BYTE
    case 2:  // ASCII
    case 6:  // SBYTE
    case 7:  // UNDEFINED
      return 1;
    case 3:  // SHORT
    case 8:  // SSHORT
      return 2;
    case 4:  // LONG
    case 9:  // SLONG
    case 11: // FLOAT
      return 4;
    case 5:   // RATIONAL
    case 10:  // SRATIONAL
    case 12:  // DOUBLE
      return 8;
    default:
      return 0;
  }
}

std::wstring TrimText(std::wstring text) {
  while (!text.empty() &&
         (text.back() == L'\0' || text.back() == L' ' || text.back() == L'\t' ||
          text.back() == L'\r' || text.back() == L'\n')) {
    text.pop_back();
  }
  size_t start = 0;
  while (start < text.size() && (text[start] == L' ' || text[start] == L'\t')) {
    ++start;
  }
  if (start != 0) text.erase(0, start);
  if (text.size() > kMaxTextChars) text.resize(kMaxTextChars);
  return text;
}

std::wstring NarrowToWide(const char* data, size_t length) {
  // EXIF text is nominally ASCII; cameras in CJK markets often write UTF-8.
  // Strip at the first NUL, then try UTF-8 and fall back to the ANSI code page
  // (瑕忓妰/22/06 鍧?8).
  size_t end = 0;
  while (end < length && data[end] != '\0') ++end;
  if (end == 0) return std::wstring();
  const int size = static_cast<int>(std::min<size_t>(end, 4096));
  if (size <= 0) return std::wstring();
  int wide_length = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, data, size,
                                        nullptr, 0);
  UINT code_page = CP_UTF8;
  if (wide_length <= 0) {
    code_page = CP_ACP;
    wide_length = MultiByteToWideChar(code_page, 0, data, size, nullptr, 0);
  }
  if (wide_length <= 0) return std::wstring();
  std::wstring out(static_cast<size_t>(wide_length), L'\0');
  MultiByteToWideChar(code_page, code_page == CP_UTF8 ? MB_ERR_INVALID_CHARS : 0,
                      data, size, out.data(), wide_length);
  return TrimText(std::move(out));
}

std::wstring DoubleText(const wchar_t* format, double value) {
  wchar_t buffer[64]{};
  swprintf_s(buffer, format, value);
  return buffer;
}

std::wstring IntText(const wchar_t* format, int value) {
  wchar_t buffer[64]{};
  swprintf_s(buffer, format, value);
  return buffer;
}

double RationalValue(const ExifRational& value) {
  if (value.denominator == 0) return 0.0;
  return static_cast<double>(value.numerator) /
         static_cast<double>(value.denominator);
}

// --- TIFF block parsing -----------------------------------------------------

struct TiffView {
  const uint8_t* data = nullptr;
  size_t size = 0;
  bool little = true;

  bool U16(size_t offset, uint16_t* out) const {
    if (out == nullptr || offset + 2 > size) return false;
    const uint8_t* p = data + offset;
    *out = little ? static_cast<uint16_t>(p[0] | (p[1] << 8))
                  : static_cast<uint16_t>((p[0] << 8) | p[1]);
    return true;
  }

  bool U32(size_t offset, uint32_t* out) const {
    if (out == nullptr || offset + 4 > size) return false;
    const uint8_t* p = data + offset;
    if (little) {
      *out = static_cast<uint32_t>(p[0]) | (static_cast<uint32_t>(p[1]) << 8) |
             (static_cast<uint32_t>(p[2]) << 16) |
             (static_cast<uint32_t>(p[3]) << 24);
    } else {
      *out = static_cast<uint32_t>(p[3]) | (static_cast<uint32_t>(p[2]) << 8) |
             (static_cast<uint32_t>(p[1]) << 16) |
             (static_cast<uint32_t>(p[0]) << 24);
    }
    return true;
  }

  bool I32(size_t offset, int32_t* out) const {
    uint32_t raw = 0;
    if (!U32(offset, &raw)) return false;
    *out = static_cast<int32_t>(raw);
    return true;
  }
};

struct TiffEntry {
  uint16_t tag = 0;
  uint16_t type = 0;
  uint32_t count = 0;
  size_t value_offset = 0;  // Absolute offset of the first value byte.
  bool valid = false;
};

// Parses one IFD; returns false when the directory itself is malformed.
bool ReadIfd(const TiffView& view, size_t offset,
             std::vector<TiffEntry>* entries) {
  if (entries == nullptr) return false;
  entries->clear();
  uint16_t count = 0;
  if (!view.U16(offset, &count)) return false;
  if (count > kMaxIfdEntries) return false;
  const size_t needed = offset + 2 + static_cast<size_t>(count) * 12;
  if (needed > view.size) return false;

  for (uint32_t i = 0; i < count; ++i) {
    const size_t entry_offset = offset + 2 + static_cast<size_t>(i) * 12;
    TiffEntry entry;
    if (!view.U16(entry_offset, &entry.tag) ||
        !view.U16(entry_offset + 2, &entry.type) ||
        !view.U32(entry_offset + 4, &entry.count)) {
      continue;
    }
    const size_t type_size = TypeSize(entry.type);
    if (type_size == 0 || entry.count == 0) continue;
    const uint64_t total = static_cast<uint64_t>(type_size) * entry.count;
    if (total > kMaxExifBlockBytes) continue;

    if (total <= 4) {
      entry.value_offset = entry_offset + 8;
    } else {
      uint32_t value = 0;
      if (!view.U32(entry_offset + 8, &value)) continue;
      entry.value_offset = value;
      if (static_cast<uint64_t>(entry.value_offset) + total > view.size) {
        continue;
      }
    }
    entry.valid = true;
    entries->push_back(entry);
  }
  return true;
}

const TiffEntry* Find(const std::vector<TiffEntry>& entries, uint16_t tag) {
  for (const TiffEntry& entry : entries) {
    if (entry.tag == tag) return &entry;
  }
  return nullptr;
}

std::wstring ReadAscii(const TiffView& view, const TiffEntry& entry) {
  if (entry.type != 2 || entry.value_offset + entry.count > view.size) {
    return std::wstring();
  }
  return NarrowToWide(reinterpret_cast<const char*>(view.data) + entry.value_offset,
                      entry.count);
}

bool ReadShorts(const TiffView& view, const TiffEntry& entry,
                std::vector<int>* out) {
  if (out == nullptr) return false;
  if (entry.type != 3) return false;
  const size_t count = std::min<uint32_t>(entry.count, 16);
  for (size_t i = 0; i < count; ++i) {
    const size_t offset = entry.value_offset + i * 2;
    uint16_t value = 0;
    if (!view.U16(offset, &value)) return false;
    out->push_back(value);
  }
  return !out->empty();
}

bool ReadLongs(const TiffView& view, const TiffEntry& entry,
               std::vector<uint32_t>* out) {
  if (out == nullptr) return false;
  if (entry.type != 4) return false;
  const size_t count = std::min<uint32_t>(entry.count, 16);
  for (size_t i = 0; i < count; ++i) {
    uint32_t value = 0;
    if (!view.U32(entry.value_offset + i * 4, &value)) return false;
    out->push_back(value);
  }
  return !out->empty();
}

// Reads `count` rationals, tolerating both RATIONAL (5) and SRATIONAL (10).
bool ReadRationals(const TiffView& view, const TiffEntry& entry,
                   std::vector<ExifRational>* out) {
  if (out == nullptr) return false;
  const bool signed_values = entry.type == 10;
  if (entry.type != 5 && !signed_values) return false;
  const size_t count = std::min<uint32_t>(entry.count, 8);
  for (size_t i = 0; i < count; ++i) {
    const size_t offset = entry.value_offset + i * 8;
    ExifRational value;
    bool ok = false;
    if (signed_values) {
      int32_t numerator = 0;
      int32_t denominator = 0;
      ok = view.I32(offset, &numerator) && view.I32(offset + 4, &denominator);
      value.numerator = numerator;
      value.denominator = denominator;
    } else {
      uint32_t numerator = 0;
      uint32_t denominator = 0;
      ok = view.U32(offset, &numerator) && view.U32(offset + 4, &denominator);
      value.numerator = static_cast<int64_t>(numerator);
      value.denominator = static_cast<int64_t>(denominator);
    }
    if (!ok) return false;
    out->push_back(value);
  }
  return !out->empty();
}

bool ReadSingleRational(const TiffView& view, const TiffEntry& entry,
                        ExifRational* out) {
  std::vector<ExifRational> values;
  if (!ReadRationals(view, entry, &values) || values.empty()) return false;
  *out = values.front();
  return true;
}

int FirstShort(const TiffView& view, const TiffEntry& entry) {
  std::vector<int> values;
  if (!ReadShorts(view, entry, &values) || values.empty()) return -1;
  return values.front();
}

bool FirstLong(const TiffView& view, const TiffEntry& entry, uint32_t* out) {
  std::vector<uint32_t> values;
  if (!ReadLongs(view, entry, &values) || values.empty()) return false;
  *out = values.front();
  return true;
}

// GPS IFD: tag 34853 in IFD0 points at a nested directory whose offsets are
// absolute inside the same TIFF block (瑕忓妰/22/06 鍧?7).
void ReadGps(const TiffView& view, const std::vector<TiffEntry>& gps,
             ExifData* out) {
  if (out == nullptr) return;
  const TiffEntry* latitude = Find(gps, 2);
  const TiffEntry* longitude = Find(gps, 4);
  const TiffEntry* altitude = Find(gps, 6);
  const TiffEntry* altitude_ref = Find(gps, 5);
  const TiffEntry* latitude_ref = Find(gps, 1);
  const TiffEntry* longitude_ref = Find(gps, 3);

  if (latitude != nullptr && longitude != nullptr) {
    std::vector<ExifRational> lat;
    std::vector<ExifRational> lon;
    if (ReadRationals(view, *latitude, &lat) && lat.size() >= 3 &&
        ReadRationals(view, *longitude, &lon) && lon.size() >= 3) {
      wchar_t lat_ref = latitude_ref != nullptr
                            ? ReadAscii(view, *latitude_ref).empty()
                                  ? L'N'
                                  : ReadAscii(view, *latitude_ref)[0]
                            : L'N';
      wchar_t lon_ref = longitude_ref != nullptr
                            ? ReadAscii(view, *longitude_ref).empty()
                                  ? L'E'
                                  : ReadAscii(view, *longitude_ref)[0]
                            : L'E';
      const std::wstring lat_text =
          FormatGpsCoordinate(lat[0], lat[1], lat[2], lat_ref);
      const std::wstring lon_text =
          FormatGpsCoordinate(lon[0], lon[1], lon[2], lon_ref);
      if (!lat_text.empty()) out->Set(ExifId::kGpsLatitude, lat_text);
      if (!lon_text.empty()) out->Set(ExifId::kGpsLongitude, lon_text);
    }
  }

  if (altitude != nullptr) {
    ExifRational value;
    if (ReadSingleRational(view, *altitude, &value)) {
      bool below = false;
      if (altitude_ref != nullptr && altitude_ref->type == 1 &&
          altitude_ref->value_offset < view.size) {
        below = view.data[altitude_ref->value_offset] != 0;
      }
      const std::wstring text = FormatGpsAltitude(value, below);
      if (!text.empty()) out->Set(ExifId::kGpsAltitude, text);
    }
  }
}

void ReadCameraTags(const TiffView& view, const std::vector<TiffEntry>& ifd0,
                    const std::vector<TiffEntry>& exif, ExifData* out) {
  if (out == nullptr) return;
  const auto text = [&view](const std::vector<TiffEntry>& entries,
                            uint16_t tag) -> std::wstring {
    const TiffEntry* entry = Find(entries, tag);
    return entry == nullptr ? std::wstring() : ReadAscii(view, *entry);
  };
  out->Set(ExifId::kMake, text(ifd0, 271));
  out->Set(ExifId::kModel, text(ifd0, 272));
  out->Set(ExifId::kSoftware, text(ifd0, 305));
  out->Set(ExifId::kDateTime, FormatExifDateTime(text(ifd0, 306)));

  out->Set(ExifId::kLens, text(exif, 42036));
  out->Set(ExifId::kDateTimeOriginal,
           FormatExifDateTime(text(exif, 36867)));

  const TiffEntry* exposure = Find(exif, 33434);
  if (exposure != nullptr) {
    ExifRational value;
    if (ReadSingleRational(view, *exposure, &value)) {
      out->Set(ExifId::kExposureTime, FormatExposureTime(value));
    }
  }
  const TiffEntry* aperture = Find(exif, 33437);
  if (aperture != nullptr) {
    ExifRational value;
    if (ReadSingleRational(view, *aperture, &value)) {
      out->Set(ExifId::kFNumber, FormatAperture(value));
    }
  }
  const TiffEntry* iso = Find(exif, 34855);
  if (iso != nullptr) {
    const int value = FirstShort(view, *iso);
    if (value > 0) out->Set(ExifId::kIso, FormatIso(value));
  }
  const TiffEntry* focal = Find(exif, 37386);
  if (focal != nullptr) {
    ExifRational value;
    if (ReadSingleRational(view, *focal, &value)) {
      out->Set(ExifId::kFocalLength, FormatFocalLength(value));
    }
  }
  const TiffEntry* bias = Find(exif, 37380);
  if (bias != nullptr) {
    ExifRational value;
    if (ReadSingleRational(view, *bias, &value)) {
      out->Set(ExifId::kExposureBias, FormatExposureBias(value));
    }
  }
  const TiffEntry* flash = Find(exif, 37385);
  if (flash != nullptr) {
    const int value = FirstShort(view, *flash);
    if (value >= 0) out->Set(ExifId::kFlash, FormatFlash(value));
  }
  const TiffEntry* metering = Find(exif, 37383);
  if (metering != nullptr) {
    const int value = FirstShort(view, *metering);
    if (value >= 0) out->Set(ExifId::kMeteringMode, FormatMeteringMode(value));
  }
  const TiffEntry* white_balance = Find(exif, 41987);
  if (white_balance != nullptr) {
    const int value = FirstShort(view, *white_balance);
    if (value >= 0) out->Set(ExifId::kWhiteBalance, FormatWhiteBalance(value));
  }
  const TiffEntry* color_space = Find(exif, 40961);
  if (color_space != nullptr) {
    const int value = FirstShort(view, *color_space);
    if (value >= 0) out->Set(ExifId::kColorSpace, FormatColorSpace(value));
  }
  const TiffEntry* pixel_x = Find(exif, 40962);
  if (pixel_x != nullptr) {
    uint32_t value = 0;
    // The spec says LONG, but several writers (Pillow among them) store the
    // pixel dimensions as SHORT; accept both instead of dropping the tag.
    if (!FirstLong(view, *pixel_x, &value)) {
      const int short_value = FirstShort(view, *pixel_x);
      value = short_value > 0 ? static_cast<uint32_t>(short_value) : 0;
    }
    if (value > 0) out->Set(ExifId::kPixelX, std::to_wstring(value));
  }
  const TiffEntry* pixel_y = Find(exif, 40963);
  if (pixel_y != nullptr) {
    uint32_t value = 0;
    if (!FirstLong(view, *pixel_y, &value)) {
      const int short_value = FirstShort(view, *pixel_y);
      value = short_value > 0 ? static_cast<uint32_t>(short_value) : 0;
    }
    if (value > 0) out->Set(ExifId::kPixelY, std::to_wstring(value));
  }
}

// --- WIC fallback (HEIC/AVIF) ----------------------------------------------

std::wstring QueryString(IWICMetadataQueryReader* reader, const wchar_t* query) {
  if (reader == nullptr || query == nullptr) return std::wstring();
  PROPVARIANT value;
  PropVariantInit(&value);
  std::wstring result;
  if (SUCCEEDED(reader->GetMetadataByName(query, &value))) {
    if (value.vt == VT_LPSTR) {
      result = NarrowToWide(value.pszVal, strlen(value.pszVal));
    } else if (value.vt == VT_LPWSTR) {
      result = TrimText(value.pwszVal);
    }
  }
  PropVariantClear(&value);
  return result;
}

bool QueryRational(IWICMetadataQueryReader* reader, const wchar_t* query,
                   ExifRational* out) {
  if (reader == nullptr || query == nullptr || out == nullptr) return false;
  PROPVARIANT value;
  PropVariantInit(&value);
  bool ok = false;
  if (SUCCEEDED(reader->GetMetadataByName(query, &value))) {
    if (value.vt == VT_UI8) {
      out->numerator = static_cast<int64_t>(value.uhVal.QuadPart >> 32);
      out->denominator =
          static_cast<int64_t>(value.uhVal.QuadPart & 0xFFFFFFFFull);
      ok = out->denominator != 0;
    } else if (value.vt == VT_R8) {
      const double v = value.dblVal;
      out->numerator = static_cast<int64_t>(std::llround(v * 1000.0));
      out->denominator = 1000;
      ok = true;
    } else if (value.vt == VT_R4) {
      const double v = static_cast<double>(value.fltVal);
      out->numerator = static_cast<int64_t>(std::llround(v * 1000.0));
      out->denominator = 1000;
      ok = true;
    } else if (value.vt == (VT_VECTOR | VT_UI4) && value.caul.cElems >= 2) {
      out->numerator = static_cast<int64_t>(value.caul.pElems[0]);
      out->denominator = static_cast<int64_t>(value.caul.pElems[1]);
      ok = out->denominator != 0;
    }
  }
  PropVariantClear(&value);
  return ok;
}

bool QueryInt(IWICMetadataQueryReader* reader, const wchar_t* query, int* out) {
  if (reader == nullptr || query == nullptr || out == nullptr) return false;
  PROPVARIANT value;
  PropVariantInit(&value);
  bool ok = false;
  if (SUCCEEDED(reader->GetMetadataByName(query, &value))) {
    if (value.vt == VT_UI2) {
      *out = value.uiVal;
      ok = true;
    } else if (value.vt == VT_UI4) {
      *out = static_cast<int>(value.ulVal);
      ok = true;
    } else if (value.vt == VT_I4) {
      *out = value.lVal;
      ok = true;
    }
  }
  PropVariantClear(&value);
  return ok;
}

ExifData ReadExifWic(const std::filesystem::path& path) {
  ExifData data;
  using Microsoft::WRL::ComPtr;
  ComPtr<IWICImagingFactory> factory;
  if (FAILED(CoCreateInstance(CLSID_WICImagingFactory, nullptr,
                              CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&factory)))) {
    return data;
  }
  ComPtr<IWICBitmapDecoder> decoder;
  if (FAILED(factory->CreateDecoderFromFilename(
          path.c_str(), nullptr, GENERIC_READ, WICDecodeMetadataCacheOnDemand,
          decoder.GetAddressOf()))) {
    return data;
  }
  ComPtr<IWICBitmapFrameDecode> frame;
  if (FAILED(decoder->GetFrame(0, frame.GetAddressOf()))) return data;
  ComPtr<IWICMetadataQueryReader> reader;
  if (FAILED(frame->GetMetadataQueryReader(reader.GetAddressOf()))) return data;

  data.Set(ExifId::kMake, QueryString(reader.Get(), L"/ifd/{ushort=271}"));
  data.Set(ExifId::kModel, QueryString(reader.Get(), L"/ifd/{ushort=272}"));
  data.Set(ExifId::kSoftware, QueryString(reader.Get(), L"/ifd/{ushort=305}"));
  data.Set(ExifId::kDateTime,
           FormatExifDateTime(QueryString(reader.Get(), L"/ifd/{ushort=306}")));
  data.Set(ExifId::kLens,
           QueryString(reader.Get(), L"/ifd/{ushort=42036}"));
  data.Set(ExifId::kDateTimeOriginal,
           FormatExifDateTime(QueryString(reader.Get(), L"/ifd/{ushort=36867}")));

  ExifRational rational;
  if (QueryRational(reader.Get(), L"/ifd/{ushort=33434}", &rational)) {
    data.Set(ExifId::kExposureTime, FormatExposureTime(rational));
  }
  if (QueryRational(reader.Get(), L"/ifd/{ushort=33437}", &rational)) {
    data.Set(ExifId::kFNumber, FormatAperture(rational));
  }
  int number = 0;
  if (QueryInt(reader.Get(), L"/ifd/{ushort=34855}", &number) && number > 0) {
    data.Set(ExifId::kIso, FormatIso(number));
  }
  if (QueryRational(reader.Get(), L"/ifd/{ushort=37386}", &rational)) {
    data.Set(ExifId::kFocalLength, FormatFocalLength(rational));
  }
  if (QueryRational(reader.Get(), L"/ifd/{ushort=37380}", &rational)) {
    data.Set(ExifId::kExposureBias, FormatExposureBias(rational));
  }
  if (QueryInt(reader.Get(), L"/ifd/{ushort=37385}", &number)) {
    data.Set(ExifId::kFlash, FormatFlash(number));
  }
  if (QueryInt(reader.Get(), L"/ifd/{ushort=37383}", &number)) {
    data.Set(ExifId::kMeteringMode, FormatMeteringMode(number));
  }
  if (QueryInt(reader.Get(), L"/ifd/{ushort=41987}", &number)) {
    data.Set(ExifId::kWhiteBalance, FormatWhiteBalance(number));
  }
  if (QueryInt(reader.Get(), L"/ifd/{ushort=40961}", &number)) {
    data.Set(ExifId::kColorSpace, FormatColorSpace(number));
  }
  data.ok = data.Any();
  return data;
}

}  // namespace

// --- ExifData ---------------------------------------------------------------

ExifData::ExifData() {
  fields.resize(kExifFieldCount);
  for (size_t i = 0; i < kExifFieldCount; ++i) {
    fields[i].id = static_cast<ExifId>(i);
  }
}

void ExifData::Set(ExifId id, std::wstring value) {
  const size_t index = static_cast<size_t>(id);
  if (index >= fields.size()) return;
  fields[index].id = id;
  fields[index].value = TrimText(std::move(value));
}

const std::wstring& ExifData::Get(ExifId id) const {
  static const std::wstring kEmpty;
  const size_t index = static_cast<size_t>(id);
  if (index >= fields.size()) return kEmpty;
  return fields[index].value;
}

bool ExifData::Has(ExifId id) const { return !Get(id).empty(); }

bool ExifData::Any() const {
  for (const ExifField& field : fields) {
    if (!field.value.empty()) return true;
  }
  return false;
}

const char* ExifIdName(ExifId id) {
  switch (id) {
    case ExifId::kMake: return "make";
    case ExifId::kModel: return "model";
    case ExifId::kLens: return "lens";
    case ExifId::kDateTimeOriginal: return "date_original";
    case ExifId::kExposureTime: return "exposure";
    case ExifId::kFNumber: return "fnumber";
    case ExifId::kIso: return "iso";
    case ExifId::kFocalLength: return "focal";
    case ExifId::kExposureBias: return "bias";
    case ExifId::kFlash: return "flash";
    case ExifId::kMeteringMode: return "metering";
    case ExifId::kWhiteBalance: return "whitebalance";
    case ExifId::kColorSpace: return "colorspace";
    case ExifId::kPixelX: return "pixel_x";
    case ExifId::kPixelY: return "pixel_y";
    case ExifId::kSoftware: return "software";
    case ExifId::kDateTime: return "date";
    case ExifId::kGpsLatitude: return "gps_latitude";
    case ExifId::kGpsLongitude: return "gps_longitude";
    case ExifId::kGpsAltitude: return "gps_altitude";
    default: return "unknown";
  }
}

// --- Formatting -------------------------------------------------------------

std::wstring FormatExposureFraction(const ExifRational& value) {
  if (value.denominator == 0 || value.numerator <= 0) return std::wstring();
  const double seconds = RationalValue(value);
  if (!(seconds > 0.0)) return std::wstring();

  // Reduce, then prefer the photographic "1/N" form for sub-second speeds.
  int64_t numerator = value.numerator;
  int64_t denominator = value.denominator;
  {
    int64_t a = numerator;
    int64_t b = denominator;
    while (b != 0) {
      const int64_t t = a % b;
      a = b;
      b = t;
    }
    if (a > 1) {
      numerator /= a;
      denominator /= a;
    }
  }
  if (seconds < 1.0) {
    if (numerator == 1) {
      return L"1/" + std::to_wstring(denominator);
    }
    const long long inverse = std::llround(1.0 / seconds);
    if (inverse <= 0) return std::wstring();
    return L"1/" + std::to_wstring(inverse);
  }
  if (denominator == 1) return std::to_wstring(numerator);
  std::wstring text = DoubleText(L"%.2f", seconds);
  while (!text.empty() && text.back() == L'0') text.pop_back();
  if (!text.empty() && text.back() == L'.') text.pop_back();
  return text;
}

std::wstring FormatExposureTime(const ExifRational& value) {
  const std::wstring fraction = FormatExposureFraction(value);
  if (fraction.empty()) return fraction;
  wchar_t buffer[96]{};
  swprintf_s(buffer, L"%s %s", fraction.c_str(),
             i18n::Text(i18n::StringId::kInfoSecondsUnit));
  return buffer;
}

std::wstring FormatAperture(const ExifRational& value) {
  if (value.denominator == 0 || value.numerator <= 0) return std::wstring();
  const double f = RationalValue(value);
  if (!(f > 0.0) || f > 1000.0) return std::wstring();
  return DoubleText(L"f/%.1f", f);
}

std::wstring FormatIso(int value) {
  if (value <= 0) return std::wstring();
  return IntText(L"ISO %d", value);
}

std::wstring FormatFocalLength(const ExifRational& value) {
  if (value.denominator == 0 || value.numerator <= 0) return std::wstring();
  const double mm = RationalValue(value);
  if (!(mm > 0.0) || mm > 10000.0) return std::wstring();
  const double rounded = std::round(mm);
  if (std::fabs(mm - rounded) < 0.05) {
    wchar_t buffer[64]{};
    swprintf_s(buffer, L"%.0f mm", rounded);
    return buffer;
  }
  wchar_t buffer[64]{};
  swprintf_s(buffer, L"%.1f mm", mm);
  return buffer;
}

std::wstring FormatExposureBias(const ExifRational& value) {
  if (value.denominator == 0) return std::wstring();
  const double ev = RationalValue(value);
  if (!(std::fabs(ev) <= 100.0)) return std::wstring();
  if (std::fabs(ev) < 0.005) return L"0 EV";
  wchar_t buffer[64]{};
  swprintf_s(buffer, L"%+.1f EV", ev);
  return buffer;
}

std::wstring FormatFlash(int value) {
  if (value < 0 || value == 0xFFFF) return std::wstring();
  return i18n::Text((value & 1) != 0 ? i18n::StringId::kInfoFlashFired
                                     : i18n::StringId::kInfoFlashNoFire);
}

std::wstring FormatMeteringMode(int value) {
  switch (value) {
    case 0: return std::wstring();  // unknown
    case 1: return i18n::Text(i18n::StringId::kInfoMeterAverage);
    case 2: return i18n::Text(i18n::StringId::kInfoMeterCenter);
    case 3: return i18n::Text(i18n::StringId::kInfoMeterSpot);
    case 4: return i18n::Text(i18n::StringId::kInfoMeterMultiSpot);
    case 5: return i18n::Text(i18n::StringId::kInfoMeterPattern);
    case 6: return i18n::Text(i18n::StringId::kInfoMeterPartial);
    default: return std::wstring();
  }
}

std::wstring FormatWhiteBalance(int value) {
  if (value == 0) return i18n::Text(i18n::StringId::kInfoWbAuto);
  if (value == 1) return i18n::Text(i18n::StringId::kInfoWbManual);
  return std::wstring();
}

std::wstring FormatColorSpace(int value) {
  if (value == 1) return i18n::Text(i18n::StringId::kInfoSrgb);
  if (value == 0xFFFF) return i18n::Text(i18n::StringId::kInfoUncalibrated);
  return std::wstring();
}

std::wstring FormatExifDateTime(const std::wstring& raw) {
  const std::wstring text = TrimText(raw);
  if (text.size() < 10) return text;
  auto is_digit = [](wchar_t c) { return c >= L'0' && c <= L'9'; };
  if (!is_digit(text[0]) || !is_digit(text[1]) || !is_digit(text[2]) ||
      !is_digit(text[3])) {
    return text;
  }
  // Normalise "YYYY:MM:DD HH:MM:SS" and "YYYY-MM-DDT HH:MM:SS" to ISO-like
  // text with a space. Fixed format, independent of the system locale.
  std::wstring out = text;
  if (out.size() >= 10) {
    if (out[4] == L':' || out[4] == L'/') out[4] = L'-';
    if (out[7] == L':' || out[7] == L'/') out[7] = L'-';
  }
  if (out.size() >= 11 && (out[10] == L'T' || out[10] == L't')) out[10] = L' ';
  return out;
}

std::wstring FormatGpsDegrees(const ExifRational& degrees,
                              const ExifRational& minutes,
                              const ExifRational& seconds) {
  const double d = RationalValue(degrees);
  const double m = RationalValue(minutes);
  const double s = RationalValue(seconds);
  if (!(d >= 0.0 && d <= 360.0) || !(m >= 0.0 && m <= 60.0) ||
      !(s >= 0.0 && s <= 60.0)) {
    return std::wstring();
  }
  if (d == 0.0 && m == 0.0 && s == 0.0) return std::wstring();
  return DoubleText(L"%.6f", d + m / 60.0 + s / 3600.0);
}

std::wstring FormatGpsCoordinate(const ExifRational& degrees,
                                 const ExifRational& minutes,
                                 const ExifRational& seconds, wchar_t ref) {
  std::wstring text = FormatGpsDegrees(degrees, minutes, seconds);
  if (text.empty()) return text;
  if (ref == L'S' || ref == L's' || ref == L'W' || ref == L'w') {
    text.insert(text.begin(), L'-');
  }
  return text;
}

std::wstring FormatGpsAltitude(const ExifRational& value,
                               bool below_sea_level) {
  if (value.denominator == 0) return std::wstring();
  double meters = RationalValue(value);
  if (!(std::fabs(meters) <= 100000.0)) return std::wstring();
  if (below_sea_level) meters = -meters;
  wchar_t buffer[64]{};
  swprintf_s(buffer, L"%.1f m", meters);
  return buffer;
}

// --- Container extraction ---------------------------------------------------

bool ExtractJpegExifBlock(const uint8_t* data, size_t size,
                          std::vector<uint8_t>* out) {
  if (data == nullptr || out == nullptr || size < 4) return false;
  out->clear();
  if (data[0] != 0xFF || data[1] != 0xD8) return false;  // SOI

  size_t offset = 2;
  while (offset + 4 <= size) {
    if (data[offset] != 0xFF) return false;
    // Skip fill bytes.
    while (offset + 1 < size && data[offset + 1] == 0xFF) ++offset;
    if (offset + 4 > size) break;
    const uint8_t marker = data[offset + 1];
    if (marker == 0xD9 || marker == 0xDA) break;  // EOI / start of scan
    if (marker == 0x01 || (marker >= 0xD0 && marker <= 0xD7)) {
      offset += 2;  // Standalone markers have no length.
      continue;
    }
    const size_t length = (static_cast<size_t>(data[offset + 2]) << 8) |
                          static_cast<size_t>(data[offset + 3]);
    if (length < 2) break;
    const size_t segment = offset + 2 + length;
    if (segment > size) break;
    if (marker == 0xE1 && length >= 8 && data[offset + 4] == 'E' &&
        data[offset + 5] == 'x' && data[offset + 6] == 'i' &&
        data[offset + 7] == 'f') {
      const size_t start = offset + 10;  // marker + length + "Exif\0\0"
      const size_t end = segment;
      if (start < end && end - start <= kMaxExifBlockBytes) {
        out->assign(data + start, data + end);
        return true;
      }
      return false;
    }
    offset = segment;
  }
  return false;
}

bool ExtractPngExifBlock(const uint8_t* data, size_t size,
                         std::vector<uint8_t>* out) {
  if (data == nullptr || out == nullptr || size < 8) return false;
  static const uint8_t kSignature[8] = {0x89, 'P', 'N', 'G', '\r', '\n', 0x1A,
                                        '\n'};
  if (std::memcmp(data, kSignature, 8) != 0) return false;
  out->clear();
  size_t offset = 8;
  while (offset + 12 <= size) {
    const uint32_t length = (static_cast<uint32_t>(data[offset]) << 24) |
                            (static_cast<uint32_t>(data[offset + 1]) << 16) |
                            (static_cast<uint32_t>(data[offset + 2]) << 8) |
                            static_cast<uint32_t>(data[offset + 3]);
    if (length > kMaxExifBlockBytes) return false;
    const size_t data_start = offset + 8;
    if (data_start + length + 4 > size) return false;
    if (std::memcmp(data + offset + 4, "eXIf", 4) == 0) {
      out->assign(data + data_start, data + data_start + length);
      return !out->empty();
    }
    if (std::memcmp(data + offset + 4, "IEND", 4) == 0) break;
    offset = data_start + length + 4;
  }
  return false;
}

bool ParseExifTiffBlock(const uint8_t* data, size_t size, ExifData* out) {
  if (data == nullptr || out == nullptr || size < 8) return false;
  TiffView view;
  view.data = data;
  view.size = size;
  if (data[0] == 'I' && data[1] == 'I') {
    view.little = true;
  } else if (data[0] == 'M' && data[1] == 'M') {
    view.little = false;
  } else {
    return false;
  }
  uint16_t magic = 0;
  if (!view.U16(2, &magic) || magic != 42) return false;
  uint32_t ifd0_offset = 0;
  if (!view.U32(4, &ifd0_offset)) return false;

  std::vector<TiffEntry> ifd0;
  if (!ReadIfd(view, ifd0_offset, &ifd0)) return false;

  std::vector<TiffEntry> exif_ifd;
  if (const TiffEntry* pointer = Find(ifd0, 34665)) {
    uint32_t next = 0;
    if (FirstLong(view, *pointer, &next)) ReadIfd(view, next, &exif_ifd);
  }

  std::vector<TiffEntry> gps_ifd;
  if (const TiffEntry* pointer = Find(ifd0, 34853)) {
    uint32_t next = 0;
    if (FirstLong(view, *pointer, &next)) ReadIfd(view, next, &gps_ifd);
  }

  ReadCameraTags(view, ifd0, exif_ifd, out);
  ReadGps(view, gps_ifd, out);
  return true;
}

// --- Public entry point -----------------------------------------------------

ExifData ReadExif(const std::filesystem::path& path) {
  ExifData data;
  std::error_code ec;
  if (!std::filesystem::is_regular_file(path, ec) || ec) return data;

  const ImageFormat format = FormatRouter::Detect(path);
  if (format == ImageFormat::kRaw) {
    // RAW is a TIFF container whose WIC metadata is wrong/misleading (鍧?10);
    // LibRaw parses the real tags while it opens the file, without unpacking.
    return libraw::ReadExif(path);
  }
  if (format == ImageFormat::kHeif || format == ImageFormat::kAvif) {
    return ReadExifWic(path);
  }
  if (format != ImageFormat::kJpeg && format != ImageFormat::kTiff &&
      format != ImageFormat::kPng && format != ImageFormat::kApng) {
    return data;  // PSD/SVG/GIF/BMP/...: the panel shows an em dash.
  }

  const uintmax_t file_size = std::filesystem::file_size(path, ec);
  if (ec || file_size == 0) return data;
  const size_t to_read = static_cast<size_t>(
      std::min<uintmax_t>(file_size, kMaxExifFileBytes));
  std::ifstream file(path, std::ios::binary);
  if (!file) return data;
  std::vector<uint8_t> bytes(to_read);
  file.read(reinterpret_cast<char*>(bytes.data()),
            static_cast<std::streamsize>(bytes.size()));
  if (!file) return data;
  bytes.resize(static_cast<size_t>(file.gcount()));
  if (bytes.empty()) return data;

  std::vector<uint8_t> block;
  bool found = false;
  if (format == ImageFormat::kPng || format == ImageFormat::kApng) {
    found = ExtractPngExifBlock(bytes.data(), bytes.size(), &block);
  } else if (format == ImageFormat::kTiff) {
    block = std::move(bytes);
    found = true;
  } else {
    found = ExtractJpegExifBlock(bytes.data(), bytes.size(), &block);
  }
  if (!found || block.empty()) return data;

  if (ParseExifTiffBlock(block.data(), block.size(), &data)) {
    data.ok = data.Any();
  }
  return data;
}

}  // namespace nv
