// v2.0 S2: EXIF formatting and TIFF-block parsing (規劃/22/06).
//
// The synthetic TIFF block keeps the test independent of sample files (the app
// runs it from `new`, and the Debug/ASan configurations must pass too). The
// real JPEG/PNG path is covered by tests/run_info_panel_check.ps1 through
// `zoreview_cli --exif`.

#include "tests/exif_tests.h"

#include "core/exif.h"
#include "platform/i18n.h"

#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

namespace nv::tests {
namespace {

int g_failures = 0;

void Check(bool ok, const char* name) {
  if (ok) {
    std::printf("ok   %s\n", name);
  } else {
    std::printf("FAIL %s\n", name);
    ++g_failures;
  }
}

void CheckText(const std::wstring& actual, const wchar_t* expected,
               const char* name) {
  const bool ok = actual == expected;
  std::printf("%s %s\n", ok ? "ok  " : "FAIL", name);
  if (!ok) {
    std::printf("     actual=\"%ls\" expected=\"%ls\"\n", actual.c_str(),
                expected);
    ++g_failures;
  }
}

// --- Little-endian TIFF builder ---------------------------------------------

class TiffWriter {
 public:
  TiffWriter() : data_{'I', 'I', 42, 0, 8, 0, 0, 0} {}

  size_t Reserve(size_t bytes) {
    const size_t offset = data_.size();
    data_.resize(offset + bytes, 0);
    return offset;
  }
  void U8(size_t offset, uint8_t value) { data_[offset] = value; }
  void U16(size_t offset, uint16_t value) {
    data_[offset] = static_cast<uint8_t>(value & 0xFF);
    data_[offset + 1] = static_cast<uint8_t>(value >> 8);
  }
  void U32(size_t offset, uint32_t value) {
    data_[offset] = static_cast<uint8_t>(value & 0xFF);
    data_[offset + 1] = static_cast<uint8_t>((value >> 8) & 0xFF);
    data_[offset + 2] = static_cast<uint8_t>((value >> 16) & 0xFF);
    data_[offset + 3] = static_cast<uint8_t>((value >> 24) & 0xFF);
  }
  void Ascii(size_t offset, const char* text) {
    std::memcpy(data_.data() + offset, text, std::strlen(text) + 1);
  }
  void Rational(size_t offset, uint32_t numerator, uint32_t denominator) {
    U32(offset, numerator);
    U32(offset + 4, denominator);
  }

  // One 12-byte IFD entry. `value` is either the inline value (<= 4 bytes) or
  // the absolute offset of the payload.
  void Entry(size_t ifd, uint16_t index, uint16_t tag, uint16_t type,
             uint32_t count, uint32_t value) {
    const size_t offset = ifd + 2 + static_cast<size_t>(index) * 12;
    U16(offset, tag);
    U16(offset + 2, type);
    U32(offset + 4, count);
    U32(offset + 8, value);
  }

  const std::vector<uint8_t>& data() const { return data_; }

 private:
  std::vector<uint8_t> data_;
};

// Builds a complete EXIF block: IFD0 (Make/Model/pointers), Exif IFD with the
// shooting fields and a GPS IFD. This mirrors what a camera writes.
std::vector<uint8_t> BuildSampleTiff() {
  TiffWriter writer;
  const size_t ifd0 = 8;
  const size_t ifd0_size = 2 + 4 * 12 + 4;
  const size_t exif = ifd0 + ifd0_size;
  const size_t exif_size = 2 + 6 * 12 + 4;
  const size_t gps = exif + exif_size;
  const size_t gps_size = 2 + 5 * 12 + 4;
  const size_t payload = gps + gps_size;

  const size_t make = payload;
  const size_t model = make + 9;      // "TestMake\0"
  const size_t date_time = model + 8; // "TestCam\0"
  const size_t taken = date_time + 20;
  const size_t exposure = taken + 20;   // "2024:08:12 14:33:05\0"
  const size_t fnumber = exposure + 8;
  const size_t latitude = fnumber + 8;
  const size_t longitude = latitude + 24;
  const size_t altitude = longitude + 24;
  writer.Reserve(altitude + 8 - writer.data().size());

  // IFD0
  writer.U16(ifd0, 4);
  writer.Entry(ifd0, 0, 0x010F, 2, 9, static_cast<uint32_t>(make));
  writer.Entry(ifd0, 1, 0x0110, 2, 8, static_cast<uint32_t>(model));
  writer.Entry(ifd0, 2, 0x8769, 4, 1, static_cast<uint32_t>(exif));
  writer.Entry(ifd0, 3, 0x8825, 4, 1, static_cast<uint32_t>(gps));
  writer.U32(ifd0 + 2 + 4 * 12, 0);

  // Exif IFD
  writer.U16(exif, 6);
  writer.Entry(exif, 0, 0x9003, 2, 20, static_cast<uint32_t>(taken));
  writer.Entry(exif, 1, 0x829A, 5, 1, static_cast<uint32_t>(exposure));
  writer.Entry(exif, 2, 0x829D, 5, 1, static_cast<uint32_t>(fnumber));
  writer.Entry(exif, 3, 0x8827, 3, 1, 400);
  writer.Entry(exif, 4, 0xA403, 3, 1, 1);  // manual white balance
  writer.Entry(exif, 5, 0x9209, 3, 1, 1);  // flash fired
  writer.U32(exif + 2 + 6 * 12, 0);

  // GPS IFD: lat ref inline ("N\0"), lat/lon 3 rationals, altitude.
  writer.U16(gps, 5);
  writer.Entry(gps, 0, 1, 2, 2, static_cast<uint32_t>('N'));
  writer.Entry(gps, 1, 2, 5, 3, static_cast<uint32_t>(latitude));
  writer.Entry(gps, 2, 3, 2, 2, static_cast<uint32_t>('E'));
  writer.Entry(gps, 3, 4, 5, 3, static_cast<uint32_t>(longitude));
  writer.Entry(gps, 4, 6, 5, 1, static_cast<uint32_t>(altitude));
  writer.U32(gps + 2 + 5 * 12, 0);

  // Payload
  writer.Ascii(make, "TestMake");
  writer.Ascii(model, "TestCam");
  writer.Ascii(taken, "2024:08:12 14:33:05");
  writer.Rational(exposure, 1, 125);
  writer.Rational(fnumber, 28, 10);
  writer.Rational(latitude, 31, 1);
  writer.Rational(latitude + 8, 13, 1);
  writer.Rational(latitude + 16, 49, 1);
  writer.Rational(longitude, 121, 1);
  writer.Rational(longitude + 8, 28, 1);
  writer.Rational(longitude + 16, 25, 1);
  writer.Rational(altitude, 15, 1);
  return writer.data();
}

std::vector<uint8_t> WrapJpeg(const std::vector<uint8_t>& tiff) {
  std::vector<uint8_t> jpeg{0xFF, 0xD8};
  const size_t length = tiff.size() + 8;  // length + "Exif\0\0" + payload
  jpeg.push_back(0xFF);
  jpeg.push_back(0xE1);
  jpeg.push_back(static_cast<uint8_t>((length >> 8) & 0xFF));
  jpeg.push_back(static_cast<uint8_t>(length & 0xFF));
  const char exif_header[6] = {'E', 'x', 'i', 'f', 0, 0};
  jpeg.insert(jpeg.end(), exif_header, exif_header + 6);
  jpeg.insert(jpeg.end(), tiff.begin(), tiff.end());
  jpeg.push_back(0xFF);
  jpeg.push_back(0xDA);  // start of scan ends metadata scanning
  jpeg.push_back(0x00);
  jpeg.push_back(0x02);
  return jpeg;
}

std::vector<uint8_t> WrapPng(const std::vector<uint8_t>& tiff) {
  std::vector<uint8_t> png{0x89, 'P', 'N', 'G', '\r', '\n', 0x1A, '\n'};
  const uint32_t length = static_cast<uint32_t>(tiff.size());
  png.push_back(static_cast<uint8_t>((length >> 24) & 0xFF));
  png.push_back(static_cast<uint8_t>((length >> 16) & 0xFF));
  png.push_back(static_cast<uint8_t>((length >> 8) & 0xFF));
  png.push_back(static_cast<uint8_t>(length & 0xFF));
  const char type[4] = {'e', 'X', 'I', 'f'};
  png.insert(png.end(), type, type + 4);
  png.insert(png.end(), tiff.begin(), tiff.end());
  const uint8_t crc[4] = {0, 0, 0, 0};  // CRC is not validated by the reader
  png.insert(png.end(), crc, crc + 4);
  const char iend[12] = {0, 0, 0, 0, 'I', 'E', 'N', 'D', 0, 0, 0, 0};
  png.insert(png.end(), iend, iend + 12);
  return png;
}

void TestFormatting() {
  CheckText(FormatExposureTime({1, 125}), L"1/125 s", "exposure 1/125");
  CheckText(FormatExposureTime({5, 2}), L"2.5 s", "exposure 2.5 s");
  CheckText(FormatExposureTime({1, 0}), L"", "zero denominator is missing");
  CheckText(FormatExposureTime({0, 1}), L"", "zero exposure is missing");
  CheckText(FormatExposureFraction({10, 1250}), L"1/125",
            "exposure fraction is reduced");
  CheckText(FormatAperture({28, 10}), L"f/2.8", "aperture f/2.8");
  CheckText(FormatAperture({0, 10}), L"", "zero aperture is missing");
  CheckText(FormatIso(400), L"ISO 400", "iso 400");
  CheckText(FormatIso(0), L"", "iso 0 is missing");
  CheckText(FormatFocalLength({50, 1}), L"50 mm", "focal 50 mm");
  CheckText(FormatFocalLength({35, 10}), L"3.5 mm", "focal 3.5 mm");
  CheckText(FormatExposureBias({3, 10}), L"+0.3 EV", "bias +0.3");
  CheckText(FormatExposureBias({-7, 10}), L"-0.7 EV", "bias -0.7");
  CheckText(FormatExposureBias({0, 1}), L"0 EV", "bias zero");
  CheckText(FormatFlash(1), L"Fired", "flash fired");
  CheckText(FormatFlash(16), L"Did not fire", "flash not fired");
  CheckText(FormatFlash(0xFFFF), L"", "unknown flash is missing");
  CheckText(FormatMeteringMode(5), L"Pattern", "metering pattern");
  CheckText(FormatMeteringMode(2), L"Center-weighted", "metering centre");
  CheckText(FormatMeteringMode(0), L"", "unknown metering is missing");
  CheckText(FormatWhiteBalance(0), L"Auto", "white balance auto");
  CheckText(FormatWhiteBalance(1), L"Manual", "white balance manual");
  CheckText(FormatColorSpace(1), L"sRGB", "colour space sRGB");
  CheckText(FormatColorSpace(0xFFFF), L"Uncalibrated",
            "colour space uncalibrated");
  CheckText(FormatExifDateTime(L"2024:08:12 14:33:05"),
            L"2024-08-12 14:33:05", "EXIF time is normalised");
  CheckText(FormatExifDateTime(L"2024-08-12T14:33:05"),
            L"2024-08-12 14:33:05", "ISO time is normalised");
  CheckText(FormatGpsCoordinate({31, 1}, {13, 1}, {49, 1}, L'N'),
            L"31.230278", "GPS north is positive");
  CheckText(FormatGpsCoordinate({31, 1}, {13, 1}, {49, 1}, L'S'),
            L"-31.230278", "GPS south is negative");
  CheckText(FormatGpsDegrees({0, 1}, {0, 1}, {0, 1}), L"",
            "zero GPS is missing");
  CheckText(FormatGpsAltitude({15, 1}, false), L"15.0 m", "GPS altitude");
  CheckText(FormatGpsAltitude({15, 1}, true), L"-15.0 m",
            "GPS altitude below sea level");
}

void TestEmptyData() {
  ExifData data;
  Check(data.fields.size() == kExifFieldCount,
        "every field id has a slot");
  Check(data.Get(ExifId::kMake).empty(), "missing value is empty");
  Check(!data.Any(), "a fresh block has no values");
  data.Set(ExifId::kMake, L" Canon  ");
  CheckText(data.Get(ExifId::kMake), L"Canon", "values are trimmed");
  Check(data.Any(), "a set value makes the block non-empty");
  data.Set(ExifId::kMake, L"");
  Check(!data.Any(), "an empty value is treated as missing");
}

void TestTiffParser() {
  const std::vector<uint8_t> tiff = BuildSampleTiff();
  ExifData data;
  Check(ParseExifTiffBlock(tiff.data(), tiff.size(), &data),
        "the synthetic TIFF block parses");
  CheckText(data.Get(ExifId::kMake), L"TestMake", "IFD0 make");
  CheckText(data.Get(ExifId::kModel), L"TestCam", "IFD0 model");
  CheckText(data.Get(ExifId::kDateTimeOriginal), L"2024-08-12 14:33:05",
            "Exif IFD date taken");
  CheckText(data.Get(ExifId::kExposureTime), L"1/125 s", "Exif IFD exposure");
  CheckText(data.Get(ExifId::kFNumber), L"f/2.8", "Exif IFD aperture");
  CheckText(data.Get(ExifId::kIso), L"ISO 400", "Exif IFD ISO");
  CheckText(data.Get(ExifId::kWhiteBalance), L"Manual", "Exif IFD white balance");
  CheckText(data.Get(ExifId::kFlash), L"Fired", "Exif IFD flash");
  CheckText(data.Get(ExifId::kGpsLatitude), L"31.230278", "GPS latitude");
  CheckText(data.Get(ExifId::kGpsLongitude), L"121.473611", "GPS longitude");
  CheckText(data.Get(ExifId::kGpsAltitude), L"15.0 m", "GPS altitude");
  CheckText(data.Get(ExifId::kFocalLength), L"",
            "a tag the camera did not write stays missing");

  // A truncated block must fail without reading outside the buffer.
  ExifData truncated;
  Check(!ParseExifTiffBlock(tiff.data(), 12, &truncated),
        "a truncated TIFF block is rejected");
  Check(!ParseExifTiffBlock(nullptr, 0, &truncated),
        "a null block is rejected");
  // Wrong byte-order magic.
  std::vector<uint8_t> bad = tiff;
  bad[3] = 43;
  Check(!ParseExifTiffBlock(bad.data(), bad.size(), &truncated),
        "a bad TIFF magic is rejected");
}

void TestContainers() {
  const std::vector<uint8_t> tiff = BuildSampleTiff();
  std::vector<uint8_t> block;
  const std::vector<uint8_t> jpeg = WrapJpeg(tiff);
  Check(ExtractJpegExifBlock(jpeg.data(), jpeg.size(), &block),
        "JPEG APP1 EXIF is found");
  Check(block == tiff, "JPEG EXIF payload is exact");

  ExifData data;
  Check(ParseExifTiffBlock(block.data(), block.size(), &data) &&
            data.Get(ExifId::kIso) == L"ISO 400",
        "JPEG extraction feeds the parser");

  block.clear();
  const std::vector<uint8_t> png = WrapPng(tiff);
  Check(ExtractPngExifBlock(png.data(), png.size(), &block),
        "PNG eXIf chunk is found");
  Check(block == tiff, "PNG EXIF payload is exact");

  block.clear();
  const std::vector<uint8_t> not_exif{0xFF, 0xD8, 0xFF, 0xD9};
  Check(!ExtractJpegExifBlock(not_exif.data(), not_exif.size(), &block),
        "a JPEG without APP1 has no EXIF");
  Check(!ExtractPngExifBlock(not_exif.data(), not_exif.size(), &block),
        "a non-PNG has no eXIf");
}

}  // namespace

int RunExifTests() {
  g_failures = 0;
  // Formatting depends on the UI language; assert the English table here (the
  // app flips the language at runtime and the labels follow it).
  const nv::i18n::Language previous = nv::i18n::CurrentLanguage();
  nv::i18n::SetLanguage(nv::i18n::Language::kEnglish);
  TestFormatting();
  TestEmptyData();
  TestTiffParser();
  TestContainers();
  nv::i18n::SetLanguage(previous);
  return g_failures;
}

}  // namespace nv::tests
