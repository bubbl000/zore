#include "decoders/psd_decoder.h"

#include "core/icc_profile.h"
#include "decoders/wic_decoder.h"
#include "platform/log.h"

#include <wincodec.h>
#include <wrl/client.h>

#include <zlib.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <limits>
#include <new>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace nv::psd {
namespace {

constexpr int kDecodeErrorBase = 2200;
constexpr uint64_t kMaxPixels = 64ull * 1000 * 1000;
constexpr uint64_t kMaxChannelBytes = 1600ull * 1024 * 1024;
constexpr uint64_t kMaxTotalChannelBytes = 2ull * 1024 * 1024 * 1024;
constexpr size_t kMaxIccProfileBytes = 4ull * 1024 * 1024;

Error MakeError(const char* message, HRESULT hr = S_OK) {
  return Error::Make(kDecodeErrorBase, message, hr);
}

uint16_t ReadBe16(const uint8_t* data) {
  return static_cast<uint16_t>((static_cast<uint16_t>(data[0]) << 8) | data[1]);
}

uint32_t ReadBe32(const uint8_t* data) {
  return (static_cast<uint32_t>(data[0]) << 24) |
         (static_cast<uint32_t>(data[1]) << 16) |
         (static_cast<uint32_t>(data[2]) << 8) | static_cast<uint32_t>(data[3]);
}

float ReadBeFloat(const uint8_t* data) {
  const uint32_t bits = ReadBe32(data);
  float value = 0.0f;
  std::memcpy(&value, &bits, sizeof(value));
  return value;
}

uint8_t Scale16To8(uint16_t value) {
  return static_cast<uint8_t>((static_cast<uint32_t>(value) * 255u + 32767u) / 65535u);
}

uint8_t FloatTo8(float value) {
  if (!(value > 0.0f)) return 0;  // also catches NaN
  if (value >= 1.0f) return 255;
  return static_cast<uint8_t>(value * 255.0f + 0.5f);
}

class FileReader {
 public:
  explicit FileReader(const std::filesystem::path& path)
      : file_(path, std::ios::binary) {
    std::error_code ec;
    size_ = std::filesystem::file_size(path, ec);
    if (ec) size_ = 0;
  }

  bool ok() const { return file_.is_open() && size_ > 0; }
  uint64_t size() const { return size_; }

  uint64_t Tell() {
    const std::streampos position = file_.tellg();
    if (position == std::streampos(-1)) return kInvalid;
    return static_cast<uint64_t>(position);
  }

  bool Seek(uint64_t offset) {
    if (offset > size_) return false;
    file_.clear();
    file_.seekg(static_cast<std::streamoff>(offset), std::ios::beg);
    return static_cast<bool>(file_);
  }

  bool Skip(uint64_t count) {
    const uint64_t position = Tell();
    if (position == kInvalid || count > size_ - position) return false;
    return Seek(position + count);
  }

  bool Read(void* destination, size_t bytes) {
    if (bytes == 0) return true;
    const uint64_t position = Tell();
    if (position == kInvalid || bytes > size_ - position) return false;
    file_.read(static_cast<char*>(destination), static_cast<std::streamsize>(bytes));
    return static_cast<bool>(file_) &&
           static_cast<size_t>(file_.gcount()) == bytes;
  }

  bool ReadU16(uint16_t* value) {
    uint8_t bytes[2];
    if (!Read(bytes, sizeof(bytes))) return false;
    *value = ReadBe16(bytes);
    return true;
  }

  bool ReadU32(uint32_t* value) {
    uint8_t bytes[4];
    if (!Read(bytes, sizeof(bytes))) return false;
    *value = ReadBe32(bytes);
    return true;
  }

  bool ReadU64(uint64_t* value) {
    uint8_t bytes[8];
    if (!Read(bytes, sizeof(bytes))) return false;
    *value = (static_cast<uint64_t>(ReadBe32(bytes)) << 32) |
             ReadBe32(bytes + 4);
    return true;
  }

 private:
  static constexpr uint64_t kInvalid = std::numeric_limits<uint64_t>::max();
  std::ifstream file_;
  uint64_t size_ = 0;
};

struct Header {
  uint16_t version = 1;
  uint16_t channels = 0;
  uint32_t width = 0;
  uint32_t height = 0;
  uint16_t depth = 8;
  uint16_t color_mode = 3;

  bool psb() const { return version == 2; }
};

uint32_t BaseChannelCount(uint16_t color_mode, uint16_t channels) {
  switch (color_mode) {
    case 0: return 1;  // Bitmap
    case 1: return 1;  // Grayscale
    case 2: return 1;  // Indexed
    case 3: return 3;  // RGB
    case 4: return 4;  // CMYK
    case 7: return channels;  // Multichannel: every channel is a spot plane.
    case 8: return 1;  // Duotone
    case 9: return 3;  // Lab
    default: return 0;
  }
}

bool ReadHeader(FileReader& reader, Header* header, Error* error) {
  uint8_t signature[4]{};
  if (!reader.Read(signature, sizeof(signature)) ||
      std::memcmp(signature, "8BPS", 4) != 0) {
    *error = MakeError("PSD: missing 8BPS signature");
    return false;
  }

  if (!reader.ReadU16(&header->version) || !reader.Skip(6) ||
      !reader.ReadU16(&header->channels) ||
      !reader.ReadU32(&header->height) ||
      !reader.ReadU32(&header->width) ||
      !reader.ReadU16(&header->depth) ||
      !reader.ReadU16(&header->color_mode)) {
    *error = MakeError("PSD: truncated file header");
    return false;
  }

  if ((header->version != 1 && header->version != 2) ||
      header->channels == 0 || header->channels > 56 ||
      header->width == 0 || header->height == 0 ||
      header->width > 300000 || header->height > 300000) {
    *error = MakeError("PSD: invalid header (version/channels/size)");
    return false;
  }
  if (header->depth != 1 && header->depth != 8 && header->depth != 16 &&
      header->depth != 32) {
    *error = MakeError("PSD: unsupported bit depth");
    return false;
  }
  if (header->color_mode > 9) {
    *error = MakeError("PSD: unsupported color mode");
    return false;
  }
  if (BaseChannelCount(header->color_mode, header->channels) == 0) {
    *error = MakeError("PSD: unsupported color mode");
    return false;
  }
  if (header->depth == 1 && header->color_mode != 0) {
    *error = MakeError("PSD: 1-bit data is only valid in bitmap mode");
    return false;
  }
  return true;
}

bool ReadColorModeData(FileReader& reader, const Header& header,
                       std::vector<uint8_t>* palette, Error* error) {
  uint32_t length = 0;
  if (!reader.ReadU32(&length) || length > reader.size() - reader.Tell()) {
    *error = MakeError("PSD: invalid color mode data length");
    return false;
  }
  if (header.color_mode == 2 && header.depth == 8) {
    // Indexed color: 768 bytes of RGB palette (some writers store fewer).
    palette->resize(std::min<uint32_t>(length, 768));
    if (!palette->empty() && !reader.Read(palette->data(), palette->size())) {
      *error = MakeError("PSD: truncated palette");
      return false;
    }
    return reader.Skip(length - palette->size());
  }
  return reader.Skip(length);
}

struct Resources {
  std::vector<uint8_t> icc_profile;
  std::vector<uint32_t> alpha_ids;  // Image resource 1053 (0 = transparency).
  double dpi = 96.0;
};

struct AlphaInfo {
  bool has_alpha = false;
  uint32_t index = 0;
};

AlphaInfo ResolveAlpha(const Header& header, const Resources& resources,
                       int16_t layer_count) {
  AlphaInfo alpha;
  const uint32_t base = BaseChannelCount(header.color_mode, header.channels);
  if (header.channels <= base || header.color_mode == 7) return alpha;

  // A negative layer count means the first extra channel is merged alpha.
  if (layer_count < 0) {
    alpha.has_alpha = true;
    alpha.index = base;
    return alpha;
  }

  // Image resource 1053 lists one ID per extra channel: 0 = alpha, >0 = spot.
  if (!resources.alpha_ids.empty()) {
    const size_t extra = std::min<size_t>(resources.alpha_ids.size(),
                                          header.channels - base);
    const uint32_t first_extra =
        header.channels - static_cast<uint32_t>(extra);
    for (size_t i = 0; i < extra; ++i) {
      if (resources.alpha_ids[i] == 0) {
        alpha.has_alpha = true;
        alpha.index = first_extra + static_cast<uint32_t>(i);
        return alpha;
      }
    }
    return alpha;  // All extra channels are spot colours.
  }

  if (layer_count > 0) return alpha;

  // No metadata: the first extra channel is the conventional transparency
  // channel. A CMYK file with several extras is more likely spots than alpha.
  if (header.color_mode == 4 && header.channels > base + 1) return alpha;

  alpha.has_alpha = true;
  alpha.index = base;
  return alpha;
}

bool ReadResources(FileReader& reader, Resources* resources, Error* error) {
  uint32_t length = 0;
  if (!reader.ReadU32(&length)) {
    *error = MakeError("PSD: missing image resources length");
    return false;
  }
  const uint64_t start = reader.Tell();
  if (length > reader.size() - start) {
    *error = MakeError("PSD: image resources exceed file size");
    return false;
  }
  const uint64_t end = start + length;

  while (reader.Tell() + 12 <= end) {
    uint8_t signature[4]{};
    if (!reader.Read(signature, sizeof(signature))) {
      *error = MakeError("PSD: truncated image resource");
      return false;
    }
    if (std::memcmp(signature, "8BIM", 4) != 0) break;

    uint16_t id = 0;
    if (!reader.ReadU16(&id)) {
      *error = MakeError("PSD: truncated image resource id");
      return false;
    }
    uint8_t name_length = 0;
    if (!reader.Read(&name_length, 1)) {
      *error = MakeError("PSD: truncated image resource name");
      return false;
    }
    const uint64_t name_bytes = static_cast<uint64_t>(name_length) +
                                ((name_length + 1) % 2);
    if (!reader.Skip(name_bytes)) {
      *error = MakeError("PSD: truncated image resource name");
      return false;
    }

    uint32_t size = 0;
    if (!reader.ReadU32(&size)) {
      *error = MakeError("PSD: truncated image resource size");
      return false;
    }
    const uint64_t data_start = reader.Tell();
    if (size > end - data_start) {
      *error = MakeError("PSD: image resource exceeds section");
      return false;
    }

    if (id == 1039 && size > 0 && size <= kMaxIccProfileBytes) {
      resources->icc_profile.resize(size);
      if (!reader.Read(resources->icc_profile.data(), size)) {
        *error = MakeError("PSD: truncated ICC profile resource");
        return false;
      }
    } else if (id == 1053 && size >= 4 && size <= 56u * 4u) {
      const size_t count = size / 4;
      resources->alpha_ids.resize(count);
      for (size_t i = 0; i < count; ++i) {
        if (!reader.ReadU32(&resources->alpha_ids[i])) {
          *error = MakeError("PSD: truncated alpha identifiers");
          return false;
        }
      }
    } else if (id == 1005 && size >= 16) {
      uint8_t resolution[16]{};
      if (!reader.Read(resolution, sizeof(resolution))) {
        *error = MakeError("PSD: truncated resolution resource");
        return false;
      }
      const double hres = static_cast<double>(ReadBe32(resolution)) / 65536.0;
      const double vres = static_cast<double>(ReadBe32(resolution + 8)) / 65536.0;
      if (hres > 1.0 && hres < 20000.0) resources->dpi = hres;
      if (vres > 1.0 && vres < 20000.0) {
        // Keep a single DPI value; the renderer uses isotropic scaling.
        resources->dpi = (resources->dpi + vres) * 0.5;
      }
    }
    if (!reader.Seek(data_start + size)) {
      *error = MakeError("PSD: truncated image resource data");
      return false;
    }
    if (size % 2 != 0 && !reader.Skip(1)) {
      *error = MakeError("PSD: truncated image resource padding");
      return false;
    }
  }
  return reader.Seek(end);
}

bool ReadLayerCountAndSkip(FileReader& reader, const Header& header,
                           int16_t* layer_count, Error* error) {
  *layer_count = 0;
  uint64_t length = 0;
  if (header.psb()) {
    if (!reader.ReadU64(&length)) {
      *error = MakeError("PSD: missing layer/mask section length");
      return false;
    }
  } else {
    uint32_t short_length = 0;
    if (!reader.ReadU32(&short_length)) {
      *error = MakeError("PSD: missing layer/mask section length");
      return false;
    }
    length = short_length;
  }
  if (length > reader.size() - reader.Tell()) {
    *error = MakeError("PSD: layer/mask section exceeds file size");
    return false;
  }
  const uint64_t section_end = reader.Tell() + length;
  if (length >= (header.psb() ? 10u : 6u)) {
    uint64_t info_length = 0;
    if (header.psb()) {
      if (!reader.ReadU64(&info_length)) {
        *error = MakeError("PSD: missing layer info length");
        return false;
      }
    } else {
      uint32_t short_info = 0;
      if (!reader.ReadU32(&short_info)) {
        *error = MakeError("PSD: missing layer info length");
        return false;
      }
      info_length = short_info;
    }
    if (info_length >= 2) {
      uint16_t raw_count = 0;
      if (!reader.ReadU16(&raw_count)) {
        *error = MakeError("PSD: missing layer count");
        return false;
      }
      // A negative layer count means the first extra channel is the merged
      // transparency channel.
      *layer_count = static_cast<int16_t>(raw_count);
    }
  }
  return reader.Seek(section_end);
}

bool DecodePackBits(const uint8_t* compressed, size_t compressed_size,
                    uint8_t* output, size_t output_size) {
  size_t in = 0;
  size_t out = 0;
  while (in < compressed_size && out < output_size) {
    const int8_t control = static_cast<int8_t>(compressed[in++]);
    if (control >= 0) {
      const size_t count = static_cast<size_t>(control) + 1;
      if (in + count > compressed_size) return false;
      const size_t copied = std::min(count, output_size - out);
      std::memcpy(output + out, compressed + in, copied);
      in += count;
      out += copied;
    } else if (control != -128) {
      const size_t count = static_cast<size_t>(1 - control);
      if (in >= compressed_size) return false;
      const uint8_t value = compressed[in++];
      const size_t copied = std::min(count, output_size - out);
      std::memset(output + out, value, copied);
      out += copied;
    }
  }
  return out == output_size;
}

bool UndoPrediction(uint8_t* data, uint32_t width, uint64_t rows,
                    uint16_t depth) {
  if (depth == 8) {
    for (uint64_t row = 0; row < rows; ++row) {
      uint8_t* line = data + row * width;
      for (uint32_t x = 1; x < width; ++x) {
        line[x] = static_cast<uint8_t>(line[x] + line[x - 1]);
      }
    }
    return true;
  }
  if (depth == 16) {
    for (uint64_t row = 0; row < rows; ++row) {
      uint8_t* line = data + row * static_cast<uint64_t>(width) * 2;
      for (uint32_t x = 1; x < width; ++x) {
        const uint16_t previous = ReadBe16(line + (x - 1) * 2);
        const uint16_t current = ReadBe16(line + x * 2);
        const uint16_t value = static_cast<uint16_t>(current + previous);
        line[x * 2] = static_cast<uint8_t>(value >> 8);
        line[x * 2 + 1] = static_cast<uint8_t>(value & 0xFF);
      }
    }
    return true;
  }
  if (depth == 32) {
    const size_t row_bytes = static_cast<size_t>(width) * 4;
    std::vector<uint8_t> shuffled(row_bytes);
    for (uint64_t row = 0; row < rows; ++row) {
      uint8_t* line = data + row * row_bytes;
      for (size_t x = 1; x < row_bytes; ++x) {
        line[x] = static_cast<uint8_t>(line[x] + line[x - 1]);
      }
      // Undo the 4-byte interleave Photoshop applies before delta encoding:
      // "123412341234" -> "111222333444".
      std::memcpy(shuffled.data(), line, row_bytes);
      size_t destination = 0;
      for (size_t offset = 0; offset < width; ++offset) {
        for (size_t x = offset; x < row_bytes; x += width) {
          line[destination++] = shuffled[x];
        }
      }
    }
    return true;
  }
  return false;
}

bool ReadMergedChannels(FileReader& reader, const Header& header,
                        uint16_t compression,
                        std::vector<std::vector<uint8_t>>* planes,
                        Error* error) {
  const uint32_t bytes_per_sample = header.depth == 1 ? 0 : header.depth / 8;
  const uint64_t row_bytes =
      header.depth == 1 ? (static_cast<uint64_t>(header.width) + 7) / 8
                        : static_cast<uint64_t>(header.width) * bytes_per_sample;
  const uint64_t channel_bytes = row_bytes * header.height;
  const uint64_t total_bytes = channel_bytes * header.channels;
  if (channel_bytes == 0 || channel_bytes > kMaxChannelBytes ||
      total_bytes > kMaxTotalChannelBytes) {
    *error = MakeError("PSD: channel data is too large");
    return false;
  }

  planes->assign(header.channels, {});
  if (compression == 0) {
    for (auto& plane : *planes) {
      plane.resize(static_cast<size_t>(channel_bytes));
      if (!reader.Read(plane.data(), plane.size())) {
        *error = MakeError("PSD: truncated raw channel data");
        return false;
      }
    }
    return true;
  }

  if (compression == 1) {
    const uint64_t rows = static_cast<uint64_t>(header.height) * header.channels;
    std::vector<uint32_t> row_sizes(static_cast<size_t>(rows));
    uint64_t compressed_total = 0;
    for (uint64_t i = 0; i < rows; ++i) {
      if (header.psb()) {
        if (!reader.ReadU32(&row_sizes[static_cast<size_t>(i)])) {
          *error = MakeError("PSD: truncated RLE row table");
          return false;
        }
      } else {
        uint16_t value = 0;
        if (!reader.ReadU16(&value)) {
          *error = MakeError("PSD: truncated RLE row table");
          return false;
        }
        row_sizes[static_cast<size_t>(i)] = value;
      }
      compressed_total += row_sizes[static_cast<size_t>(i)];
    }
    if (compressed_total > reader.size() - reader.Tell()) {
      *error = MakeError("PSD: RLE data exceeds file size");
      return false;
    }

    std::vector<uint8_t> row;
    for (uint32_t channel = 0; channel < header.channels; ++channel) {
      auto& plane = (*planes)[channel];
      plane.resize(static_cast<size_t>(channel_bytes));
      for (uint32_t y = 0; y < header.height; ++y) {
        const uint32_t size = row_sizes[static_cast<size_t>(channel) *
                                            header.height +
                                        y];
        row.resize(size);
        if (size > 0 && !reader.Read(row.data(), size)) {
          *error = MakeError("PSD: truncated RLE channel data");
          return false;
        }
        uint8_t* destination = plane.data() + row_bytes * y;
        if (!DecodePackBits(row.data(), row.size(), destination,
                            static_cast<size_t>(row_bytes))) {
          *error = MakeError("PSD: corrupt RLE row");
          return false;
        }
      }
    }
    return true;
  }

  if (compression == 2 || compression == 3) {
    const uint64_t remaining = reader.size() - reader.Tell();
    if (remaining == 0 || remaining > 1600ull * 1024 * 1024) {
      *error = MakeError("PSD: ZIP data is missing or too large");
      return false;
    }
    std::vector<uint8_t> compressed(static_cast<size_t>(remaining));
    if (!reader.Read(compressed.data(), compressed.size())) {
      *error = MakeError("PSD: truncated ZIP channel data");
      return false;
    }

    std::vector<uint8_t> unpacked(static_cast<size_t>(total_bytes));
    uLongf unpacked_size = static_cast<uLongf>(unpacked.size());
    const int result = uncompress(unpacked.data(), &unpacked_size,
                                  compressed.data(),
                                  static_cast<uLong>(compressed.size()));
    if (result != Z_OK || unpacked_size != unpacked.size()) {
      *error = MakeError("PSD: ZIP decompression failed");
      return false;
    }
    if (compression == 3 &&
        !UndoPrediction(unpacked.data(), header.width,
                        static_cast<uint64_t>(header.height) * header.channels,
                        header.depth)) {
      *error = MakeError("PSD: unsupported prediction depth");
      return false;
    }
    for (uint32_t channel = 0; channel < header.channels; ++channel) {
      auto& plane = (*planes)[channel];
      plane.resize(static_cast<size_t>(channel_bytes));
      std::memcpy(plane.data(),
                  unpacked.data() + channel_bytes * channel,
                  static_cast<size_t>(channel_bytes));
    }
    return true;
  }

  *error = MakeError("PSD: unsupported compression method");
  return false;
}

// Lab (D65) -> linear sRGB -> gamma-encoded sRGB, matching the usual
// Photoshop 8-bit Lab encoding (L 0..100, a/b -128..127).
uint8_t LabToSrgbChannel(double l, double a, double b, int channel) {
  const double fy = (l + 16.0) / 116.0;
  const double fx = fy + a / 500.0;
  const double fz = fy - b / 200.0;
  auto inverse = [](double t) {
    constexpr double kDelta = 6.0 / 29.0;
    if (t > kDelta) return t * t * t;
    return 3.0 * kDelta * kDelta * (t - 4.0 / 29.0);
  };
  const double x = 0.95047 * inverse(fx);
  const double y = 1.00000 * inverse(fy);
  const double z = 1.08883 * inverse(fz);
  const double r = 3.2404542 * x - 1.5371385 * y - 0.4985314 * z;
  const double g = -0.9692660 * x + 1.8760108 * y + 0.0415560 * z;
  const double bl = 0.0556434 * x - 0.2040259 * y + 1.0572252 * z;
  double value = channel == 0 ? r : (channel == 1 ? g : bl);
  value = value <= 0.0031308 ? 12.92 * value
                             : 1.055 * std::pow(value, 1.0 / 2.4) - 0.055;
  return FloatTo8(static_cast<float>(value));
}

struct Decoded {
  std::vector<uint8_t> bgra;
  uint32_t width = 0;
  uint32_t height = 0;
  bool has_alpha = false;
};

bool ComposeBgra(const Header& header,
                 const std::vector<std::vector<uint8_t>>& planes,
                 const std::vector<uint8_t>& palette, const AlphaInfo& alpha_info,
                 Decoded* decoded, Error* error) {
  const uint32_t width = header.width;
  const uint32_t height = header.height;
  const size_t pixel_count = static_cast<size_t>(width) * height;
  if (pixel_count > kMaxPixels) {
    *error = MakeError("PSD: image is too large to decode (over 64 MP)");
    return false;
  }

  decoded->bgra.resize(pixel_count * 4);
  decoded->width = width;
  decoded->height = height;
  decoded->has_alpha = alpha_info.has_alpha;

  const uint8_t* plane0 = planes.empty() ? nullptr : planes[0].data();

  // Bitmap mode stores one bit per pixel: 1 = black, 0 = white.
  if (header.color_mode == 0 && header.depth == 1) {
    const uint64_t row_bytes = (static_cast<uint64_t>(width) + 7) / 8;
    for (uint32_t y = 0; y < height; ++y) {
      const uint8_t* row = plane0 + row_bytes * y;
      for (uint32_t x = 0; x < width; ++x) {
        const bool black = (row[x / 8] & (0x80 >> (x % 8))) != 0;
        const uint8_t value = black ? 0 : 255;
        uint8_t* out = decoded->bgra.data() +
                       (static_cast<size_t>(y) * width + x) * 4;
        out[0] = out[1] = out[2] = value;
        out[3] = 255;
      }
    }
    return true;
  }

  auto sample8 = [&](uint32_t channel, size_t index) -> uint8_t {
    const uint8_t* plane = planes[channel].data();
    switch (header.depth) {
      case 8:
        return plane[index];
      case 16:
        return Scale16To8(ReadBe16(plane + index * 2));
      case 32:
        return FloatTo8(ReadBeFloat(plane + index * 4));
      default:
        return 0;
    }
  };
  auto sample_unit = [&](uint32_t channel, size_t index) -> double {
    const uint8_t* plane = planes[channel].data();
    switch (header.depth) {
      case 8:
        return static_cast<double>(plane[index]) / 255.0;
      case 16:
        return static_cast<double>(ReadBe16(plane + index * 2)) / 65535.0;
      case 32:
        return std::clamp(static_cast<double>(ReadBeFloat(plane + index * 4)),
                          0.0, 1.0);
      default:
        return 0.0;
    }
  };

  const uint32_t alpha_channel = alpha_info.index;
  const uint8_t* palette_data = palette.empty() ? nullptr : palette.data();
  const size_t palette_entries = palette.size() / 3;

  for (uint32_t y = 0; y < height; ++y) {
    for (uint32_t x = 0; x < width; ++x) {
      const size_t index = static_cast<size_t>(y) * width + x;
      uint8_t* out = decoded->bgra.data() + index * 4;
      uint8_t alpha = 255;
      uint8_t red = 0;
      uint8_t green = 0;
      uint8_t blue = 0;

      switch (header.color_mode) {
        case 0:
        case 1:
        case 7:
        case 8:
          red = green = blue = sample8(0, index);
          break;
        case 2: {
          const uint8_t entry = sample8(0, index);
          if (palette_data != nullptr && entry < palette_entries) {
            red = palette_data[entry * 3];
            green = palette_data[entry * 3 + 1];
            blue = palette_data[entry * 3 + 2];
          }
          break;
        }
        case 3:
          red = sample8(0, index);
          green = sample8(1, index);
          blue = sample8(2, index);
          break;
        case 4: {
          // Photoshop stores CMYK inverted; multiplying the stored values is
          // equivalent to (255-C)*(255-K)/255 after inversion.
          const uint32_t c = sample8(0, index);
          const uint32_t m = sample8(1, index);
          const uint32_t yellow = sample8(2, index);
          const uint32_t k = sample8(3, index);
          red = static_cast<uint8_t>((c * k + 127) / 255);
          green = static_cast<uint8_t>((m * k + 127) / 255);
          blue = static_cast<uint8_t>((yellow * k + 127) / 255);
          break;
        }
        case 9: {
          const double l = sample_unit(0, index) * 100.0;
          const double a = sample_unit(1, index) * 255.0 - 128.0;
          const double b = sample_unit(2, index) * 255.0 - 128.0;
          red = LabToSrgbChannel(l, a, b, 0);
          green = LabToSrgbChannel(l, a, b, 1);
          blue = LabToSrgbChannel(l, a, b, 2);
          break;
        }
        default:
          break;
      }

      if (alpha_info.has_alpha && alpha_channel < planes.size()) {
        alpha = sample8(alpha_channel, index);
      }
      out[0] = blue;
      out[1] = green;
      out[2] = red;
      out[3] = alpha;
    }
  }
  return true;
}

bool ConvertToSrgb(Image* image, const std::vector<uint8_t>& profile) {
  using Microsoft::WRL::ComPtr;
  if (profile.empty() || nv::IsSrgbProfile(profile.data(), profile.size())) {
    return false;
  }
  if (!wic::EnsureComInitialized()) return false;

  ComPtr<IWICImagingFactory> factory;
  if (FAILED(CoCreateInstance(CLSID_WICImagingFactory, nullptr,
                              CLSCTX_INPROC_SERVER,
                              IID_PPV_ARGS(factory.GetAddressOf())))) {
    return false;
  }

  ComPtr<IWICBitmap> source;
  // 32bppBGR reads four bytes per pixel but ignores alpha, which is exactly
  // what IWICColorTransform needs here (BGRA sources are a no-op on Windows).
  if (FAILED(factory->CreateBitmapFromMemory(
          image->info.width, image->info.height, GUID_WICPixelFormat32bppBGR,
          image->info.stride, static_cast<UINT>(image->pixels.size()),
          image->pixels.data(), source.GetAddressOf()))) {
    return false;
  }

  ComPtr<IWICColorContext> from;
  ComPtr<IWICColorContext> to;
  if (FAILED(factory->CreateColorContext(from.GetAddressOf())) ||
      FAILED(factory->CreateColorContext(to.GetAddressOf())) ||
      FAILED(from->InitializeFromMemory(
          const_cast<BYTE*>(profile.data()), static_cast<UINT>(profile.size()))) ||
      FAILED(to->InitializeFromExifColorSpace(1))) {
    return false;
  }

  ComPtr<IWICColorTransform> transform;
  if (FAILED(factory->CreateColorTransformer(transform.GetAddressOf())) ||
      FAILED(transform->Initialize(source.Get(), from.Get(), to.Get(),
                                   GUID_WICPixelFormat32bppBGRA))) {
    return false;
  }

  std::vector<uint8_t> converted(image->pixels.size());
  if (FAILED(transform->CopyPixels(nullptr, image->info.stride,
                                   static_cast<UINT>(converted.size()),
                                   converted.data()))) {
    return false;
  }
  // Restore the original alpha plane: the transform ran on an opaque copy.
  for (size_t i = 3; i < converted.size(); i += 4) {
    converted[i] = image->pixels[i];
  }
  image->pixels = std::move(converted);
  return true;
}

std::vector<uint8_t> DownscaleBgra(const std::vector<uint8_t>& source,
                                   uint32_t source_width,
                                   uint32_t source_height,
                                   uint32_t target_width,
                                   uint32_t target_height) {
  std::vector<uint8_t> target(static_cast<size_t>(target_width) *
                              target_height * 4);
  for (uint32_t y = 0; y < target_height; ++y) {
    const uint32_t y0 = static_cast<uint32_t>(
        static_cast<uint64_t>(y) * source_height / target_height);
    uint32_t y1 = static_cast<uint32_t>(
        static_cast<uint64_t>(y + 1) * source_height / target_height);
    if (y1 <= y0) y1 = y0 + 1;
    for (uint32_t x = 0; x < target_width; ++x) {
      const uint32_t x0 = static_cast<uint32_t>(
          static_cast<uint64_t>(x) * source_width / target_width);
      uint32_t x1 = static_cast<uint32_t>(
          static_cast<uint64_t>(x + 1) * source_width / target_width);
      if (x1 <= x0) x1 = x0 + 1;

      uint64_t blue = 0;
      uint64_t green = 0;
      uint64_t red = 0;
      uint64_t alpha = 0;
      uint64_t count = 0;
      for (uint32_t sy = y0; sy < y1; ++sy) {
        const uint8_t* row = source.data() +
                             (static_cast<size_t>(sy) * source_width + x0) * 4;
        for (uint32_t sx = x0; sx < x1; ++sx, row += 4) {
          blue += row[0];
          green += row[1];
          red += row[2];
          alpha += row[3];
          ++count;
        }
      }
      uint8_t* out = target.data() +
                     (static_cast<size_t>(y) * target_width + x) * 4;
      out[0] = static_cast<uint8_t>((blue + count / 2) / count);
      out[1] = static_cast<uint8_t>((green + count / 2) / count);
      out[2] = static_cast<uint8_t>((red + count / 2) / count);
      out[3] = static_cast<uint8_t>((alpha + count / 2) / count);
    }
  }
  return target;
}

}  // namespace

ProbeResult Probe(const std::filesystem::path& path) {
  ProbeResult result;
  FileReader reader(path);
  if (!reader.ok()) {
    result.error = MakeError("PSD: cannot open file");
    return result;
  }

  Header header;
  if (!ReadHeader(reader, &header, &result.error)) return result;

  std::vector<uint8_t> palette;
  if (!ReadColorModeData(reader, header, &palette, &result.error)) {
    return result;
  }
  Resources resources;
  if (!ReadResources(reader, &resources, &result.error)) return result;
  int16_t layer_count = 0;
  if (!ReadLayerCountAndSkip(reader, header, &layer_count, &result.error)) {
    return result;
  }
  const AlphaInfo alpha =
      ResolveAlpha(header, resources, layer_count);

  result.format = ImageFormat::kPsd;
  result.width = header.width;
  result.height = header.height;
  result.has_alpha = alpha.has_alpha;
  result.has_icc = !resources.icc_profile.empty();
  result.icc_is_srgb =
      result.has_icc &&
      nv::IsSrgbProfile(resources.icc_profile.data(),
                        resources.icc_profile.size());
  result.orientation = 1;
  result.orientation_applied = false;
  result.decoder = "psd";
  return result;
}

DecodeResult DecodeImpl(const std::filesystem::path& path,
                        const DecodeOptions& options) {
  DecodeResult result;
  FileReader reader(path);
  if (!reader.ok()) {
    result.error = MakeError("PSD: cannot open file");
    return result;
  }

  Header header;
  if (!ReadHeader(reader, &header, &result.error)) return result;

  const uint64_t pixels = static_cast<uint64_t>(header.width) * header.height;
  if (pixels > kMaxPixels) {
    result.error = MakeError("PSD: image is too large to decode (over 64 MP)");
    return result;
  }

  std::vector<uint8_t> palette;
  if (!ReadColorModeData(reader, header, &palette, &result.error)) {
    return result;
  }
  Resources resources;
  if (!ReadResources(reader, &resources, &result.error)) return result;
  int16_t layer_count = 0;
  if (!ReadLayerCountAndSkip(reader, header, &layer_count, &result.error)) {
    return result;
  }
  const AlphaInfo alpha_info =
      ResolveAlpha(header, resources, layer_count);

  uint16_t compression = 0;
  if (!reader.ReadU16(&compression)) {
    result.error = MakeError("PSD: missing merged image data");
    return result;
  }

  std::vector<std::vector<uint8_t>> planes;
  if (!ReadMergedChannels(reader, header, compression, &planes,
                          &result.error)) {
    return result;
  }

  Decoded decoded;
  if (!ComposeBgra(header, planes, palette, alpha_info, &decoded,
                   &result.error)) {
    return result;
  }

  Image image;
  image.info.width = decoded.width;
  image.info.height = decoded.height;
  image.info.stride = decoded.width * 4u;
  image.info.format = PixelFormat::kBgra8;
  image.info.has_alpha = decoded.has_alpha;
  image.info.full_width = header.width;
  image.info.full_height = header.height;
  image.info.orientation = 1;
  image.info.orientation_applied = false;
  image.info.dpi_x = resources.dpi;
  image.info.dpi_y = resources.dpi;
  image.info.icc_profile = resources.icc_profile;
  image.info.source = "psd";
  image.pixels = std::move(decoded.bgra);
  // Convert only device-RGB/gray sources. A CMYK or Lab profile cannot be
  // applied to an already-composed RGB buffer (the CMM would reject or
  // misinterpret it); those modes keep the approximated pixels.
  const bool profile_applicable = header.color_mode == 1 ||
                                  header.color_mode == 3 ||
                                  header.color_mode == 8;
  image.info.icc_applied =
      profile_applicable && ConvertToSrgb(&image, resources.icc_profile);

  if (options.max_edge > 0 &&
      std::max(decoded.width, decoded.height) > options.max_edge) {
    const double scale = static_cast<double>(options.max_edge) /
                         std::max(decoded.width, decoded.height);
    const uint32_t target_width =
        std::max<uint32_t>(1, static_cast<uint32_t>(decoded.width * scale + 0.5));
    const uint32_t target_height = std::max<uint32_t>(
        1, static_cast<uint32_t>(decoded.height * scale + 0.5));
    image.pixels = DownscaleBgra(image.pixels, decoded.width, decoded.height,
                                 target_width, target_height);
    image.info.width = target_width;
    image.info.height = target_height;
    image.info.stride = target_width * 4u;
  }

  {
    char line[192]{};
    std::snprintf(line, sizeof(line),
                  "psd: version=%u %ux%u depth=%u mode=%u channels=%u "
                  "compression=%u alpha=%d icc=%s",
                  static_cast<unsigned>(header.version), header.width,
                  header.height, static_cast<unsigned>(header.depth),
                  static_cast<unsigned>(header.color_mode),
                  static_cast<unsigned>(header.channels),
                  static_cast<unsigned>(compression),
                  decoded.has_alpha ? 1 : 0,
                  image.info.icc_applied
                      ? "converted"
                      : (resources.icc_profile.empty() ? "none" : "kept"));
    nv::platform::LogInfo(line);
  }

  result.image = std::make_shared<Image>(std::move(image));
  return result;
}

DecodeResult Decode(const std::filesystem::path& path,
                    const DecodeOptions& options) {
  try {
    return DecodeImpl(path, options);
  } catch (const std::bad_alloc&) {
    DecodeResult result;
    result.error = MakeError("PSD: out of memory while decoding");
    return result;
  } catch (const std::length_error&) {
    DecodeResult result;
    result.error = MakeError("PSD: decoded image is too large");
    return result;
  }
}

}  // namespace nv::psd
