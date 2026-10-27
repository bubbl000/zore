#include "decoders/svg_decoder.h"

#include "decoders/wic_decoder.h"
#include "platform/log.h"

#include <d2d1_3.h>
#include <d3d11.h>
#include <dxgi.h>
#include <wrl/client.h>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iterator>
#include <sstream>
#include <string>
#include <vector>

using Microsoft::WRL::ComPtr;

namespace nv::svg {
namespace {

constexpr int kDecodeErrorBase = 2300;
constexpr uint32_t kMaxSvgBytes = 32u * 1024 * 1024;
constexpr uint32_t kMaxDimension = 4096;
constexpr uint64_t kMaxPixels = 64ull * 1000 * 1000;

Error MakeError(const char* message, HRESULT hr = S_OK) {
  return Error::Make(kDecodeErrorBase, message, hr);
}

std::string ToLowerAscii(std::string text) {
  for (char& c : text) {
    if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
  }
  return text;
}

std::string Trim(std::string text) {
  const auto begin = text.find_first_not_of(" \t\r\n");
  if (begin == std::string::npos) return {};
  const auto end = text.find_last_not_of(" \t\r\n");
  return text.substr(begin, end - begin + 1);
}

// Finds `name` as a standalone attribute in an already-lowercased tag.
std::string FindAttribute(const std::string& tag, const char* name) {
  const std::string needle(name);
  size_t position = 0;
  while ((position = tag.find(needle, position)) != std::string::npos) {
    const bool left_ok =
        position == 0 || std::isspace(static_cast<unsigned char>(tag[position - 1])) ||
        tag[position - 1] == '<';
    size_t cursor = position + needle.size();
    while (cursor < tag.size() &&
           std::isspace(static_cast<unsigned char>(tag[cursor]))) {
      ++cursor;
    }
    if (!left_ok || cursor >= tag.size() || tag[cursor] != '=') {
      position += needle.size();
      continue;
    }
    ++cursor;
    while (cursor < tag.size() &&
           std::isspace(static_cast<unsigned char>(tag[cursor]))) {
      ++cursor;
    }
    if (cursor >= tag.size()) return {};
    const char quote = tag[cursor];
    if (quote == '"' || quote == '\'') {
      const size_t end = tag.find(quote, cursor + 1);
      if (end == std::string::npos) return {};
      return tag.substr(cursor + 1, end - cursor - 1);
    }
    const size_t end = tag.find_first_of(" \t\r\n>", cursor);
    return tag.substr(cursor, end == std::string::npos ? std::string::npos
                                                       : end - cursor);
  }
  return {};
}

bool ParseLength(const std::string& text, double* value) {
  const std::string trimmed = Trim(text);
  if (trimmed.empty() || trimmed.back() == '%') return false;
  char* end = nullptr;
  const double number = std::strtod(trimmed.c_str(), &end);
  if (end == trimmed.c_str() || !std::isfinite(number) || number <= 0.0) {
    return false;
  }
  const std::string unit = ToLowerAscii(Trim(std::string(end)));
  double scale = 1.0;
  if (unit.empty() || unit == "px") {
    scale = 1.0;
  } else if (unit == "pt") {
    scale = 96.0 / 72.0;
  } else if (unit == "pc") {
    scale = 16.0;
  } else if (unit == "in") {
    scale = 96.0;
  } else if (unit == "cm") {
    scale = 96.0 / 2.54;
  } else if (unit == "mm") {
    scale = 96.0 / 25.4;
  } else {
    return false;
  }
  *value = number * scale;
  return std::isfinite(*value) && *value > 0.0;
}

bool ParseViewBox(const std::string& text, double* width, double* height) {
  std::string normalized = text;
  std::ranges::replace(normalized, ',', ' ');
  std::istringstream stream(normalized);
  double min_x = 0.0;
  double min_y = 0.0;
  double width_value = 0.0;
  double height_value = 0.0;
  if (!(stream >> min_x >> min_y >> width_value >> height_value)) return false;
  if (!std::isfinite(width_value) || !std::isfinite(height_value) ||
      width_value <= 0.0 || height_value <= 0.0) {
    return false;
  }
  *width = width_value;
  *height = height_value;
  return true;
}

struct SvgMetrics {
  double intrinsic_width = 0.0;
  double intrinsic_height = 0.0;
  uint32_t width = 0;
  uint32_t height = 0;
};

bool ParseMetrics(const std::string& text, SvgMetrics* metrics, Error* error) {
  const std::string lower = ToLowerAscii(text);
  const size_t start = lower.find("<svg");
  if (start == std::string::npos) {
    *error = MakeError("SVG: no <svg> root element");
    return false;
  }
  size_t end = start;
  char quote = '\0';
  for (; end < lower.size(); ++end) {
    const char c = lower[end];
    if (quote != '\0') {
      if (c == quote) quote = '\0';
      continue;
    }
    if (c == '"' || c == '\'') {
      quote = c;
    } else if (c == '>') {
      break;
    }
  }
  if (end >= lower.size()) {
    *error = MakeError("SVG: unterminated <svg> tag");
    return false;
  }
  const std::string tag = lower.substr(start, end - start + 1);

  double width = 0.0;
  double height = 0.0;
  const bool has_width = ParseLength(FindAttribute(tag, "width"), &width);
  const bool has_height = ParseLength(FindAttribute(tag, "height"), &height);

  double box_width = 0.0;
  double box_height = 0.0;
  const bool has_viewbox =
      ParseViewBox(FindAttribute(tag, "viewbox"), &box_width, &box_height);

  if (!has_width && !has_height) {
    if (has_viewbox) {
      width = box_width;
      height = box_height;
    } else {
      // CSS/SVG default replaced-element size.
      width = 300.0;
      height = 150.0;
    }
  } else if (!has_width) {
    width = has_viewbox ? height * box_width / box_height : height;
  } else if (!has_height) {
    height = has_viewbox ? width * box_height / box_width : width;
  }

  if (!(width > 0.0) || !(height > 0.0) || !std::isfinite(width) ||
      !std::isfinite(height)) {
    *error = MakeError("SVG: invalid width/height");
    return false;
  }

  const double limit = static_cast<double>(kMaxDimension);
  const double scale = std::min(1.0, limit / std::max(width, height));
  metrics->intrinsic_width = width;
  metrics->intrinsic_height = height;
  metrics->width = std::max<uint32_t>(
      1, static_cast<uint32_t>(width * scale + 0.5));
  metrics->height = std::max<uint32_t>(
      1, static_cast<uint32_t>(height * scale + 0.5));
  if (static_cast<uint64_t>(metrics->width) * metrics->height > kMaxPixels) {
    *error = MakeError("SVG: rendered image is too large");
    return false;
  }
  return true;
}

bool ReadSvgText(const std::filesystem::path& path, std::string* text,
                 Error* error) {
  std::ifstream file(path, std::ios::binary);
  if (!file) {
    *error = MakeError("SVG: cannot open file");
    return false;
  }
  file.seekg(0, std::ios::end);
  const std::streamoff size = file.tellg();
  if (size <= 0 || size > kMaxSvgBytes) {
    *error = MakeError("SVG: file is empty or larger than 32 MB");
    return false;
  }
  file.seekg(0, std::ios::beg);
  text->resize(static_cast<size_t>(size));
  file.read(text->data(), size);
  if (!file) {
    *error = MakeError("SVG: failed to read file");
    return false;
  }
  if (text->size() >= 3 && static_cast<uint8_t>((*text)[0]) == 0xEF &&
      static_cast<uint8_t>((*text)[1]) == 0xBB &&
      static_cast<uint8_t>((*text)[2]) == 0xBF) {
    text->erase(0, 3);
  }
  return true;
}

bool ValidateSvgText(const std::string& text, Error* error) {
  const std::string lower = ToLowerAscii(text);
  if (lower.find("<!doctype") != std::string::npos ||
      lower.find("<!entity") != std::string::npos) {
    *error = MakeError("SVG: DOCTYPE/ENTITY is not allowed");
    return false;
  }
  if (lower.find("<script") != std::string::npos) {
    *error = MakeError("SVG: script elements are not allowed");
    return false;
  }
  // Direct2D's SVG renderer does not fetch external resources, but reject
  // them explicitly so a document never depends on network/file access.
  constexpr const char* kExternalSchemes[] = {
      "http:", "https:", "file:", "ftp:", "//"};
  size_t position = 0;
  while ((position = lower.find("href", position)) != std::string::npos) {
    size_t cursor = position + 4;
    while (cursor < lower.size() &&
           std::isspace(static_cast<unsigned char>(lower[cursor]))) {
      ++cursor;
    }
    if (cursor < lower.size() && lower[cursor] == '=') {
      ++cursor;
      while (cursor < lower.size() &&
             std::isspace(static_cast<unsigned char>(lower[cursor]))) {
        ++cursor;
      }
      if (cursor < lower.size() &&
          (lower[cursor] == '"' || lower[cursor] == '\'')) {
        ++cursor;
      }
      for (const char* scheme : kExternalSchemes) {
        if (lower.compare(cursor, std::strlen(scheme), scheme) == 0) {
          *error = MakeError("SVG: external resources are not allowed");
          return false;
        }
      }
    }
    position += 4;
  }
  if (lower.find("url(http") != std::string::npos ||
      lower.find("url(https") != std::string::npos ||
      lower.find("url(//") != std::string::npos ||
      lower.find("@import") != std::string::npos) {
    *error = MakeError("SVG: external resources are not allowed");
    return false;
  }
  return true;
}

struct SvgContext {
  ComPtr<ID3D11Device> d3d_device;
  ComPtr<ID2D1Factory> factory;
  ComPtr<ID2D1Factory1> factory1;
  ComPtr<ID2D1Device> d2d_device;
  ComPtr<ID2D1DeviceContext5> context;
  bool ready = false;
};

bool InitializeContext(SvgContext* context) {
  UINT flags = D3D11_CREATE_DEVICE_BGRA_SUPPORT;
  const D3D_FEATURE_LEVEL levels[] = {
      D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0, D3D_FEATURE_LEVEL_10_1,
      D3D_FEATURE_LEVEL_10_0};
  HRESULT hr = D3D11CreateDevice(
      nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, flags, levels,
      static_cast<UINT>(std::size(levels)), D3D11_SDK_VERSION,
      context->d3d_device.GetAddressOf(), nullptr, nullptr);
  if (FAILED(hr)) {
    hr = D3D11CreateDevice(
        nullptr, D3D_DRIVER_TYPE_WARP, nullptr, flags, levels,
        static_cast<UINT>(std::size(levels)), D3D11_SDK_VERSION,
        context->d3d_device.GetAddressOf(), nullptr, nullptr);
  }
  if (FAILED(hr)) return false;

  ComPtr<IDXGIDevice> dxgi_device;
  if (FAILED(context->d3d_device.As(&dxgi_device))) return false;

  if (FAILED(D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED,
                               context->factory.GetAddressOf()))) {
    return false;
  }
  if (FAILED(context->factory.As(&context->factory1))) return false;
  if (FAILED(context->factory1->CreateDevice(dxgi_device.Get(),
                                             context->d2d_device.GetAddressOf()))) {
    return false;
  }
  ComPtr<ID2D1DeviceContext> base_context;
  if (FAILED(context->d2d_device->CreateDeviceContext(
          D2D1_DEVICE_CONTEXT_OPTIONS_NONE, base_context.GetAddressOf()))) {
    return false;
  }
  if (FAILED(base_context.As(&context->context))) return false;
  context->ready = true;
  return true;
}

SvgContext*& ContextSlot() {
  thread_local SvgContext* slot = nullptr;
  return slot;
}

SvgContext* GetContext() {
  SvgContext*& slot = ContextSlot();
  if (slot == nullptr) slot = new SvgContext();
  if (!slot->ready) {
    (void)InitializeContext(slot);
  }
  return slot->ready ? slot : nullptr;
}

void ResetContext(SvgContext* context) {
  context->context.Reset();
  context->d2d_device.Reset();
  context->factory1.Reset();
  context->factory.Reset();
  context->d3d_device.Reset();
  context->ready = false;
}

bool RenderSvg(SvgContext* context, const std::string& text,
               const SvgMetrics& metrics, uint32_t target_width,
               uint32_t target_height, std::vector<uint8_t>* pixels,
               Error* error) {
  ComPtr<IStream> stream;
  if (FAILED(CreateStreamOnHGlobal(nullptr, TRUE, stream.GetAddressOf()))) {
    *error = MakeError("SVG: cannot create XML stream");
    return false;
  }
  if (FAILED(stream->Write(text.data(), static_cast<ULONG>(text.size()),
                           nullptr))) {
    *error = MakeError("SVG: cannot write XML stream");
    return false;
  }
  LARGE_INTEGER zero{};
  if (FAILED(stream->Seek(zero, STREAM_SEEK_SET, nullptr))) {
    *error = MakeError("SVG: cannot rewind XML stream");
    return false;
  }

  ComPtr<ID2D1SvgDocument> document;
  HRESULT hr = context->context->CreateSvgDocument(
      stream.Get(),
      D2D1::SizeF(static_cast<float>(metrics.intrinsic_width),
                  static_cast<float>(metrics.intrinsic_height)),
      document.GetAddressOf());
  if (FAILED(hr)) {
    *error = MakeError(
        "SVG: Direct2D could not parse the document (unsupported feature?)", hr);
    return false;
  }

  const D2D1_SIZE_U target_size = D2D1::SizeU(target_width, target_height);
  const D2D1_BITMAP_PROPERTIES1 target_properties = D2D1::BitmapProperties1(
      D2D1_BITMAP_OPTIONS_TARGET,
      D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM,
                        D2D1_ALPHA_MODE_PREMULTIPLIED));
  ComPtr<ID2D1Bitmap1> target;
  hr = context->context->CreateBitmap(target_size, nullptr, 0, target_properties,
                                     target.GetAddressOf());
  if (FAILED(hr)) {
    *error = MakeError("SVG: cannot create render target", hr);
    return false;
  }

  context->context->SetTarget(target.Get());
  context->context->BeginDraw();
  context->context->Clear(D2D1::ColorF(0.0f, 0.0f, 0.0f, 0.0f));
  const float scale_x = static_cast<float>(target_width) /
                        static_cast<float>(metrics.intrinsic_width);
  const float scale_y = static_cast<float>(target_height) /
                        static_cast<float>(metrics.intrinsic_height);
  context->context->SetTransform(D2D1::Matrix3x2F::Scale(scale_x, scale_y));
  context->context->DrawSvgDocument(document.Get());
  context->context->SetTransform(D2D1::Matrix3x2F::Identity());
  hr = context->context->EndDraw();
  context->context->SetTarget(nullptr);
  if (FAILED(hr)) {
    *error = MakeError("SVG: rendering failed", hr);
    return false;
  }

  const D2D1_BITMAP_PROPERTIES1 staging_properties = D2D1::BitmapProperties1(
      D2D1_BITMAP_OPTIONS_CANNOT_DRAW | D2D1_BITMAP_OPTIONS_CPU_READ,
      D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM,
                        D2D1_ALPHA_MODE_PREMULTIPLIED));
  ComPtr<ID2D1Bitmap1> staging;
  hr = context->context->CreateBitmap(target_size, nullptr, 0,
                                     staging_properties,
                                     staging.GetAddressOf());
  if (FAILED(hr)) {
    *error = MakeError("SVG: cannot create staging bitmap", hr);
    return false;
  }
  hr = staging->CopyFromBitmap(nullptr, target.Get(), nullptr);
  if (FAILED(hr)) {
    *error = MakeError("SVG: cannot copy rendered bitmap", hr);
    return false;
  }

  D2D1_MAPPED_RECT mapped{};
  if (FAILED(staging->Map(D2D1_MAP_OPTIONS_READ, &mapped))) {
    *error = MakeError("SVG: cannot map rendered bitmap");
    return false;
  }
  const uint32_t stride = target_width * 4u;
  pixels->resize(static_cast<size_t>(stride) * target_height);
  for (uint32_t y = 0; y < target_height; ++y) {
    std::memcpy(pixels->data() + static_cast<size_t>(y) * stride,
                mapped.bits + static_cast<size_t>(y) * mapped.pitch, stride);
  }
  staging->Unmap();

  // Direct2D renders premultiplied BGRA; the decoders and renderer use
  // straight alpha.
  uint8_t* data = pixels->data();
  for (size_t i = 0; i < pixels->size(); i += 4) {
    const uint32_t alpha = data[i + 3];
    if (alpha == 0) {
      data[i + 0] = 0;
      data[i + 1] = 0;
      data[i + 2] = 0;
    } else if (alpha != 255) {
      data[i + 0] = static_cast<uint8_t>(
          std::min<uint32_t>(255, (data[i + 0] * 255u + alpha / 2) / alpha));
      data[i + 1] = static_cast<uint8_t>(
          std::min<uint32_t>(255, (data[i + 1] * 255u + alpha / 2) / alpha));
      data[i + 2] = static_cast<uint8_t>(
          std::min<uint32_t>(255, (data[i + 2] * 255u + alpha / 2) / alpha));
    }
  }
  return true;
}

}  // namespace

void ReleaseThreadContext() {
  SvgContext*& slot = ContextSlot();
  delete slot;
  slot = nullptr;
}

ProbeResult Probe(const std::filesystem::path& path) {
  ProbeResult result;
  std::string text;
  if (!ReadSvgText(path, &text, &result.error)) return result;
  if (!ValidateSvgText(text, &result.error)) return result;
  SvgMetrics metrics;
  if (!ParseMetrics(text, &metrics, &result.error)) return result;

  result.format = ImageFormat::kSvg;
  result.width = metrics.width;
  result.height = metrics.height;
  result.has_alpha = true;
  result.orientation = 1;
  result.orientation_applied = false;
  result.decoder = "svg";
  return result;
}

DecodeResult Decode(const std::filesystem::path& path,
                    const DecodeOptions& options) {
  DecodeResult result;
  if (!wic::EnsureComInitialized()) {
    result.error = MakeError("SVG: COM initialization failed");
    return result;
  }

  std::string text;
  if (!ReadSvgText(path, &text, &result.error)) return result;
  if (!ValidateSvgText(text, &result.error)) return result;
  SvgMetrics metrics;
  if (!ParseMetrics(text, &metrics, &result.error)) return result;

  uint32_t target_width = metrics.width;
  uint32_t target_height = metrics.height;
  if (options.max_edge > 0 &&
      std::max(target_width, target_height) > options.max_edge) {
    const double scale = static_cast<double>(options.max_edge) /
                         static_cast<double>(std::max(target_width, target_height));
    target_width = std::max<uint32_t>(
        1, static_cast<uint32_t>(target_width * scale + 0.5));
    target_height = std::max<uint32_t>(
        1, static_cast<uint32_t>(target_height * scale + 0.5));
  }

  Image image;
  image.info.width = target_width;
  image.info.height = target_height;
  image.info.stride = target_width * 4u;
  image.info.format = PixelFormat::kBgra8;
  image.info.has_alpha = true;
  image.info.full_width = metrics.width;
  image.info.full_height = metrics.height;
  image.info.orientation = 1;
  image.info.orientation_applied = false;
  image.info.dpi_x = 96.0;
  image.info.dpi_y = 96.0;
  image.info.source = "svg";

  bool rendered = false;
  for (int attempt = 0; attempt < 2 && !rendered; ++attempt) {
    SvgContext* context = GetContext();
    if (context == nullptr) {
      result.error = MakeError("SVG: Direct2D device is unavailable");
      return result;
    }
    if (RenderSvg(context, text, metrics, target_width, target_height,
                  &image.pixels, &result.error)) {
      rendered = true;
    } else {
      // A lost/reset device is the only case worth retrying from scratch.
      ResetContext(context);
      result.error = MakeError("SVG: rendering failed");
    }
  }
  if (!rendered) return result;

  if (ToLowerAscii(text).find("<text") != std::string::npos) {
    nv::platform::LogInfo(
        "svg: <text> elements are skipped by the Direct2D SVG renderer");
  }

  result.image = std::make_shared<Image>(std::move(image));
  return result;
}

}  // namespace nv::svg
