#pragma once

#include "core/image.h"

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace nv::platform {

// Every container the app can write (規劃/22/14 §1). Availability is decided at
// runtime by ProbeEncoders(): a missing encoder never appears in the save
// dialog instead of failing after the user picked it.
enum class ImageSaveFormat {
  kPng,
  kJpeg,
  kBmp,
  kTiff,
  kWebp,
  kGif,
};

inline constexpr size_t kImageSaveFormatCount = 6;

// "png", "jpeg", "bmp", "tiff", "webp", "gif" (log/CLI tokens).
const char* SaveFormatToken(ImageSaveFormat format);
// Upper-case display label used in the save-options title: "JPEG", "WebP".
const wchar_t* SaveFormatLabel(ImageSaveFormat format);
// "* .png" style mask for an OPENFILENAMEW filter row (no dot escaping).
const wchar_t* SaveFormatPatterns(ImageSaveFormat format);
// Preferred extension including the leading dot (".jpg" for JPEG).
const wchar_t* SaveFormatExtension(ImageSaveFormat format);
// JPEG and WebP expose a 1..100 quality; the lossless containers do not.
bool SaveFormatHasQuality(ImageSaveFormat format);

ImageSaveFormat ImageFormatFromPath(const std::filesystem::path& path,
                                    ImageSaveFormat fallback);

// True only for a known save extension; the caller can then distinguish "the
// user typed .txt" from "no extension" and append the filter's extension.
bool TryImageFormatFromPath(const std::filesystem::path& path,
                            ImageSaveFormat* format);

// Encodes BGRA8 (straight alpha) and atomically replaces the destination via a
// sibling ".nvtmp" file, so a failed save never leaves a truncated image
// behind. JPEG/BMP are written as 24bpp BGR; PNG/TIFF keep alpha; WebP keeps
// alpha; GIF quantises and keeps 1-bit transparency.
bool SaveBgra(const std::filesystem::path& path, const uint8_t* bgra,
              uint32_t width, uint32_t height, uint32_t stride,
              ImageSaveFormat format, int quality, std::wstring* error);

bool SaveImage(const std::filesystem::path& path, const Image& image,
               ImageSaveFormat format, int quality, std::wstring* error);

// Encodes into memory with the exact same code path as SaveBgra(). Used by the
// save dialog's size estimate (規劃/22/14 §4.2) and by the startup encoder
// probe, which asks for a 2x2 image and treats success as "available".
bool EncodeBgraToMemory(const uint8_t* bgra, uint32_t width, uint32_t height,
                        uint32_t stride, ImageSaveFormat format, int quality,
                        std::vector<uint8_t>* output, std::wstring* error);

}  // namespace nv::platform
