#include "decoders/apng_decoder.h"

#include "decoders/wic_decoder.h"
#include "platform/log.h"

#include <zlib.h>

#include <algorithm>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <string>
#include <utility>
#include <vector>

namespace nv::apng {
namespace {

constexpr int kDecodeErrorBase = 2500;
constexpr uint32_t kMaxFrames = 512;
constexpr uint64_t kMaxAnimationBytes = 128ull * 1024 * 1024;
constexpr uint64_t kMaxFileBytes = 512ull * 1024 * 1024;
constexpr uint32_t kDefaultFrameDelayMs = 100;
constexpr uint32_t kMinimumFrameDelayMs = 16;
constexpr uint32_t kMaximumFrameDelayMs = 60000;

constexpr uint8_t kSignature[8] = {0x89, 'P', 'N', 'G', 0x0D,
                                   0x0A, 0x1A, 0x0A};

Error MakeError(const char* message, HRESULT hr = S_OK) {
  return Error::Make(kDecodeErrorBase, message, hr);
}

uint32_t ReadU32(const uint8_t* data) {
  return (static_cast<uint32_t>(data[0]) << 24) |
         (static_cast<uint32_t>(data[1]) << 16) |
         (static_cast<uint32_t>(data[2]) << 8) | static_cast<uint32_t>(data[3]);
}

uint16_t ReadU16(const uint8_t* data) {
  return static_cast<uint16_t>((static_cast<uint16_t>(data[0]) << 8) |
                               static_cast<uint16_t>(data[1]));
}

bool IsChunk(const uint8_t* type, const char* name) {
  return std::memcmp(type, name, 4) == 0;
}

struct Header {
  uint32_t width = 0;
  uint32_t height = 0;
  uint8_t bit_depth = 0;
  uint8_t color_type = 0;
  uint8_t interlace = 0;
};

struct Frame {
  uint32_t width = 0;
  uint32_t height = 0;
  uint32_t x = 0;
  uint32_t y = 0;
  uint16_t delay_num = 0;
  uint16_t delay_den = 0;
  uint8_t dispose = 0;
  uint8_t blend = 0;
  std::vector<uint8_t> data;  // Compressed scanlines of this sub-image.
};

int ChannelCount(uint8_t color_type) {
  switch (color_type) {
    case 0: return 1;  // Grayscale.
    case 2: return 3;  // RGB.
    case 3: return 1;  // Palette index.
    case 4: return 2;  // Gray + alpha.
    case 6: return 4;  // RGBA.
    default: return 0;
  }
}

bool IsValidDepth(uint8_t color_type, uint8_t bit_depth) {
  switch (color_type) {
    case 0:
      return bit_depth == 1 || bit_depth == 2 || bit_depth == 4 ||
             bit_depth == 8 || bit_depth == 16;
    case 3:
      return bit_depth == 1 || bit_depth == 2 || bit_depth == 4 ||
             bit_depth == 8;
    case 2:
    case 4:
    case 6:
      return bit_depth == 8 || bit_depth == 16;
    default:
      return false;
  }
}

uint32_t DelayMs(uint16_t numerator, uint16_t denominator) {
  if (numerator == 0) return kDefaultFrameDelayMs;
  const uint32_t den = denominator == 0 ? 100 : denominator;
  uint64_t milliseconds =
      static_cast<uint64_t>(numerator) * 1000ull / static_cast<uint64_t>(den);
  if (milliseconds < kMinimumFrameDelayMs) milliseconds = kMinimumFrameDelayMs;
  if (milliseconds > kMaximumFrameDelayMs) milliseconds = kMaximumFrameDelayMs;
  return static_cast<uint32_t>(milliseconds);
}

// Parses IHDR/acTL/PLTE/tRNS/fcTL/IDAT/fdAT into animation frames. `frames`
// only contains frames that belong to the animation; a default image that is
// not part of it (fcTL after IDAT) is skipped, matching the APNG spec.
bool ParsePng(const std::vector<uint8_t>& file, Header* header,
              std::vector<uint8_t>* palette,
              std::vector<uint8_t>* transparency,
              std::vector<Frame>* frames, uint32_t* plays, bool* is_apng,
              Error* error) {
  if (file.size() < sizeof(kSignature) + 12 ||
      std::memcmp(file.data(), kSignature, sizeof(kSignature)) != 0) {
    *error = MakeError("APNG: not a PNG file");
    return false;
  }

  bool have_header = false;
  bool seen_idat = false;
  int idat_frame = -1;  // Frame that owns the IDAT payload, or -1 (poster).
  size_t offset = sizeof(kSignature);

  while (offset + 12 <= file.size()) {
    const uint32_t length = ReadU32(file.data() + offset);
    const uint8_t* type = file.data() + offset + 4;
    if (static_cast<uint64_t>(offset) + 12ull + length > file.size()) {
      *error = MakeError("APNG: truncated chunk");
      return false;
    }
    const uint8_t* payload = file.data() + offset + 8;

    if (IsChunk(type, "IHDR")) {
      if (length != 13 || have_header) {
        *error = MakeError("APNG: invalid IHDR");
        return false;
      }
      header->width = ReadU32(payload);
      header->height = ReadU32(payload + 4);
      header->bit_depth = payload[8];
      header->color_type = payload[9];
      header->interlace = payload[12];
      if (header->width == 0 || header->height == 0 ||
          payload[10] != 0 || payload[11] != 0 ||
          !IsValidDepth(header->color_type, header->bit_depth)) {
        *error = MakeError("APNG: unsupported image header");
        return false;
      }
      if (header->interlace != 0) {
        *error = MakeError("APNG: interlaced frames are not supported");
        return false;
      }
      have_header = true;
    } else if (IsChunk(type, "acTL")) {
      if (length != 8) {
        *error = MakeError("APNG: invalid acTL");
        return false;
      }
      *is_apng = true;
      *plays = ReadU32(payload + 4);
    } else if (IsChunk(type, "PLTE")) {
      if (length % 3 != 0 || length > 768) {
        *error = MakeError("APNG: invalid palette");
        return false;
      }
      palette->assign(payload, payload + length);
    } else if (IsChunk(type, "tRNS")) {
      transparency->assign(payload, payload + length);
    } else if (IsChunk(type, "fcTL")) {
      if (!have_header) {
        *error = MakeError("APNG: fcTL before IHDR");
        return false;
      }
      if (length != 26) {
        *error = MakeError("APNG: invalid fcTL");
        return false;
      }
      Frame frame;
      // fcTL: sequence(4) width(4) height(4) x(4) y(4) delay_num(2)
      // delay_den(2) dispose(1) blend(1).
      frame.width = ReadU32(payload + 4);
      frame.height = ReadU32(payload + 8);
      frame.x = ReadU32(payload + 12);
      frame.y = ReadU32(payload + 16);
      frame.delay_num = ReadU16(payload + 20);
      frame.delay_den = ReadU16(payload + 22);
      frame.dispose = payload[24];
      frame.blend = payload[25];
      if (frame.width == 0 || frame.height == 0 || frame.dispose > 2 ||
          frame.blend > 1 ||
          static_cast<uint64_t>(frame.x) + frame.width > header->width ||
          static_cast<uint64_t>(frame.y) + frame.height > header->height) {
        *error = MakeError("APNG: frame rectangle outside the canvas");
        return false;
      }
      frames->push_back(std::move(frame));
      if (!seen_idat && idat_frame < 0 && frames->size() == 1) {
        idat_frame = 0;
      }
    } else if (IsChunk(type, "IDAT")) {
      seen_idat = true;
      if (idat_frame >= 0 &&
          static_cast<size_t>(idat_frame) < frames->size()) {
        std::vector<uint8_t>& data = frames->at(static_cast<size_t>(idat_frame)).data;
        data.insert(data.end(), payload, payload + length);
      }
    } else if (IsChunk(type, "fdAT")) {
      if (length < 4 || frames->empty()) {
        *error = MakeError("APNG: invalid fdAT");
        return false;
      }
      std::vector<uint8_t>& data = frames->back().data;
      data.insert(data.end(), payload + 4, payload + length);
    } else if (IsChunk(type, "IEND")) {
      break;
    }

    offset += 12u + length;
  }

  if (!have_header) {
    *error = MakeError("APNG: missing IHDR");
    return false;
  }
  if (!*is_apng) {
    *error = MakeError("APNG: missing acTL");
    return false;
  }
  if (frames->empty()) {
    *error = MakeError("APNG: no animation frames");
    return false;
  }
  for (const Frame& frame : *frames) {
    if (frame.data.empty()) {
      *error = MakeError("APNG: frame has no image data");
      return false;
    }
  }
  if (header->color_type == 3 && palette->empty()) {
    *error = MakeError("APNG: palette image without PLTE");
    return false;
  }
  return true;
}

bool LoadFile(const std::filesystem::path& path, std::vector<uint8_t>* file,
              Error* error) {
  std::ifstream input(path, std::ios::binary);
  if (!input) {
    *error = MakeError("APNG: cannot open the file");
    return false;
  }
  input.seekg(0, std::ios::end);
  const std::streamoff size = input.tellg();
  if (size <= 0 || static_cast<uint64_t>(size) > kMaxFileBytes) {
    *error = MakeError("APNG: file is empty or too large");
    return false;
  }
  input.seekg(0, std::ios::beg);
  file->resize(static_cast<size_t>(size));
  input.read(reinterpret_cast<char*>(file->data()), size);
  if (!input) {
    *error = MakeError("APNG: cannot read the file");
    return false;
  }
  return true;
}

int Paeth(int left, int up, int upper_left) {
  const int estimate = left + up - upper_left;
  const int distance_left = std::abs(estimate - left);
  const int distance_up = std::abs(estimate - up);
  const int distance_upper_left = std::abs(estimate - upper_left);
  if (distance_left <= distance_up && distance_left <= distance_upper_left) {
    return left;
  }
  return distance_up <= distance_upper_left ? up : upper_left;
}

bool UnfilterRows(uint8_t* raw, uint32_t height, size_t row_bytes,
                  size_t bytes_per_pixel) {
  const size_t stride = row_bytes + 1;
  for (uint32_t y = 0; y < height; ++y) {
    uint8_t* row = raw + static_cast<size_t>(y) * stride;
    const uint8_t filter = row[0];
    uint8_t* pixels = row + 1;
    const uint8_t* prior =
        y == 0 ? nullptr : raw + (static_cast<size_t>(y) - 1) * stride + 1;
    switch (filter) {
      case 0:
        break;
      case 1:
        for (size_t i = bytes_per_pixel; i < row_bytes; ++i) {
          pixels[i] = static_cast<uint8_t>(pixels[i] + pixels[i - bytes_per_pixel]);
        }
        break;
      case 2:
        if (prior != nullptr) {
          for (size_t i = 0; i < row_bytes; ++i) {
            pixels[i] = static_cast<uint8_t>(pixels[i] + prior[i]);
          }
        }
        break;
      case 3:
        for (size_t i = 0; i < row_bytes; ++i) {
          const int left = i >= bytes_per_pixel ? pixels[i - bytes_per_pixel] : 0;
          const int up = prior != nullptr ? prior[i] : 0;
          pixels[i] = static_cast<uint8_t>(pixels[i] + ((left + up) >> 1));
        }
        break;
      case 4:
        for (size_t i = 0; i < row_bytes; ++i) {
          const int left = i >= bytes_per_pixel ? pixels[i - bytes_per_pixel] : 0;
          const int up = prior != nullptr ? prior[i] : 0;
          const int upper_left =
              (prior != nullptr && i >= bytes_per_pixel)
                  ? prior[i - bytes_per_pixel]
                  : 0;
          pixels[i] =
              static_cast<uint8_t>(pixels[i] + Paeth(left, up, upper_left));
        }
        break;
      default:
        return false;
    }
  }
  return true;
}

uint32_t SampleValue(const uint8_t* row, uint32_t index, uint8_t bit_depth) {
  switch (bit_depth) {
    case 16:
      return (static_cast<uint32_t>(row[static_cast<size_t>(index) * 2]) << 8) |
             row[static_cast<size_t>(index) * 2 + 1];
    case 8:
      return row[index];
    case 4:
      return (row[index / 2] >> ((index % 2) == 0 ? 4 : 0)) & 0x0Fu;
    case 2:
      return (row[index / 4] >> (6 - 2 * (index % 4))) & 0x03u;
    case 1:
      return (row[index / 8] >> (7 - (index % 8))) & 0x01u;
    default:
      return 0;
  }
}

uint8_t ToByte(uint32_t value, uint8_t bit_depth) {
  if (bit_depth == 8) return static_cast<uint8_t>(value);
  if (bit_depth == 16) return static_cast<uint8_t>(value >> 8);
  const uint32_t maximum = (1u << bit_depth) - 1u;
  return static_cast<uint8_t>((value * 255u + maximum / 2u) / maximum);
}

void ConvertRow(const uint8_t* row, uint32_t width, const Header& header,
                const std::vector<uint8_t>& palette,
                const std::vector<uint8_t>& transparency, uint8_t* output) {
  const uint8_t depth = header.bit_depth;
  uint32_t key_gray = 0;
  uint32_t key_red = 0;
  uint32_t key_green = 0;
  uint32_t key_blue = 0;
  if (header.color_type == 0 && transparency.size() >= 2) {
    key_gray = ReadU16(transparency.data());
  } else if (header.color_type == 2 && transparency.size() >= 6) {
    key_red = ReadU16(transparency.data());
    key_green = ReadU16(transparency.data() + 2);
    key_blue = ReadU16(transparency.data() + 4);
  }
  const bool color_key = header.color_type == 0 || header.color_type == 2;

  for (uint32_t x = 0; x < width; ++x) {
    uint8_t* pixel = output + static_cast<size_t>(x) * 4;
    switch (header.color_type) {
      case 0: {
        const uint32_t gray = SampleValue(row, x, depth);
        const uint8_t value = ToByte(gray, depth);
        pixel[0] = value;
        pixel[1] = value;
        pixel[2] = value;
        pixel[3] = (color_key && !transparency.empty() && gray == key_gray) ? 0 : 255;
        break;
      }
      case 2: {
        const uint32_t red = SampleValue(row, x * 3, depth);
        const uint32_t green = SampleValue(row, x * 3 + 1, depth);
        const uint32_t blue = SampleValue(row, x * 3 + 2, depth);
        pixel[0] = ToByte(blue, depth);
        pixel[1] = ToByte(green, depth);
        pixel[2] = ToByte(red, depth);
        pixel[3] = (color_key && !transparency.empty() && red == key_red &&
                    green == key_green && blue == key_blue)
                       ? 0
                       : 255;
        break;
      }
      case 3: {
        const uint32_t index = SampleValue(row, x, depth);
        const size_t entry = static_cast<size_t>(index) * 3;
        if (entry + 2 < palette.size()) {
          pixel[0] = palette[entry + 2];
          pixel[1] = palette[entry + 1];
          pixel[2] = palette[entry];
        } else {
          pixel[0] = 0;
          pixel[1] = 0;
          pixel[2] = 0;
        }
        pixel[3] = index < transparency.size() ? transparency[index] : 255;
        break;
      }
      case 4: {
        const uint32_t gray = SampleValue(row, x * 2, depth);
        const uint32_t alpha = SampleValue(row, x * 2 + 1, depth);
        const uint8_t value = ToByte(gray, depth);
        pixel[0] = value;
        pixel[1] = value;
        pixel[2] = value;
        pixel[3] = ToByte(alpha, depth);
        break;
      }
      case 6: {
        const uint32_t red = SampleValue(row, x * 4, depth);
        const uint32_t green = SampleValue(row, x * 4 + 1, depth);
        const uint32_t blue = SampleValue(row, x * 4 + 2, depth);
        const uint32_t alpha = SampleValue(row, x * 4 + 3, depth);
        pixel[0] = ToByte(blue, depth);
        pixel[1] = ToByte(green, depth);
        pixel[2] = ToByte(red, depth);
        pixel[3] = ToByte(alpha, depth);
        break;
      }
      default:
        break;
    }
  }
}

// Straight-alpha source-over. Unlike GIF (1-bit alpha) APNG frames carry 8-bit
// alpha, so the destination alpha has to be composited with the full formula.
void BlendPixel(uint8_t* destination, const uint8_t* source) {
  const uint32_t alpha = source[3];
  if (alpha == 255) {
    destination[0] = source[0];
    destination[1] = source[1];
    destination[2] = source[2];
    destination[3] = 255;
    return;
  }
  if (alpha == 0) return;
  const uint32_t dst_weight =
      (static_cast<uint32_t>(destination[3]) * (255 - alpha) + 127) / 255;
  const uint32_t out_alpha = alpha + dst_weight;
  if (out_alpha == 0) {
    destination[3] = 0;
    return;
  }
  for (int channel = 0; channel < 3; ++channel) {
    destination[channel] = static_cast<uint8_t>(
        (source[channel] * alpha + destination[channel] * dst_weight +
         out_alpha / 2) /
        out_alpha);
  }
  destination[3] = static_cast<uint8_t>(out_alpha);
}

bool DecodeAnimated(const std::filesystem::path& path, Image* image,
                    Error* error) {
  std::vector<uint8_t> file;
  if (!LoadFile(path, &file, error)) return false;

  Header header;
  std::vector<uint8_t> palette;
  std::vector<uint8_t> transparency;
  std::vector<Frame> frames;
  uint32_t plays = 0;
  bool is_apng = false;
  if (!ParsePng(file, &header, &palette, &transparency, &frames, &plays,
                &is_apng, error)) {
    return false;
  }

  const uint64_t canvas_bytes =
      static_cast<uint64_t>(header.width) * header.height * 4;
  if (canvas_bytes == 0 || canvas_bytes > kMaxAnimationBytes) {
    *error = MakeError("APNG: canvas is too large for animation");
    return false;
  }

  const uint32_t frame_limit = std::min<uint32_t>(
      std::min<uint32_t>(static_cast<uint32_t>(frames.size()), kMaxFrames),
      static_cast<uint32_t>(kMaxAnimationBytes / canvas_bytes));
  const int channels = ChannelCount(header.color_type);
  const size_t bytes_per_pixel =
      std::max<size_t>(1, (static_cast<size_t>(channels) * header.bit_depth + 7) / 8);

  std::vector<uint8_t> canvas(static_cast<size_t>(canvas_bytes), 0);
  std::vector<uint8_t> previous;
  std::vector<ImageFrame> output;
  output.reserve(frame_limit);

  for (uint32_t index = 0; index < frame_limit; ++index) {
    const Frame& frame = frames[index];
    const uint64_t row_bytes =
        (static_cast<uint64_t>(frame.width) * static_cast<uint64_t>(channels) *
             header.bit_depth +
         7) /
        8;
    const uint64_t expected = (row_bytes + 1) * frame.height;
    if (expected == 0 || expected > kMaxAnimationBytes) break;

    std::vector<uint8_t> raw(static_cast<size_t>(expected));
    uLongf raw_size = static_cast<uLongf>(expected);
    const int status =
        uncompress(raw.data(), &raw_size, frame.data.data(),
                   static_cast<uLong>(frame.data.size()));
    if (status != Z_OK || raw_size != static_cast<uLongf>(expected)) {
      nv::platform::LogInfo("apng: frame " + std::to_string(index) +
                            " failed to inflate; truncating animation");
      break;
    }
    if (!UnfilterRows(raw.data(), frame.height, static_cast<size_t>(row_bytes),
                      bytes_per_pixel)) {
      nv::platform::LogInfo("apng: frame " + std::to_string(index) +
                            " uses an unsupported filter; truncating animation");
      break;
    }

    const size_t frame_stride = static_cast<size_t>(frame.width) * 4;
    std::vector<uint8_t> pixels(frame_stride * frame.height);
    for (uint32_t y = 0; y < frame.height; ++y) {
      ConvertRow(raw.data() + static_cast<size_t>(y) * (row_bytes + 1) + 1,
                 frame.width, header, palette, transparency,
                 pixels.data() + static_cast<size_t>(y) * frame_stride);
    }

    if (frame.dispose == 2) previous = canvas;

    for (uint32_t y = 0; y < frame.height; ++y) {
      const uint8_t* source =
          pixels.data() + static_cast<size_t>(y) * frame_stride;
      uint8_t* destination =
          canvas.data() +
          ((static_cast<size_t>(frame.y + y) * header.width + frame.x) * 4);
      for (uint32_t x = 0; x < frame.width; ++x) {
        if (frame.blend == 0) {
          std::memcpy(destination + static_cast<size_t>(x) * 4,
                      source + static_cast<size_t>(x) * 4, 4);
        } else {
          BlendPixel(destination + static_cast<size_t>(x) * 4,
                     source + static_cast<size_t>(x) * 4);
        }
      }
    }

    ImageFrame composed;
    composed.pixels = canvas;
    composed.delay_ms = DelayMs(frame.delay_num, frame.delay_den);
    output.push_back(std::move(composed));

    if (frame.dispose == 1) {
      for (uint32_t y = 0; y < frame.height; ++y) {
        uint8_t* destination =
            canvas.data() +
            ((static_cast<size_t>(frame.y + y) * header.width + frame.x) * 4);
        std::memset(destination, 0, frame_stride);
      }
    } else if (frame.dispose == 2) {
      canvas = std::move(previous);
      previous.clear();
    }
  }

  if (output.empty()) {
    *error = MakeError("APNG: no frame could be decoded");
    return false;
  }

  image->info.width = header.width;
  image->info.height = header.height;
  image->info.stride = header.width * 4u;
  image->info.format = PixelFormat::kBgra8;
  image->info.has_alpha = true;
  image->info.full_width = header.width;
  image->info.full_height = header.height;
  image->info.orientation = 1;
  image->info.frame_count = static_cast<uint32_t>(output.size());
  image->info.animated = output.size() > 1;
  image->info.dpi_x = 96.0;
  image->info.dpi_y = 96.0;
  image->info.source = "apng";
  image->loop_count = plays;
  image->pixels = output[0].pixels;
  if (output.size() > 1) image->frames = std::move(output);

  if (frame_limit < frames.size()) {
    image->info.frames_truncated = true;
    nv::platform::LogInfo("apng: animation truncated at " +
                          std::to_string(frame_limit) + "/" +
                          std::to_string(frames.size()) + " frames");
  }
  return true;
}

}  // namespace

Info Inspect(const std::filesystem::path& path) {
  Info result;
  std::ifstream file(path, std::ios::binary);
  if (!file) return result;
  uint8_t signature[8]{};
  if (!file.read(reinterpret_cast<char*>(signature), sizeof(signature)) ||
      std::memcmp(signature, kSignature, sizeof(kSignature)) != 0) {
    return result;
  }
  for (;;) {
    uint8_t chunk[8]{};
    if (!file.read(reinterpret_cast<char*>(chunk), sizeof(chunk))) return result;
    const uint32_t length = ReadU32(chunk);
    if (IsChunk(chunk + 4, "acTL")) {
      uint8_t data[8]{};
      if (!file.read(reinterpret_cast<char*>(data), sizeof(data))) return result;
      result.is_apng = true;
      result.frames = ReadU32(data);
      result.plays = ReadU32(data + 4);
      if (result.frames == 0) result.frames = 1;
      return result;
    }
    if (IsChunk(chunk + 4, "IDAT")) return result;
    if (length > 0x7FFFFFFFu) return result;
    file.seekg(static_cast<std::streamoff>(length) + 4, std::ios::cur);
    if (!file) return result;
  }
}

DecodeResult Decode(const std::filesystem::path& path,
                    const DecodeOptions& options) {
  DecodeResult result;
  if (!options.animation || options.max_edge > 0) {
    result = wic::Decode(path, options);
    if (result.ok()) {
      const Info info = Inspect(path);
      result.image->info.frame_count = info.frames;
      result.image->info.animated = info.frames > 1;
    }
    return result;
  }

  Image image;
  if (!DecodeAnimated(path, &image, &result.error)) {
    return result;
  }
  if (image.frames.size() > 1) {
    nv::platform::LogInfo("apng: decoded " + std::to_string(image.frames.size()) +
                          " frames (plays=" + std::to_string(image.loop_count) +
                          ")");
  }
  result.image = std::make_shared<Image>(std::move(image));
  return result;
}

}  // namespace nv::apng
