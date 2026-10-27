// Generates the extra-format samples used by the format matrix (規劃/22/16 §4).
//
// The generator links the same FreeImage build the application uses, so a
// format is only produced when that build can actually write it. Formats whose
// plugin is read-only are reported as SKIP and must be documented in
// SOURCES.md instead of being faked.

#include <FreeImage.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <cwchar>
#include <filesystem>
#include <string>
#include <vector>

namespace {

constexpr int kWidth = 96;
constexpr int kHeight = 64;

void Fill24(FIBITMAP* dib) {
  for (int y = 0; y < kHeight; ++y) {
    BYTE* line = FreeImage_GetScanLine(dib, kHeight - 1 - y);
    for (int x = 0; x < kWidth; ++x) {
      const double fx = static_cast<double>(x) / (kWidth - 1);
      const double fy = static_cast<double>(y) / (kHeight - 1);
      BYTE* pixel = line + x * 3;
      pixel[FI_RGBA_BLUE] = static_cast<BYTE>(std::lround(255.0 * fy));
      pixel[FI_RGBA_GREEN] = static_cast<BYTE>(std::lround(255.0 * (1.0 - fx)));
      pixel[FI_RGBA_RED] =
          static_cast<BYTE>(std::lround(255.0 * (0.25 + 0.75 * fx * fy)));
    }
  }
}

// RGB float with values above 1.0 so a tone-mapped decode is visibly different
// from a naive clamp.
void FillRgbFloat(FIBITMAP* dib) {
  for (int y = 0; y < kHeight; ++y) {
    float* line =
        reinterpret_cast<float*>(FreeImage_GetScanLine(dib, kHeight - 1 - y));
    for (int x = 0; x < kWidth; ++x) {
      const double fx = static_cast<double>(x) / (kWidth - 1);
      const double fy = static_cast<double>(y) / (kHeight - 1);
      float* pixel = line + x * 3;
      pixel[FI_RGBA_RED] = static_cast<float>(0.2 + 7.0 * fx);
      pixel[FI_RGBA_GREEN] = static_cast<float>(0.2 + 7.0 * fy);
      pixel[FI_RGBA_BLUE] = static_cast<float>(0.2 + 3.0 * (1.0 - fx * fy));
    }
  }
}

void FillRgbaFloat(FIBITMAP* dib) {
  FillRgbFloat(dib);
  for (int y = 0; y < kHeight; ++y) {
    float* line =
        reinterpret_cast<float*>(FreeImage_GetScanLine(dib, kHeight - 1 - y));
    for (int x = 0; x < kWidth; ++x) {
      line[x * 4 + 3] = x < kWidth / 2 ? 0.6f : 1.0f;
    }
  }
}

void ReportFormats() {
  std::printf("fif,name,read,write,extensions\n");
  for (int i = 0; i < FreeImage_GetFIFCount(); ++i) {
    const auto fif = static_cast<FREE_IMAGE_FORMAT>(i);
    const char* name = FreeImage_GetFormatFromFIF(fif);
    const char* extensions = FreeImage_GetFIFExtensionList(fif);
    std::printf("%d,%s,%d,%d,%s\n", i, name != nullptr ? name : "?",
                FreeImage_FIFSupportsReading(fif) ? 1 : 0,
                FreeImage_FIFSupportsWriting(fif) ? 1 : 0,
                extensions != nullptr ? extensions : "");
  }
}

struct Target {
  const char* extension;
  FREE_IMAGE_FORMAT fif;
  bool float_image;
};

const char* FolderForExtension(const char* extension) {
  if (std::strcmp(extension, ".hdr") == 0) return "hdr";
  if (std::strcmp(extension, ".exr") == 0) return "exr";
  if (std::strcmp(extension, ".pfm") == 0) return "pfm";
  if (std::strcmp(extension, ".jp2") == 0 ||
      std::strcmp(extension, ".j2k") == 0) {
    return "jp2";
  }
  if (std::strcmp(extension, ".jxr") == 0) return "jxr";
  if (std::strcmp(extension, ".wbmp") == 0) return "wbmp";
  return "mng";  // .mng / .jng
}

bool SaveOne(const std::filesystem::path& directory, const Target& target,
             FIBITMAP* base) {
  FIBITMAP* dib = nullptr;
  FIBITMAP* owned = nullptr;
  if (target.float_image) {
    owned = FreeImage_AllocateT(FIT_RGBF, kWidth, kHeight);
    if (owned == nullptr) return false;
    FillRgbFloat(owned);
    dib = owned;
  } else if (std::strcmp(target.extension, ".wbmp") == 0) {
    FIBITMAP* gray = FreeImage_ConvertTo8Bits(base);
    if (gray == nullptr) return false;
    owned = FreeImage_Threshold(gray, 128);
    FreeImage_Unload(gray);
    if (owned == nullptr) return false;
    dib = owned;
  } else {
    dib = base;
  }

  const std::filesystem::path format_dir =
      directory / FolderForExtension(target.extension);
  std::error_code ec;
  std::filesystem::create_directories(format_dir, ec);
  const std::filesystem::path path =
      format_dir / (std::string("sample") + target.extension);
  const BOOL saved = FreeImage_SaveU(target.fif, dib, path.c_str(), 0);
  if (owned != nullptr) FreeImage_Unload(owned);
  std::printf("%s %s\n", saved ? "WROTE" : "FAILED", path.string().c_str());
  return saved != FALSE;
}

}  // namespace

int wmain(int argc, wchar_t** argv) {
  FreeImage_Initialise(FALSE);
  FreeImage_SetOutputMessage([](FREE_IMAGE_FORMAT, const char* message) {
    if (message != nullptr) std::printf("FreeImage: %s\n", message);
  });

  std::filesystem::path output;
  std::filesystem::path load_path;
  bool report = false;
  for (int i = 1; i < argc; ++i) {
    if (std::wcscmp(argv[i], L"--report") == 0) {
      report = true;
    } else if (std::wcscmp(argv[i], L"--out") == 0 && i + 1 < argc) {
      output = std::filesystem::path(argv[++i]);
    } else if (std::wcscmp(argv[i], L"--load") == 0 && i + 1 < argc) {
      load_path = std::filesystem::path(argv[++i]);
    }
  }

  if (!load_path.empty()) {
    const FREE_IMAGE_FORMAT fif = FreeImage_GetFileTypeU(load_path.c_str(), 0);
    std::printf("fif=%d (%s) read=%d\n", static_cast<int>(fif),
                fif != FIF_UNKNOWN ? FreeImage_GetFormatFromFIF(fif) : "unknown",
                fif != FIF_UNKNOWN && FreeImage_FIFSupportsReading(fif) ? 1 : 0);
    FIBITMAP* dib = FreeImage_LoadU(fif, load_path.c_str(), 0);
    std::printf("load=%s\n", dib != nullptr ? "ok" : "null");
    if (dib != nullptr) {
      std::printf("size=%dx%d type=%d\n", FreeImage_GetWidth(dib),
                  FreeImage_GetHeight(dib),
                  static_cast<int>(FreeImage_GetImageType(dib)));
      FreeImage_Unload(dib);
    }
    FreeImage_DeInitialise();
    return 0;
  }

  if (report) ReportFormats();
  if (!output.empty()) {
    std::error_code ec;
    std::filesystem::create_directories(output, ec);
    FIBITMAP* base = FreeImage_Allocate(kWidth, kHeight, 24);
    if (base == nullptr) return 2;
    Fill24(base);

    const Target targets[] = {
        {".hdr", FIF_HDR, true},   {".exr", FIF_EXR, true},
        {".pfm", FIF_PFM, true},   {".jp2", FIF_JP2, false},
        {".j2k", FIF_J2K, false},  {".jxr", FIF_JXR, false},
        {".wbmp", FIF_WBMP, false}, {".mng", FIF_MNG, false},
        {".jng", FIF_JNG, false},
    };
    for (const Target& target : targets) {
      if (!FreeImage_FIFSupportsWriting(target.fif)) {
        std::printf("SKIP %s (plugin read-only)\n", target.extension);
        continue;
      }
      SaveOne(output, target, base);
    }

    // Alpha-bearing JPEG XR: the WIC path must preserve the alpha plane
    // (規劃/22/16 §7). FreeImage writes 32-bit BGRA.
    FIBITMAP* rgba = FreeImage_Allocate(kWidth, kHeight, 32);
    if (rgba != nullptr) {
      for (int y = 0; y < kHeight; ++y) {
        BYTE* line = FreeImage_GetScanLine(rgba, kHeight - 1 - y);
        for (int x = 0; x < kWidth; ++x) {
          BYTE* pixel = line + x * 4;
          pixel[FI_RGBA_BLUE] = 40;
          pixel[FI_RGBA_GREEN] = 80;
          pixel[FI_RGBA_RED] = 160;
          pixel[FI_RGBA_ALPHA] = x < kWidth / 2 ? 0 : 255;
        }
      }
      std::error_code alpha_ec;
      std::filesystem::create_directories(output / "jxr", alpha_ec);
      const std::filesystem::path alpha_path =
          output / "jxr" / "sample_alpha.jxr";
      const BOOL saved =
          FreeImage_SaveU(FIF_JXR, rgba, alpha_path.c_str(), 0);
      std::printf("%s %s\n", saved ? "WROTE" : "FAILED",
                  alpha_path.string().c_str());
      FreeImage_Unload(rgba);
    }
    FreeImage_Unload(base);
  }

  FreeImage_DeInitialise();
  return 0;
}
