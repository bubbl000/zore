#pragma once

#include "core/format_router.h"

#include <cstdint>
#include <cstddef>
#include <filesystem>
#include <string>

namespace nv::wic {

bool EnsureComInitialized();

// True when an installed WIC decoder claims `extension` (lowercase, with the
// leading dot, e.g. ".heic" or ".avif"). HEIC/AVIF decoding comes from the
// Microsoft Store image extensions, so the empty-state format line must hide
// them when the codec is absent (規劃/22/03 §3.3). Enumerates the decoder
// components instead of probing a sample file.
bool DecoderSupportsExtension(const std::wstring& extension);

ProbeResult Probe(const std::filesystem::path& path);
DecodeResult Decode(const std::filesystem::path& path, const DecodeOptions& options);

// Decodes an image payload that is already in memory (規劃/22/13 §3.3: the
// camera-embedded JPEG of a RAW file). Same orientation/ICC/alpha handling as
// Decode(); the bytes are copied into an owned stream, so `data` may be freed
// after the call.
DecodeResult DecodeMemory(const uint8_t* data, size_t size,
                          const DecodeOptions& options);

}  // namespace nv::wic
