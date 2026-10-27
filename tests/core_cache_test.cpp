// V3-04 (規劃/25 §7.1–§7.5): unit checks for the display-tier chain that do not
// need a window.
//
// Covered: cache-key format (full vs tier, disk vs memory), the tier ladder and
// the upward lookup, the WIC downscale contract of MakeDisplayTier, the
// compressed tier payload (JPEG/PNG round-trip), the disk-cache v3 header
// (round-trip, size advantage, version rejection, truncated payload) and the
// "tier never equals the full-frame entry" key separation.
//
// The GUI-side behaviour (which tier a view asks for, cache-hit logging, the
// prefetch of the same tier) is exercised by tests/tools/run_v3_tier_check.ps1,
// because it depends on the real window geometry.

#include "core/cache_key.h"
#include "core/decode_service.h"
#include "core/disk_cache.h"
#include "core/display_tier.h"
#include "core/image_cache.h"
#include "core/tier_payload.h"
#include "decoders/wic_decoder.h"
#include "render/draw_quality.h"

#include <windows.h>

#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <functional>
#include <memory>
#include <string>
#include <thread>
#include <vector>

namespace {

int g_checks = 0;
int g_failures = 0;

void Check(bool condition, const char* name) {
  ++g_checks;
  std::printf("%s %s\n", condition ? "ok  " : "FAIL", name);
  if (!condition) ++g_failures;
}

nv::Image MakePattern(uint32_t width, uint32_t height, bool alpha,
                      uint32_t full_width = 0, uint32_t full_height = 0) {
  nv::Image image;
  image.info.width = width;
  image.info.height = height;
  image.info.stride = width * 4u;
  image.info.format = nv::PixelFormat::kBgra8;
  image.info.has_alpha = alpha;
  image.info.full_width = full_width != 0 ? full_width : width;
  image.info.full_height = full_height != 0 ? full_height : height;
  image.pixels.resize(static_cast<size_t>(width) * height * 4u);
  for (uint32_t y = 0; y < height; ++y) {
    for (uint32_t x = 0; x < width; ++x) {
      uint8_t* p = image.pixels.data() +
                   static_cast<size_t>(y) * image.info.stride + x * 4u;
      p[0] = static_cast<uint8_t>((x * 3 + y) & 0xFF);
      p[1] = static_cast<uint8_t>((x + y * 2) & 0xFF);
      p[2] = static_cast<uint8_t>((x * 7 + y * 5) & 0xFF);
      p[3] = alpha ? static_cast<uint8_t>(200 + (x + y) % 56) : 255;
    }
  }
  return image;
}

uint64_t CountCacheFiles(const std::filesystem::path& directory) {
  std::error_code ec;
  uint64_t count = 0;
  for (const auto& item : std::filesystem::directory_iterator(
           directory, std::filesystem::directory_options::skip_permission_denied,
           ec)) {
    if (item.path().extension() == L".nvimg") ++count;
  }
  return count;
}

std::filesystem::path SingleCacheFile(const std::filesystem::path& directory) {
  std::error_code ec;
  for (const auto& item : std::filesystem::directory_iterator(
           directory, std::filesystem::directory_options::skip_permission_denied,
           ec)) {
    if (item.path().extension() == L".nvimg") return item.path();
  }
  return {};
}

void TestCacheKeys(const std::filesystem::path& scratch) {
  const std::filesystem::path file = scratch / L"key.jpg";
  {
    std::ofstream out(file, std::ios::binary);
    out << "x";
  }
  const std::wstring full = nv::MakeCacheKey(file);
  Check(full == nv::MakeCacheKey(file, 0), "edge 0 keeps the historical key");
  Check(full.find(L"|e") == std::wstring::npos, "full-frame key has no tier");
  const std::wstring tier = nv::MakeCacheKey(file, 1280);
  Check(tier != full, "a tier key differs from the full-frame key");
  Check(tier.ends_with(L"|e1280"), "tier key ends with |e<edge>");
  Check(nv::MakeCacheKey(file, 2048) != nv::MakeCacheKey(file, 1280),
        "different tiers never share a key");
  // A missing file has no mtime: the path alone is the key, so two tiers must
  // still be distinct (otherwise a tier would collide with the full frame).
  const std::filesystem::path missing = scratch / L"missing.jpg";
  Check(nv::MakeCacheKey(missing) == missing.wstring(),
        "missing file falls back to the path");
  Check(nv::MakeCacheKey(missing, 512) != nv::MakeCacheKey(missing),
        "missing file still separates tiers");
}

// V3.1-07 S5 (規劃/v3.1/07 §3 S5): the in-memory LRU key also carries the file
// length, while the disk key must not change (坑 336: MakeCacheKey names both
// the `.nvimg` entries and the thumbnail directory).
void TestMemoryCacheKeys(const std::filesystem::path& scratch) {
  const std::filesystem::path file = scratch / L"memkey.jpg";
  {
    std::ofstream out(file, std::ios::binary);
    out << "x";
  }
  const std::wstring disk = nv::MakeCacheKey(file);
  const std::wstring memory = nv::MakeMemoryCacheKey(file);
  Check(memory != disk, "memory key differs from the disk key");
  Check(memory.find(L"|s1") != std::wstring::npos,
        "memory key carries the file size");
  Check(memory == nv::MakeMemoryCacheKey(file, 0),
        "memory edge 0 is the full-frame key");
  const std::wstring tier = nv::MakeMemoryCacheKey(file, 1280);
  Check(tier != memory, "memory tier key differs from the full-frame key");
  Check(tier.ends_with(L"|e1280"), "memory tier key ends with |e<edge>");
  Check(nv::MakeCacheKey(file, 1280).find(L"|s") == std::wstring::npos,
        "the disk key still has no size component (坑 336)");

  // S5 acceptance: a length-only edit (timestamp restored by hand) must change
  // the memory key — that is the point of the size component.
  std::error_code ec;
  const auto stamp = std::filesystem::last_write_time(file, ec);
  Check(!ec, "memory key test read the timestamp");
  {
    std::ofstream out(file, std::ios::binary | std::ios::trunc);
    out << "xx";
  }
  std::filesystem::last_write_time(file, stamp, ec);
  Check(!ec, "memory key test restored the timestamp");
  const std::wstring resized = nv::MakeMemoryCacheKey(file);
  Check(resized != memory, "a new file size alone changes the memory key");
  Check(nv::MakeCacheKey(file) == disk,
        "the same length-only edit leaves the disk key unchanged");

  // The timestamp is still part of the identity.
  std::filesystem::last_write_time(file, stamp + std::chrono::seconds(2), ec);
  Check(!ec, "memory key test bumped the timestamp");
  Check(nv::MakeMemoryCacheKey(file) != resized,
        "a timestamp change alone changes the memory key");

  // A missing file falls back to the path exactly like MakeCacheKey.
  const std::filesystem::path missing = scratch / L"missing-mem.jpg";
  Check(nv::MakeMemoryCacheKey(missing) == missing.wstring(),
        "missing file falls back to the path for the memory key");
  Check(nv::MakeMemoryCacheKey(missing, 512) != nv::MakeMemoryCacheKey(missing),
        "missing file still separates memory tiers");
}

// The LRU behaviour the key change exists for: an entry stored under the old
// identity must not answer a lookup after the file changed length.
void TestMemoryCacheInvalidatesOnSize(const std::filesystem::path& scratch) {
  const std::filesystem::path file = scratch / L"memcache.jpg";
  {
    std::ofstream out(file, std::ios::binary);
    out << "x";
  }
  nv::ImageCache cache;
  cache.Put(nv::MakeMemoryCacheKey(file),
            std::make_shared<nv::Image>(MakePattern(2, 2, false)));
  Check(cache.Get(nv::MakeMemoryCacheKey(file)) != nullptr,
        "memory cache hits while the file is unchanged");

  std::error_code ec;
  const auto stamp = std::filesystem::last_write_time(file, ec);
  {
    std::ofstream out(file, std::ios::binary | std::ios::trunc);
    out << "xx";
  }
  std::filesystem::last_write_time(file, stamp, ec);
  Check(cache.Get(nv::MakeMemoryCacheKey(file)) == nullptr,
        "a new file size invalidates the memory cache entry");
}

void TestTierLadder() {
  Check(nv::ChooseTier(0) == 0, "no needed edge means no tier");
  Check(nv::ChooseTier(1) == 512, "tiny need clamps to the smallest tier");
  Check(nv::ChooseTier(512) == 512, "exact tier is kept");
  Check(nv::ChooseTier(513) == 768, "one pixel over a tier moves up");
  Check(nv::ChooseTier(1280) == 1280, "1280 is a tier");
  Check(nv::ChooseTier(1281) == 1600, "1281 moves to 1600");
  Check(nv::ChooseTier(3000) == 3200, "3000 moves to 3200");
  Check(nv::ChooseTier(6000) == 4096, "above the ladder clamps to 4096");
  Check(nv::ChooseTier(1000000) == nv::kDisplayTierMax, "clamp is the max tier");

  Check(nv::NextTier(0) == 0, "no-tier has no successor");
  Check(nv::NextTier(512) == 768, "upward lookup walks the ladder");
  Check(nv::NextTier(1280) == 1600, "upward lookup from 1280");
  Check(nv::NextTier(4000) == 4096, "upward lookup reaches the max");
  Check(nv::NextTier(nv::kDisplayTierMax) == 0, "max tier ends the walk");

  // The §7.3 upward walk must terminate and cover every tier >= T.
  int walked = 0;
  for (uint32_t edge = 1280; edge != 0; edge = nv::NextTier(edge)) ++walked;
  Check(walked == 6, "walk from 1280 visits 1280..4096 (6 tiers)");
}

// §7.6/§7.3 load policy (V3-04b). These are the rules that used to live inline
// in app/main.cpp; getting one wrong is invisible until the user zooms in (a
// tier instead of real pixels) or navigates back (a blur after the full frame).
void TestTierLoadPolicy() {
  // Fit view of a 24 MP photo: the content needs 1280 px, so a tier is allowed.
  nv::TierLoadInputs fit;
  fit.needed_long_edge = 1280;
  fit.scale = 0.21f;
  fit.source_long_edge = 6000;
  Check(nv::DecideTierEdge(fit) == 1280, "a fit view of a big file uses a tier");

  // A loaded file always asks for the same ladder entry as the fit edge.
  fit.needed_long_edge = 1300;
  Check(nv::DecideTierEdge(fit) == 1600, "the tuple is rounded up to the ladder");

  // §7.6: 100 % needs real pixels.
  nv::TierLoadInputs actual = fit;
  actual.needed_long_edge = 1280;
  actual.scale = 1.0f;
  Check(nv::DecideTierEdge(actual) == 0, "a 100 % view never loads a tier");
  actual.scale = 0.999f;
  Check(nv::DecideTierEdge(actual) == 1280,
        "just below 100 % still uses a tier");

  // §7.3 rule 3: no falling back once the full frame existed.
  nv::TierLoadInputs seen = fit;
  seen.full_frame_available = true;
  Check(nv::DecideTierEdge(seen) == 0,
        "a file whose full frame was already decoded stays full");

  // Nothing to gain when the file is not larger than the tier.
  nv::TierLoadInputs small;
  small.needed_long_edge = 400;
  small.scale = 0.5f;
  small.source_long_edge = 512;
  Check(nv::DecideTierEdge(small) == 0, "a file at the tier size is decoded once");

  // §7.8 --display-cache=force: a small sample can still prove the tier chain,
  // but a decoded full frame still wins (rule §7.3-3 is not a strategy flag).
  nv::TierLoadInputs forced = small;
  forced.force = true;
  Check(nv::DecideTierEdge(forced) == 512, "force ignores the size rule");
  nv::TierLoadInputs forced_actual = actual;
  forced_actual.force = true;
  Check(nv::DecideTierEdge(forced_actual) == 1280,
        "force ignores the 100 % rule");
  nv::TierLoadInputs forced_seen = seen;
  forced_seen.force = true;
  Check(nv::DecideTierEdge(forced_seen) == 0,
        "force still never blurs a file whose full frame was decoded");

  // Degenerate inputs (no content rectangle / no probe) must not pick a tier.
  nv::TierLoadInputs empty;
  Check(nv::DecideTierEdge(empty) == 0, "no content rectangle means no tier");
  empty.needed_long_edge = 900;
  empty.scale = 0.1f;
  empty.source_long_edge = 0;
  Check(nv::DecideTierEdge(empty) == 0,
        "an unknown source size is not treated as 'bigger than the tier'");
}

// V3.1-08: when the view upgrades from the display tier to the full frame.
// The three branches (upscale ratio / 100 % / pixel mode) plus the two vetoes
// (RAW below 100 %, huge file) are the whole policy, so they are pinned here.
void TestTierUpgradePolicy() {
  nv::TierUpgradeInputs jpeg;
  jpeg.have_long_edge = 1280;   // fit tier of a 6000 px file
  jpeg.file_long_edge = 6000;
  jpeg.file_bytes = 22ull * 1024ull * 1024ull;

  jpeg.file_scale = 0.21f;  // fit: need 1260 <= 1472
  Check(!nv::ShouldUpgradeToFullFrame(jpeg), "fit never upgrades");
  jpeg.file_scale = 0.245f;  // need 1470, still inside the 1.15x slack
  Check(!nv::ShouldUpgradeToFullFrame(jpeg), "1.15x slack absorbs a small zoom");
  jpeg.file_scale = 0.246f;  // need 1476 > 1472
  Check(nv::ShouldUpgradeToFullFrame(jpeg),
        "a non-RAW view upgrades once the tier would be upscaled past 1.15x");
  jpeg.file_scale = 1.0f;
  Check(nv::ShouldUpgradeToFullFrame(jpeg), "100 % always upgrades");

  // RAW keeps the 100 % rule: its preview decode is ~80 ms, a full one 1.1-1.5 s.
  nv::TierUpgradeInputs raw;
  raw.have_long_edge = 1920;  // camera-embedded preview
  raw.file_long_edge = 5999;
  raw.file_bytes = 23ull * 1024ull * 1024ull;
  raw.raw = true;
  raw.file_scale = 0.5f;  // need 3000 > 2400, but RAW stays on the preview
  Check(!nv::ShouldUpgradeToFullFrame(raw), "RAW does not upgrade below 100 %");
  raw.file_scale = 0.99f;
  Check(!nv::ShouldUpgradeToFullFrame(raw), "RAW waits for 100 %");
  raw.file_scale = 1.0f;
  Check(nv::ShouldUpgradeToFullFrame(raw), "RAW upgrades at 100 %");

  // Pixel mode promises source pixels, for every format.
  nv::TierUpgradeInputs pixel = jpeg;
  pixel.file_scale = 0.3f;
  pixel.pixel_mode = true;
  Check(nv::ShouldUpgradeToFullFrame(pixel), "pixel mode always needs the file");

  // A huge file is never pulled in just for an upscale (the replica's gate).
  nv::TierUpgradeInputs huge = jpeg;
  huge.file_bytes = nv::kFullDecodeFileLimitBytes + 1;
  huge.file_scale = 0.5f;
  Check(!nv::ShouldUpgradeToFullFrame(huge), "an oversized file is not upgraded");
  huge.file_scale = 1.0f;
  Check(nv::ShouldUpgradeToFullFrame(huge),
        "100 % still upgrades an oversized file (the user asked for real pixels)");
  huge.file_bytes = 0;  // unknown size keeps the gate open
  huge.file_scale = 0.5f;
  Check(nv::ShouldUpgradeToFullFrame(huge),
        "an unknown file size does not block the upgrade");

  // Nothing to gain when the bitmap already is the file.
  nv::TierUpgradeInputs full = jpeg;
  full.have_long_edge = 6000;
  full.file_scale = 1.0f;
  Check(!nv::ShouldUpgradeToFullFrame(full),
        "the full frame is never 'upgraded' again");
  nv::TierUpgradeInputs empty;
  Check(!nv::ShouldUpgradeToFullFrame(empty), "degenerate inputs never upgrade");
}

// §7.7: the 1/4 proxy is only ever baked from the full frame.
void TestProxyPolicy() {
  nv::render::ProxyInputs in;
  in.image_width = 8000;
  in.image_height = 6000;
  in.content_width = 1200;
  in.content_height = 800;
  in.scale = 0.15f;
  Check(nv::render::ShouldUseProxy(in), "a full frame at 15 % gets a proxy");

  in.source_is_tier = true;
  Check(!nv::render::ShouldUseProxy(in),
        "a display-tier source never gets a proxy (§7.7)");

  nv::render::ProxyInputs tier_like = in;
  tier_like.source_is_tier = false;
  tier_like.image_width = 1280;
  tier_like.image_height = 720;
  Check(!nv::render::ShouldUseProxy(tier_like),
        "a tier-sized frame is not 4x the content area");

  in.source_is_tier = false;
  in.enabled = false;
  Check(!nv::render::ShouldUseProxy(in), "--proxy off still disables the proxy");
  in.enabled = true;
  in.pixel_mode = true;
  Check(!nv::render::ShouldUseProxy(in), "pixel mode never uses the proxy");
  in.pixel_mode = false;
  in.scale = 0.5f;
  Check(!nv::render::ShouldUseProxy(in), "50 % is the proxy threshold");
}

void TestMakeDisplayTier() {
  const nv::Image source = MakePattern(1000, 500, false, 4000, 2000);
  Check(nv::ImageLongEdge(source) == 1000, "long edge is the width");

  auto tier = nv::MakeDisplayTier(source, 512);
  Check(tier != nullptr, "a smaller tier is produced");
  if (tier) {
    Check(nv::ImageLongEdge(*tier) == 512, "tier long edge equals the request");
    Check(tier->info.width == 512 && tier->info.height == 256,
          "tier keeps the aspect ratio");
    Check(tier->info.format == nv::PixelFormat::kBgra8, "tier stays BGRA8");
    Check(tier->pixels.size() ==
              static_cast<size_t>(tier->info.stride) * tier->info.height,
          "tier payload matches its stride");
    Check(tier->info.full_width == 4000 && tier->info.full_height == 2000,
          "tier keeps the real file size");
    Check(!tier->info.animated && tier->frames.empty(),
          "a tier is a still frame");
  }

  Check(nv::MakeDisplayTier(source, 0) == nullptr, "edge 0 is not a tier");
  Check(nv::MakeDisplayTier(source, 1000) == nullptr,
        "a tier that would not shrink is refused");
  Check(nv::MakeDisplayTier(source, 4096) == nullptr,
        "a tier larger than the source is refused");
  nv::Image rgba16 = source;
  rgba16.info.format = nv::PixelFormat::kRgba16;
  Check(nv::MakeDisplayTier(rgba16, 512) == nullptr,
        "non-BGRA8 frames are left to the decoder");
}

void TestTierPayload() {
  nv::Image opaque = MakePattern(640, 480, false);
  nv::TierCodec codec = nv::TierCodec::kBgra;
  std::vector<uint8_t> payload;
  Check(nv::EncodeTierPayload(opaque, &codec, &payload),
        "opaque tier encodes");
  Check(codec == nv::TierCodec::kJpeg, "opaque tier uses JPEG");
  Check(!payload.empty() && payload.size() < opaque.pixels.size(),
        "JPEG payload is smaller than BGRA");

  const nv::DecodeResult decoded =
      nv::wic::DecodeMemory(payload.data(), payload.size(), {});
  Check(decoded.ok(), "JPEG payload decodes back");
  if (decoded.ok()) {
    Check(decoded.image->info.width == 640 && decoded.image->info.height == 480,
          "JPEG payload keeps the tier size");
    Check(decoded.image->pixels.size() == opaque.pixels.size(),
          "JPEG payload decodes to a full BGRA frame");
  }

  nv::Image alpha = MakePattern(320, 200, true);
  nv::TierCodec alpha_codec = nv::TierCodec::kBgra;
  std::vector<uint8_t> alpha_payload;
  Check(nv::EncodeTierPayload(alpha, &alpha_codec, &alpha_payload),
        "alpha tier encodes");
  Check(alpha_codec == nv::TierCodec::kPng, "alpha tier uses PNG");
  const nv::DecodeResult alpha_decoded =
      nv::wic::DecodeMemory(alpha_payload.data(), alpha_payload.size(), {});
  Check(alpha_decoded.ok() && alpha_decoded.image->info.width == 320,
        "PNG payload decodes back");
}

void TestDiskCache(const std::filesystem::path& scratch) {
  const std::filesystem::path full_directory = scratch / L"disk-full";
  const std::filesystem::path tier_directory = scratch / L"disk-tier";
  nv::DiskCache full_cache(full_directory, 64ull * 1024 * 1024);
  nv::DiskCache tier_cache(tier_directory, 64ull * 1024 * 1024);
  const std::filesystem::path source = scratch / L"image.jpg";
  {
    std::ofstream out(source, std::ios::binary);
    out << "source-bytes";
  }

  // Full-resolution entry: unchanged raw-BGRA semantics.
  const nv::Image full = MakePattern(64, 32, false);
  full_cache.Save(source, full);
  auto loaded = full_cache.Load(source);
  Check(loaded != nullptr, "full-frame entry round-trips");
  if (loaded) {
    Check(loaded->info.width == 64 && loaded->info.height == 32,
          "full-frame size is preserved");
    Check(loaded->pixels == full.pixels, "full-frame pixels are identical");
    Check(loaded->info.source == "disk-cache", "full-frame source tag");
  }
  Check(full_cache.LoadTier(source, 1280) == nullptr,
        "a full-frame entry is not served as a tier");

  // Tier entry: stored compressed, decoded back at the tier size.
  const nv::Image tier_source = MakePattern(1280, 720, false, 6000, 4000);
  tier_cache.SaveTier(source, 1280, tier_source);
  auto tier = tier_cache.LoadTier(source, 1280);
  Check(tier != nullptr, "tier entry round-trips");
  if (tier) {
    Check(tier->info.width == 1280 && tier->info.height == 720,
          "tier size is preserved");
    Check(tier->info.full_width == 6000 && tier->info.full_height == 4000,
          "tier keeps the real source size");
    Check(tier->pixels.size() ==
              static_cast<size_t>(tier->info.stride) * tier->info.height,
          "decoded tier payload matches its stride");
  }
  Check(tier_cache.Load(source) == nullptr,
        "a tier entry is not served as the full frame");
  Check(tier_cache.LoadTier(source, 4096) == nullptr,
        "another tier is not served from the 1280 entry");
  Check(CountCacheFiles(tier_directory) == 1, "a tier owns exactly one file");

  // The stored tier must be much smaller than raw BGRA (3.6 MB for 1280x720).
  const std::filesystem::path tier_file = SingleCacheFile(tier_directory);
  const uint64_t stored_bytes =
      tier_file.empty() ? 0 : std::filesystem::file_size(tier_file);
  const uint64_t raw_tier_bytes =
      static_cast<uint64_t>(tier_source.info.stride) * tier_source.info.height;
  Check(stored_bytes > 0 && stored_bytes < raw_tier_bytes / 4,
        "a tier is stored re-encoded, not as BGRA");

  // Version rejection: any other header version is a miss and is deleted
  // (規劃/25 §7.1: old entries are invalidated, not migrated).
  if (!tier_file.empty()) {
    std::fstream file(tier_file,
                      std::ios::binary | std::ios::in | std::ios::out);
    file.seekp(0);
    file.write("NVIMG02", 7);
    file.close();
  }
  Check(tier_cache.LoadTier(source, 1280) == nullptr,
        "a different header version is a miss");
  Check(CountCacheFiles(tier_directory) == 0,
        "the rejected file is deleted instead of retried");

  // Truncated payload: also a miss, also deleted.
  const std::filesystem::path full_file = SingleCacheFile(full_directory);
  Check(!full_file.empty(), "the full-frame entry is still on disk");
  if (!full_file.empty()) {
    std::error_code ec;
    std::filesystem::resize_file(full_file, 20, ec);
    Check(!ec, "the entry can be truncated for the test");
    Check(full_cache.Load(source) == nullptr, "a truncated entry is a miss");
    Check(CountCacheFiles(full_directory) == 0,
          "the truncated entry is deleted");
  }
}

// --- V3.1-01: prefetch gate and same-key merge ------------------------------
//
// The gate itself is pure (nv::CanStartPrefetchNow), so every rule is checked
// exactly. The service wiring (burst flag → worker start, merge → one decode)
// is checked against a real DecodeService with generated BMPs; the completion
// count is the evidence, not a timing assumption.

void TestPrefetchGate() {
  nv::PrefetchGate gate;
  Check(!nv::CanStartPrefetchNow(gate), "prefetch: an empty queue never starts");

  // Outside a burst a pending current request still closes the channel
  // (V3-04 behaviour, unchanged).
  gate.queue_empty = false;
  gate.queue_tier = true;
  gate.current_pending = true;
  Check(!nv::CanStartPrefetchNow(gate),
        "prefetch: no burst, current pending -> tier prefetch stays gated");
  gate.queue_tier = false;
  Check(!nv::CanStartPrefetchNow(gate),
        "prefetch: no burst, current pending -> full prefetch stays gated");

  // In a burst exactly one tier prefetch may run beside the current.
  gate.queue_tier = true;
  gate.burst = true;
  Check(nv::CanStartPrefetchNow(gate),
        "prefetch: burst allows one tier prefetch beside the current");
  gate.in_flight_tier = 1;
  Check(!nv::CanStartPrefetchNow(gate),
        "prefetch: burst rejects the second tier prefetch");
  gate.in_flight_tier = 0;
  gate.queue_tier = false;
  Check(!nv::CanStartPrefetchNow(gate),
        "prefetch: burst does not open the full-frame channel");

  // After the burst: the V3-04 caps are back (2 tiers, 1 full frame).
  gate.burst = false;
  gate.current_pending = false;
  gate.queue_tier = true;
  gate.in_flight_tier = 1;
  Check(nv::CanStartPrefetchNow(gate),
        "prefetch: settled, a second tier prefetch may start");
  gate.in_flight_tier = 2;
  Check(!nv::CanStartPrefetchNow(gate),
        "prefetch: settled, a third tier prefetch is still capped");
  gate.queue_tier = false;
  gate.in_flight_full = 1;
  Check(!nv::CanStartPrefetchNow(gate),
        "prefetch: settled, a second full-frame prefetch is capped");
}

// Writes a plain 24-bit BMP (BI_RGB, bottom-up). Used as a decode source that
// needs no sample folder; the pixel content is irrelevant to these checks.
void WriteBmp(const std::filesystem::path& path, uint32_t width,
              uint32_t height) {
  const uint32_t row_bytes = (width * 3u + 3u) & ~3u;
  const uint32_t pixel_bytes = row_bytes * height;
  BITMAPFILEHEADER file{};
  BITMAPINFOHEADER info{};
  file.bfType = 0x4D42;
  file.bfOffBits = sizeof(file) + sizeof(info);
  file.bfSize = file.bfOffBits + pixel_bytes;
  info.biSize = sizeof(info);
  info.biWidth = static_cast<LONG>(width);
  info.biHeight = static_cast<LONG>(height);
  info.biPlanes = 1;
  info.biBitCount = 24;
  info.biCompression = BI_RGB;
  info.biSizeImage = pixel_bytes;
  std::ofstream out(path, std::ios::binary);
  out.write(reinterpret_cast<const char*>(&file), sizeof(file));
  out.write(reinterpret_cast<const char*>(&info), sizeof(info));
  std::vector<char> row(row_bytes, 0);
  for (uint32_t y = 0; y < height; ++y) {
    out.write(row.data(), static_cast<std::streamsize>(row_bytes));
  }
}

constexpr UINT kDecodeDoneV31 = WM_APP + 0x41;

double NowSeconds() {
  return std::chrono::duration<double>(
             std::chrono::steady_clock::now().time_since_epoch())
      .count();
}

// Pumps the completion message until `predicate` is true or the deadline
// expires; returns the number of completions consumed.
int PumpUntil(HWND window, double timeout_s,
              const std::function<bool()>& done,
              std::vector<std::unique_ptr<nv::DecodeCompletion>>* received) {
  const double deadline = NowSeconds() + timeout_s;
  int count = 0;
  while (NowSeconds() < deadline) {
    MSG msg{};
    while (PeekMessageW(&msg, window, 0, 0, PM_REMOVE)) {
      if (msg.message == kDecodeDoneV31) {
        received->emplace_back(
            reinterpret_cast<nv::DecodeCompletion*>(msg.lParam));
        ++count;
      }
    }
    if (done()) break;
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }
  return count;
}

void TestDecodeServiceGateAndMerge(const std::filesystem::path& scratch) {
  const HWND window =
      CreateWindowExW(0, L"STATIC", L"", 0, 0, 0, 0, 0, HWND_MESSAGE, nullptr,
                      nullptr, nullptr);
  if (window == nullptr) {
    Check(false, "decode service: message-only window created");
    return;
  }

  const std::filesystem::path current_file = scratch / L"v31-current.bmp";
  const std::filesystem::path neighbour_a = scratch / L"v31-a.bmp";
  const std::filesystem::path neighbour_b = scratch / L"v31-b.bmp";
  // 24 MP: a BMP decode at 1280 is long enough (tens of ms) that "the current
  // is still running" is a fact rather than a race, and the neighbours are
  // mid-sized so a second prefetch overlaps the first.
  WriteBmp(current_file, 6000, 4000);
  WriteBmp(neighbour_a, 3000, 2000);
  WriteBmp(neighbour_b, 3000, 2000);

  {
    nv::DecodeService service(window, kDecodeDoneV31, 2);
    std::vector<std::unique_ptr<nv::DecodeCompletion>> received;
    auto drain = [&]() {
      PumpUntil(window, 0.02, [] { return false; }, &received);
    };

    // --- Gate: a queued tier prefetch waits for the burst flag -------------
    service.Request(current_file, 1280);
    service.Prefetch({neighbour_a, neighbour_b}, 1280);
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
    drain();
    Check(service.PrefetchInFlight() == 0,
          "decode: outside a burst the pending current blocks the prefetch");
    service.SetBurstMode(true);
    PumpUntil(window, 1.0, [&] { return service.PrefetchInFlight() >= 1; },
              &received);
    Check(service.PrefetchInFlight() == 1,
          "decode: burst starts exactly one tier prefetch");
    Check(service.PrefetchInFlight() == 1 && received.empty(),
          "decode: burst keeps the second tier prefetch gated while the current runs");
    service.SetBurstMode(false);
    PumpUntil(window, 10.0, [&] { return received.size() >= 3; }, &received);
    Check(received.size() >= 3,
          "decode: after the burst the queued neighbours still decode");
    received.clear();

    // --- Merge: a Request over a running prefetch of the same key ----------
    service.Prefetch({neighbour_a}, 1280);
    PumpUntil(window, 2.0, [&] { return service.PrefetchInFlight() == 1; },
              &received);
    Check(service.PrefetchInFlight() == 1,
          "decode: the prefetch is running before the merge request");
    service.Request(neighbour_a, 1280);
    PumpUntil(window, 10.0,
              [&] { return service.PrefetchInFlight() == 0 && !received.empty(); },
              &received);
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    drain();
    Check(received.size() == 1,
          "decode: a merged request produces one completion, not two");
    Check(received.size() == 1 && !received.front()->prefetch,
          "decode: the merged completion is the current request");
    Check(received.size() == 1 && received.front()->max_edge == 1280,
          "decode: the merged completion keeps the requested tier");
  }
  DestroyWindow(window);
}

}  // namespace

int main() {
  // WIC needs COM on this thread; the app does the same on its UI thread.
  if (!nv::wic::EnsureComInitialized()) {
    std::printf("FAIL COM/WIC initialization\n");
    return 1;
  }

  std::error_code ec;
  const std::filesystem::path scratch =
      std::filesystem::temp_directory_path(ec) /
      (L"nv-v3-04-coretests-" + std::to_wstring(GetCurrentProcessId()));
  std::filesystem::remove_all(scratch, ec);
  std::filesystem::create_directories(scratch, ec);
  if (ec) {
    std::printf("FAIL cannot create the scratch directory\n");
    return 1;
  }

  TestCacheKeys(scratch);
  TestMemoryCacheKeys(scratch);
  TestMemoryCacheInvalidatesOnSize(scratch);
  TestTierLadder();
  TestTierLoadPolicy();
  TestTierUpgradePolicy();
  TestProxyPolicy();
  TestMakeDisplayTier();
  TestTierPayload();
  TestDiskCache(scratch);
  TestPrefetchGate();
  TestDecodeServiceGateAndMerge(scratch);

  std::filesystem::remove_all(scratch, ec);
  if (g_failures != 0) {
    std::printf("%d check(s) failed (%d run)\n", g_failures, g_checks);
    return 1;
  }
  std::printf("OK: %d checks passed\n", g_checks);
  return 0;
}
