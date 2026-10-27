#include "platform/file_drop_source.h"

#include "platform/file_data_object.h"
#include "platform/log.h"

#include <cstdio>

namespace nv::platform {

FileDropSource* FileDropSource::Create() { return new FileDropSource(); }

IFACEMETHODIMP FileDropSource::QueryInterface(REFIID riid, void** result) {
  if (result == nullptr) return E_POINTER;
  *result = nullptr;
  if (riid == IID_IUnknown || riid == IID_IDropSource) {
    *result = static_cast<IDropSource*>(this);
    AddRef();
    return S_OK;
  }
  return E_NOINTERFACE;
}

IFACEMETHODIMP_(ULONG) FileDropSource::AddRef() {
  return refs_.fetch_add(1, std::memory_order_relaxed) + 1;
}

IFACEMETHODIMP_(ULONG) FileDropSource::Release() {
  const ULONG remaining = refs_.fetch_sub(1, std::memory_order_acq_rel) - 1;
  if (remaining == 0) delete this;
  return remaining;
}

IFACEMETHODIMP FileDropSource::QueryContinueDrag(BOOL escape_pressed,
                                                 DWORD key_state) {
  if (escape_pressed) return DRAGDROP_S_CANCEL;
  if ((key_state & MK_LBUTTON) == 0) return DRAGDROP_S_DROP;
  return S_OK;
}

IFACEMETHODIMP FileDropSource::GiveFeedback(DWORD) {
  // The default OLE cursors already show the copy badge for DROPEFFECT_COPY and
  // the "no drop" cursor for DROPEFFECT_NONE; do not override them.
  return DRAGDROP_S_USEDEFAULTCURSORS;
}

DWORD BeginFileDrag(HWND hwnd, const std::filesystem::path& path) {
  if (hwnd == nullptr || path.empty()) return DROPEFFECT_NONE;

  FileDataObject* data = FileDataObject::Create(path);
  FileDropSource* source = FileDropSource::Create();
  if (data == nullptr || source == nullptr) {
    if (data != nullptr) data->Release();
    if (source != nullptr) source->Release();
    return DROPEFFECT_NONE;
  }

  DWORD effect = DROPEFFECT_NONE;
  const HRESULT hr =
      DoDragDrop(data, source, DROPEFFECT_COPY, &effect);
  if (FAILED(hr)) {
    char line[96]{};
    std::snprintf(line, sizeof(line), "drag: DoDragDrop failed hr=0x%08lX",
                  static_cast<unsigned long>(hr));
    LogError(line);
    effect = DROPEFFECT_NONE;
  }
  // COPY only: even a misbehaving target must not turn this into a move.
  if ((effect & DROPEFFECT_MOVE) != 0 && (effect & DROPEFFECT_COPY) == 0) {
    LogWarn("drag: target asked for MOVE; reporting NONE");
    effect = DROPEFFECT_NONE;
  }

  source->Release();
  data->Release();
  return effect;
}

}  // namespace nv::platform
