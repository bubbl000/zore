#include "ui/app_logo.h"

#include <windows.h>
#include <wincodec.h>

namespace nv::ui {
namespace {

// Must match src/app/version.rc.
constexpr int kLogoResource = 102;

Microsoft::WRL::ComPtr<IWICImagingFactory> CreateFactory() {
  Microsoft::WRL::ComPtr<IWICImagingFactory> factory;
  CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER,
                   IID_PPV_ARGS(&factory));
  return factory;
}

}  // namespace

bool AppLogo::Load() {
  tried_ = true;
  const HRSRC resource = FindResourceW(GetModuleHandleW(nullptr),
                                       MAKEINTRESOURCEW(kLogoResource),
                                       MAKEINTRESOURCEW(10) /* RT_RCDATA */);
  if (resource == nullptr) return false;
  const HGLOBAL handle = LoadResource(nullptr, resource);
  const void* data = handle != nullptr ? LockResource(handle) : nullptr;
  const DWORD size = SizeofResource(nullptr, resource);
  if (data == nullptr || size == 0) return false;

  Microsoft::WRL::ComPtr<IWICImagingFactory> factory = CreateFactory();
  if (!factory) return false;

  Microsoft::WRL::ComPtr<IWICStream> stream;
  if (FAILED(factory->CreateStream(&stream))) return false;
  if (FAILED(stream->InitializeFromMemory(
          static_cast<BYTE*>(const_cast<void*>(data)), size))) {
    return false;
  }

  Microsoft::WRL::ComPtr<IWICBitmapDecoder> decoder;
  if (FAILED(factory->CreateDecoderFromStream(
          stream.Get(), nullptr, WICDecodeMetadataCacheOnDemand, &decoder))) {
    return false;
  }
  Microsoft::WRL::ComPtr<IWICBitmapFrameDecode> frame;
  if (FAILED(decoder->GetFrame(0, &frame))) return false;
  if (FAILED(factory->CreateFormatConverter(&source_))) return false;
  // Premultiplied BGRA is the only format D2D accepts for DrawBitmap.
  if (FAILED(source_->Initialize(frame.Get(), GUID_WICPixelFormat32bppPBGRA,
                                 WICBitmapDitherTypeNone, nullptr, 0.0,
                                 WICBitmapPaletteTypeCustom))) {
    source_.Reset();
    return false;
  }
  return true;
}

ID2D1Bitmap* AppLogo::Get(ID2D1DeviceContext* context) {
  if (context == nullptr) return nullptr;
  if (!tried_ && !Load()) return nullptr;
  if (!source_) return nullptr;

  Microsoft::WRL::ComPtr<ID2D1Device> device;
  context->GetDevice(&device);
  if (bitmap_ && device_ == device) return bitmap_.Get();

  bitmap_.Reset();
  if (FAILED(context->CreateBitmapFromWicBitmap(source_.Get(), nullptr,
                                                &bitmap_))) {
    return nullptr;
  }
  device_ = device;
  return bitmap_.Get();
}

}  // namespace nv::ui
