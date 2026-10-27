#include "platform/gif_encoder.h"

#include <windows.h>

#include <algorithm>
#include <array>
#include <cstring>
#include <mutex>

#ifdef NV_WITH_FREEIMAGE
#include <FreeImage.h>
#endif

namespace nv::platform {
namespace {

constexpr int kPaletteSize = 256;
constexpr int kTransparentIndex = 255;
constexpr int kQualityColors = 255;  // one index is reserved for transparency

struct Rgb {
  uint8_t r = 0;
  uint8_t g = 0;
  uint8_t b = 0;
};

#ifdef NV_WITH_FREEIMAGE
void GifFreeImageOutput(FREE_IMAGE_FORMAT /*format*/, const char* message) {
  if (message != nullptr) OutputDebugStringA(message);
}

void EnsureFreeImage() {
  static std::once_flag once;
  std::call_once(once, [] {
    FreeImage_Initialise(FALSE);
    FreeImage_SetOutputMessage(GifFreeImageOutput);
  });
}
#endif

bool HasTransparency(const uint8_t* bgra, uint32_t width, uint32_t height,
                     uint32_t stride) {
  for (uint32_t y = 0; y < height; ++y) {
    const uint8_t* row = bgra + static_cast<size_t>(y) * stride;
    for (uint32_t x = 0; x < width; ++x) {
      if (row[x * 4u + 3u] < 128) return true;
    }
  }
  return false;
}

// Deterministic fallback for WIC-only builds: a 6x6x6 colour cube.
void BuildCubePalette(std::array<Rgb, kPaletteSize>* palette) {
  int index = 0;
  for (int r = 0; r < 6; ++r) {
    for (int g = 0; g < 6; ++g) {
      for (int b = 0; b < 6; ++b) {
        Rgb& entry = (*palette)[static_cast<size_t>(index++)];
        entry.r = static_cast<uint8_t>(r * 255 / 5);
        entry.g = static_cast<uint8_t>(g * 255 / 5);
        entry.b = static_cast<uint8_t>(b * 255 / 5);
      }
    }
  }
  for (; index < kPaletteSize; ++index) {
    (*palette)[static_cast<size_t>(index)] = Rgb{0, 0, 0};
  }
}

void CubeQuantize(const uint8_t* bgra, uint32_t width, uint32_t height,
                  uint32_t stride, std::vector<uint8_t>* indices,
                  std::array<Rgb, kPaletteSize>* palette) {
  BuildCubePalette(palette);
  indices->resize(static_cast<size_t>(width) * height);
  for (uint32_t y = 0; y < height; ++y) {
    const uint8_t* row = bgra + static_cast<size_t>(y) * stride;
    for (uint32_t x = 0; x < width; ++x) {
      const uint8_t b = row[x * 4u + 0u];
      const uint8_t g = row[x * 4u + 1u];
      const uint8_t r = row[x * 4u + 2u];
      const int ri = (r * 5 + 127) / 255;
      const int gi = (g * 5 + 127) / 255;
      const int bi = (b * 5 + 127) / 255;
      (*indices)[static_cast<size_t>(y) * width + x] =
          static_cast<uint8_t>((ri * 6 + gi) * 6 + bi);
    }
  }
}

#ifdef NV_WITH_FREEIMAGE
bool FreeImageQuantize(const uint8_t* bgra, uint32_t width, uint32_t height,
                       uint32_t stride, std::vector<uint8_t>* indices,
                       std::array<Rgb, kPaletteSize>* palette) {
  EnsureFreeImage();
  FIBITMAP* rgb = FreeImage_Allocate(static_cast<int>(width),
                                     static_cast<int>(height), 24);
  if (rgb == nullptr) return false;
  for (uint32_t y = 0; y < height; ++y) {
    BYTE* destination =
        FreeImage_GetScanLine(rgb, static_cast<int>(height - 1 - y));
    if (destination == nullptr) {
      FreeImage_Unload(rgb);
      return false;
    }
    const uint8_t* source = bgra + static_cast<size_t>(y) * stride;
    for (uint32_t x = 0; x < width; ++x) {
      destination[x * 3u + 0u] = source[x * 4u + 0u];
      destination[x * 3u + 1u] = source[x * 4u + 1u];
      destination[x * 3u + 2u] = source[x * 4u + 2u];
    }
  }
  FIBITMAP* quantised = FreeImage_ColorQuantizeEx(rgb, FIQ_WUQUANT,
                                                  kQualityColors, 0, nullptr);
  FreeImage_Unload(rgb);
  if (quantised == nullptr) return false;

  const RGBQUAD* source_palette = FreeImage_GetPalette(quantised);
  *palette = {};
  if (source_palette != nullptr) {
    for (int i = 0; i < kQualityColors; ++i) {
      Rgb& entry = (*palette)[static_cast<size_t>(i)];
      entry.r = source_palette[i].rgbRed;
      entry.g = source_palette[i].rgbGreen;
      entry.b = source_palette[i].rgbBlue;
    }
  }

  indices->resize(static_cast<size_t>(width) * height);
  for (uint32_t y = 0; y < height; ++y) {
    const BYTE* source =
        FreeImage_GetScanLine(quantised, static_cast<int>(height - 1 - y));
    if (source == nullptr) {
      FreeImage_Unload(quantised);
      return false;
    }
    std::memcpy(indices->data() + static_cast<size_t>(y) * width, source,
                width);
  }
  FreeImage_Unload(quantised);
  return true;
}
#endif

// Bit packer for the GIF LZW stream, including 255-byte sub-block framing.
class LzwWriter {
 public:
  explicit LzwWriter(std::vector<uint8_t>* output) : output_(output) {}

  void WriteCode(uint16_t code, int bits) {
    bits_ |= static_cast<uint32_t>(code) << bit_count_;
    bit_count_ += bits;
    while (bit_count_ >= 8) {
      pending_.push_back(static_cast<uint8_t>(bits_ & 0xFFu));
      bits_ >>= 8;
      bit_count_ -= 8;
      if (pending_.size() == 255) FlushBlock();
    }
  }

  void Finish() {
    if (bit_count_ > 0) {
      pending_.push_back(static_cast<uint8_t>(bits_ & 0xFFu));
      bits_ = 0;
      bit_count_ = 0;
      if (pending_.size() == 255) FlushBlock();
    }
    if (!pending_.empty()) FlushBlock();
    output_->push_back(0);  // block terminator
  }

 private:
  void FlushBlock() {
    output_->push_back(static_cast<uint8_t>(pending_.size()));
    output_->insert(output_->end(), pending_.begin(), pending_.end());
    pending_.clear();
  }

  std::vector<uint8_t>* output_;
  std::vector<uint8_t> pending_;
  uint32_t bits_ = 0;
  int bit_count_ = 0;
};

void EncodeLzw(const std::vector<uint8_t>& indices,
               std::vector<uint8_t>* output) {
  constexpr int kClearCode = 256;
  constexpr int kEndCode = 257;
  constexpr int kFirstFreeCode = 258;
  constexpr int kMaxCode = 4096;
  constexpr size_t kTableSize = 8192;  // power of two, > 4096
  constexpr uint32_t kEmpty = 0xFFFFFFFFu;

  LzwWriter writer(output);
  int code_size = 9;
  writer.WriteCode(kClearCode, code_size);

  if (indices.empty()) {
    writer.WriteCode(kEndCode, code_size);
    writer.Finish();
    return;
  }

  std::vector<uint32_t> keys(kTableSize, kEmpty);
  std::vector<uint16_t> codes(kTableSize, 0);
  const auto hash = [](uint32_t key) {
    return static_cast<size_t>((key * 2654435761u) >> 19) & (kTableSize - 1);
  };

  int next_code = kFirstFreeCode;
  int prefix = indices[0];
  for (size_t i = 1; i < indices.size(); ++i) {
    const uint16_t suffix = indices[i];
    const uint32_t key =
        (static_cast<uint32_t>(prefix) << 8) | static_cast<uint32_t>(suffix);
    size_t slot = hash(key);
    bool found = false;
    while (keys[slot] != kEmpty) {
      if (keys[slot] == key) {
        prefix = codes[slot];
        found = true;
        break;
      }
      slot = (slot + 1) & (kTableSize - 1);
    }
    if (found) continue;

    writer.WriteCode(static_cast<uint16_t>(prefix), code_size);
    if (next_code < kMaxCode) {
      keys[slot] = key;
      codes[slot] = static_cast<uint16_t>(next_code);
      ++next_code;
      // Widen once the next code no longer fits: the decoder's table lags one
      // entry behind, so this is the state where both sides agree on the width.
      if (next_code > (1 << code_size) && code_size < 12) ++code_size;
    } else {
      writer.WriteCode(kClearCode, code_size);
      std::fill(keys.begin(), keys.end(), kEmpty);
      next_code = kFirstFreeCode;
      code_size = 9;
    }
    prefix = suffix;
  }
  writer.WriteCode(static_cast<uint16_t>(prefix), code_size);
  writer.WriteCode(kEndCode, code_size);
  writer.Finish();
}

void AppendU16(std::vector<uint8_t>* output, uint32_t value) {
  output->push_back(static_cast<uint8_t>(value & 0xFFu));
  output->push_back(static_cast<uint8_t>((value >> 8) & 0xFFu));
}

}  // namespace

bool EncodeGif(const uint8_t* bgra, uint32_t width, uint32_t height,
               uint32_t stride, std::vector<uint8_t>* output,
               std::wstring* error) {
  if (output == nullptr) return false;
  output->clear();
  if (bgra == nullptr || width == 0 || height == 0 ||
      stride < width * 4u) {
    if (error != nullptr) *error = L"Invalid GIF arguments.";
    return false;
  }
  if (width > 65535u || height > 65535u) {
    if (error != nullptr) *error = L"Image is too large for GIF.";
    return false;
  }

  const bool transparent = HasTransparency(bgra, width, height, stride);
  std::vector<uint8_t> indices;
  std::array<Rgb, kPaletteSize> palette{};
  bool quantised = false;
#ifdef NV_WITH_FREEIMAGE
  quantised = FreeImageQuantize(bgra, width, height, stride, &indices, &palette);
#endif
  if (!quantised) {
    CubeQuantize(bgra, width, height, stride, &indices, &palette);
  }

  if (transparent) {
    for (uint32_t y = 0; y < height; ++y) {
      const uint8_t* row = bgra + static_cast<size_t>(y) * stride;
      uint8_t* index_row = indices.data() + static_cast<size_t>(y) * width;
      for (uint32_t x = 0; x < width; ++x) {
        if (row[x * 4u + 3u] < 128) index_row[x] = kTransparentIndex;
      }
    }
  }
  palette[kTransparentIndex] = Rgb{0, 0, 0};

  output->reserve(static_cast<size_t>(width) * height / 2u + 1024u);
  const char header[] = "GIF89a";
  output->insert(output->end(), header, header + 6);
  AppendU16(output, width);
  AppendU16(output, height);
  output->push_back(0xF7);  // global colour table, 8-bit, 256 entries
  output->push_back(static_cast<uint8_t>(transparent ? kTransparentIndex : 0));
  output->push_back(0);  // pixel aspect ratio
  for (const Rgb& entry : palette) {
    output->push_back(entry.r);
    output->push_back(entry.g);
    output->push_back(entry.b);
  }

  // Graphic control extension: 1-bit transparency, no delay (still image).
  output->push_back(0x21);
  output->push_back(0xF9);
  output->push_back(0x04);
  output->push_back(transparent ? 0x01 : 0x00);
  output->push_back(0);
  output->push_back(0);
  output->push_back(static_cast<uint8_t>(kTransparentIndex));
  output->push_back(0x00);

  // Image descriptor: full frame, no local colour table, not interlaced.
  output->push_back(0x2C);
  AppendU16(output, 0);
  AppendU16(output, 0);
  AppendU16(output, width);
  AppendU16(output, height);
  output->push_back(0x00);

  output->push_back(8);  // LZW minimum code size
  EncodeLzw(indices, output);
  output->push_back(0x3B);  // trailer
  return true;
}

}  // namespace nv::platform
