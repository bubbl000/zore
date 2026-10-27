#include "platform/drag_drop.h"

#include <shellapi.h>
#include <shlobj.h>

#include <algorithm>

namespace nv::platform {
namespace {

std::wstring HResultText(const wchar_t* step, HRESULT hr) {
  wchar_t buffer[160]{};
  swprintf_s(buffer, L"%s failed (HRESULT=0x%08lX)", step,
             static_cast<unsigned long>(hr));
  return buffer;
}

}  // namespace

void DropTargetReleaser::operator()(DropTarget* target) const noexcept {
  if (target != nullptr) target->Release();
}

DropTarget::~DropTarget() = default;

bool DropTarget::Register(HWND hwnd, DropHandler on_drop, HoverHandler on_hover,
                          std::wstring* error) {
  if (hwnd == nullptr) {
    if (error != nullptr) *error = L"RegisterDragDrop failed: no window";
    return false;
  }
  if (registered_) {
    if (error != nullptr) *error = L"RegisterDragDrop failed: already registered";
    return false;
  }
  hwnd_ = hwnd;
  on_drop_ = std::move(on_drop);
  on_hover_ = std::move(on_hover);

  const HRESULT hr = RegisterDragDrop(hwnd, this);
  if (FAILED(hr)) {
    hwnd_ = nullptr;
    if (error != nullptr) {
      *error = HResultText(L"RegisterDragDrop", hr);
    }
    return false;
  }
  registered_ = true;
  return true;
}

void DropTarget::Revoke() {
  if (!registered_) return;
  registered_ = false;
  hovering_ = false;
  RevokeDragDrop(hwnd_);
  hwnd_ = nullptr;
  on_drop_ = nullptr;
  on_hover_ = nullptr;
  // The owner's DropTargetPtr releases its reference when it is reset; if OLE
  // still holds one (drop in flight) the object lives until that arrives.
}

IFACEMETHODIMP DropTarget::QueryInterface(REFIID riid, void** result) {
  if (result == nullptr) return E_POINTER;
  *result = nullptr;
  if (riid == IID_IUnknown || riid == IID_IDropTarget) {
    *result = static_cast<IDropTarget*>(this);
    AddRef();
    return S_OK;
  }
  return E_NOINTERFACE;
}

IFACEMETHODIMP_(ULONG) DropTarget::AddRef() {
  return refs_.fetch_add(1, std::memory_order_acq_rel) + 1;
}

IFACEMETHODIMP_(ULONG) DropTarget::Release() {
  const ULONG remaining =
      refs_.fetch_sub(1, std::memory_order_acq_rel) - 1;
  if (remaining == 0) delete this;
  return remaining;
}

bool DropTarget::ReadPaths(IDataObject* data,
                           std::vector<std::filesystem::path>* paths) {
  if (data == nullptr || paths == nullptr) return false;
  FORMATETC format{CF_HDROP, nullptr, DVASPECT_CONTENT, -1, TYMED_HGLOBAL};
  STGMEDIUM medium{};
  if (FAILED(data->GetData(&format, &medium))) return false;

  bool ok = false;
  const HDROP drop = static_cast<HDROP>(medium.hGlobal);
  if (drop != nullptr) {
    const UINT count = DragQueryFileW(drop, 0xFFFFFFFFu, nullptr, 0);
    for (UINT i = 0; i < count; ++i) {
      const UINT length = DragQueryFileW(drop, i, nullptr, 0);
      if (length == 0) continue;
      std::wstring text(length + 1, L'\0');
      if (DragQueryFileW(drop, i, text.data(), length + 1) == 0) continue;
      text.resize(length);
      paths->emplace_back(text);
    }
    ok = !paths->empty();
  }
  ReleaseStgMedium(&medium);
  return ok;
}

void DropTarget::SetHovering(bool hovering) {
  if (hovering_ == hovering) return;
  hovering_ = hovering;
  if (on_hover_) on_hover_(hovering);
}

IFACEMETHODIMP DropTarget::DragEnter(IDataObject* data, DWORD /*key_state*/,
                                     POINTL /*point*/, DWORD* effect) {
  if (effect == nullptr) return E_POINTER;
  std::vector<std::filesystem::path> paths;
  const bool has_files = ReadPaths(data, &paths);
  SetHovering(has_files);
  *effect = has_files ? DROPEFFECT_COPY : DROPEFFECT_NONE;
  return S_OK;
}

IFACEMETHODIMP DropTarget::DragOver(DWORD /*key_state*/, POINTL /*point*/,
                                    DWORD* effect) {
  if (effect == nullptr) return E_POINTER;
  // The app repaints on the hover transitions only, so a per-frame DragOver
  // costs nothing (規劃/22/03 坑 3).
  *effect = hovering_ ? DROPEFFECT_COPY : DROPEFFECT_NONE;
  return S_OK;
}

IFACEMETHODIMP DropTarget::DragLeave() {
  SetHovering(false);
  return S_OK;
}

IFACEMETHODIMP DropTarget::Drop(IDataObject* data, DWORD /*key_state*/,
                                POINTL /*point*/, DWORD* effect) {
  if (effect == nullptr) return E_POINTER;
  SetHovering(false);
  std::vector<std::filesystem::path> paths;
  if (!ReadPaths(data, &paths)) {
    *effect = DROPEFFECT_NONE;
    return S_OK;
  }
  *effect = DROPEFFECT_COPY;
  if (on_drop_) on_drop_(paths);
  return S_OK;
}

}  // namespace nv::platform
