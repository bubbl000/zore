#pragma once

#include <cstdint>
#include <filesystem>
#include <string>

namespace nv {

// Cache identity for a source image: absolute path plus its last-write time,
// so an edited file is never served from a stale cache entry.
//
// V3-04 (規劃/25 §7.1): `edge` names a *display tier* of the same source. 0 is
// the full-resolution frame and keeps the historical `path|mtime` format, so
// nothing that already works (thumbnails, full frames) changes meaning. A tier
// appends `|e<edge>`, which keeps tier entries, full-frame entries and the
// thumbnail directory in separate namespaces.
inline std::wstring MakeCacheKey(const std::filesystem::path& path,
                                 uint32_t edge = 0) {
  std::error_code ec;
  const auto stamp = std::filesystem::last_write_time(path, ec);
  std::wstring key = path.wstring();
  if (!ec) {
    key += L"|";
    key += std::to_wstring(stamp.time_since_epoch().count());
  }
  if (edge != 0) {
    key += L"|e";
    key += std::to_wstring(edge);
  }
  return key;
}

// V3.1-07 S5 (規劃/v3.1/07 §3 S5): cache identity for `ImageCache` (the decoded
// frame LRU in image_cache.h) only. It adds the file length, so an in-place edit
// that keeps the timestamp (or is written inside the filesystem's timestamp
// granularity) cannot be served from the stale decoded frame. Memory entries
// never outlive the process, so the stricter format costs nothing on disk.
//
// Do NOT make `MakeCacheKey` do this (坑 336): that function also names the
// `.nvimg` files (disk_cache.cpp) and the thumbnail disk directory
// (thumbnail_service.cpp), so changing it would invalidate both caches in full.
// The disk key therefore stays `path|mtime[|e<edge>]`; only call sites that
// touch `ImageCache` use this one. `ThumbnailService`'s own memory LRU keeps
// `MakeCacheKey` because its index and its disk entries share one key — leave
// that alone unless the two keys are split there.
inline std::wstring MakeMemoryCacheKey(const std::filesystem::path& path,
                                       uint32_t edge = 0) {
  std::error_code ec;
  const auto stamp = std::filesystem::last_write_time(path, ec);
  std::error_code size_ec;
  const auto size = std::filesystem::file_size(path, size_ec);
  std::wstring key = path.wstring();
  // A missing file has neither component; the path alone stays the key (same
  // fallback as MakeCacheKey), and the edge still separates tiers.
  if (!size_ec) {
    key += L"|s";
    key += std::to_wstring(size);
  }
  if (!ec) {
    key += L"|t";
    key += std::to_wstring(stamp.time_since_epoch().count());
  }
  if (edge != 0) {
    key += L"|e";
    key += std::to_wstring(edge);
  }
  return key;
}

}  // namespace nv
