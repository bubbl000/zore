#include "platform/screenshot.h"

#include <windows.h>
#include <wincodec.h>
#include <wrl/client.h>

#include <cstdint>

using Microsoft::WRL::ComPtr;

namespace nv::platform {

bool SavePng(const std::filesystem::path& path, const uint8_t* bgra,
             uint32_t width, uint32_t height, uint32_t stride) {
  if (bgra == nullptr || width == 0 || height == 0 || stride < width * 4u) {
    return false;
  }

  // WIC needs COM on the calling thread. The app intentionally does not
  // initialize COM on its UI thread during startup (it cost ~70 ms), so the
  // --screenshot test hook initializes it here and lets process exit clean up.
  const HRESULT com_hr = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
  if (FAILED(com_hr) && com_hr != RPC_E_CHANGED_MODE) return false;

  ComPtr<IWICImagingFactory> factory;
  if (FAILED(CoCreateInstance(CLSID_WICImagingFactory, nullptr,
                              CLSCTX_INPROC_SERVER,
                              IID_PPV_ARGS(factory.GetAddressOf())))) {
    return false;
  }

  ComPtr<IWICStream> stream;
  if (FAILED(factory->CreateStream(stream.GetAddressOf()))) return false;
  if (FAILED(stream->InitializeFromFilename(path.c_str(), GENERIC_WRITE))) {
    return false;
  }

  ComPtr<IWICBitmapEncoder> encoder;
  if (FAILED(factory->CreateEncoder(GUID_ContainerFormatPng, nullptr,
                                    encoder.GetAddressOf()))) {
    return false;
  }
  if (FAILED(encoder->Initialize(stream.Get(), WICBitmapEncoderNoCache))) {
    return false;
  }

  ComPtr<IWICBitmapFrameEncode> frame;
  ComPtr<IPropertyBag2> options;
  if (FAILED(encoder->CreateNewFrame(frame.GetAddressOf(),
                                     options.GetAddressOf()))) {
    return false;
  }
  if (FAILED(frame->Initialize(options.Get()))) return false;
  if (FAILED(frame->SetSize(width, height))) return false;

  WICPixelFormatGUID format = GUID_WICPixelFormat32bppBGRA;
  if (FAILED(frame->SetPixelFormat(&format))) return false;
  if (!IsEqualGUID(format, GUID_WICPixelFormat32bppBGRA)) return false;

  const uint64_t total = static_cast<uint64_t>(stride) * height;
  if (total > 0xFFFFFFFFull) return false;
  if (FAILED(frame->WritePixels(height, stride, static_cast<UINT>(total),
                                const_cast<BYTE*>(bgra)))) {
    return false;
  }
  if (FAILED(frame->Commit())) return false;
  return SUCCEEDED(encoder->Commit());
}

}  // namespace nv::platform
