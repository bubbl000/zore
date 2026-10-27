#include "platform/save_service.h"

#include "core/image_transform.h"

#include <chrono>
#include <utility>

namespace nv::platform {

SaveService::~SaveService() { Shutdown(); }

bool SaveService::Start(HWND hwnd, UINT message) {
  if (thread_.joinable()) return true;
  hwnd_ = hwnd;
  message_ = message;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    stopping_ = false;
    busy_ = false;
  }
  thread_ = std::thread([this] { Run(); });
  return true;
}

bool SaveService::Save(std::shared_ptr<const nv::Image> image, int rotation,
                       const std::filesystem::path& target,
                       ImageSaveFormat format, int quality) {
  if (!image || target.empty()) return false;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    if (busy_ || pending_) return false;
    image_ = std::move(image);
    rotation_ = rotation;
    target_ = target;
    format_ = format;
    quality_ = quality;
    pending_ = true;
    busy_ = true;
  }
  cv_.notify_one();
  return true;
}

void SaveService::Shutdown() {
  {
    std::lock_guard<std::mutex> lock(mutex_);
    stopping_ = true;
    pending_ = false;
    image_.reset();
  }
  cv_.notify_one();
  if (thread_.joinable()) thread_.join();
  hwnd_ = nullptr;
}

void SaveService::Run() {
  for (;;) {
    std::shared_ptr<const nv::Image> image;
    std::filesystem::path target;
    ImageSaveFormat format = ImageSaveFormat::kPng;
    int quality = 90;
    int rotation = 0;
    {
      std::unique_lock<std::mutex> lock(mutex_);
      cv_.wait(lock, [this] { return stopping_ || pending_; });
      if (stopping_) return;
      image = image_;
      target = target_;
      format = format_;
      quality = quality_;
      rotation = rotation_;
      // Drop the service's reference: the local keeps the frame alive for this
      // encode, and navigating away afterwards must not retain an 8K frame.
      image_.reset();
      pending_ = false;
    }

    const auto start = std::chrono::steady_clock::now();
    auto exported = nv::MakeExportImage(image, rotation);
    std::wstring error;
    const bool ok =
        exported && SaveImage(target, *exported, format, quality, &error);

    uint64_t bytes = 0;
    if (ok) {
      std::error_code ec;
      const uintmax_t size = std::filesystem::file_size(target, ec);
      if (!ec) bytes = static_cast<uint64_t>(size);
    }
    const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                             std::chrono::steady_clock::now() - start)
                             .count();

    auto* outcome = new SaveOutcome();
    outcome->ok = ok;
    outcome->bytes = bytes;
    outcome->ms = static_cast<double>(elapsed);
    outcome->error = ok ? std::wstring() : error;
    outcome->target = target;
    {
      std::lock_guard<std::mutex> lock(mutex_);
      busy_ = false;
    }
    if (hwnd_ != nullptr) {
      PostMessageW(hwnd_, message_, 0, reinterpret_cast<LPARAM>(outcome));
    } else {
      delete outcome;
    }
  }
}

}  // namespace nv::platform
