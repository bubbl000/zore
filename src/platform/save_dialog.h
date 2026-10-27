#pragma once

#include "core/image.h"
#include "platform/image_io.h"

#include <windows.h>

#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>

namespace nv::platform {

// Everything the system "Save as" dialog needs from the app. The dialog itself
// only selects a path/options; the actual encode (or byte copy) stays in the
// app.
struct SaveAsRequest {
  std::shared_ptr<const Image> image;
  std::filesystem::path suggested_path;
  std::filesystem::path source_path;
  ImageSaveFormat source_format = ImageSaveFormat::kPng;
  int initial_quality = 90;
  int rotation = 0;
  // True when the current view is the untouched source (no view rotation).
  // Only then can "use original image format" copy the source bytes verbatim.
  bool source_unmodified = false;
  uint64_t source_bytes = 0;
  HWND owner = nullptr;
  // Filter rows supplied by the app (localized labels, only available encoders).
  // The order must match `formats`; `default_format_index` is 0-based.
  std::wstring filter;
  std::vector<ImageSaveFormat> formats;
  size_t default_format_index = 0;
  std::wstring title;
};

struct SaveAsOutcome {
  bool accepted = false;
  std::filesystem::path path;
  ImageSaveFormat format = ImageSaveFormat::kPng;
  int quality = 90;
  uint64_t estimated_bytes = 0;
  // The source bytes were copied verbatim; the app must not encode again.
  bool byte_copy = false;
};

// Shows the Explorer-style system save dialog with an embedded options panel
// (quality slider, estimated size, "use original image format"). Returns
// accepted=false on cancel. If the template/hook cannot be created, retries
// with a plain GetSaveFileNameW (no options panel) so saving still works.
SaveAsOutcome ShowSaveAsDialog(const SaveAsRequest& request);

}  // namespace nv::platform
