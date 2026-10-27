#pragma once

#include <windows.h>
#include <oleidl.h>

#include <atomic>
#include <filesystem>

namespace nv::platform {

// Copy-only drop source for one file (規劃/22/19 §3.3).
//
// QueryContinueDrag never returns DROPEFFECT_MOVE: the source file must stay
// where it is. Escape cancels; releasing the left button drops.
class FileDropSource final : public IDropSource {
 public:
  // Returns a new object with one reference for the caller.
  static FileDropSource* Create();

  // IUnknown
  IFACEMETHODIMP QueryInterface(REFIID riid, void** result) override;
  IFACEMETHODIMP_(ULONG) AddRef() override;
  IFACEMETHODIMP_(ULONG) Release() override;

  // IDropSource
  IFACEMETHODIMP QueryContinueDrag(BOOL escape_pressed,
                                   DWORD key_state) override;
  IFACEMETHODIMP GiveFeedback(DWORD effect) override;

 private:
  FileDropSource() = default;
  ~FileDropSource() = default;

  FileDropSource(const FileDropSource&) = delete;
  FileDropSource& operator=(const FileDropSource&) = delete;

  std::atomic<ULONG> refs_{1};
};

// Runs DoDragDrop(hwnd, FileDataObject(path), FileDropSource(), COPY).
// Returns the effect the target accepted, or DROPEFFECT_NONE on cancel/failure.
// Requires OLE initialized on the calling thread (the app uses OleInitialize).
DWORD BeginFileDrag(HWND hwnd, const std::filesystem::path& path);

}  // namespace nv::platform
