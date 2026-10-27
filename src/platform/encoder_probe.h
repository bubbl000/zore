#pragma once

#include "platform/image_io.h"

#include <string>

namespace nv::platform {

// Which containers this build + this machine can actually write (規劃/22/14
// §3). Probed once at startup by encoding a 2x2 image through the real save
// path, so a codec that exists but rejects our pixel format is hidden instead
// of failing after the user picks it.
struct EncoderCaps {
  bool png = false;
  bool jpeg = false;
  bool bmp = false;
  bool tiff = false;
  bool webp = false;
  bool gif = false;
};

const EncoderCaps& ProbeEncoders();

bool EncoderAvailable(ImageSaveFormat format);

// Comma-separated list of the available formats in a stable order, for the
// `save: encoders=...` log line and the automated checks.
std::string EncoderList();

}  // namespace nv::platform
