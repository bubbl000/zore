#include "core/disk_cache.h"

#include "core/cache_key.h"
#include "core/tier_payload.h"
#include "decoders/wic_decoder.h"
#include "platform/log.h"

#include <windows.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <functional>
#include <iomanip>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

namespace nv {
namespace {

// Version 3 (V3-04): the header gained the tier edge, the real source size and
// the payload codec. The magic string carries the version as well, so a manual
// hexdump cannot be mistaken for the current format.
constexpr char kMagic[8] = {'N', 'V', 'I', 'M', 'G', '0', '3', '\0'};
constexpr uint32_t kVersion = 3;
constexpr uint64_t kMaxPayload = 256ull * 1024 * 1024;

struct Header {
  char magic[8];
  uint32_t version;
  uint32_t width;
  uint32_t height;
  uint32_t stride;
  uint32_t format;
  uint32_t has_alpha;
  uint32_t orientation;
  uint32_t full_width;
  uint32_t full_height;
  uint32_t edge;           // 0 = full frame, otherwise the tier long edge.
  uint32_t source_width;   // Real (upright) file size, never the tier size.
  uint32_t source_height;
  uint32_t codec;          // TierCodec: 0 BGRA, 1 JPEG, 2 PNG.
  uint64_t payload_size;
};

std::wstring CacheKeyText(const std::filesystem::path& source, uint32_t edge) {
  // "|v3" isolates the on-disk format: entries written by version 2 hash to a
  // different name and are reaped by Trim instead of being misread.
  return MakeCacheKey(source, edge) + L"|v3";
}

std::wstring HexHash(const std::wstring& text) {
  const size_t value = std::hash<std::wstring>{}(text);
  std::wostringstream out;
  out << std::hex << std::setw(16) << std::setfill(L'0') << value;
  return out.str();
}

// A tier is bounded by the tier budget (edge^2 * 4, the size a raw BGRA tier
// would need); the full frame keeps the historical 256 MB corruption guard.
uint64_t PayloadLimit(uint32_t edge) {
  if (edge == 0) return kMaxPayload;
  const uint64_t budget = static_cast<uint64_t>(edge) * edge * 4ull;
  return std::min(budget, kMaxPayload);
}

bool ValidHeader(const Header& header) {
  return std::memcmp(header.magic, kMagic, sizeof(kMagic)) == 0 &&
         header.version == kVersion && header.width > 0 && header.height > 0 &&
         header.stride >= header.width * 4u && header.payload_size > 0 &&
         header.payload_size <= kMaxPayload &&
         header.codec <= static_cast<uint32_t>(TierCodec::kPng);
}

// A per-process, per-call suffix keeps two instances writing the same key from
// sharing a staging file (規劃/22/15 §6/坑 13). Trim recognises the ".tmp-"
// marker and never deletes a live staging file.
std::wstring TempPath(const std::filesystem::path& final_path) {
  static std::atomic<unsigned> counter{0};
  return final_path.wstring() + L".tmp-" +
         std::to_wstring(GetCurrentProcessId()) + L"-" +
         std::to_wstring(GetTickCount64()) + L"-" +
         std::to_wstring(counter.fetch_add(1));
}

bool WriteAll(HANDLE file, const void* data, size_t size) {
  const auto* bytes = static_cast<const char*>(data);
  size_t offset = 0;
  while (offset < size) {
    const DWORD chunk = static_cast<DWORD>(
        std::min<size_t>(size - offset, 1u << 30));
    DWORD written = 0;
    if (!WriteFile(file, bytes + offset, chunk, &written, nullptr) ||
        written != chunk) {
      return false;
    }
    offset += chunk;
  }
  return true;
}

}  // namespace

DiskCache::DiskCache(std::filesystem::path directory, uint64_t max_bytes)
    : directory_(std::move(directory)), max_bytes_(max_bytes) {}

std::filesystem::path DiskCache::CacheFile(
    const std::filesystem::path& source, uint32_t edge) const {
  return directory_ / (HexHash(CacheKeyText(source, edge)) + L".nvimg");
}

std::shared_ptr<Image> DiskCache::Load(const std::filesystem::path& source,
                                       uint64_t max_payload) const {
  return LoadInternal(source, 0, max_payload);
}

std::shared_ptr<Image> DiskCache::LoadTier(const std::filesystem::path& source,
                                           uint32_t edge,
                                           uint64_t max_payload) const {
  if (edge == 0) return Load(source, max_payload);
  return LoadInternal(source, edge, max_payload);
}

std::shared_ptr<Image> DiskCache::LoadInternal(
    const std::filesystem::path& source, uint32_t edge,
    uint64_t max_payload) const {
  const std::filesystem::path cache_file = CacheFile(source, edge);
  std::ifstream file(cache_file, std::ios::binary);
  if (!file) return nullptr;

  // A final file only appears through MoveFileEx, so an invalid one is a
  // leftover/corrupt entry from an older build or a crashed writer: drop it
  // and report a miss instead of failing every load (規劃/22/15 §6).
  const auto discard = [&cache_file, &file]() -> std::shared_ptr<Image> {
    file.close();
    std::error_code ec;
    std::filesystem::remove(cache_file, ec);
    return nullptr;
  };

  Header header{};
  file.read(reinterpret_cast<char*>(&header), sizeof(header));
  if (!file || !ValidHeader(header)) return discard();
  // The filename already encodes the edge; a mismatch means a hash collision or
  // a hand-edited file. Treat it as a miss rather than serving the wrong tier.
  if (header.edge != edge) return discard();
  // Large frames (8K is 126 MB) are deliberately not served from disk: the
  // 127 MB read stalls the UI thread and adds another full frame on top of the
  // decoded buffer, which breaks the 8K memory budget.
  if (max_payload != 0 && header.payload_size > max_payload) return nullptr;

  std::vector<uint8_t> payload(static_cast<size_t>(header.payload_size));
  file.read(reinterpret_cast<char*>(payload.data()),
            static_cast<std::streamsize>(payload.size()));
  if (!file) return discard();
  file.close();

  auto image = std::make_shared<Image>();
  const TierCodec codec = static_cast<TierCodec>(header.codec);
  if (codec == TierCodec::kBgra) {
    const uint64_t expected =
        static_cast<uint64_t>(header.stride) * header.height;
    if (header.payload_size != expected) return discard();
    image->pixels = std::move(payload);
  } else {
    // Compressed tier: one small decode (~10 ms at 1280 px) instead of reading
    // a 4–16 MB BGRA payload (規劃/25 §7.5).
    DecodeResult decoded = wic::DecodeMemory(payload.data(), payload.size(), {});
    if (!decoded.ok()) return discard();
    if (decoded.image->info.width != header.width ||
        decoded.image->info.height != header.height) {
      return discard();
    }
    image->pixels = std::move(decoded.image->pixels);
  }

  image->info.width = header.width;
  image->info.height = header.height;
  image->info.stride = header.stride;
  image->info.format = static_cast<PixelFormat>(header.format);
  image->info.has_alpha = header.has_alpha != 0;
  image->info.orientation = static_cast<uint16_t>(header.orientation);
  image->info.full_width = header.full_width;
  image->info.full_height = header.full_height;
  image->info.source = "disk-cache";
  if (image->pixels.size() <
      static_cast<size_t>(header.stride) * header.height) {
    return discard();
  }
  return image;
}

void DiskCache::Save(const std::filesystem::path& source, const Image& image,
                     bool trim) const {
  SaveInternal(source, 0, image, trim);
}

void DiskCache::SaveTier(const std::filesystem::path& source, uint32_t edge,
                         const Image& image, bool trim) const {
  SaveInternal(source, edge, image, trim);
}

void DiskCache::SaveInternal(const std::filesystem::path& source, uint32_t edge,
                             const Image& image, bool trim) const {
  if (image.Empty() || image.info.stride < image.info.width * 4u) return;

  const uint64_t raw_bytes =
      static_cast<uint64_t>(image.info.stride) * image.info.height;
  if (raw_bytes == 0 || raw_bytes > kMaxPayload ||
      image.pixels.size() < raw_bytes) {
    return;
  }

  Header header{};
  std::memcpy(header.magic, kMagic, sizeof(kMagic));
  header.version = kVersion;
  header.width = image.info.width;
  header.height = image.info.height;
  header.stride = image.info.stride;
  header.format = static_cast<uint32_t>(image.info.format);
  header.has_alpha = image.info.has_alpha ? 1u : 0u;
  header.orientation = image.info.orientation;
  header.full_width = image.info.full_width != 0 ? image.info.full_width
                                                 : image.info.width;
  header.full_height = image.info.full_height != 0 ? image.info.full_height
                                                   : image.info.height;
  header.edge = edge;
  header.source_width = header.full_width;
  header.source_height = header.full_height;

  std::vector<uint8_t> payload;
  if (edge != 0) {
    TierCodec codec = TierCodec::kBgra;
    if (EncodeTierPayload(image, &codec, &payload)) {
      header.codec = static_cast<uint32_t>(codec);
      header.payload_size = payload.size();
    } else {
      // No encoder (or an unsupported pixel layout): keep the raw frame if the
      // tier budget allows it, otherwise skip the write (規劃/25 §7.5).
      payload.assign(image.pixels.begin(),
                     image.pixels.begin() + static_cast<ptrdiff_t>(raw_bytes));
      header.codec = static_cast<uint32_t>(TierCodec::kBgra);
      header.payload_size = payload.size();
    }
  } else {
    payload.assign(image.pixels.begin(),
                   image.pixels.begin() + static_cast<ptrdiff_t>(raw_bytes));
    header.codec = static_cast<uint32_t>(TierCodec::kBgra);
    header.payload_size = payload.size();
  }

  if (header.payload_size == 0 ||
      header.payload_size > PayloadLimit(edge)) {
    return;
  }

  std::error_code ec;
  std::filesystem::create_directories(directory_, ec);

  const std::filesystem::path final_path = CacheFile(source, edge);
  const std::wstring temp_path = TempPath(final_path);

  HANDLE file = CreateFileW(temp_path.c_str(), GENERIC_WRITE, 0, nullptr,
                            CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
  if (file == INVALID_HANDLE_VALUE) return;
  const bool written =
      WriteAll(file, &header, sizeof(header)) &&
      WriteAll(file, payload.data(), payload.size()) &&
      FlushFileBuffers(file) != FALSE;
  CloseHandle(file);
  if (!written) {
    DeleteFileW(temp_path.c_str());
    return;
  }

  // Atomic publish: readers see either the old complete file or the new one,
  // never a partial payload, even with several instances writing one key.
  if (!MoveFileExW(temp_path.c_str(), final_path.c_str(),
                   MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
    DeleteFileW(temp_path.c_str());
    return;
  }

  if (trim) Trim();
}

void DiskCache::Trim() const {
  if (max_bytes_ == 0) return;

  struct Entry {
    std::filesystem::path path;
    uint64_t bytes = 0;
    std::filesystem::file_time_type stamp{};
  };

  std::error_code ec;
  std::vector<Entry> entries;
  uint64_t total = 0;
  const auto now = std::filesystem::file_time_type::clock::now();
  for (const auto& item : std::filesystem::directory_iterator(
           directory_, std::filesystem::directory_options::skip_permission_denied,
           ec)) {
    if (!item.is_regular_file(ec)) continue;
    const std::wstring name = item.path().filename().wstring();
    const bool staging = name.find(L".tmp-") != std::wstring::npos;
    if (staging) {
      // Another instance may still be writing; only reap abandoned staging
      // files (規劃/22/15 坑 14).
      const auto stamp = item.last_write_time(ec);
      if (!ec && now - stamp > std::chrono::hours(1)) {
        std::filesystem::remove(item.path(), ec);
      }
      continue;
    }
    if (item.path().extension() != L".nvimg") continue;
    const uint64_t bytes = item.file_size(ec);
    if (ec) continue;
    entries.push_back(Entry{item.path(), bytes, item.last_write_time(ec)});
    total += bytes;
  }

  if (total <= max_bytes_) return;

  std::ranges::sort(entries, [](const Entry& a, const Entry& b) {
    return a.stamp < b.stamp;
  });
  for (const Entry& entry : entries) {
    if (total <= max_bytes_) break;
    std::filesystem::remove(entry.path, ec);
    if (!ec) total -= entry.bytes;
  }
}

}  // namespace nv
