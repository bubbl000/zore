// M2 format regression harness: PSD/PSB merged images, HEIC/AVIF through the
// system codec, multi-page TIFF (first page only), SVG, animated GIF/APNG
// frame composition and corrupt-file handling.
//
// Run from build\Release (defaults to ..\测试文件夹\images):
//   zoreview_formattests.exe
//   zoreview_formattests.exe --images "<path to 测试文件夹\images>"

#include "core/format_router.h"
#include "decoders/freeimage_decoder.h"

#include <windows.h>
#include <objbase.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

namespace {

int g_failures = 0;

void Check(bool condition, const char* name, const std::string& detail = {}) {
  std::printf("%s %s%s%s\n", condition ? "ok  " : "FAIL", name,
              detail.empty() ? "" : ": ", detail.c_str());
  if (!condition) ++g_failures;
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

std::wstring ToWide(const std::string& text) {
  if (text.empty()) return {};
  const int size = MultiByteToWideChar(CP_UTF8, 0, text.data(),
                                       static_cast<int>(text.size()), nullptr, 0);
  if (size <= 0) return {};
  std::wstring out(static_cast<size_t>(size), L'\0');
  MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()),
                      out.data(), size);
  return out;
}

struct Case {
  const char* file;
  const char* reference;
  const char* format;
  int tolerance;
  bool compare_alpha;
};

// References are generated next to the samples (see 测试文件夹\tools).
// HEIC/AVIF references come from an independent decoder (ffmpeg), so small
// codec differences are expected; exact comparisons are used for PSD and TIFF.
constexpr Case kCases[] = {
    {"psd/psd_rgb_raw.psd", "psd/reference/psd_rgb_raw.png", "PSD/PSB", 0, true},
    {"psd/psd_rgb_rle.psd", "psd/reference/psd_rgb_rle.png", "PSD/PSB", 0, true},
    {"psd/psd_rgb_zip.psd", "psd/reference/psd_rgb_zip.png", "PSD/PSB", 0, true},
    {"psd/psd_rgb_zip_pred.psd", "psd/reference/psd_rgb_zip_pred.png", "PSD/PSB", 0, true},
    {"psd/psd_rgba_rle.psd", "psd/reference/psd_rgba_rle.png", "PSD/PSB", 0, true},
    {"psd/psd_rgba_zip_pred.psd", "psd/reference/psd_rgba_zip_pred.png", "PSD/PSB", 0, true},
    {"psd/psd_gray.psd", "psd/reference/psd_gray.png", "PSD/PSB", 0, true},
    {"psd/psd_indexed_rle.psd", "psd/reference/psd_indexed_rle.png", "PSD/PSB", 0, true},
    {"psd/psd_bitmap_rle.psd", "psd/reference/psd_bitmap_rle.png", "PSD/PSB", 0, true},
    {"psd/psd_cmyk_zip.psd", "psd/reference/psd_cmyk_zip.png", "PSD/PSB", 0, true},
    {"psd/psd_cmyk_spots.psd", "psd/reference/psd_cmyk_spots.png", "PSD/PSB", 0, true},
    {"psd/psd_rgb16_rle.psd", "psd/reference/psd_rgb16_rle.png", "PSD/PSB", 0, true},
    {"psd/psd_rgb16_zip_pred.psd", "psd/reference/psd_rgb16_zip_pred.png", "PSD/PSB", 0, true},
    {"psd/psd_rgba16_zip_pred.psd", "psd/reference/psd_rgba16_zip_pred.png", "PSD/PSB", 0, true},
    {"psd/psd_rgb32_zip_pred.psd", "psd/reference/psd_rgb32_zip_pred.png", "PSD/PSB", 0, true},
    {"psd/psb_rgba_rle.psb", "psd/reference/psb_rgba_rle.png", "PSD/PSB", 0, true},
    {"psd/psb_rgb16_zip_pred.psb", "psd/reference/psb_rgb16_zip_pred.png", "PSD/PSB", 0, true},
    {"psd/psd_rgb_icc_linear.psd", "png/icc_linear.png", "PSD/PSB", 1, true},
    {"heic/example.heic", "heic/example.png", "HEIC/HEIF", 80, false},
    {"avif/photo.avif", "png/photo_rgb.png", "AVIF", 12, false},
    {"avif/alpha_noispe.avif", "avif/alpha_noispe.png", "AVIF", 25, false},
    {"tiff/multipage.tif", "tiff/multipage_first.png", "TIFF", 0, false},
    {"apng/anim.png", "apng/anim_first.png", "APNG", 0, true},
    {"svg/basic.svg", "svg/reference/basic.png", "SVG", 60, true},
    {"svg/viewbox.svg", "svg/reference/viewbox.png", "SVG", 60, true},
    {"svg/shapes.svg", "", "SVG", 0, true},
    {"tga/photo.tga", "tga/photo_ref.png", "TGA", 0, true},
    {"pcx/photo.pcx", "pcx/photo_ref.png", "PCX", 0, true},
    {"pnm/bw.pbm", "pnm/bw_ref.png", "PNM", 0, true},
    {"pnm/gray.pgm", "pnm/gray_ref.png", "PNM", 0, true},
    {"pnm/photo.ppm", "pnm/photo_ref.png", "PNM", 0, true},
    {"xbm/tiny.xbm", "xbm/tiny_ref.png", "XBM", 0, true},
    {"xpm/tiny.xpm", "xpm/tiny_ref.png", "XPM", 0, true},
    // 22/16 extra formats. Float references use the same fixed Reinhard +
    // gamma 2.2 mapping; HDR additionally carries RGBE quantization.
    {"extra/hdr/sample.hdr", "extra/reference/hdr.png", "HDR", 3, false},
    {"extra/exr/sample.exr", "extra/reference/float.png", "EXR", 3, false},
    {"extra/pfm/sample.pfm", "extra/reference/float.png", "PFM", 3, false},
    {"extra/jp2/sample.jp2", "extra/reference/base24.png", "JP2", 3, false},
    {"extra/jp2/sample.j2k", "extra/reference/base24.png", "JP2", 3, false},
    {"extra/jxr/sample.jxr", "extra/reference/base24.png", "JXR", 8, false},
    {"extra/jxr/sample.wdp", "extra/reference/base24.png", "JXR", 8, false},
    {"extra/wbmp/sample.wbmp", "extra/reference/wbmp.png", "WBMP", 0, false},
    {"extra/mng/sample.mng", "extra/reference/base24.png", "MNG", 0, false},
    {"extra/mng/sample.jng", "extra/reference/base24.png", "MNG", 10, false},
};

// Probe must reject these outright.
constexpr const char* kBadHeaders[] = {
    "psd/corrupt/bad_signature.psd",
    "psd/corrupt/bad_depth.psd",
    "corrupt/truncated.tif",
    "svg/corrupt/truncated.svg",
    "svg/corrupt/external.svg",
    "svg/corrupt/doctype.svg",
};

// Header is fine but the pixel payload is missing/truncated: probe may
// succeed, decode must fail.
constexpr const char* kBadPayloads[] = {
    "psd/corrupt/truncated.psd",
    "psd/corrupt/truncated_zip.psd",
    "psd/corrupt/bad_rle_table.psd",
    "psd/corrupt/no_image_data.psd",
    "corrupt/truncated.heic",
    "corrupt/truncated.avif",
    "corrupt/truncated.hdr",
    "corrupt/truncated.exr",
    "corrupt/truncated.pfm",
    "corrupt/truncated.jp2",
    "corrupt/truncated.jxr",
    "corrupt/truncated.wbmp",
    "corrupt/truncated.mng",
    "corrupt/truncated.jng",
};

int CompareImages(const nv::Image& actual, const nv::Image& expected,
                  bool compare_alpha, int* max_difference) {
  if (actual.info.width != expected.info.width ||
      actual.info.height != expected.info.height) {
    return -1;
  }
  int maximum = 0;
  for (uint32_t y = 0; y < actual.info.height; ++y) {
    const uint8_t* a = actual.pixels.data() + actual.info.stride * y;
    const uint8_t* b = expected.pixels.data() + expected.info.stride * y;
    for (uint32_t x = 0; x < actual.info.width; ++x, a += 4, b += 4) {
      for (int channel = 0; channel < 4; ++channel) {
        if (channel == 3 && !compare_alpha) continue;
        const int difference =
            std::abs(static_cast<int>(a[channel]) - static_cast<int>(b[channel]));
        maximum = std::max(maximum, difference);
      }
    }
  }
  *max_difference = maximum;
  return 0;
}

}  // namespace

int wmain(int argc, wchar_t** argv) {
  std::filesystem::path images = L"..\\测试文件夹\\images";
  for (int i = 1; i < argc; ++i) {
    if (std::wstring(argv[i]) == L"--images" && i + 1 < argc) {
      images = argv[++i];
    }
  }

  if (!std::filesystem::exists(images)) {
    std::printf("FAIL images directory not found: %s\n",
                Utf8(images.wstring()).c_str());
    return 1;
  }

  if (FAILED(CoInitializeEx(nullptr, COINIT_MULTITHREADED))) {
    std::printf("FAIL CoInitializeEx\n");
    return 1;
  }

  for (const Case& test : kCases) {
    const std::filesystem::path file = images / ToWide(test.file);
    const std::filesystem::path reference = images / ToWide(test.reference);
    const std::string name = test.file;
    const bool freeimage_only = std::string(test.format) == "TGA" ||
                                std::string(test.format) == "PCX" ||
                                std::string(test.format) == "PNM" ||
                                std::string(test.format) == "XBM" ||
                                std::string(test.format) == "XPM" ||
                                std::string(test.format) == "HDR" ||
                                std::string(test.format) == "EXR" ||
                                std::string(test.format) == "PFM" ||
                                std::string(test.format) == "WBMP" ||
                                std::string(test.format) == "MNG";
    if (freeimage_only && !nv::freeimage::Available()) {
      Check(true, name.c_str(), "skipped (FreeImage not built)");
      continue;
    }

    const bool has_reference = test.reference[0] != '\0';
    if (!std::filesystem::exists(file) ||
        (has_reference && !std::filesystem::exists(reference))) {
      Check(false, name.c_str(), "sample or reference missing");
      continue;
    }

    const nv::ProbeResult probe = nv::FormatRouter::Probe(file);
    if (!probe.ok()) {
      Check(false, name.c_str(), "probe failed: " + probe.error.message);
      continue;
    }
    const std::string format = nv::FormatNameUtf8(probe.format);
    if (format != test.format) {
      Check(false, name.c_str(), "format=" + format + " expected=" + test.format);
      continue;
    }

    const nv::DecodeResult decoded = nv::FormatRouter::Decode(file);
    if (!decoded.ok()) {
      Check(false, name.c_str(), "decode failed: " + decoded.error.message);
      continue;
    }
    if (!has_reference) {
      Check(true, name.c_str(),
            std::to_string(probe.width) + "x" + std::to_string(probe.height) +
                " decode ok (no pixel reference)");
      continue;
    }
    const nv::DecodeResult expected = nv::FormatRouter::Decode(reference);
    if (!expected.ok()) {
      Check(false, name.c_str(),
            "reference decode failed: " + expected.error.message);
      continue;
    }

    int maximum = 0;
    const int status = CompareImages(*decoded.image, *expected.image,
                                     test.compare_alpha, &maximum);
    if (status != 0) {
      Check(false, name.c_str(), "size differs from reference");
      continue;
    }
    if (maximum > test.tolerance) {
      Check(false, name.c_str(),
            "max pixel diff " + std::to_string(maximum) + " > " +
                std::to_string(test.tolerance));
      continue;
    }
    Check(true, name.c_str(),
          std::to_string(probe.width) + "x" + std::to_string(probe.height) +
              " max_diff=" + std::to_string(maximum));
  }

  for (const char* entry : kBadHeaders) {
    const std::filesystem::path file = images / ToWide(entry);
    const nv::ProbeResult probe = nv::FormatRouter::Probe(file);
    Check(!probe.ok(), entry, probe.ok() ? "unexpected success" : "rejected");
  }

  for (const char* entry : kBadPayloads) {
    const std::filesystem::path file = images / ToWide(entry);
    const nv::DecodeResult decoded = nv::FormatRouter::Decode(file);
    Check(!decoded.ok(), entry,
          decoded.ok() ? "unexpected success" : "decode rejected");
  }

  struct FrameCase {
    const char* file;
    const char* format;
    bool animated;
    uint32_t min_frames;
  };
  constexpr FrameCase kFrameCases[] = {
      {"apng/anim.png", "APNG", true, 4},
      {"apng/rgba_full.png", "APNG", true, 3},
      {"apng/single.png", "APNG", false, 1},
      {"apng/poster.png", "APNG", true, 2},
      {"gif/anim.gif", "GIF", true, 2},
      {"gif/disposal.gif", "GIF", true, 4},
      {"gif/static.gif", "GIF", false, 1},
      {"tiff/multipage.tif", "TIFF", true, 3},
      {"extra/mng/sample.mng", "MNG", false, 1},
      {"extra/mng/sample.jng", "MNG", false, 1},
  };
  for (const FrameCase& test : kFrameCases) {
    const nv::ProbeResult probe =
        nv::FormatRouter::Probe(images / ToWide(test.file));
    const std::string detail =
        probe.ok() ? "frames=" + std::to_string(probe.frame_count) +
                         " animated=" + (probe.animated ? "yes" : "no")
                   : probe.error.message;
    Check(probe.ok() &&
              std::string(nv::FormatNameUtf8(probe.format)) == test.format &&
              probe.animated == test.animated &&
              probe.frame_count >= test.min_frames,
          test.file, detail);
  }

  // JPEG XR alpha (規劃/22/16 §7): the WIC path must keep the left half
  // transparent and the right half opaque.
  {
    const std::filesystem::path alpha_file =
        images / L"extra" / L"jxr" / L"sample_alpha.jxr";
    const nv::ProbeResult probe = nv::FormatRouter::Probe(alpha_file);
    const nv::DecodeResult decoded = nv::FormatRouter::Decode(alpha_file);
    bool alpha_ok = probe.ok() && probe.has_alpha && decoded.ok();
    if (alpha_ok) {
      const nv::Image& image = *decoded.image;
      const auto alpha_at = [&image](uint32_t x, uint32_t y) {
        return image.pixels[image.info.stride * y + x * 4 + 3];
      };
      alpha_ok = alpha_at(10, 32) == 0 && alpha_at(80, 32) == 255;
    }
    Check(alpha_ok, "extra/jxr/sample_alpha.jxr",
          alpha_ok ? "alpha plane preserved"
                   : "alpha probe/decode failed");
  }

  // Animated GIF: every composited frame must match Pillow's frame reference.
  // `disposal.gif` uses disposal method 2, so this also covers the clear path.
  auto check_gif_animation = [&](const wchar_t* file, const wchar_t* prefix,
                                 size_t expected_frames,
                                 const std::string& label) {
    const std::filesystem::path gif_path = images / L"gif" / file;
    const nv::DecodeResult decoded = nv::FormatRouter::Decode(gif_path);
    const bool complete =
        decoded.ok() && decoded.image->frames.size() == expected_frames;
    Check(complete, label.c_str(),
          decoded.ok() ? "frames=" + std::to_string(decoded.image->frames.size())
                       : decoded.error.message);
    if (!complete) return;
    int maximum = 0;
    for (size_t i = 0; i < decoded.image->frames.size(); ++i) {
      const std::filesystem::path reference =
          images / L"gif" / (prefix + std::to_wstring(i) + L".png");
      const nv::DecodeResult expected = nv::FormatRouter::Decode(reference);
      nv::Image frame;
      frame.info = decoded.image->info;
      frame.pixels = decoded.image->frames[i].pixels;
      int frame_max = 0;
      if (!expected.ok() ||
          CompareImages(frame, *expected.image, true, &frame_max) != 0) {
        maximum = 255;
        break;
      }
      maximum = std::max(maximum, frame_max);
    }
    Check(maximum <= 1, label.c_str(),
          "max_diff=" + std::to_string(maximum));
  };
  check_gif_animation(L"anim.gif", L"anim_frame", 4, "gif/anim.gif");
  check_gif_animation(L"disposal.gif", L"disposal_frame", 4,
                      "gif/disposal.gif");
  {
    const nv::DecodeResult decoded =
        nv::FormatRouter::Decode(images / L"gif" / L"anim.gif");
    Check(decoded.ok() && !decoded.image->frames.empty() &&
              decoded.image->frames[0].delay_ms == 120,
          "gif/anim.gif delay",
          decoded.ok() && !decoded.image->frames.empty()
              ? std::to_string(decoded.image->frames[0].delay_ms) + "ms"
              : "no frames");
  }

  // APNG: every composited frame must match Pillow's reference. The samples
  // cover full-canvas frames, sub-rectangles with OVER blending, dispose
  // BACKGROUND (1) and PREVIOUS (2), a palette with tRNS, and sub-byte gray.
  //
  // Pillow composites APNG frames with `Image.paste(mask)`, which interpolates
  // the alpha channel too instead of using Porter-Duff source-over. Its RGB is
  // the spec result but its alpha is not, so OVER samples are compared on RGB
  // only and their alpha is asserted explicitly below.
  auto check_apng_animation = [&](const char* stem, size_t expected_frames,
                                  const std::vector<uint32_t>& delays,
                                  int tolerance, bool compare_alpha,
                                  size_t compare_frames) {
    const std::wstring wide = ToWide(stem);
    const std::string label = "apng/" + std::string(stem) + ".png";
    const nv::DecodeResult decoded =
        nv::FormatRouter::Decode(images / L"apng" / (wide + L".png"));
    const bool complete =
        decoded.ok() && decoded.image->frames.size() == expected_frames &&
        decoded.image->frames.size() == delays.size();
    Check(complete, label.c_str(),
          decoded.ok()
              ? "frames=" + std::to_string(decoded.image->frames.size())
              : decoded.error.message);
    if (!complete) return;

    int maximum = 0;
    bool timing_ok = true;
    const size_t limit = std::min(compare_frames, decoded.image->frames.size());
    for (size_t i = 0; i < decoded.image->frames.size(); ++i) {
      if (decoded.image->frames[i].delay_ms != delays[i]) timing_ok = false;
      if (i >= limit) continue;
      const std::filesystem::path reference =
          images / L"apng" / (wide + L"_f" + std::to_wstring(i) + L".png");
      const nv::DecodeResult expected = nv::FormatRouter::Decode(reference);
      nv::Image frame;
      frame.info = decoded.image->info;
      frame.pixels = decoded.image->frames[i].pixels;
      int frame_max = 0;
      if (!expected.ok() ||
          CompareImages(frame, *expected.image, compare_alpha, &frame_max) !=
              0) {
        maximum = 255;
        break;
      }
      maximum = std::max(maximum, frame_max);
    }
    Check(timing_ok, (label + " delay").c_str());
    Check(maximum <= tolerance, (label + " frames").c_str(),
          "max_diff=" + std::to_string(maximum));
  };
  check_apng_animation("rgba_full", 3, {120, 60, 240}, 0, true, 3);
  check_apng_animation("subrect_over", 4, {100, 100, 100, 100}, 2, false, 4);
  // Pillow's paste model also scales RGB by the mask when the canvas is
  // transparent, so its frame 2 is not the spec result; frames 0-1 and the
  // explicit pixels below cover the disposal path.
  check_apng_animation("dispose_bg", 3, {100, 100, 100}, 2, false, 2);
  check_apng_animation("dispose_prev", 3, {100, 100, 100}, 0, true, 3);
  check_apng_animation("palette4", 2, {80, 160}, 0, true, 2);
  check_apng_animation("gray2", 2, {100, 100}, 1, true, 2);

  // WebP animation (規劃/22/12 §3.4): the RIFF container supplies the loop
  // count and frame durations, WIC supplies the pixels. Pillow writes
  // full-canvas lossless frames, so every frame is compared exactly.
  {
    const nv::DecodeResult decoded =
        nv::FormatRouter::Decode(images / L"webp" / L"anim.webp");
    const bool ready =
        decoded.ok() && decoded.image->frames.size() == 4 &&
        decoded.image->loop_count == 2 && decoded.image->info.animated &&
        !decoded.image->info.frames_truncated;
    Check(ready, "webp/anim.webp",
          decoded.ok() ? "frames=" + std::to_string(decoded.image->frames.size()) +
                             " loop=" + std::to_string(decoded.image->loop_count)
                       : decoded.error.message);
    if (ready) {
      int maximum = 0;
      bool timing_ok = true;
      for (size_t i = 0; i < decoded.image->frames.size(); ++i) {
        if (decoded.image->frames[i].delay_ms != 120) timing_ok = false;
        const std::filesystem::path reference =
            images / L"webp" / (L"anim_frame" + std::to_wstring(i) + L".png");
        const nv::DecodeResult expected = nv::FormatRouter::Decode(reference);
        nv::Image frame;
        frame.info = decoded.image->info;
        frame.pixels = decoded.image->frames[i].pixels;
        int frame_max = 0;
        if (!expected.ok() ||
            CompareImages(frame, *expected.image, true, &frame_max) != 0) {
          maximum = 255;
          break;
        }
        maximum = std::max(maximum, frame_max);
      }
      Check(timing_ok, "webp/anim.webp delay");
      Check(maximum <= 2, "webp/anim.webp frames",
            "max_diff=" + std::to_string(maximum));
    }

    const nv::DecodeResult once =
        nv::FormatRouter::Decode(images / L"webp" / L"anim_loop1.webp");
    Check(once.ok() && once.image->loop_count == 1 &&
              once.image->frames.size() == 4,
          "webp/anim_loop1.webp loop",
          once.ok() ? "loop=" + std::to_string(once.image->loop_count) : "error");

    // A static WebP must never grow a frame sequence (the pill stays hidden).
    const nv::DecodeResult still =
        nv::FormatRouter::Decode(images / L"webp" / L"photo_lossy.webp");
    Check(still.ok() && still.image->frames.empty() && !still.image->info.animated,
          "webp/photo_lossy.webp stays static");
  }

  {
    // Spec-correct OVER: an opaque canvas stays opaque, and a frame drawn over
    // a cleared rectangle keeps its own eight-bit alpha.
    auto pixel = [](const nv::Image& image, size_t frame, uint32_t x,
                    uint32_t y) {
      return image.frames[frame].pixels.data() +
             static_cast<size_t>(y) * image.info.stride + x * 4;
    };
    const nv::DecodeResult over =
        nv::FormatRouter::Decode(images / L"apng" / L"subrect_over.png");
    const bool over_ready = over.ok() && over.image->frames.size() == 4;
    Check(over_ready && pixel(*over.image, 1, 10, 26)[3] == 255 &&
              pixel(*over.image, 1, 10, 26)[2] == 235 &&
              pixel(*over.image, 1, 60, 26)[3] == 255,
          "apng/subrect_over.png alpha",
          over_ready
              ? "alpha=" + std::to_string(pixel(*over.image, 1, 10, 26)[3])
              : "no frames");

    const nv::DecodeResult cleared =
        nv::FormatRouter::Decode(images / L"apng" / L"dispose_bg.png");
    const bool cleared_ready = cleared.ok() && cleared.image->frames.size() == 3;
    Check(cleared_ready && pixel(*cleared.image, 2, 10, 20)[3] == 128 &&
              pixel(*cleared.image, 2, 10, 20)[0] == 220 &&
              pixel(*cleared.image, 2, 40, 20)[3] == 0 &&
              pixel(*cleared.image, 2, 80, 50)[3] == 255,
          "apng/dispose_bg.png alpha",
          cleared_ready
              ? "covered=" +
                    std::to_string(pixel(*cleared.image, 2, 10, 20)[3]) +
                    " cleared=" +
                    std::to_string(pixel(*cleared.image, 2, 40, 20)[3])
              : "no frames");
  }

  {
    // 16-bit gray: Pillow clips 'I' mode, so the values are asserted directly.
    const nv::DecodeResult decoded =
        nv::FormatRouter::Decode(images / L"apng" / L"gray16.png");
    const bool ready = decoded.ok() && decoded.image->frames.size() == 2 &&
                       decoded.image->info.width == 64;
    Check(ready, "apng/gray16.png",
          decoded.ok() ? "frames=" + std::to_string(decoded.image->frames.size())
                       : decoded.error.message);
    if (ready) {
      const uint8_t first = decoded.image->frames[0].pixels[0];
      const uint8_t second = decoded.image->frames[1].pixels[0];
      Check(first == 128 && second == 240, "apng/gray16.png pixels",
            "frame0=" + std::to_string(first) +
                " frame1=" + std::to_string(second));
    }
  }

  {
    // The default image of this APNG is not part of the animation, so the
    // canvas must start transparent instead of reusing the poster.
    const nv::DecodeResult decoded =
        nv::FormatRouter::Decode(images / L"apng" / L"poster.png");
    const bool ready = decoded.ok() && decoded.image->frames.size() == 2;
    Check(ready, "apng/poster.png",
          decoded.ok() ? "frames=" + std::to_string(decoded.image->frames.size())
                       : decoded.error.message);
    if (ready) {
      const nv::Image& image = *decoded.image;
      const uint8_t* corner = image.frames[0].pixels.data();
      const size_t center =
          (static_cast<size_t>(24) * image.info.width + 32) * 4;
      const uint8_t* middle = image.frames[0].pixels.data() + center;
      Check(corner[3] == 0 && middle[2] == 200 && middle[1] == 0,
            "apng/poster.png composition",
            "corner_alpha=" + std::to_string(corner[3]));
    }
  }

  {
    // Real-world sample: 4 frames at 120 ms and an infinite loop count.
    const nv::DecodeResult decoded =
        nv::FormatRouter::Decode(images / L"apng" / L"anim.png");
    const bool ready = decoded.ok() && decoded.image->frames.size() == 4;
    Check(ready && decoded.image->loop_count == 0 &&
              decoded.image->frames[0].delay_ms == 120,
          "apng/anim.png timing",
          ready ? "frames=" + std::to_string(decoded.image->frames.size()) +
                      " delay=" +
                      std::to_string(decoded.image->frames[0].delay_ms) +
                      " loop=" + std::to_string(decoded.image->loop_count)
                : "no frames");
  }

  {
    // num_plays = 2 must survive into Image::loop_count.
    const nv::DecodeResult decoded =
        nv::FormatRouter::Decode(images / L"apng" / L"rgba_full.png");
    Check(decoded.ok() && decoded.image->loop_count == 2,
          "apng/rgba_full.png loop count",
          decoded.ok() ? std::to_string(decoded.image->loop_count) : "failed");
  }

  // V3-02: strong magic must beat a RAW extension; unknown extensions get one
  // WIC/FreeImage probe before the router reports "unsupported".
  {
    const std::filesystem::path temp = std::filesystem::temp_directory_path();
    const std::wstring prefix =
        L"nv_v302_" + std::to_wstring(GetCurrentProcessId()) + L"_";
    const auto temp_file = [&](const wchar_t* name) {
      return temp / (prefix + name);
    };
    const auto copy_sample = [&](const std::filesystem::path& from,
                                 const std::filesystem::path& to) {
      std::error_code ec;
      std::filesystem::remove(to, ec);
      ec.clear();
      std::filesystem::copy_file(from, to,
                                 std::filesystem::copy_options::overwrite_existing,
                                 ec);
      return !ec;
    };

    {
      const auto file = temp_file(L"photo.cr2");
      const bool copied = copy_sample(images / L"jpeg" / L"photo_1600.jpg", file);
      const nv::ImageFormat detected = nv::FormatRouter::Detect(file);
      const nv::ProbeResult probe = nv::FormatRouter::Probe(file);
      const nv::DecodeResult decoded = nv::FormatRouter::Decode(file);
      Check(copied && detected == nv::ImageFormat::kJpeg && probe.ok() &&
                probe.format == nv::ImageFormat::kJpeg && decoded.ok(),
            "v3-02: JPEG renamed .cr2",
            copied ? std::string("detect=") + nv::FormatNameUtf8(detected) +
                         " probe=" +
                         (probe.ok() ? nv::FormatNameUtf8(probe.format)
                                     : probe.error.message)
                   : "copy failed");
      std::error_code ec;
      std::filesystem::remove(file, ec);
    }

    {
      const auto file = temp_file(L"no_extension_png");
      const bool copied = copy_sample(images / L"png" / L"photo_rgb.png", file);
      const nv::ImageFormat detected = nv::FormatRouter::Detect(file);
      const nv::ProbeResult probe = nv::FormatRouter::Probe(file);
      const nv::DecodeResult decoded = nv::FormatRouter::Decode(file);
      Check(copied && detected == nv::ImageFormat::kPng && probe.ok() &&
                probe.format == nv::ImageFormat::kPng && decoded.ok(),
            "v3-02: PNG without extension",
            copied ? std::string("detect=") + nv::FormatNameUtf8(detected) +
                         " probe=" +
                         (probe.ok() ? nv::FormatNameUtf8(probe.format)
                                     : probe.error.message)
                   : "copy failed");
      std::error_code ec;
      std::filesystem::remove(file, ec);
    }

    {
      const auto file = temp_file(L"text.png");
      {
        std::ofstream out(file, std::ios::binary);
        out << "not an image\n";
      }
      const nv::ProbeResult probe = nv::FormatRouter::Probe(file);
      const nv::DecodeResult decoded = nv::FormatRouter::Decode(file);
      Check(!probe.ok() && !decoded.ok(), "v3-02: text renamed .png stays an error",
            probe.ok() ? "probe unexpectedly succeeded" : "probe rejected");
      std::error_code ec;
      std::filesystem::remove(file, ec);
    }

    if (nv::freeimage::Available()) {
      const auto file = temp_file(L"no_extension_tga");
      const bool copied = copy_sample(images / L"tga" / L"photo.tga", file);
      const nv::ProbeResult probe = nv::FormatRouter::Probe(file);
      const nv::DecodeResult decoded = nv::FormatRouter::Decode(file);
      Check(copied && probe.ok() && probe.format == nv::ImageFormat::kTga &&
                decoded.ok(),
            "v3-02: TGA without extension",
            copied ? std::string("probe=") +
                         (probe.ok() ? nv::FormatNameUtf8(probe.format)
                                     : probe.error.message)
                   : "copy failed");
      std::error_code ec;
      std::filesystem::remove(file, ec);
    }
  }

  CoUninitialize();
  std::printf("failures=%d\n", g_failures);
  return g_failures == 0 ? 0 : 1;
}
