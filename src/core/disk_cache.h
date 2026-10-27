#pragma once

#include "core/image.h"

#include <cstdint>
#include <filesystem>
#include <memory>

namespace nv {

// Cross-session cache for decoded BGRA images. The cache key includes the
// source path and its last-write time, so an edited file is not served stale.
//
// V3-04 (規劃/25 §7.1/§7.5): the file format is version 3 and can hold either
// the raw BGRA frame (edge 0, the historical behaviour) or a *display tier*
// stored as a compressed JPEG/PNG payload. A version-2 file is treated as a
// miss and deleted on first access, so the two semantics never coexist; Trim
// reaps anything that is never touched again.
class DiskCache {
 public:
  explicit DiskCache(std::filesystem::path directory,
                     uint64_t max_bytes = 1024ull * 1024 * 1024);

  // Full-resolution entry (edge 0).
  std::shared_ptr<Image> Load(const std::filesystem::path& source,
                              uint64_t max_payload = 0) const;

  // Display-tier entry (edge > 0). `max_payload` is a corruption guard for the
  // stored bytes, not a decoded-size budget.
  std::shared_ptr<Image> LoadTier(const std::filesystem::path& source,
                                  uint32_t edge,
                                  uint64_t max_payload = 0) const;

  // `trim` may be disabled for bulk writers (thumbnails) that trim on their
  // own schedule, because each trim walks the whole cache directory.
  void Save(const std::filesystem::path& source, const Image& image,
            bool trim = true) const;

  // Re-encodes the tier (JPEG q92 / PNG) and stores that payload. Raw BGRA is
  // only written when the encode fails and the frame still fits the tier
  // budget (規劃/25 §7.5).
  void SaveTier(const std::filesystem::path& source, uint32_t edge,
                const Image& image, bool trim = true) const;

 private:
  std::filesystem::path CacheFile(const std::filesystem::path& source,
                                  uint32_t edge) const;
  std::shared_ptr<Image> LoadInternal(const std::filesystem::path& source,
                                      uint32_t edge, uint64_t max_payload) const;
  void SaveInternal(const std::filesystem::path& source, uint32_t edge,
                    const Image& image, bool trim) const;
  void Trim() const;

  std::filesystem::path directory_;
  uint64_t max_bytes_ = 0;
};

}  // namespace nv
