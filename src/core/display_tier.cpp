#include "core/display_tier.h"

#include "decoders/wic_decoder.h"
#include "platform/log.h"

#include <wincodec.h>
#include <wrl/client.h>

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <utility>
#include <vector>

using Microsoft::WRL::ComPtr;

namespace nv {
namespace {

double QpcMs() {
  static const LARGE_INTEGER frequency = [] {
    LARGE_INTEGER value{};
    QueryPerformanceFrequency(&value);
    return value;
  }();
  LARGE_INTEGER now{};
  QueryPerformanceCounter(&now);
  return static_cast<double>(now.QuadPart) * 1000.0 /
         static_cast<double>(frequency.QuadPart);
}

uint32_t ScaleDimension(uint32_t value, double scale) {
  const double scaled = static_cast<double>(value) * scale + 0.5;
  if (scaled < 1.0) return 1;
  if (scaled > 65535.0) return 65535;
  return static_cast<uint32_t>(scaled);
}

}  // namespace

uint32_t ChooseTier(uint32_t needed_long_edge) {
  if (needed_long_edge == 0) return 0;
  for (const uint32_t tier : kDisplayTiers) {
    if (tier >= needed_long_edge) return tier;
  }
  return kDisplayTierMax;
}

uint32_t DecideTierEdge(const TierLoadInputs& in) {
  // §7.3 rule 3 first: a session that already has the full frame must not go
  // back to a tier, not even with --display-cache=force (that hook is about the
  // load strategy, not about undoing a sharper frame).
  if (in.full_frame_available) return 0;
  if (in.needed_long_edge == 0) return 0;
  const uint32_t tier = ChooseTier(in.needed_long_edge);
  if (tier == 0) return 0;
  if (!in.force) {
    if (in.scale >= 1.0f) return 0;             // §7.6: 100 % needs real pixels
    if (tier >= in.source_long_edge) return 0;  // nothing to gain
  }
  return tier;
}

bool ShouldUpgradeToFullFrame(const TierUpgradeInputs& in) {
  if (in.have_long_edge == 0 || in.file_long_edge == 0) return false;
  // The bitmap already is the file (or bigger): nothing sharper to ask for.
  if (in.have_long_edge >= in.file_long_edge) return false;
  // Pixel mode promises hard source pixels, so the tier can never serve it.
  if (in.pixel_mode) return true;
  // 100 % always upgrades, for every format (V3-04b behaviour kept).
  if (in.file_scale >= 1.0f) return true;
  // Below 100 % only non-RAW upgrades early: a LibRaw full decode costs 1.1-1.5 s
  // while the embedded preview is ~80 ms, and the preview is still sharp at the
  // scales where this would fire.
  if (in.raw) return false;
  if (in.file_bytes > kFullDecodeFileLimitBytes) return false;
  const double need =
      static_cast<double>(in.file_long_edge) * static_cast<double>(in.file_scale);
  if (!(need > 0.0)) return false;
  return need >
         static_cast<double>(in.have_long_edge) * static_cast<double>(kTierUpscaleLimit);
}

uint32_t NextTier(uint32_t edge) {
  if (edge == 0) return 0;  // "no tier" has no successor
  for (const uint32_t tier : kDisplayTiers) {
    if (tier > edge) return tier;
  }
  return 0;
}

uint32_t PrevTier(uint32_t edge) {
  if (edge == 0) return 0;  // the full frame has no smaller tier
  uint32_t previous = 0;
  for (const uint32_t tier : kDisplayTiers) {
    if (tier >= edge) break;
    previous = tier;
  }
  return previous;
}

std::shared_ptr<Image> MakeDisplayTier(const Image& source, uint32_t edge) {
  if (source.Empty() || edge == 0) return nullptr;
  if (source.info.format != PixelFormat::kBgra8) return nullptr;
  const uint32_t width = source.info.width;
  const uint32_t height = source.info.height;
  const uint32_t longest = ImageLongEdge(source);
  if (width == 0 || height == 0 || edge >= longest) return nullptr;
  if (source.info.stride < width * 4u) return nullptr;
  const uint64_t source_bytes =
      static_cast<uint64_t>(source.info.stride) * height;
  if (source.pixels.size() < source_bytes) return nullptr;
  if (!wic::EnsureComInitialized()) return nullptr;

  const double scale =
      static_cast<double>(edge) / static_cast<double>(longest);
  const uint32_t target_width = ScaleDimension(width, scale);
  const uint32_t target_height = ScaleDimension(height, scale);
  if (target_width == width && target_height == height) return nullptr;

  const double started = QpcMs();
  ComPtr<IWICImagingFactory> factory;
  HRESULT hr = CoCreateInstance(CLSID_WICImagingFactory, nullptr,
                                CLSCTX_INPROC_SERVER,
                                IID_PPV_ARGS(factory.GetAddressOf()));
  if (FAILED(hr)) return nullptr;

  ComPtr<IWICBitmap> bitmap;
  hr = factory->CreateBitmapFromMemory(
      width, height, GUID_WICPixelFormat32bppBGRA, source.info.stride,
      static_cast<UINT>(source_bytes),
      const_cast<BYTE*>(source.pixels.data()), bitmap.GetAddressOf());
  if (FAILED(hr)) return nullptr;

  ComPtr<IWICBitmapScaler> scaler;
  hr = factory->CreateBitmapScaler(scaler.GetAddressOf());
  if (SUCCEEDED(hr)) {
    // Fant is the filter wic_decoder.cpp already uses for max_edge decodes, so
    // a fallback tier and a decoder-side tier look the same (規劃/25 §7.4b).
    hr = scaler->Initialize(bitmap.Get(), target_width, target_height,
                            WICBitmapInterpolationModeFant);
  }
  if (FAILED(hr)) return nullptr;

  auto tier = std::make_shared<Image>();
  tier->info.width = target_width;
  tier->info.height = target_height;
  tier->info.stride = target_width * 4u;
  tier->info.format = PixelFormat::kBgra8;
  tier->info.has_alpha = source.info.has_alpha;
  tier->info.orientation = source.info.orientation;
  tier->info.orientation_applied = source.info.orientation_applied;
  // The real file size stays available for the title/info panel and for the
  // §7.6 upgrade (規劃/25 §7.4b: real size must come from the header, not from
  // the decoded result).
  tier->info.full_width =
      source.info.full_width != 0 ? source.info.full_width : width;
  tier->info.full_height =
      source.info.full_height != 0 ? source.info.full_height : height;
  tier->info.icc_profile = source.info.icc_profile;
  tier->info.icc_applied = source.info.icc_applied;
  tier->info.frame_count = 1;
  tier->info.animated = false;
  tier->info.source = "display-tier";
  tier->pixels.resize(static_cast<size_t>(tier->info.stride) * target_height);

  hr = scaler->CopyPixels(nullptr, tier->info.stride,
                          static_cast<UINT>(tier->pixels.size()),
                          tier->pixels.data());
  if (FAILED(hr)) return nullptr;

  char line[192]{};
  std::snprintf(line, sizeof(line),
                "display tier: %ux%u -> %ux%u edge=%u ms=%.1f", width, height,
                target_width, target_height, edge, QpcMs() - started);
  platform::LogInfo(line);
  return tier;
}

}  // namespace nv
