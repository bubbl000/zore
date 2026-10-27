#include "decoders/webp_anim.h"

#include "decoders/wic_decoder.h"
#include "platform/log.h"

#include <wincodec.h>
#include <wrl/client.h>

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <memory>
#include <string>
#include <vector>

using Microsoft::WRL::ComPtr;

namespace nv::webp {
namespace {

constexpr int kDecodeErrorBase = 2450;
constexpr uint32_t kMaxFrames = 512;
constexpr uint64_t kMaxAnimationBytes = 128ull * 1024 * 1024;
constexpr uint32_t kDefaultFrameDelayMs = 100;
constexpr size_t kMaxContainerBytes = 256ull * 1024 * 1024;

Error MakeError(const char* message, HRESULT hr = S_OK) {
  return Error::Make(kDecodeErrorBase, message, hr);
}

uint32_t ReadU32(const uint8_t* data) {
  return static_cast<uint32_t>(data[0]) |
         (static_cast<uint32_t>(data[1]) << 8) |
         (static_cast<uint32_t>(data[2]) << 16) |
         (static_cast<uint32_t>(data[3]) << 24);
}

uint32_t ReadU24(const uint8_t* data) {
  return static_cast<uint32_t>(data[0]) |
         (static_cast<uint32_t>(data[1]) << 8) |
         (static_cast<uint32_t>(data[2]) << 16);
}

bool ReadContainer(const std::filesystem::path& path,
                   std::vector<uint8_t>* data) {
  std::ifstream file(path, std::ios::binary);
  if (!file) return false;
  file.seekg(0, std::ios::end);
  const std::streamoff size = file.tellg();
  if (size < 12 || static_cast<uint64_t>(size) > kMaxContainerBytes) {
    return false;
  }
  file.seekg(0, std::ios::beg);
  data->resize(static_cast<size_t>(size));
  file.read(reinterpret_cast<char*>(data->data()), size);
  return file.good() || file.eof();
}

// Composites one decoded BGRA frame onto the canvas. Straight-alpha source-over,
// matching the APNG rules (規劃/22/12 §3.4); a full-canvas frame is assigned by
// the caller instead, which also covers the common "every frame is complete"
// WebP layout.
void BlendOnto(uint8_t* canvas, uint32_t canvas_width, uint32_t canvas_height,
               const uint8_t* source, uint32_t width, uint32_t height,
               uint32_t x, uint32_t y, bool blend) {
  for (uint32_t row = 0; row < height; ++row) {
    const uint32_t cy = y + row;
    if (cy >= canvas_height) break;
    const uint8_t* src = source + static_cast<size_t>(row) * width * 4u;
    uint8_t* dst = canvas + (static_cast<size_t>(cy) * canvas_width + x) * 4u;
    for (uint32_t col = 0; col < width; ++col, src += 4, dst += 4) {
      if (x + col >= canvas_width) break;
      const uint32_t sa = src[3];
      if (!blend || sa == 255) {
        dst[0] = src[0];
        dst[1] = src[1];
        dst[2] = src[2];
        dst[3] = src[3];
        continue;
      }
      if (sa == 0) continue;
      const uint32_t da = dst[3];
      const uint32_t out_a = sa + da * (255u - sa) / 255u;
      if (out_a == 0) {
        dst[0] = dst[1] = dst[2] = dst[3] = 0;
        continue;
      }
      for (int channel = 0; channel < 3; ++channel) {
        const uint32_t sc = src[channel];
        const uint32_t dc = dst[channel];
        const uint32_t value =
            (sc * sa + dc * da * (255u - sa) / 255u) / out_a;
        dst[channel] = static_cast<uint8_t>(std::min<uint32_t>(value, 255u));
      }
      dst[3] = static_cast<uint8_t>(std::min<uint32_t>(out_a, 255u));
    }
  }
}

void ClearRect(uint8_t* canvas, uint32_t canvas_width, uint32_t canvas_height,
               uint32_t x, uint32_t y, uint32_t width, uint32_t height) {
  for (uint32_t row = 0; row < height; ++row) {
    const uint32_t cy = y + row;
    if (cy >= canvas_height) break;
    uint8_t* dst = canvas + (static_cast<size_t>(cy) * canvas_width + x) * 4u;
    for (uint32_t col = 0; col < width && x + col < canvas_width; ++col) {
      dst[0] = dst[1] = dst[2] = dst[3] = 0;
      dst += 4;
    }
  }
}

bool ReadFrameBgra(IWICImagingFactory* factory, IWICBitmapDecoder* decoder,
                   UINT index, uint32_t* width, uint32_t* height,
                   std::vector<uint8_t>* pixels, Error* error) {
  ComPtr<IWICBitmapFrameDecode> frame;
  HRESULT hr = decoder->GetFrame(index, frame.GetAddressOf());
  if (FAILED(hr)) {
    *error = MakeError("WebP: GetFrame failed", hr);
    return false;
  }
  ComPtr<IWICFormatConverter> converter;
  hr = factory->CreateFormatConverter(converter.GetAddressOf());
  if (FAILED(hr)) {
    *error = MakeError("WebP: CreateFormatConverter failed", hr);
    return false;
  }
  hr = converter->Initialize(frame.Get(), GUID_WICPixelFormat32bppBGRA,
                             WICBitmapDitherTypeNone, nullptr, 0.0,
                             WICBitmapPaletteTypeCustom);
  if (FAILED(hr)) {
    *error = MakeError("WebP: format conversion failed", hr);
    return false;
  }
  UINT frame_width = 0;
  UINT frame_height = 0;
  hr = converter->GetSize(&frame_width, &frame_height);
  if (FAILED(hr) || frame_width == 0 || frame_height == 0) {
    *error = MakeError("WebP: invalid frame size", hr);
    return false;
  }
  const uint32_t stride = static_cast<uint32_t>(frame_width) * 4u;
  pixels->resize(static_cast<size_t>(stride) * frame_height);
  hr = converter->CopyPixels(nullptr, stride,
                             static_cast<UINT>(pixels->size()),
                             pixels->data());
  if (FAILED(hr)) {
    *error = MakeError("WebP: CopyPixels failed", hr);
    return false;
  }
  *width = frame_width;
  *height = frame_height;
  return true;
}

}  // namespace

AnimInfo Inspect(const std::filesystem::path& path) {
  AnimInfo info;
  std::vector<uint8_t> data;
  if (!ReadContainer(path, &data)) return info;
  if (std::memcmp(data.data(), "RIFF", 4) != 0 ||
      std::memcmp(data.data() + 8, "WEBP", 4) != 0) {
    return info;
  }

  size_t offset = 12;
  while (offset + 8 <= data.size()) {
    const uint8_t* chunk = data.data() + offset;
    const uint32_t size = ReadU32(chunk + 4);
    const size_t payload = offset + 8;
    if (payload + size > data.size()) break;
    if (std::memcmp(chunk, "VP8X", 4) == 0 && size >= 10) {
      const uint8_t* p = data.data() + payload;
      info.animated = (p[0] & 0x02) != 0;
      info.canvas_width = 1 + ReadU24(p + 4);
      info.canvas_height = 1 + ReadU24(p + 7);
    } else if (std::memcmp(chunk, "ANIM", 4) == 0 && size >= 6) {
      const uint8_t* p = data.data() + payload;
      info.loop_count = static_cast<uint32_t>(p[4]) |
                        (static_cast<uint32_t>(p[5]) << 8);
    } else if (std::memcmp(chunk, "ANMF", 4) == 0 && size >= 16) {
      const uint8_t* p = data.data() + payload;
      AnimFrameInfo frame;
      frame.x = ReadU24(p) * 2u;
      frame.y = ReadU24(p + 3) * 2u;
      frame.width = 1 + ReadU24(p + 6);
      frame.height = 1 + ReadU24(p + 9);
      frame.duration_ms = ReadU24(p + 12);
      frame.dispose_to_background = (p[15] & 0x01) != 0;
      frame.no_blend = (p[15] & 0x02) != 0;
      info.frames.push_back(frame);
    }
    offset = payload + size + (size & 1u);
  }
  if (!info.animated || info.frames.size() < 2) {
    info.animated = false;
  }
  if (info.canvas_width == 0 || info.canvas_height == 0) {
    info.canvas_width = info.frames.empty() ? 0 : info.frames[0].width;
    info.canvas_height = info.frames.empty() ? 0 : info.frames[0].height;
  }
  return info;
}

DecodeResult Decode(const std::filesystem::path& path,
                    const DecodeOptions& options) {
  // Thumbnails and prefetch only need the first frame; the normal WIC path is
  // cheaper and returns the same pixels (坑 13).
  if (!options.animation || options.max_edge > 0) {
    return wic::Decode(path, options);
  }

  const AnimInfo info = Inspect(path);
  if (!info.animated) return wic::Decode(path, options);
  if (info.canvas_width == 0 || info.canvas_height == 0) {
    DecodeResult result;
    result.error = MakeError("WebP: animation has no canvas");
    return result;
  }
  if (!wic::EnsureComInitialized()) {
    DecodeResult result;
    result.error = MakeError("WebP: COM initialization failed");
    return result;
  }

  ComPtr<IWICImagingFactory> factory;
  HRESULT hr = CoCreateInstance(CLSID_WICImagingFactory, nullptr,
                                CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&factory));
  if (FAILED(hr)) {
    DecodeResult result;
    result.error = MakeError("WebP: WIC factory creation failed", hr);
    return result;
  }
  ComPtr<IWICBitmapDecoder> decoder;
  hr = factory->CreateDecoderFromFilename(path.c_str(), nullptr, GENERIC_READ,
                                          WICDecodeMetadataCacheOnDemand,
                                          decoder.GetAddressOf());
  if (FAILED(hr)) {
    DecodeResult result;
    result.error = MakeError("WebP: decoder creation failed", hr);
    return result;
  }

  UINT container_frames = 0;
  if (SUCCEEDED(decoder->GetFrameCount(&container_frames))) {
    // Trust the decoder only up to what the container declares; a mismatch is
    // logged and the smaller count wins (規劃/22/12 坑 11/20).
    if (container_frames != info.frames.size()) {
      nv::platform::LogInfo(
          "webp: container declares " + std::to_string(info.frames.size()) +
          " frames, decoder reports " + std::to_string(container_frames));
    }
  }

  const uint32_t canvas_width = info.canvas_width;
  const uint32_t canvas_height = info.canvas_height;
  const uint64_t canvas_bytes =
      static_cast<uint64_t>(canvas_width) * canvas_height * 4u;
  if (canvas_bytes == 0) {
    DecodeResult result;
    result.error = MakeError("WebP: empty canvas");
    return result;
  }

  size_t wanted = std::min<size_t>(info.frames.size(), kMaxFrames);
  if (canvas_bytes > 0) {
    const uint64_t budget = kMaxAnimationBytes / canvas_bytes;
    wanted = std::min<size_t>(wanted, static_cast<size_t>(budget));
  }
  const bool truncated = wanted < info.frames.size();
  if (wanted < 2) {
    DecodeResult result;
    result.error = MakeError("WebP: animation exceeds the size limit");
    return result;
  }

  std::vector<uint8_t> canvas(static_cast<size_t>(canvas_bytes), 0);
  std::vector<ImageFrame> frames;
  frames.reserve(wanted);

  for (size_t i = 0; i < wanted; ++i) {
    uint32_t frame_width = 0;
    uint32_t frame_height = 0;
    std::vector<uint8_t> pixels;
    Error error;
    if (!ReadFrameBgra(factory.Get(), decoder.Get(), static_cast<UINT>(i),
                       &frame_width, &frame_height, &pixels, &error)) {
      if (frames.empty()) {
        DecodeResult result;
        result.error = error;
        return result;
      }
      nv::platform::LogInfo("webp: stopped at frame " + std::to_string(i) +
                            " (" + error.message + ")");
      break;
    }

    const AnimFrameInfo& meta = info.frames[i];
    if (frame_width == canvas_width && frame_height == canvas_height) {
      // The common WebP layout: WIC already composited the full canvas, so a
      // full replacement can never leave a ghost (規劃/22/12 坑 10).
      canvas = std::move(pixels);
    } else {
      const uint32_t x = std::min(meta.x, canvas_width - 1);
      const uint32_t y = std::min(meta.y, canvas_height - 1);
      const uint32_t width = std::min(frame_width, canvas_width - x);
      const uint32_t height = std::min(frame_height, canvas_height - y);
      BlendOnto(canvas.data(), canvas_width, canvas_height, pixels.data(),
                width, height, x, y, !meta.no_blend);
      if (meta.dispose_to_background) {
        ClearRect(canvas.data(), canvas_width, canvas_height, x, y, width,
                  height);
      }
    }

    ImageFrame frame;
    frame.pixels = canvas;
    frame.delay_ms = meta.duration_ms == 0 ? kDefaultFrameDelayMs
                                           : meta.duration_ms;
    frames.push_back(std::move(frame));
  }

  if (frames.empty()) {
    DecodeResult result;
    result.error = MakeError("WebP: no frame could be decoded");
    return result;
  }

  Image image;
  image.info.width = canvas_width;
  image.info.height = canvas_height;
  image.info.stride = canvas_width * 4u;
  image.info.format = PixelFormat::kBgra8;
  image.info.has_alpha = true;
  image.info.full_width = canvas_width;
  image.info.full_height = canvas_height;
  image.info.orientation = 1;
  const size_t decoded_frames = frames.size();
  image.info.frame_count = static_cast<uint32_t>(decoded_frames);
  // `animated` also stays true when the container advertises frames that the
  // system decoder could not enumerate: the pill then reports "first frame
  // only" instead of silently pretending the file is static.
  image.info.animated = decoded_frames > 1 || info.animated;
  image.info.frames_truncated =
      truncated || decoded_frames < info.frames.size();
  image.info.dpi_x = 96.0;
  image.info.dpi_y = 96.0;
  image.info.source = "webp";
  image.loop_count = info.loop_count;
  image.pixels = frames[0].pixels;
  if (frames.size() > 1) image.frames = std::move(frames);

  char line[192]{};
  std::snprintf(line, sizeof(line),
                "webp: animation frames=%u loop=%u canvas=%ux%u%s",
                static_cast<unsigned>(image.info.frame_count),
                static_cast<unsigned>(image.loop_count), canvas_width,
                canvas_height, image.info.frames_truncated ? " truncated" : "");
  nv::platform::LogInfo(line);

  DecodeResult result;
  result.image = std::make_shared<Image>(std::move(image));
  return result;
}

}  // namespace nv::webp
