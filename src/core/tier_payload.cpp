#include "core/tier_payload.h"

#include "decoders/wic_decoder.h"
#include "platform/log.h"

#include <windows.h>
#include <wincodec.h>
#include <wrl/client.h>

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <vector>

using Microsoft::WRL::ComPtr;

namespace nv {
namespace {

// JPEG needs packed 24bpp BGR; the tier frame is BGRA8 (image_io.cpp does the
// same conversion for its own saves).
bool PackBgr(const Image& image, std::vector<uint8_t>* packed) {
  const uint32_t width = image.info.width;
  const uint32_t height = image.info.height;
  const uint32_t stride = image.info.stride;
  if (width == 0 || height == 0 || stride < width * 4u) return false;
  const uint64_t total = static_cast<uint64_t>(width) * 3u * height;
  if (total > 0xFFFFFFFFull) return false;
  packed->resize(static_cast<size_t>(total));
  for (uint32_t y = 0; y < height; ++y) {
    const uint8_t* source = image.pixels.data() + static_cast<size_t>(y) * stride;
    uint8_t* destination = packed->data() + static_cast<size_t>(y) * width * 3u;
    for (uint32_t x = 0; x < width; ++x) {
      destination[x * 3u + 0] = source[x * 4u + 0];
      destination[x * 3u + 1] = source[x * 4u + 1];
      destination[x * 3u + 2] = source[x * 4u + 2];
    }
  }
  return true;
}

bool ReadStreamBytes(IStream* stream, std::vector<uint8_t>* bytes) {
  STATSTG stat{};
  if (FAILED(stream->Stat(&stat, STATFLAG_NONAME))) return false;
  const uint64_t size = stat.cbSize.QuadPart;
  if (size == 0 || size > 64ull * 1024 * 1024) return false;
  bytes->resize(static_cast<size_t>(size));
  LARGE_INTEGER origin{};
  if (FAILED(stream->Seek(origin, STREAM_SEEK_SET, nullptr))) return false;
  ULONG read = 0;
  if (FAILED(stream->Read(bytes->data(), static_cast<ULONG>(size), &read))) {
    return false;
  }
  if (read != size) return false;
  return true;
}

}  // namespace

bool EncodeTierPayload(const Image& image, TierCodec* codec,
                       std::vector<uint8_t>* payload) {
  if (codec == nullptr || payload == nullptr) return false;
  if (image.Empty() || image.info.format != PixelFormat::kBgra8) return false;
  if (!wic::EnsureComInitialized()) return false;

  const bool alpha = image.info.has_alpha;
  const TierCodec chosen = alpha ? TierCodec::kPng : TierCodec::kJpeg;

  ComPtr<IWICImagingFactory> factory;
  HRESULT hr = CoCreateInstance(CLSID_WICImagingFactory, nullptr,
                                CLSCTX_INPROC_SERVER,
                                IID_PPV_ARGS(factory.GetAddressOf()));
  if (FAILED(hr)) return false;

  ComPtr<IStream> stream;
  hr = CreateStreamOnHGlobal(nullptr, TRUE, stream.GetAddressOf());
  if (FAILED(hr)) return false;

  ComPtr<IWICStream> wic_stream;
  hr = factory->CreateStream(wic_stream.GetAddressOf());
  if (SUCCEEDED(hr)) hr = wic_stream->InitializeFromIStream(stream.Get());
  if (FAILED(hr)) return false;

  ComPtr<IWICBitmapEncoder> encoder;
  hr = factory->CreateEncoder(alpha ? GUID_ContainerFormatPng
                                    : GUID_ContainerFormatJpeg,
                              nullptr, encoder.GetAddressOf());
  if (SUCCEEDED(hr)) {
    hr = encoder->Initialize(wic_stream.Get(), WICBitmapEncoderNoCache);
  }
  if (FAILED(hr)) return false;

  ComPtr<IWICBitmapFrameEncode> frame;
  ComPtr<IPropertyBag2> options;
  hr = encoder->CreateNewFrame(frame.GetAddressOf(), options.GetAddressOf());
  if (SUCCEEDED(hr) && !alpha && options) {
    PROPBAG2 option{};
    option.pstrName = const_cast<LPOLESTR>(L"ImageQuality");
    VARIANT value;
    VariantInit(&value);
    value.vt = VT_R4;
    value.fltVal = static_cast<float>(kTierJpegQuality) / 100.0f;
    options->Write(1, &option, &value);
    VariantClear(&value);
  }
  if (SUCCEEDED(hr)) hr = frame->Initialize(options.Get());
  if (SUCCEEDED(hr)) hr = frame->SetSize(image.info.width, image.info.height);
  if (FAILED(hr)) return false;

  WICPixelFormatGUID pixel_format =
      alpha ? GUID_WICPixelFormat32bppBGRA : GUID_WICPixelFormat24bppBGR;
  hr = frame->SetPixelFormat(&pixel_format);
  if (FAILED(hr)) return false;
  if (!IsEqualGUID(pixel_format, alpha ? GUID_WICPixelFormat32bppBGRA
                                       : GUID_WICPixelFormat24bppBGR)) {
    return false;
  }

  if (alpha) {
    const uint64_t total =
        static_cast<uint64_t>(image.info.stride) * image.info.height;
    if (total > 0xFFFFFFFFull) return false;
    hr = frame->WritePixels(image.info.height, image.info.stride,
                            static_cast<UINT>(total),
                            const_cast<BYTE*>(image.pixels.data()));
  } else {
    std::vector<uint8_t> packed;
    if (!PackBgr(image, &packed)) return false;
    const uint32_t row_bytes = image.info.width * 3u;
    const uint64_t total = static_cast<uint64_t>(row_bytes) * image.info.height;
    if (total > 0xFFFFFFFFull) return false;
    hr = frame->WritePixels(image.info.height, row_bytes,
                            static_cast<UINT>(total), packed.data());
  }
  if (FAILED(hr)) return false;
  hr = frame->Commit();
  if (SUCCEEDED(hr)) hr = encoder->Commit();
  if (FAILED(hr)) return false;
  frame.Reset();
  encoder.Reset();
  wic_stream.Reset();

  if (!ReadStreamBytes(stream.Get(), payload)) return false;
  *codec = chosen;
  return true;
}

}  // namespace nv
