#pragma once

#include "core/image.h"
#include "platform/image_io.h"

#include <windows.h>

#include <condition_variable>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <mutex>
#include <string>
#include <thread>

namespace nv::platform {

// Result of one background save. Owned by the UI thread.
struct SaveOutcome {
  bool ok = false;
  uint64_t bytes = 0;
  double ms = 0.0;
  std::wstring error;
  std::filesystem::path target;
};

// Encodes the current view on a worker thread so an 8K save never freezes the
// window (規劃/22/14 §4 step 6). The atomic write itself stays in SaveImage:
// the worker only chooses the pixels (view rotation) and the encoder.
class SaveService {
 public:
  SaveService() = default;
  ~SaveService();

  SaveService(const SaveService&) = delete;
  SaveService& operator=(const SaveService&) = delete;

  bool Start(HWND hwnd, UINT message);
  // Returns false when a save is already running (the caller keeps the UI in
  // its busy state) or when the service was not started.
  bool Save(std::shared_ptr<const nv::Image> image, int rotation,
            const std::filesystem::path& target, ImageSaveFormat format,
            int quality);
  bool busy() const { return busy_; }
  void Shutdown();

 private:
  void Run();

  HWND hwnd_ = nullptr;
  UINT message_ = 0;
  std::thread thread_;
  std::mutex mutex_;
  std::condition_variable cv_;

  std::shared_ptr<const nv::Image> image_;
  int rotation_ = 0;
  std::filesystem::path target_;
  ImageSaveFormat format_ = ImageSaveFormat::kPng;
  int quality_ = 90;
  bool pending_ = false;
  bool stopping_ = false;
  bool busy_ = false;
};

}  // namespace nv::platform
