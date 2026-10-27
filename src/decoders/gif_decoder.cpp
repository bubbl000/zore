#include "decoders/gif_decoder.h"

#include "decoders/wic_decoder.h"
#include "platform/log.h"

#include <wincodec.h>
#include <wrl/client.h>

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <string>
#include <vector>

using Microsoft::WRL::ComPtr;

namespace nv::gif {
namespace {

constexpr int kDecodeErrorBase = 2400;
constexpr uint32_t kMaxFrames = 512;
constexpr uint64_t kMaxAnimationBytes = 128ull * 1024 * 1024;
constexpr uint32_t kDefaultFrameDelayMs = 100;

Error MakeError(const char* message, HRESULT hr = S_OK) {
  return Error::Make(kDecodeErrorBase, message, hr);
}

bool QueryUInt(IWICMetadataQueryReader* reader, const wchar_t* name,
               uint32_t* value) {
  if (reader == nullptr) return false;
  PROPVARIANT variant;
  PropVariantInit(&variant);
  const HRESULT hr = reader->GetMetadataByName(name, &variant);
  bool ok = false;
  if (SUCCEEDED(hr)) {
    switch (variant.vt) {
      case VT_UI1: *value = variant.bVal; ok = true; break;
      case VT_UI2: *value = variant.uiVal; ok = true; break;
      case VT_UI4: *value = variant.ulVal; ok = true; break;
      case VT_UI8: *value = static_cast<uint32_t>(variant.uhVal.QuadPart); ok = true; break;
      default: break;
    }
  }
  PropVariantClear(&variant);
  return ok;
}

uint32_t ParseLoopCount(const std::filesystem::path& path) {
  std::ifstream file(path, std::ios::binary);
  if (!file) return 1;
  file.seekg(0, std::ios::end);
  const std::streamoff size = file.tellg();
  if (size <= 0 || size > 64ll * 1024 * 1024) return 1;
  file.seekg(0, std::ios::beg);
  std::vector<uint8_t> data(static_cast<size_t>(size));
  file.read(reinterpret_cast<char*>(data.data()), size);
  if (!file) return 1;

  static constexpr uint8_t kNetScape[] = {'N', 'E', 'T', 'S', 'C', 'A', 'P', 'E',
                                          '2', '.', '0'};
  for (size_t i = 0; i + sizeof(kNetScape) + 5 < data.size(); ++i) {
    if (std::memcmp(data.data() + i, kNetScape, sizeof(kNetScape)) != 0) {
      continue;
    }
    const size_t sub = i + sizeof(kNetScape);
    // Sub-block: 0x03, 0x01, loop_count little-endian.
    if (data[sub] == 0x03 && data[sub + 1] == 0x01) {
      return static_cast<uint32_t>(data[sub + 2]) |
             (static_cast<uint32_t>(data[sub + 3]) << 8);
    }
  }
  return 1;  // No NETSCAPE extension: play once.
}

void BlendBgra(uint8_t* destination, const uint8_t* source) {
  const uint32_t alpha = source[3];
  if (alpha == 255) {
    destination[0] = source[0];
    destination[1] = source[1];
    destination[2] = source[2];
    destination[3] = 255;
    return;
  }
  if (alpha == 0) return;
  const uint32_t inverse = 255 - alpha;
  destination[0] = static_cast<uint8_t>(
      (source[0] * alpha + destination[0] * inverse + 127) / 255);
  destination[1] = static_cast<uint8_t>(
      (source[1] * alpha + destination[1] * inverse + 127) / 255);
  destination[2] = static_cast<uint8_t>(
      (source[2] * alpha + destination[2] * inverse + 127) / 255);
  destination[3] = static_cast<uint8_t>(
      std::max<uint32_t>(destination[3], alpha));
}

bool DecodeAnimated(const std::filesystem::path& path, Image* image,
                    Error* error) {
  ComPtr<IWICImagingFactory> factory;
  if (FAILED(CoCreateInstance(CLSID_WICImagingFactory, nullptr,
                              CLSCTX_INPROC_SERVER,
                              IID_PPV_ARGS(factory.GetAddressOf())))) {
    *error = MakeError("GIF: WIC factory creation failed");
    return false;
  }

  ComPtr<IWICBitmapDecoder> decoder;
  if (FAILED(factory->CreateDecoderFromFilename(
          path.c_str(), nullptr, GENERIC_READ, WICDecodeMetadataCacheOnDemand,
          decoder.GetAddressOf()))) {
    *error = MakeError("GIF: CreateDecoderFromFilename failed");
    return false;
  }

  UINT frame_count = 1;
  decoder->GetFrameCount(&frame_count);

  ComPtr<IWICBitmapFrameDecode> first_frame;
  if (FAILED(decoder->GetFrame(0, first_frame.GetAddressOf()))) {
    *error = MakeError("GIF: cannot read the first frame");
    return false;
  }

  uint32_t canvas_width = 0;
  uint32_t canvas_height = 0;
  ComPtr<IWICMetadataQueryReader> global_reader;
  if (SUCCEEDED(decoder->GetMetadataQueryReader(global_reader.GetAddressOf()))) {
    QueryUInt(global_reader.Get(), L"/logscrdesc/Width", &canvas_width);
    QueryUInt(global_reader.Get(), L"/logscrdesc/Height", &canvas_height);
  }
  if (canvas_width == 0 || canvas_height == 0) {
    UINT width = 0;
    UINT height = 0;
    if (FAILED(first_frame->GetSize(&width, &height))) {
      *error = MakeError("GIF: cannot read the canvas size");
      return false;
    }
    canvas_width = width;
    canvas_height = height;
  }

  const uint64_t canvas_bytes =
      static_cast<uint64_t>(canvas_width) * canvas_height * 4;
  if (canvas_bytes == 0 || canvas_bytes > kMaxAnimationBytes) {
    *error = MakeError("GIF: canvas is too large for animation");
    return false;
  }

  const uint32_t frame_limit = std::min<uint32_t>(
      frame_count, static_cast<uint32_t>(kMaxAnimationBytes / canvas_bytes));
  std::vector<uint8_t> canvas(static_cast<size_t>(canvas_bytes), 0);
  std::vector<uint8_t> previous;
  std::vector<ImageFrame> frames;
  frames.reserve(frame_limit);

  for (uint32_t index = 0; index < frame_limit; ++index) {
    ComPtr<IWICBitmapFrameDecode> frame;
    if (FAILED(decoder->GetFrame(index, frame.GetAddressOf()))) break;

    UINT frame_width = 0;
    UINT frame_height = 0;
    if (FAILED(frame->GetSize(&frame_width, &frame_height)) ||
        frame_width == 0 || frame_height == 0) {
      break;
    }

    ComPtr<IWICFormatConverter> converter;
    if (FAILED(factory->CreateFormatConverter(converter.GetAddressOf())) ||
        FAILED(converter->Initialize(frame.Get(), GUID_WICPixelFormat32bppBGRA,
                                     WICBitmapDitherTypeNone, nullptr, 0.0,
                                     WICBitmapPaletteTypeCustom))) {
      break;
    }
    std::vector<uint8_t> frame_pixels(static_cast<size_t>(frame_width) *
                                      frame_height * 4);
    const UINT frame_stride = frame_width * 4;
    if (FAILED(converter->CopyPixels(nullptr, frame_stride,
                                     static_cast<UINT>(frame_pixels.size()),
                                     frame_pixels.data()))) {
      break;
    }

    ComPtr<IWICMetadataQueryReader> reader;
    uint32_t left = 0;
    uint32_t top = 0;
    uint32_t delay = 0;
    uint32_t disposal = 0;
    if (SUCCEEDED(frame->GetMetadataQueryReader(reader.GetAddressOf()))) {
      QueryUInt(reader.Get(), L"/imgdesc/Left", &left);
      QueryUInt(reader.Get(), L"/imgdesc/Top", &top);
      QueryUInt(reader.Get(), L"/grctlext/Delay", &delay);
      QueryUInt(reader.Get(), L"/grctlext/Disposal", &disposal);
    }

    if (disposal == 3) previous = canvas;

    const uint32_t copy_width =
        std::min<uint32_t>(frame_width, canvas_width > left ? canvas_width - left : 0);
    const uint32_t copy_height = std::min<uint32_t>(
        frame_height, canvas_height > top ? canvas_height - top : 0);
    for (uint32_t y = 0; y < copy_height; ++y) {
      const uint8_t* source = frame_pixels.data() +
                              (static_cast<size_t>(y) * frame_width) * 4;
      uint8_t* destination = canvas.data() +
                             ((static_cast<size_t>(top + y) * canvas_width +
                               left) * 4);
      for (uint32_t x = 0; x < copy_width; ++x) {
        BlendBgra(destination + static_cast<size_t>(x) * 4,
                  source + static_cast<size_t>(x) * 4);
      }
    }

    ImageFrame output;
    output.pixels = canvas;
    output.delay_ms = delay == 0 ? kDefaultFrameDelayMs : delay * 10;
    frames.push_back(std::move(output));

    if (disposal == 2) {
      for (uint32_t y = 0; y < copy_height; ++y) {
        uint8_t* destination = canvas.data() +
                               ((static_cast<size_t>(top + y) * canvas_width +
                                 left) * 4);
        std::memset(destination, 0, static_cast<size_t>(copy_width) * 4);
      }
    } else if (disposal == 3) {
      canvas = std::move(previous);
    }
  }

  if (frames.empty()) {
    *error = MakeError("GIF: no frame could be decoded");
    return false;
  }

  image->info.width = canvas_width;
  image->info.height = canvas_height;
  image->info.stride = canvas_width * 4u;
  image->info.format = PixelFormat::kBgra8;
  image->info.has_alpha = true;
  image->info.full_width = canvas_width;
  image->info.full_height = canvas_height;
  image->info.orientation = 1;
  image->info.frame_count = static_cast<uint32_t>(frames.size());
  image->info.animated = frames.size() > 1;
  image->info.dpi_x = 96.0;
  image->info.dpi_y = 96.0;
  image->info.source = "gif";
  image->loop_count = ParseLoopCount(path);
  image->pixels = frames[0].pixels;
  if (frames.size() > 1) image->frames = std::move(frames);

  if (frame_limit < frame_count) {
    image->info.frames_truncated = true;
    nv::platform::LogInfo("gif: animation truncated at " +
                          std::to_string(frame_limit) + "/" +
                          std::to_string(frame_count) + " frames");
  }
  return true;
}

}  // namespace

DecodeResult Decode(const std::filesystem::path& path,
                    const DecodeOptions& options) {
  DecodeResult result;
  if (!options.animation || options.max_edge > 0) {
    result = wic::Decode(path, options);
    if (result.ok()) {
      result.image->info.animated = result.image->info.frame_count > 1;
    }
    return result;
  }

  if (!wic::EnsureComInitialized()) {
    result.error = MakeError("GIF: COM initialization failed");
    return result;
  }

  Image image;
  if (!DecodeAnimated(path, &image, &result.error)) {
    // Fall back to the first frame rather than failing the open entirely.
    DecodeResult fallback = wic::Decode(path, options);
    if (fallback.ok()) {
      fallback.image->info.animated = fallback.image->info.frame_count > 1;
      return fallback;
    }
    return result;
  }
  result.image = std::make_shared<Image>(std::move(image));
  return result;
}

}  // namespace nv::gif
