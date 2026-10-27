#pragma once

#include <d2d1_1.h>
#include <wincodec.h>
#include <wrl/client.h>

namespace nv::ui {

// The application logo shown at the left of the custom title bar. The artwork
// lives in src/app/app_logo.png and is embedded as RCDATA by version.rc, so the
// exe stays self-contained; it is decoded through WIC once and recreated when
// the D2D device changes (device loss).
class AppLogo {
 public:
  // Returns the bitmap for this device context, or nullptr when the resource is
  // missing (the chrome then falls back to the vector glyph).
  ID2D1Bitmap* Get(ID2D1DeviceContext* context);

 private:
  bool Load();

  bool tried_ = false;
  Microsoft::WRL::ComPtr<IWICFormatConverter> source_;
  Microsoft::WRL::ComPtr<ID2D1Bitmap> bitmap_;
  Microsoft::WRL::ComPtr<ID2D1Device> device_;
};

}  // namespace nv::ui
