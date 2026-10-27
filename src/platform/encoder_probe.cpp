#include "platform/encoder_probe.h"

#include <cstdint>
#include <vector>

namespace nv::platform {
namespace {

constexpr ImageSaveFormat kFormats[kImageSaveFormatCount] = {
    ImageSaveFormat::kPng,  ImageSaveFormat::kJpeg, ImageSaveFormat::kBmp,
    ImageSaveFormat::kTiff, ImageSaveFormat::kWebp, ImageSaveFormat::kGif,
};

// 2x2 BGRA with one opaque and one transparent pixel: the same probe therefore
// also proves that the alpha-capable paths (PNG/TIFF/GIF/WebP) accept alpha.
constexpr uint8_t kProbePixels[16] = {
    0, 0, 0, 255,      // opaque black
    255, 255, 255, 0,  // transparent white
    255, 0, 0, 255,     // opaque blue (BGRA)
    0, 255, 0, 255,     // opaque green
};

bool ProbeOne(ImageSaveFormat format) {
  std::vector<uint8_t> bytes;
  std::wstring error;
  if (!EncodeBgraToMemory(kProbePixels, 2, 2, 8, format, 90, &bytes, &error)) {
    return false;
  }
  return !bytes.empty();
}

EncoderCaps BuildCaps() {
  EncoderCaps caps;
  caps.png = ProbeOne(ImageSaveFormat::kPng);
  caps.jpeg = ProbeOne(ImageSaveFormat::kJpeg);
  caps.bmp = ProbeOne(ImageSaveFormat::kBmp);
  caps.tiff = ProbeOne(ImageSaveFormat::kTiff);
  caps.webp = ProbeOne(ImageSaveFormat::kWebp);
  caps.gif = ProbeOne(ImageSaveFormat::kGif);
  return caps;
}

}  // namespace

const EncoderCaps& ProbeEncoders() {
  static const EncoderCaps caps = BuildCaps();
  return caps;
}

bool EncoderAvailable(ImageSaveFormat format) {
  const EncoderCaps& caps = ProbeEncoders();
  switch (format) {
    case ImageSaveFormat::kJpeg: return caps.jpeg;
    case ImageSaveFormat::kBmp: return caps.bmp;
    case ImageSaveFormat::kTiff: return caps.tiff;
    case ImageSaveFormat::kWebp: return caps.webp;
    case ImageSaveFormat::kGif: return caps.gif;
    case ImageSaveFormat::kPng:
    default: return caps.png;
  }
}

std::string EncoderList() {
  const EncoderCaps& caps = ProbeEncoders();
  std::string list;
  const auto add = [&list](const char* token) {
    if (!list.empty()) list += ',';
    list += token;
  };
  if (caps.png) add("png");
  if (caps.jpeg) add("jpeg");
  if (caps.bmp) add("bmp");
  if (caps.tiff) add("tiff");
  if (caps.webp) add("webp");
  if (caps.gif) add("gif");
  return list;
}

}  // namespace nv::platform
