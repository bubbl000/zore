#pragma once

#include "core/image.h"

#include <cstddef>
#include <list>
#include <memory>
#include <string>
#include <unordered_map>

namespace nv {

// Small UI-thread-only LRU cache for decoded images.
//
// max_entry_bytes keeps one huge frame (an 8K image is 127 MB) out of the cache:
// browsing several 8K images would otherwise hold the whole budget plus the GPU
// texture and its upload staging buffer at the same time. Large frames are
// re-decoded on demand (or read from the disk cache).
class ImageCache {
 public:
  explicit ImageCache(size_t budget_bytes = 512ull * 1024 * 1024,
                      size_t max_entry_bytes = 96ull * 1024 * 1024)
      : budget_bytes_(budget_bytes), max_entry_bytes_(max_entry_bytes) {}

  std::shared_ptr<Image> Get(const std::wstring& key) {
    const auto it = index_.find(key);
    if (it == index_.end()) return nullptr;
    lru_.splice(lru_.begin(), lru_, it->second);
    return it->second->image;
  }

  void Put(const std::wstring& key, std::shared_ptr<Image> image) {
    if (!image || image->Empty() || budget_bytes_ == 0) return;

    const size_t bytes = image->pixels.size();
    if (bytes > budget_bytes_ || bytes > max_entry_bytes_) return;

    const auto existing = index_.find(key);
    if (existing != index_.end()) {
      used_bytes_ -= existing->second->bytes;
      lru_.erase(existing->second);
      index_.erase(existing);
    }

    while (!lru_.empty() && used_bytes_ + bytes > budget_bytes_) {
      auto& back = lru_.back();
      used_bytes_ -= back.bytes;
      index_.erase(back.key);
      lru_.pop_back();
    }

    lru_.push_front(Entry{key, std::move(image), bytes});
    index_[key] = lru_.begin();
    used_bytes_ += bytes;
  }

  void Clear() {
    lru_.clear();
    index_.clear();
    used_bytes_ = 0;
  }

 private:
  struct Entry {
    std::wstring key;
    std::shared_ptr<Image> image;
    size_t bytes = 0;
  };

  size_t budget_bytes_ = 0;
  size_t max_entry_bytes_ = 0;
  size_t used_bytes_ = 0;
  std::list<Entry> lru_;
  std::unordered_map<std::wstring, std::list<Entry>::iterator> index_;
};

}  // namespace nv
