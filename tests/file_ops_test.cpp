// Automated checks for the M1-6 file operations that do not need the GUI:
// atomic PNG/JPEG/BMP save + reload, rename, recycle-bin delete, and the
// clipboard DIB/text formats.
//
// The clipboard is replaced while this runs; the optional --wallpaper check
// temporarily changes the desktop wallpaper and restores the previous one.

#include "core/format_router.h"
#include "platform/clipboard.h"
#include "platform/encoder_probe.h"
#include "platform/file_ops.h"
#include "platform/image_io.h"

#include <windows.h>
#include <objbase.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cwchar>
#include <filesystem>
#include <iterator>
#include <string>
#include <vector>

namespace {

int g_failures = 0;

void Check(bool condition, const char* name) {
  std::printf("%s %s\n", condition ? "ok  " : "FAIL", name);
  if (!condition) ++g_failures;
}

nv::Image MakeImage(uint32_t width, uint32_t height) {
  nv::Image image;
  image.info.width = width;
  image.info.height = height;
  image.info.stride = width * 4u;
  image.info.format = nv::PixelFormat::kBgra8;
  image.info.has_alpha = true;
  image.pixels.resize(static_cast<size_t>(image.info.stride) * height);
  for (uint32_t y = 0; y < height; ++y) {
    for (uint32_t x = 0; x < width; ++x) {
      const size_t index = (static_cast<size_t>(y) * width + x) * 4u;
      image.pixels[index + 0] = static_cast<uint8_t>(
          width > 1 ? (x * 255u) / (width - 1u) : 0u);
      image.pixels[index + 1] = static_cast<uint8_t>(
          height > 1 ? (y * 255u) / (height - 1u) : 0u);
      const uint32_t red = (width > 1 ? (x * 255u) / (width - 1u) : 0u) +
                           (height > 1 ? (y * 255u) / (height - 1u) : 0u);
      image.pixels[index + 2] = static_cast<uint8_t>(red / 2u);
      image.pixels[index + 3] = static_cast<uint8_t>(32u + ((x * 5u + y * 3u) % 224u));
    }
  }
  return image;
}

std::string Utf8(const std::wstring& text) {
  if (text.empty()) return {};
  const int size = WideCharToMultiByte(CP_UTF8, 0, text.data(),
                                       static_cast<int>(text.size()), nullptr, 0,
                                       nullptr, nullptr);
  if (size <= 0) return {};
  std::string out(static_cast<size_t>(size), '\0');
  WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()),
                      out.data(), size, nullptr, nullptr);
  return out;
}

int ChannelDiff(uint8_t a, uint8_t b) {
  return std::abs(static_cast<int>(a) - static_cast<int>(b));
}

bool CloseTo(const nv::Image& actual, const nv::Image& expected, int tolerance,
             bool compare_alpha) {
  if (actual.info.width != expected.info.width ||
      actual.info.height != expected.info.height) {
    return false;
  }
  for (uint32_t y = 0; y < expected.info.height; ++y) {
    for (uint32_t x = 0; x < expected.info.width; ++x) {
      const size_t a = (static_cast<size_t>(y) * expected.info.width + x) * 4u;
      for (int channel = 0; channel < 4; ++channel) {
        if (channel == 3 && !compare_alpha) continue;
        if (ChannelDiff(actual.pixels[a + channel], expected.pixels[a + channel]) >
            tolerance) {
          return false;
        }
      }
    }
  }
  return true;
}

// Compares RGB only where the source is opaque: GIF/WebP transparent pixels
// have no meaningful colour after a round trip.
bool OpaqueCloseTo(const nv::Image& actual, const nv::Image& expected,
                   int tolerance) {
  if (actual.info.width != expected.info.width ||
      actual.info.height != expected.info.height) {
    return false;
  }
  for (size_t i = 0; i + 3 < expected.pixels.size(); i += 4) {
    if (expected.pixels[i + 3] < 128) continue;
    for (int channel = 0; channel < 3; ++channel) {
      if (ChannelDiff(actual.pixels[i + channel], expected.pixels[i + channel]) >
          tolerance) {
        return false;
      }
    }
  }
  return true;
}

bool AlphaMatches(const nv::Image& actual, const nv::Image& expected) {
  if (actual.pixels.size() != expected.pixels.size()) return false;
  for (size_t i = 3; i < expected.pixels.size(); i += 4) {
    if (actual.pixels[i] != expected.pixels[i]) return false;
  }
  return true;
}

}  // namespace

int wmain(int argc, wchar_t** argv) {
  const bool test_wallpaper =
      argc > 1 && std::wstring(argv[1]) == L"--wallpaper";

  const HRESULT com_hr = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
  if (FAILED(com_hr) && com_hr != RPC_E_CHANGED_MODE) {
    std::printf("FAIL CoInitializeEx\n");
    return 1;
  }

  std::error_code ec;
  std::filesystem::path directory =
      std::filesystem::temp_directory_path(ec) /
      (L"nv-fileops-" + std::to_wstring(GetCurrentProcessId()));
  std::filesystem::remove_all(directory, ec);
  std::filesystem::create_directories(directory, ec);
  if (ec) {
    std::printf("FAIL create temp directory\n");
    return 1;
  }

  const nv::Image image = MakeImage(97, 61);

  // PNG keeps straight alpha exactly.
  const std::filesystem::path png = directory / L"sample.png";
  std::wstring error;
  const bool png_saved =
      nv::platform::SaveImage(png, image, nv::platform::ImageSaveFormat::kPng,
                              95, &error);
  Check(png_saved, "save PNG");
  if (!png_saved) std::printf("  error=%s\n", Utf8(error).c_str());
  nv::DecodeResult png_result = nv::FormatRouter::Decode(png);
  Check(png_result.ok(), "reload PNG");
  if (png_result.ok()) {
    Check(png_result.image->info.width == image.info.width &&
              png_result.image->info.height == image.info.height,
          "PNG dimensions");
    Check(png_result.image->pixels == image.pixels, "PNG pixels (alpha exact)");
  }

  // JPEG is lossy: dimensions must match and the center sample must be close.
  const std::filesystem::path jpeg = directory / L"sample.jpg";
  Check(nv::platform::SaveImage(jpeg, image,
                                nv::platform::ImageSaveFormat::kJpeg, 92,
                                &error),
        "save JPEG");
  nv::DecodeResult jpeg_result = nv::FormatRouter::Decode(jpeg);
  Check(jpeg_result.ok(), "reload JPEG");
  if (jpeg_result.ok()) {
    Check(jpeg_result.image->info.width == image.info.width &&
              jpeg_result.image->info.height == image.info.height,
          "JPEG dimensions");
    const bool jpeg_close = CloseTo(*jpeg_result.image, image, 20, false);
    Check(jpeg_close, "JPEG pixels within tolerance");
    if (!jpeg_close) {
      int max_diff = 0;
      for (size_t i = 0; i < image.pixels.size(); ++i) {
        if ((i % 4) == 3) continue;
        max_diff = std::max(max_diff,
                            ChannelDiff(jpeg_result.image->pixels[i],
                                        image.pixels[i]));
      }
      std::printf("  jpeg_max_diff=%d\n", max_diff);
    }
  }

  // BMP drops alpha; RGB must round-trip exactly.
  const std::filesystem::path bmp = directory / L"sample.bmp";
  Check(nv::platform::SaveImage(bmp, image, nv::platform::ImageSaveFormat::kBmp,
                                95, &error),
        "save BMP");
  nv::DecodeResult bmp_result = nv::FormatRouter::Decode(bmp);
  Check(bmp_result.ok(), "reload BMP");
  if (bmp_result.ok()) {
    Check(CloseTo(*bmp_result.image, image, 0, false), "BMP pixels exact");
  }

  // TIFF also encodes 24bpp; RGB must round-trip exactly.
  const std::filesystem::path tiff = directory / L"sample.tif";
  Check(nv::platform::SaveImage(tiff, image,
                                nv::platform::ImageSaveFormat::kTiff, 95,
                                &error),
        "save TIFF");
  nv::DecodeResult tiff_result = nv::FormatRouter::Decode(tiff);
  Check(tiff_result.ok(), "reload TIFF");
  if (tiff_result.ok()) {
    Check(CloseTo(*tiff_result.image, image, 0, false), "TIFF pixels exact");
  }

  // --- v2.0 S3 / 22-14: WebP + GIF + size estimates ------------------------
  // The probe is what the save dialog shows: a format that fails here must not
  // appear as an option.
  const nv::platform::EncoderCaps& caps = nv::platform::ProbeEncoders();
  Check(caps.png, "encoder: PNG");
  Check(caps.jpeg, "encoder: JPEG");
  Check(caps.bmp, "encoder: BMP");
  Check(caps.tiff, "encoder: TIFF");
  std::printf("info encoders=%s\n",
              nv::platform::EncoderList().c_str());

  if (caps.webp) {
    // A smooth 256x256 sample: lossy WebP keeps alpha exactly but shifts RGB.
    nv::Image smooth = MakeImage(256, 256);
    const std::filesystem::path webp = directory / L"sample.webp";
    Check(nv::platform::SaveImage(webp, smooth,
                                  nv::platform::ImageSaveFormat::kWebp, 90,
                                  &error),
          "save WebP");
    nv::DecodeResult webp_result = nv::FormatRouter::Decode(webp);
    Check(webp_result.ok(), "reload WebP");
    if (webp_result.ok()) {
      Check(webp_result.image->info.width == 256 &&
                webp_result.image->info.height == 256,
            "WebP dimensions");
      Check(CloseTo(*webp_result.image, smooth, 24, false),
            "WebP pixels within tolerance");
      Check(webp_result.image->info.has_alpha, "WebP reports an alpha channel");
      Check(AlphaMatches(*webp_result.image, smooth),
            "WebP alpha channel round-trips exactly");
    }

    // Quality must change the encoded size (the slider is not cosmetic).
    const std::filesystem::path webp_low = directory / L"sample-q20.webp";
    Check(nv::platform::SaveImage(webp_low, smooth,
                                  nv::platform::ImageSaveFormat::kWebp, 20,
                                  &error),
          "save WebP q20");
    std::error_code size_ec;
    const uintmax_t high = std::filesystem::file_size(webp, size_ec);
    const uintmax_t low = std::filesystem::file_size(webp_low, size_ec);
    Check(low < high, "WebP quality 20 is smaller than 90");

    // The estimate must match the file the save path writes (<10%).
    std::vector<uint8_t> estimate;
    Check(nv::platform::EncodeBgraToMemory(
              smooth.pixels.data(), smooth.info.width, smooth.info.height,
              smooth.info.stride, nv::platform::ImageSaveFormat::kWebp, 90,
              &estimate, &error),
          "estimate WebP");
    if (high > 0) {
      const double error_ratio =
          std::abs(static_cast<double>(estimate.size()) -
                   static_cast<double>(high)) /
          static_cast<double>(high);
      Check(error_ratio < 0.10, "WebP estimate within 10% of the file");
    }
  } else {
    std::printf("skip WebP encoder checks (no encoder)\n");
  }

  if (caps.gif) {
    // GIF: 255-colour quantisation, 1-bit alpha. A flat colour patch must come
    // back exactly; a transparent pixel must decode back as transparent.
    nv::Image flat = MakeImage(64, 48);
    for (size_t i = 0; i < flat.pixels.size(); i += 4) {
      flat.pixels[i + 0] = 40;
      flat.pixels[i + 1] = 160;
      flat.pixels[i + 2] = 220;
      flat.pixels[i + 3] = 255;
    }
    for (uint32_t y = 8; y < 24; ++y) {
      for (uint32_t x = 8; x < 24; ++x) {
        flat.pixels[(static_cast<size_t>(y) * 64 + x) * 4u + 3u] = 0;
      }
    }
    const std::filesystem::path gif = directory / L"sample.gif";
    Check(nv::platform::SaveImage(gif, flat,
                                  nv::platform::ImageSaveFormat::kGif, 90,
                                  &error),
          "save GIF");
    nv::DecodeResult gif_result = nv::FormatRouter::Decode(gif);
    Check(gif_result.ok(), "reload GIF");
    if (gif_result.ok()) {
      Check(gif_result.image->info.width == 64 &&
                gif_result.image->info.height == 48,
            "GIF dimensions");
      // Opaque area keeps the flat colour (within quantisation error).
      Check(OpaqueCloseTo(*gif_result.image, flat, 12),
            "GIF opaque pixels within tolerance");
      const size_t transparent_index = (12u * 64u + 12u) * 4u;
      if (gif_result.image->info.has_alpha) {
        Check(gif_result.image->pixels[transparent_index + 3u] < 128,
              "GIF transparent pixel stays transparent");
        const size_t opaque_index = (40u * 64u + 40u) * 4u;
        Check(gif_result.image->pixels[opaque_index + 3u] >= 250,
              "GIF opaque pixel stays opaque");
      } else {
        std::printf("skip GIF alpha check (decoder reports no alpha)\n");
      }
    }
  } else {
    std::printf("skip GIF encoder checks (no encoder)\n");
  }

  {
    // JPEG estimate (the dialog's default format) must match the real file.
    std::vector<uint8_t> estimate;
    Check(nv::platform::EncodeBgraToMemory(
              image.pixels.data(), image.info.width, image.info.height,
              image.info.stride, nv::platform::ImageSaveFormat::kJpeg, 92,
              &estimate, &error),
          "estimate JPEG");
    std::error_code size_ec;
    const uintmax_t jpeg_bytes = std::filesystem::file_size(jpeg, size_ec);
    if (jpeg_bytes > 0) {
      const double error_ratio =
          std::abs(static_cast<double>(estimate.size()) -
                   static_cast<double>(jpeg_bytes)) /
          static_cast<double>(jpeg_bytes);
      Check(error_ratio < 0.10, "JPEG estimate within 10% of the file");
    }
  }

  // Overwrite path: save a second, different image over the first file.
  const nv::Image replacement = MakeImage(33, 19);
  Check(nv::platform::SaveImage(png, replacement,
                                nv::platform::ImageSaveFormat::kPng, 95,
                                &error),
        "overwrite PNG atomically");
  nv::DecodeResult replaced = nv::FormatRouter::Decode(png);
  Check(replaced.ok() && replaced.image->info.width == 33 &&
            replaced.image->info.height == 19,
        "overwritten PNG dimensions");

  // Rename + delete.
  const std::filesystem::path renamed = directory / L"renamed.png";
  Check(nv::platform::RenamePath(png, renamed, &error), "rename file");
  Check(std::filesystem::exists(renamed, ec) && !std::filesystem::exists(png, ec),
        "rename result on disk");

  const std::filesystem::path deleted = directory / L"deleted.png";
  Check(nv::platform::SaveImage(deleted, replacement,
                                nv::platform::ImageSaveFormat::kPng, 95,
                                &error),
        "create file for recycle bin");
  Check(nv::platform::DeleteToRecycleBin(deleted, &error),
        "delete to recycle bin");
  Check(!std::filesystem::exists(deleted, ec), "deleted file is gone");

  // Clipboard: CF_DIB must contain a 32bpp bottom-up DIB.
  Check(nv::platform::CopyImageToClipboard(nullptr, image, &error),
        "copy image to clipboard");
  bool dib_ok = false;
  bool dib_alpha_ok = false;
  for (int attempt = 0; attempt < 5 && !dib_ok; ++attempt) {
    if (OpenClipboard(nullptr)) {
      HANDLE data = GetClipboardData(CF_DIB);
      if (data != nullptr) {
        auto* header = static_cast<BITMAPINFOHEADER*>(GlobalLock(data));
        if (header != nullptr) {
          dib_ok = header->biSize == sizeof(BITMAPINFOHEADER) &&
                   header->biWidth == static_cast<LONG>(image.info.width) &&
                   header->biHeight == static_cast<LONG>(image.info.height) &&
                   header->biBitCount == 32 &&
                   header->biCompression == BI_RGB;
          if (dib_ok) {
            const auto* pixels = reinterpret_cast<const uint8_t*>(header) +
                                 sizeof(BITMAPINFOHEADER);
            const size_t last_row =
                static_cast<size_t>(image.info.height - 1) *
                image.info.width * 4u;
            dib_alpha_ok =
                pixels[3] == image.pixels[last_row + 3];
          }
          GlobalUnlock(data);
        }
      }
      CloseClipboard();
    } else {
      Sleep(20);
    }
  }
  Check(dib_ok, "clipboard CF_DIB header");
  Check(dib_alpha_ok, "clipboard CF_DIB pixels");

  Check(nv::platform::CopyTextToClipboard(nullptr, L"zoreview-path", &error),
        "copy text to clipboard");
  bool text_ok = false;
  for (int attempt = 0; attempt < 5 && !text_ok; ++attempt) {
    if (OpenClipboard(nullptr)) {
      HANDLE data = GetClipboardData(CF_UNICODETEXT);
      if (data != nullptr) {
        const auto* text = static_cast<const wchar_t*>(GlobalLock(data));
        if (text != nullptr) {
          text_ok = std::wstring(text) == L"zoreview-path";
          GlobalUnlock(data);
        }
      }
      CloseClipboard();
    } else {
      Sleep(20);
    }
  }
  Check(text_ok, "clipboard CF_UNICODETEXT");

  if (test_wallpaper) {
    wchar_t previous[32768]{};
    const bool had_previous =
        SystemParametersInfoW(SPI_GETDESKWALLPAPER,
                              static_cast<UINT>(std::size(previous)), previous,
                              0) != FALSE;
    const std::filesystem::path wallpaper = directory / L"wallpaper.bmp";
    bool wallpaper_ok =
        nv::platform::SaveImage(wallpaper, image,
                                nv::platform::ImageSaveFormat::kBmp, 95,
                                &error) &&
        nv::platform::SetDesktopWallpaper(wallpaper, &error);
    if (wallpaper_ok) {
      wchar_t current[32768]{};
      wallpaper_ok =
          SystemParametersInfoW(SPI_GETDESKWALLPAPER,
                                static_cast<UINT>(std::size(current)), current,
                                0) != FALSE &&
          _wcsicmp(current, wallpaper.c_str()) == 0;
    }
    if (had_previous && previous[0] != L'\0') {
      nv::platform::SetDesktopWallpaper(previous, &error);
    }
    Check(wallpaper_ok, "set desktop wallpaper (restored afterwards)");
  }

  std::filesystem::remove_all(directory, ec);
  if (g_failures != 0) {
    std::printf("%d file-op check(s) failed\n", g_failures);
    return 1;
  }
  std::printf("all file-op checks passed\n");
  return 0;
}
