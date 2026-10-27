#pragma once

#include <windows.h>
#include <objidl.h>
#include <oleidl.h>

#include <atomic>
#include <filesystem>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace nv::platform {

class DropTarget;

// Releases one app reference; the object deletes itself when OLE is done too.
struct DropTargetReleaser {
  void operator()(DropTarget* target) const noexcept;
};
using DropTargetPtr = std::unique_ptr<DropTarget, DropTargetReleaser>;

// OLE drop target for the main window.
//
// WM_DROPFILES only delivers paths after the mouse is released, so "a file is
// hovering the window" cannot be drawn with it. OLE's IDropTarget provides
// DragEnter/DragOver/DragLeave and is the single drop path from now on
// (規劃/22/03 §4.1; DragAcceptFiles must not stay registered next to it).
//
// RegisterDragDrop and DoDragDrop need OLE initialized on the owning thread
// (OleInitialize), which is why the app uses OleInitialize instead of a bare
// CoInitializeEx.
class DropTarget final : public IDropTarget {
 public:
  using DropHandler =
      std::function<void(const std::vector<std::filesystem::path>&)>;
  using HoverHandler = std::function<void(bool hovering)>;

  DropTarget() = default;
  DropTarget(const DropTarget&) = delete;
  DropTarget& operator=(const DropTarget&) = delete;

  // Registers on `hwnd`. Returns false with a technical (English) message when
  // OLE was not initialized or the window is already registered.
  bool Register(HWND hwnd, DropHandler on_drop, HoverHandler on_hover,
                std::wstring* error);
  // Revokes the registration. Safe to call twice; call it before destroying
  // the window. The owner then resets its DropTargetPtr to drop its own
  // reference (OLE may still hold one while a drop is in flight).
  void Revoke();

  // IUnknown
  IFACEMETHODIMP QueryInterface(REFIID riid, void** result) override;
  IFACEMETHODIMP_(ULONG) AddRef() override;
  IFACEMETHODIMP_(ULONG) Release() override;

  // IDropTarget
  IFACEMETHODIMP DragEnter(IDataObject* data, DWORD key_state, POINTL point,
                           DWORD* effect) override;
  IFACEMETHODIMP DragOver(DWORD key_state, POINTL point, DWORD* effect) override;
  IFACEMETHODIMP DragLeave() override;
  IFACEMETHODIMP Drop(IDataObject* data, DWORD key_state, POINTL point,
                      DWORD* effect) override;

 private:
  ~DropTarget();

  // CF_HDROP -> paths; false when the data object has no file list.
  static bool ReadPaths(IDataObject* data,
                        std::vector<std::filesystem::path>* paths);

  void SetHovering(bool hovering);

  std::atomic<ULONG> refs_{1};
  HWND hwnd_ = nullptr;
  bool registered_ = false;
  bool hovering_ = false;
  DropHandler on_drop_;
  HoverHandler on_hover_;
};

}  // namespace nv::platform
