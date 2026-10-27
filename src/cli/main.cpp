#include "core/exif.h"
#include "core/format_router.h"
#include "core/icc_profile.h"
#include "core/version.h"

#include <windows.h>
#include <objbase.h>

#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <string>
#include <vector>

namespace {

std::string ToUtf8(const std::wstring& text) {
  if (text.empty()) return {};
  const int size = WideCharToMultiByte(CP_UTF8, 0, text.c_str(),
                                       static_cast<int>(text.size()), nullptr, 0,
                                       nullptr, nullptr);
  if (size <= 0) return {};
  std::string out(static_cast<size_t>(size), '\0');
  WideCharToMultiByte(CP_UTF8, 0, text.c_str(), static_cast<int>(text.size()),
                      out.data(), size, nullptr, nullptr);
  return out;
}

void PrintError(const nv::Error& error) {
  std::cerr << "error: " << error.message;
  if (error.code != 0) std::cerr << " (code=" << error.code << ")";
  if (FAILED(error.hr)) {
    std::cerr << " (hr=0x" << std::hex << std::setw(8) << std::setfill('0')
              << static_cast<unsigned long>(error.hr) << std::dec << ")";
  }
  std::cerr << "\n";
}

void PrintUsage() {
  std::cout
      << "ZoreView CLI " << ToUtf8(nv::kVersion) << "\n\n"
      << "Usage:\n"
      << "  zoreview_cli --probe <file>\n"
      << "  zoreview_cli --decode <file> [--out <file.bgra>]\n"
      << "  zoreview_cli --thumb <file> [--edge 128] [--out <file.bgra>]\n"
      << "  zoreview_cli --exif <file>\n"
      << "  zoreview_cli --version\n\n"
      << "Supported: JPEG, PNG, BMP, GIF, WebP, TIFF, ICO/CUR, PSD/PSB,\n"
      << "           HEIC/HEIF, AVIF, SVG, TGA, PCX, PNM, XBM, XPM, RAW\n"
      << "           (WIC/FreeImage/LibRaw/PSD/SVG).\n";
}

const char* IccState(const std::vector<uint8_t>& profile, bool applied) {
  if (profile.empty()) return "none";
  if (applied) return "converted";
  if (nv::IsSrgbProfile(profile.data(), profile.size())) return "srgb";
  return "other";
}

int Probe(const std::filesystem::path& path) {
  const auto start = std::chrono::steady_clock::now();
  const nv::ProbeResult result = nv::FormatRouter::Probe(path);
  const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                           std::chrono::steady_clock::now() - start)
                           .count();

  if (!result.ok()) {
    PrintError(result.error);
    return 2;
  }

  std::cout << "file: " << ToUtf8(path.wstring()) << "\n"
            << "format: " << nv::FormatNameUtf8(result.format) << "\n"
            << "size: " << result.width << "x" << result.height << "\n"
            << "alpha: " << (result.has_alpha ? "yes" : "no") << "\n"
            << "orientation: " << result.orientation << "\n"
            << "orientation_applied: " << (result.orientation_applied ? "yes" : "no")
            << "\n"
            << "icc: "
            << (result.has_icc ? (result.icc_is_srgb ? "srgb" : "other") : "none")
            << "\n"
            << "frames: " << result.frame_count << "\n"
            << "animated: " << (result.animated ? "yes (metadata only)" : "no")
            << "\n"
            << "decoder: " << result.decoder << "\n"
            << "probe_ms: " << elapsed << "\n";
  return 0;
}

int Decode(const std::filesystem::path& path, const std::filesystem::path& output) {
  const auto start = std::chrono::steady_clock::now();
  const nv::DecodeResult result = nv::FormatRouter::Decode(path);
  const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                           std::chrono::steady_clock::now() - start)
                           .count();

  if (!result.ok()) {
    PrintError(result.error);
    return 2;
  }

  std::cout << "file: " << ToUtf8(path.wstring()) << "\n"
            << "size: " << result.image->info.width << "x" << result.image->info.height << "\n"
            << "stride: " << result.image->info.stride << "\n"
            << "format: BGRA8\n"
            << "alpha: " << (result.image->info.has_alpha ? "yes" : "no") << "\n"
            << "orientation: " << result.image->info.orientation << "\n"
            << "orientation_applied: "
            << (result.image->info.orientation_applied ? "yes" : "no") << "\n"
            << "icc: " << IccState(result.image->info.icc_profile,
                                   result.image->info.icc_applied)
            << "\n"
            << "frames: " << result.image->info.frame_count << "\n"
            << "animated: "
            << (result.image->info.animated ? "yes" : "no")
            << "\n"
            << "decoder: " << result.image->info.source << "\n"
            << "decode_ms: " << elapsed << "\n"
            << "bytes: " << result.image->pixels.size() << "\n";

  if (!output.empty()) {
    std::ofstream file(output, std::ios::binary);
    if (!file) {
      std::cerr << "error: cannot open output: " << ToUtf8(output.wstring()) << "\n";
      return 3;
    }
    file.write(reinterpret_cast<const char*>(result.image->pixels.data()),
               static_cast<std::streamsize>(result.image->pixels.size()));
    if (!file) {
      std::cerr << "error: write failed: " << ToUtf8(output.wstring()) << "\n";
      return 3;
    }
    std::cout << "out: " << ToUtf8(output.wstring()) << "\n";
  }
  return 0;
}

int Thumbnail(const std::filesystem::path& path, uint32_t edge,
              const std::filesystem::path& output) {
  nv::DecodeOptions options;
  options.max_edge = edge;

  const auto start = std::chrono::steady_clock::now();
  const nv::DecodeResult result = nv::FormatRouter::Decode(path, options);
  const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                           std::chrono::steady_clock::now() - start)
                           .count();

  if (!result.ok()) {
    PrintError(result.error);
    return 2;
  }

  const nv::Image& image = *result.image;
  std::cout << "file: " << ToUtf8(path.wstring()) << "\n"
            << "size: " << image.info.width << "x" << image.info.height << "\n"
            << "full: " << image.info.full_width << "x" << image.info.full_height
            << "\n"
            << "stride: " << image.info.stride << "\n"
            << "format: BGRA8\n"
            << "edge: " << edge << "\n"
            << "orientation: " << image.info.orientation << "\n"
            << "orientation_applied: "
            << (image.info.orientation_applied ? "yes" : "no") << "\n"
            << "icc: " << IccState(image.info.icc_profile, image.info.icc_applied)
            << "\n"
            << "frames: " << image.info.frame_count << "\n"
            << "animated: " << (image.info.animated ? "yes" : "no") << "\n"
            << "bytes: " << image.pixels.size() << "\n"
            << "decoder: " << image.info.source << "\n"
            << "decode_ms: " << elapsed << "\n";

  if (!output.empty()) {
    std::ofstream file(output, std::ios::binary);
    if (!file) {
      std::cerr << "error: cannot open output: " << ToUtf8(output.wstring()) << "\n";
      return 3;
    }
    file.write(reinterpret_cast<const char*>(image.pixels.data()),
               static_cast<std::streamsize>(image.pixels.size()));
    if (!file) {
      std::cerr << "error: write failed: " << ToUtf8(output.wstring()) << "\n";
      return 3;
    }
    std::cout << "out: " << ToUtf8(output.wstring()) << "\n";
  }
  return 0;
}

// Info-panel data source: every field in fixed order (規劃/22/06 §7). The GUI
// uses the exact same reader on its EXIF worker; this command exists so the
// samples and the field formatting can be checked without a window.
int Exif(const std::filesystem::path& path) {
  const HRESULT com = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
  const nv::ExifData data = nv::ReadExif(path);
  if (SUCCEEDED(com)) CoUninitialize();

  std::cout << "file: " << ToUtf8(path.wstring()) << "\n"
            << "exif: " << (data.ok ? "yes" : "no") << "\n";
  for (const nv::ExifField& field : data.fields) {
    std::cout << "exif_" << nv::ExifIdName(field.id) << ": "
              << ToUtf8(field.value) << "\n";
  }
  return data.ok ? 0 : 3;
}

}  // namespace

int wmain(int argc, wchar_t** argv) {
  if (argc < 2) {
    PrintUsage();
    return 1;
  }

  const std::wstring command = argv[1];
  if (command == L"--help" || command == L"-h") {
    PrintUsage();
    return 0;
  }
  if (command == L"--version") {
    std::cout << ToUtf8(nv::kAppName) << " " << ToUtf8(nv::kVersion) << "\n";
    return 0;
  }

  if (command == L"--probe") {
    if (argc < 3) {
      PrintUsage();
      return 1;
    }
    return Probe(argv[2]);
  }

  if (command == L"--decode") {
    if (argc < 3) {
      PrintUsage();
      return 1;
    }
    std::filesystem::path output;
    if (argc >= 5 && std::wstring(argv[3]) == L"--out") {
      output = argv[4];
    }
    return Decode(argv[2], output);
  }

  if (command == L"--thumb") {
    if (argc < 3) {
      PrintUsage();
      return 1;
    }
    uint32_t edge = 128;
    std::filesystem::path output;
    for (int i = 3; i < argc; ++i) {
      const std::wstring option = argv[i];
      if (option == L"--edge" && i + 1 < argc) {
        edge = static_cast<uint32_t>(std::wcstoul(argv[++i], nullptr, 10));
      } else if (option == L"--out" && i + 1 < argc) {
        output = argv[++i];
      }
    }
    if (edge < 16) edge = 16;
    return Thumbnail(argv[2], edge, output);
  }

  if (command == L"--exif") {
    if (argc < 3) {
      PrintUsage();
      return 1;
    }
    return Exif(argv[2]);
  }

  PrintUsage();
  return 1;
}
