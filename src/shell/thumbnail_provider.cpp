#include "shell/thumbnail_provider.h"

#include "core/format_router.h"
#include "core/image.h"
#include "decoders/svg_decoder.h"
#include "shell/module_state.h"

#include <objbase.h>
#include <shobjidl.h>
#include <thumbcache.h>
#include <wrl/client.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <cwchar>
#include <filesystem>
#include <iterator>
#include <memory>
#include <mutex>
#include <new>
#include <string>
#include <thread>
#include <utility>
#include <vector>

using Microsoft::WRL::ComPtr;

namespace nv::shell {
namespace {

// Shell asks for 16..1024 pixels; larger requests are clamped so a hostile or
// confused host cannot make us allocate a huge bitmap.
constexpr UINT kMaxThumbnailEdge = 1024;
// Decode at twice the requested width and downsample, which keeps the result
// sharp without a second full-size decode.
constexpr uint32_t kMaxDecodeEdge = 2048;
// Shell expects fast failures rather than a hung Explorer window.
constexpr DWORD kDecodeBudgetMs = 2000;
constexpr int kThumbnailErrorBase = 3000;

struct DecodeJob {
  std::mutex mutex;
  std::condition_variable done_event;
  bool done = false;
  DecodeResult result;
};

// Opt-in trace used by tests and field diagnosis. The handler stays silent in
// normal Explorer use (no log files written unless NV_THUMB_TRACE is set).
bool TraceEnabled() {
  static const bool enabled = [] {
    wchar_t value[8]{};
    if (GetEnvironmentVariableW(L"NV_THUMB_TRACE", value,
                                static_cast<DWORD>(std::size(value))) > 0) {
      return true;
    }
    // A marker file lets a test trace a handler that Shell loaded in another
    // process (COM surrogate), where the environment is not inherited.
    wchar_t temp[MAX_PATH]{};
    if (GetTempPathW(static_cast<DWORD>(std::size(temp)), temp) == 0) {
      return false;
    }
    const std::wstring marker = std::wstring(temp) + L"nv-thumb-trace.on";
    return GetFileAttributesW(marker.c_str()) != INVALID_FILE_ATTRIBUTES;
  }();
  return enabled;
}

void Trace(const std::wstring& message) {
  if (!TraceEnabled()) return;
  wchar_t temp[MAX_PATH]{};
  if (GetTempPathW(static_cast<DWORD>(std::size(temp)), temp) == 0) return;
  const std::wstring path = std::wstring(temp) + L"nv-thumb-trace.log";
  FILE* file = nullptr;
  if (_wfopen_s(&file, path.c_str(), L"a, ccs=UTF-8") != 0 || file == nullptr) {
    return;
  }
  std::fwprintf(file, L"%s\n", message.c_str());
  std::fclose(file);
}

std::vector<uint8_t> ScaleStraightBgra(const uint8_t* source, uint32_t width,
                                       uint32_t height, uint32_t stride,
                                       uint32_t target_width,
                                       uint32_t target_height) {
  std::vector<uint8_t> destination(
      static_cast<size_t>(target_width) * target_height * 4u);
  for (uint32_t y = 0; y < target_height; ++y) {
    const uint32_t y0 = static_cast<uint32_t>(
        static_cast<uint64_t>(y) * height / target_height);
    uint32_t y1 = static_cast<uint32_t>(
        static_cast<uint64_t>(y + 1) * height / target_height);
    if (y1 <= y0) y1 = y0 + 1;
    for (uint32_t x = 0; x < target_width; ++x) {
      const uint32_t x0 = static_cast<uint32_t>(
          static_cast<uint64_t>(x) * width / target_width);
      uint32_t x1 = static_cast<uint32_t>(
          static_cast<uint64_t>(x + 1) * width / target_width);
      if (x1 <= x0) x1 = x0 + 1;

      // Average in premultiplied space so transparent edges do not darken,
      // then store straight alpha again (what Shell expects).
      uint64_t sum_b = 0;
      uint64_t sum_g = 0;
      uint64_t sum_r = 0;
      uint64_t sum_a = 0;
      uint64_t count = 0;
      for (uint32_t sy = y0; sy < y1; ++sy) {
        const uint8_t* row = source + static_cast<size_t>(sy) * stride;
        for (uint32_t sx = x0; sx < x1; ++sx) {
          const uint8_t* pixel = row + static_cast<size_t>(sx) * 4u;
          const uint64_t alpha = pixel[3];
          sum_b += pixel[0] * alpha;
          sum_g += pixel[1] * alpha;
          sum_r += pixel[2] * alpha;
          sum_a += alpha;
          ++count;
        }
      }

      uint8_t* out = destination.data() +
                     (static_cast<size_t>(y) * target_width + x) * 4u;
      if (sum_a == 0) {
        out[0] = 0;
        out[1] = 0;
        out[2] = 0;
        out[3] = 0;
      } else {
        out[0] = static_cast<uint8_t>(sum_b / sum_a);
        out[1] = static_cast<uint8_t>(sum_g / sum_a);
        out[2] = static_cast<uint8_t>(sum_r / sum_a);
        out[3] = static_cast<uint8_t>((sum_a + count / 2) / count);
      }
    }
  }
  return destination;
}

// Creates the 32bpp top-down DIB Shell expects. Pixels stay straight alpha:
// Shell composites thumbnails itself and does not want premultiplied data.
HRESULT CreateThumbnailBitmap(const Image& image, UINT cx, HBITMAP* bitmap,
                              WTS_ALPHATYPE* alpha_type) {
  const uint32_t width = image.info.width;
  const uint32_t height = image.info.height;
  if (width == 0 || height == 0 || image.pixels.empty()) return E_FAIL;

  uint32_t target_width = width;
  uint32_t target_height = height;
  if (width > cx) {
    target_width = cx;
    target_height = static_cast<uint32_t>(std::max<uint64_t>(
        1, static_cast<uint64_t>(height) * cx / width));
  }

  const uint32_t stride =
      image.info.stride != 0 ? image.info.stride : width * 4u;
  std::vector<uint8_t> scaled;
  const uint8_t* pixels = image.pixels.data();
  uint32_t source_stride = stride;
  if (target_width != width || target_height != height) {
    scaled = ScaleStraightBgra(pixels, width, height, stride, target_width,
                               target_height);
    pixels = scaled.data();
    source_stride = target_width * 4u;
  }

  BITMAPINFO info{};
  info.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
  info.bmiHeader.biWidth = static_cast<LONG>(target_width);
  info.bmiHeader.biHeight = -static_cast<LONG>(target_height);  // top-down
  info.bmiHeader.biPlanes = 1;
  info.bmiHeader.biBitCount = 32;
  info.bmiHeader.biCompression = BI_RGB;

  void* bits = nullptr;
  HBITMAP result =
      CreateDIBSection(nullptr, &info, DIB_RGB_COLORS, &bits, nullptr, 0);
  if (result == nullptr || bits == nullptr) {
    if (result != nullptr) DeleteObject(result);
    return E_OUTOFMEMORY;
  }

  const size_t row_bytes = static_cast<size_t>(target_width) * 4u;
  auto* destination = static_cast<uint8_t*>(bits);
  for (uint32_t y = 0; y < target_height; ++y) {
    std::memcpy(destination + static_cast<size_t>(y) * row_bytes,
                pixels + static_cast<size_t>(y) * source_stride, row_bytes);
  }
  if (!image.info.has_alpha) {
    for (size_t i = 3; i < static_cast<size_t>(target_width) * target_height * 4u;
         i += 4) {
      destination[i] = 255;
    }
  }

  *bitmap = result;
  *alpha_type = image.info.has_alpha ? WTSAT_ARGB : WTSAT_RGB;
  return S_OK;
}

// Runs the decode on a worker thread so the shell's calling thread is never
// blocked longer than the budget. The worker holds a module reference until it
// finishes, so a timed-out decode cannot outlive the DLL.
HRESULT DecodeWithBudget(const std::wstring& path, uint32_t cx,
                         DecodeResult* result) {
  DecodeOptions options;
  options.animation = false;  // Only the first frame is shown in Explorer.
  options.max_edge = std::min<uint32_t>(
      kMaxDecodeEdge, std::max<uint32_t>(16, cx * 2));

  auto job = std::make_shared<DecodeJob>();
  auto activity = std::make_shared<DecodeActivity>();
  try {
    std::thread worker([job, activity, path, options] {
      HRESULT com = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
      DecodeResult decoded;
      try {
        decoded = FormatRouter::Decode(path, options);
      } catch (const std::bad_alloc&) {
        decoded.error = Error::Make(kThumbnailErrorBase, "out of memory");
      } catch (...) {
        decoded.error = Error::Make(kThumbnailErrorBase, "decode failed");
      }
      // Direct2D's per-thread device must not be torn down from TLS
      // destructors (see HANDOFF pit 68); the worker owns it and releases it
      // before exiting.
      try {
        svg::ReleaseThreadContext();
      } catch (...) {
      }
      {
        std::lock_guard lock(job->mutex);
        job->result = std::move(decoded);
        job->done = true;
      }
      job->done_event.notify_all();
      if (SUCCEEDED(com)) CoUninitialize();
    });
    worker.detach();
  } catch (...) {
    return E_FAIL;
  }

  std::unique_lock lock(job->mutex);
  if (!job->done_event.wait_for(
          lock, std::chrono::milliseconds(kDecodeBudgetMs),
          [&job] { return job->done; })) {
    return HRESULT_FROM_WIN32(ERROR_TIMEOUT);
  }
  *result = std::move(job->result);
  return S_OK;
}

class ThumbnailProvider final : public IThumbnailProvider,
                                public IInitializeWithItem,
                                public IInitializeWithFile {
 public:
  ThumbnailProvider() { AddLiveObject(); }
  ~ThumbnailProvider() { ReleaseLiveObject(); }

  ThumbnailProvider(const ThumbnailProvider&) = delete;
  ThumbnailProvider& operator=(const ThumbnailProvider&) = delete;

  // IUnknown
  HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid,
                                           void** object) override {
    if (object == nullptr) return E_POINTER;
    *object = nullptr;
    if (IsEqualIID(riid, IID_IUnknown) ||
        IsEqualIID(riid, IID_IThumbnailProvider)) {
      *object = static_cast<IThumbnailProvider*>(this);
    } else if (IsEqualIID(riid, IID_IInitializeWithItem)) {
      *object = static_cast<IInitializeWithItem*>(this);
    } else if (IsEqualIID(riid, IID_IInitializeWithFile)) {
      *object = static_cast<IInitializeWithFile*>(this);
    } else {
      return E_NOINTERFACE;
    }
    AddRef();
    return S_OK;
  }

  ULONG STDMETHODCALLTYPE AddRef() override {
    return references_.fetch_add(1) + 1;
  }

  ULONG STDMETHODCALLTYPE Release() override {
    const ULONG remaining = references_.fetch_sub(1) - 1;
    if (remaining == 0) delete this;
    return remaining;
  }

  // IInitializeWithItem
  HRESULT STDMETHODCALLTYPE Initialize(IShellItem* item,
                                       DWORD mode) override {
    if (item == nullptr) return E_INVALIDARG;
    if ((mode & STGM_WRITE) != 0) return E_INVALIDARG;

    LPWSTR path = nullptr;
    const HRESULT hr =
        item->GetDisplayName(SIGDN_FILESYSPATH, &path);
    if (FAILED(hr)) {
      if (TraceEnabled()) Trace(L"init-item failed");
      return hr;
    }
    SetPath(path);
    if (TraceEnabled()) Trace(std::wstring(L"init-item ") + path);
    CoTaskMemFree(path);
    return S_OK;
  }

  // IInitializeWithFile
  HRESULT STDMETHODCALLTYPE Initialize(LPCWSTR file_path,
                                       DWORD mode) override {
    if (file_path == nullptr || file_path[0] == L'\0') return E_INVALIDARG;
    if ((mode & STGM_WRITE) != 0) return E_INVALIDARG;
    SetPath(file_path);
    if (TraceEnabled()) Trace(std::wstring(L"init-file ") + file_path);
    return S_OK;
  }

  // IThumbnailProvider
  HRESULT STDMETHODCALLTYPE GetThumbnail(UINT cx, HBITMAP* bitmap,
                                         WTS_ALPHATYPE* alpha_type) override {
    if (bitmap == nullptr || alpha_type == nullptr) return E_POINTER;
    *bitmap = nullptr;
    *alpha_type = WTSAT_UNKNOWN;

    std::wstring path;
    {
      AcquireSRWLockShared(&path_lock_);
      path = path_;
      ReleaseSRWLockShared(&path_lock_);
    }
    if (path.empty()) return E_UNEXPECTED;

    const UINT requested = std::clamp<UINT>(cx, 1, kMaxThumbnailEdge);
    try {
      if (TraceEnabled()) {
        Trace(L"get cx=" + std::to_wstring(requested) + L" " + path);
      }
      DecodeResult decoded;
      const HRESULT hr = DecodeWithBudget(path, requested, &decoded);
      if (FAILED(hr)) {
        if (TraceEnabled()) {
          Trace(L"get failed hr=" + std::to_wstring(hr));
        }
        return hr;
      }
      if (!decoded.ok()) {
        if (TraceEnabled()) {
          Trace(L"get decode error " +
                std::wstring(decoded.error.message.begin(),
                             decoded.error.message.end()));
        }
        return E_FAIL;
      }
      const HRESULT created =
          CreateThumbnailBitmap(*decoded.image, requested, bitmap, alpha_type);
      if (TraceEnabled()) Trace(L"get ok hr=" + std::to_wstring(created));
      return created;
    } catch (...) {
      if (*bitmap != nullptr) {
        DeleteObject(*bitmap);
        *bitmap = nullptr;
      }
      *alpha_type = WTSAT_UNKNOWN;
      return E_FAIL;
    }
  }

 private:
  void SetPath(const wchar_t* path) {
    AcquireSRWLockExclusive(&path_lock_);
    path_ = path;
    ReleaseSRWLockExclusive(&path_lock_);
  }

  std::atomic<ULONG> references_{1};
  SRWLOCK path_lock_ = SRWLOCK_INIT;
  std::wstring path_;
};

}  // namespace

HRESULT CreateThumbnailProvider(REFIID riid, void** object) {
  if (object == nullptr) return E_POINTER;
  *object = nullptr;
  auto* provider = new (std::nothrow) ThumbnailProvider();
  if (provider == nullptr) return E_OUTOFMEMORY;
  const HRESULT hr = provider->QueryInterface(riid, object);
  provider->Release();
  return hr;
}

}  // namespace nv::shell
