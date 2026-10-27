#include "core/icc_profile.h"

#include <algorithm>
#include <cctype>
#include <cstring>

namespace nv {
namespace {

constexpr size_t kHeaderSize = 128;
constexpr size_t kTagTableSize = 4;
constexpr uint32_t kMaxTags = 512;
constexpr size_t kMaxDescription = 256;

uint32_t ReadU32(const uint8_t* data, size_t offset) {
  return (static_cast<uint32_t>(data[offset]) << 24) |
         (static_cast<uint32_t>(data[offset + 1]) << 16) |
         (static_cast<uint32_t>(data[offset + 2]) << 8) |
         static_cast<uint32_t>(data[offset + 3]);
}

uint16_t ReadU16(const uint8_t* data, size_t offset) {
  return static_cast<uint16_t>((static_cast<uint16_t>(data[offset]) << 8) |
                               static_cast<uint16_t>(data[offset + 1]));
}

bool HasRoom(size_t size, size_t offset, size_t length) {
  return offset <= size && length <= size - offset;
}

bool SignatureIs(const uint8_t* data, size_t offset, const char* signature) {
  return std::memcmp(data + offset, signature, 4) == 0;
}

// ICC text may be 7-bit ASCII (v2 'desc'/'text') or UTF-16BE ('mluc' in v4).
// Only printable ASCII is kept; the result is used for matching and logging.
std::string AsciiText(const uint8_t* data, size_t length) {
  std::string text;
  text.reserve(std::min(length, kMaxDescription));
  for (size_t i = 0; i < length && text.size() < kMaxDescription; ++i) {
    const uint8_t ch = data[i];
    if (ch == 0) break;
    if (ch >= 0x20 && ch < 0x7F) {
      text.push_back(static_cast<char>(ch));
    } else {
      text.push_back(' ');
    }
  }
  while (!text.empty() && text.back() == ' ') text.pop_back();
  return text;
}

std::string Utf16BeText(const uint8_t* data, size_t length) {
  std::string text;
  text.reserve(std::min(length / 2, kMaxDescription));
  for (size_t i = 0; i + 1 < length && text.size() < kMaxDescription; i += 2) {
    const uint16_t ch = ReadU16(data, i);
    if (ch == 0) break;
    if (ch >= 0x20 && ch < 0x7F) {
      text.push_back(static_cast<char>(ch));
    } else if (ch < 0x20 || ch == 0x7F) {
      text.push_back(' ');
    } else {
      // Non-ASCII glyphs are not needed for sRGB matching.
      text.push_back('?');
    }
  }
  while (!text.empty() && text.back() == ' ') text.pop_back();
  return text;
}

std::string ReadTagText(const uint8_t* data, size_t size, size_t tag_offset,
                        size_t tag_length) {
  if (!HasRoom(size, tag_offset, tag_length) || tag_length < 12) return {};
  if (SignatureIs(data, tag_offset, "desc")) {
    const uint32_t count = ReadU32(data, tag_offset + 8);
    const size_t available = std::min<size_t>(tag_length - 12, count);
    return AsciiText(data + tag_offset + 12, available);
  }
  if (SignatureIs(data, tag_offset, "text")) {
    return AsciiText(data + tag_offset + 8, tag_length - 8);
  }
  if (SignatureIs(data, tag_offset, "mluc")) {
    const uint32_t records = ReadU32(data, tag_offset + 8);
    const uint32_t record_size = ReadU32(data, tag_offset + 12);
    if (records == 0 || record_size < 12 || !HasRoom(size, tag_offset + 16,
                                                     record_size)) {
      return {};
    }
    const size_t record = tag_offset + 16;
    const uint32_t string_length = ReadU32(data, record + 4);
    const uint32_t string_offset = ReadU32(data, record + 8);
    if (!HasRoom(size, tag_offset, string_offset) ||
        !HasRoom(size, tag_offset + string_offset, string_length)) {
      return {};
    }
    return Utf16BeText(data + tag_offset + string_offset, string_length);
  }
  return {};
}

struct TagEntry {
  bool found = false;
  size_t offset = 0;
  size_t length = 0;
};

TagEntry FindTag(const uint8_t* data, size_t size, const char* signature) {
  TagEntry entry;
  if (!HasRoom(size, kHeaderSize, kTagTableSize)) return entry;

  const uint32_t count = ReadU32(data, kHeaderSize);
  if (count == 0 || count > kMaxTags) return entry;
  const size_t table_end = kHeaderSize + kTagTableSize +
                           static_cast<size_t>(count) * 12;
  if (table_end > size) return entry;

  for (uint32_t i = 0; i < count; ++i) {
    const size_t record = kHeaderSize + kTagTableSize + static_cast<size_t>(i) * 12;
    if (!SignatureIs(data, record, signature)) continue;
    const uint32_t offset = ReadU32(data, record + 4);
    const uint32_t length = ReadU32(data, record + 8);
    if (!HasRoom(size, offset, length)) continue;
    entry.found = true;
    entry.offset = offset;
    entry.length = length;
    return entry;
  }
  return entry;
}

std::string LowerAscii(std::string text) {
  std::ranges::transform(text, text.begin(), [](char ch) {
    return static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
  });
  return text;
}

}  // namespace

bool IsValidIccProfile(const uint8_t* data, size_t size) {
  if (data == nullptr || size < kHeaderSize + kTagTableSize) return false;
  // 'acsp' at byte 36 is the ICC profile file signature.
  if (!SignatureIs(data, 36, "acsp")) return false;

  const uint32_t declared_size = ReadU32(data, 0);
  if (declared_size < kHeaderSize + kTagTableSize || declared_size > size) {
    return false;
  }
  const uint32_t count = ReadU32(data, kHeaderSize);
  if (count == 0 || count > kMaxTags) return false;
  return kHeaderSize + kTagTableSize + static_cast<size_t>(count) * 12 <= size;
}

std::string IccProfileDescription(const uint8_t* data, size_t size) {
  if (!IsValidIccProfile(data, size)) return {};

  for (const char* signature : {"desc", "dmdd"}) {
    const TagEntry tag = FindTag(data, size, signature);
    if (!tag.found) continue;
    std::string text = ReadTagText(data, size, tag.offset, tag.length);
    if (!text.empty()) return text;
  }
  return {};
}

bool IsSrgbProfile(const uint8_t* data, size_t size) {
  if (!IsValidIccProfile(data, size)) return false;
  const std::string description = LowerAscii(IccProfileDescription(data, size));
  if (description.empty()) return false;
  return description.find("srgb") != std::string::npos ||
         description.find("iec61966-2.1") != std::string::npos;
}

}  // namespace nv
