// Unit checks for the EXIF orientation baking and the ICC profile header
// reader. Everything here is pure CPU; the WIC decode path is exercised by
// tests/run_orientation_check.ps1 and tests/run_icc_check.ps1.

#include "tests/color_tests.h"

#include "core/icc_profile.h"
#include "core/image.h"
#include "core/image_transform.h"
#include "decoders/libraw_decoder.h"

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

namespace {

int g_failures = 0;

void Check(bool condition, const char* name) {
  std::printf("%s %s\n", condition ? "ok  " : "FAIL", name);
  if (!condition) ++g_failures;
}

void SetU16(std::vector<uint8_t>& out, size_t offset, uint16_t value) {
  out[offset + 0] = static_cast<uint8_t>(value >> 8);
  out[offset + 1] = static_cast<uint8_t>(value & 0xFF);
}

void SetU32(std::vector<uint8_t>& out, size_t offset, uint32_t value) {
  out[offset + 0] = static_cast<uint8_t>(value >> 24);
  out[offset + 1] = static_cast<uint8_t>((value >> 16) & 0xFF);
  out[offset + 2] = static_cast<uint8_t>((value >> 8) & 0xFF);
  out[offset + 3] = static_cast<uint8_t>(value & 0xFF);
}

void SetSignature(std::vector<uint8_t>& out, size_t offset, const char* text) {
  std::memcpy(out.data() + offset, text, 4);
}

// Minimal ICC v2 profile with a single 'desc' tag.
std::vector<uint8_t> MakeDescProfile(const std::string& description) {
  const uint32_t text_length = static_cast<uint32_t>(description.size() + 1);
  const uint32_t data_length = 12 + text_length;
  const uint32_t total = 144 + data_length;

  std::vector<uint8_t> profile(total, 0);
  SetU32(profile, 0, total);
  SetSignature(profile, 36, "acsp");
  SetU32(profile, 128, 1);
  SetSignature(profile, 132, "desc");
  SetU32(profile, 136, 144);
  SetU32(profile, 140, data_length);
  SetSignature(profile, 144, "desc");
  SetU32(profile, 152, text_length);
  std::memcpy(profile.data() + 156, description.c_str(), text_length);
  return profile;
}

// Minimal ICC v4 profile with a single 'mluc' tag.
std::vector<uint8_t> MakeMlucProfile(const std::string& description) {
  const uint32_t text_length = static_cast<uint32_t>(description.size() * 2);
  const uint32_t data_length = 28 + text_length;
  const uint32_t total = 144 + data_length;

  std::vector<uint8_t> profile(total, 0);
  SetU32(profile, 0, total);
  SetSignature(profile, 36, "acsp");
  SetU32(profile, 128, 1);
  SetSignature(profile, 132, "desc");
  SetU32(profile, 136, 144);
  SetU32(profile, 140, data_length);
  SetSignature(profile, 144, "mluc");
  SetU32(profile, 152, 1);   // record count
  SetU32(profile, 156, 12);  // record size
  // Record: language, country, length, offset (relative to the tag start).
  SetU32(profile, 164, text_length);
  SetU32(profile, 168, 28);
  for (size_t i = 0; i < description.size(); ++i) {
    SetU16(profile, 172 + i * 2,
           static_cast<uint16_t>(static_cast<unsigned char>(description[i])));
  }
  return profile;
}

std::vector<uint8_t> MakeSourcePixels() {
  // 3x2 BGRA image. The blue channel encodes the pixel index 0..5 so a wrong
  // orientation is obvious; alpha is opaque.
  std::vector<uint8_t> pixels(6 * 4, 255);
  for (uint8_t i = 0; i < 6; ++i) {
    pixels[i * 4 + 0] = i;
    pixels[i * 4 + 1] = static_cast<uint8_t>(i + 1);
    pixels[i * 4 + 2] = static_cast<uint8_t>(i + 2);
  }
  return pixels;
}

const uint8_t kExpected[8][6] = {
    {0, 1, 2, 3, 4, 5},  // 1
    {2, 1, 0, 5, 4, 3},  // 2
    {5, 4, 3, 2, 1, 0},  // 3
    {3, 4, 5, 0, 1, 2},  // 4
    {0, 3, 1, 4, 2, 5},  // 5: 2x3
    {3, 0, 4, 1, 5, 2},  // 6: 2x3
    {5, 2, 4, 1, 3, 0},  // 7: 2x3
    {2, 5, 1, 4, 0, 3},  // 8: 2x3
};

void TestOrientationTransforms() {
  const std::vector<uint8_t> source = MakeSourcePixels();
  for (uint16_t orientation = 1; orientation <= 8; ++orientation) {
    uint32_t width = 0;
    uint32_t height = 0;
    const std::vector<uint8_t> out = nv::TransformBgra(
        source.data(), 3, 2, 3 * 4, orientation, &width, &height);
    const bool swaps = orientation >= 5;
    const bool dims_ok = width == (swaps ? 2u : 3u) && height == (swaps ? 3u : 2u);
    bool pixels_ok = out.size() == 24;
    if (pixels_ok) {
      for (size_t i = 0; i < 6; ++i) {
        const uint8_t index = out[i * 4];
        if (index != kExpected[orientation - 1][i]) pixels_ok = false;
      }
    }
    char name[96]{};
    std::snprintf(name, sizeof(name), "orientation %u dimensions", orientation);
    Check(dims_ok, name);
    std::snprintf(name, sizeof(name), "orientation %u pixel layout", orientation);
    Check(pixels_ok, name);
  }

  nv::Image image;
  image.info.width = 3;
  image.info.height = 2;
  image.info.stride = 12;
  image.info.full_width = 3;
  image.info.full_height = 2;
  image.info.format = nv::PixelFormat::kBgra8;
  image.pixels = source;
  nv::ApplyOrientation(&image, 6);
  Check(image.info.width == 2 && image.info.height == 3 &&
            image.info.stride == 8 && image.info.full_width == 2 &&
            image.info.full_height == 3 && image.info.orientation_applied,
        "ApplyOrientation updates size/full size/flag");
}

void TestIccProfiles() {
  const std::vector<uint8_t> srgb = MakeDescProfile("sRGB IEC61966-2.1");
  Check(nv::IsValidIccProfile(srgb.data(), srgb.size()), "v2 profile is valid");
  Check(nv::IccProfileDescription(srgb.data(), srgb.size()) ==
            "sRGB IEC61966-2.1",
        "v2 desc text");
  Check(nv::IsSrgbProfile(srgb.data(), srgb.size()), "v2 sRGB is recognized");

  const std::vector<uint8_t> adobe = MakeDescProfile("Adobe RGB (1998)");
  Check(nv::IccProfileDescription(adobe.data(), adobe.size()) ==
            "Adobe RGB (1998)",
        "v2 non-sRGB desc text");
  Check(!nv::IsSrgbProfile(adobe.data(), adobe.size()),
        "v2 Adobe RGB is not sRGB");

  const std::vector<uint8_t> mluc = MakeMlucProfile("sRGB IEC61966-2.1");
  Check(nv::IccProfileDescription(mluc.data(), mluc.size()) ==
            "sRGB IEC61966-2.1",
        "v4 mluc text");
  Check(nv::IsSrgbProfile(mluc.data(), mluc.size()), "v4 sRGB is recognized");

  Check(!nv::IsSrgbProfile(nullptr, 0), "empty profile is not sRGB");
  std::vector<uint8_t> corrupt = srgb;
  corrupt[36] = 'X';
  Check(!nv::IsValidIccProfile(corrupt.data(), corrupt.size()),
        "missing acsp signature is rejected");
}

void TestRawFlipMapping() {
  const uint16_t expected[8] = {1, 2, 4, 3, 5, 8, 6, 7};
  for (int flip = 0; flip <= 7; ++flip) {
    char name[64]{};
    std::snprintf(name, sizeof(name), "LibRaw flip %d -> EXIF %u", flip,
                  static_cast<unsigned>(expected[flip]));
    Check(nv::libraw::ExifOrientationFromFlip(flip) == expected[flip], name);
  }
  Check(nv::libraw::ExifOrientationFromFlip(90) == 6,
        "LibRaw 90 degrees -> EXIF 6");
  Check(nv::libraw::ExifOrientationFromFlip(270) == 8,
        "LibRaw 270 degrees -> EXIF 8");
  Check(nv::libraw::ExifOrientationFromFlip(-1) == 1,
        "invalid LibRaw flip -> EXIF 1");
  Check(nv::libraw::FlipSwapsImageAxes(5) &&
            nv::libraw::FlipSwapsImageAxes(6) &&
            nv::libraw::FlipSwapsImageAxes(90) &&
            nv::libraw::FlipSwapsImageAxes(270),
        "LibRaw 90-degree flips swap axes");
  Check(!nv::libraw::FlipSwapsImageAxes(0) &&
            !nv::libraw::FlipSwapsImageAxes(3),
        "LibRaw 0/180 do not swap axes");
}

}  // namespace

int RunColorTests() {
  g_failures = 0;
  TestOrientationTransforms();
  TestIccProfiles();
  TestRawFlipMapping();
  return g_failures;
}
