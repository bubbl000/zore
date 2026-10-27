#include "platform/file_data_object.h"

#include <shellapi.h>
#include <shlobj.h>
#include <shlwapi.h>

#include <cstring>
#include <utility>

namespace nv::platform {
namespace {

// CF_HDROP is CFSTR_SHELLIDLIST's file-list sibling; both are registered by
// OLE/Shell. CF_HDROP (15) is a standard clipboard format, so no registration
// is needed.
constexpr CLIPFORMAT kDropFiles = static_cast<CLIPFORMAT>(CF_HDROP);

FORMATETC MakeFormat(CLIPFORMAT format) {
  FORMATETC value{};
  value.cfFormat = format;
  value.ptd = nullptr;
  value.dwAspect = DVASPECT_CONTENT;
  value.lindex = -1;
  value.tymed = TYMED_HGLOBAL;
  return value;
}

}  // namespace

FileDataObject* FileDataObject::Create(const std::filesystem::path& path) {
  return new FileDataObject(path);
}

FileDataObject::FileDataObject(std::filesystem::path path)
    : path_(std::move(path).wstring()) {}

FileDataObject::~FileDataObject() = default;

IFACEMETHODIMP FileDataObject::QueryInterface(REFIID riid, void** result) {
  if (result == nullptr) return E_POINTER;
  *result = nullptr;
  if (riid == IID_IUnknown || riid == IID_IDataObject) {
    *result = static_cast<IDataObject*>(this);
    AddRef();
    return S_OK;
  }
  return E_NOINTERFACE;
}

IFACEMETHODIMP_(ULONG) FileDataObject::AddRef() {
  return refs_.fetch_add(1, std::memory_order_relaxed) + 1;
}

IFACEMETHODIMP_(ULONG) FileDataObject::Release() {
  const ULONG remaining = refs_.fetch_sub(1, std::memory_order_acq_rel) - 1;
  if (remaining == 0) delete this;
  return remaining;
}

bool FileDataObject::MatchesFormat(const FORMATETC& format) {
  if (format.cfFormat != kDropFiles &&
      format.cfFormat != static_cast<CLIPFORMAT>(CF_UNICODETEXT)) {
    return false;
  }
  return format.dwAspect == DVASPECT_CONTENT && format.lindex == -1 &&
         (format.tymed & TYMED_HGLOBAL) != 0;
}

HGLOBAL FileDataObject::CreateHdrop(const std::wstring& path) {
  // DROPFILES header + path + terminating NUL + list-terminating NUL.
  const size_t bytes = sizeof(DROPFILES) + (path.size() + 2) * sizeof(wchar_t);
  HGLOBAL memory = GlobalAlloc(GHND, bytes);
  if (memory == nullptr) return nullptr;
  auto* header = static_cast<DROPFILES*>(GlobalLock(memory));
  if (header == nullptr) {
    GlobalFree(memory);
    return nullptr;
  }
  header->pFiles = static_cast<DWORD>(sizeof(DROPFILES));
  header->fWide = TRUE;
  auto* text = reinterpret_cast<wchar_t*>(
      reinterpret_cast<BYTE*>(header) + sizeof(DROPFILES));
  std::memcpy(text, path.c_str(), (path.size() + 1) * sizeof(wchar_t));
  text[path.size() + 1] = L'\0';
  GlobalUnlock(memory);
  return memory;
}

HGLOBAL FileDataObject::CreateUnicodeText(const std::wstring& path) {
  const size_t bytes = (path.size() + 1) * sizeof(wchar_t);
  HGLOBAL memory = GlobalAlloc(GHND, bytes);
  if (memory == nullptr) return nullptr;
  void* target = GlobalLock(memory);
  if (target == nullptr) {
    GlobalFree(memory);
    return nullptr;
  }
  std::memcpy(target, path.c_str(), bytes);
  GlobalUnlock(memory);
  return memory;
}

IFACEMETHODIMP FileDataObject::GetData(FORMATETC* format, STGMEDIUM* medium) {
  if (format == nullptr || medium == nullptr) return E_INVALIDARG;
  if (!MatchesFormat(*format)) return DV_E_FORMATETC;

  HGLOBAL payload = nullptr;
  if (format->cfFormat == kDropFiles) {
    payload = CreateHdrop(path_);
  } else {
    payload = CreateUnicodeText(path_);
  }
  if (payload == nullptr) return STG_E_MEDIUMFULL;

  medium->tymed = TYMED_HGLOBAL;
  medium->hGlobal = payload;
  medium->pUnkForRelease = nullptr;
  return S_OK;
}

IFACEMETHODIMP FileDataObject::GetDataHere(FORMATETC*, STGMEDIUM*) {
  return E_NOTIMPL;
}

IFACEMETHODIMP FileDataObject::QueryGetData(FORMATETC* format) {
  if (format == nullptr) return E_INVALIDARG;
  return MatchesFormat(*format) ? S_OK : DV_E_FORMATETC;
}

IFACEMETHODIMP FileDataObject::GetCanonicalFormatEtc(FORMATETC* format,
                                                     FORMATETC* output) {
  if (output == nullptr) return E_INVALIDARG;
  *output = format != nullptr ? *format : FORMATETC{};
  output->ptd = nullptr;
  return DATA_S_SAMEFORMATETC;
}

IFACEMETHODIMP FileDataObject::SetData(FORMATETC*, STGMEDIUM*, BOOL) {
  // Read-only source: a target must never be able to write back into us.
  return E_NOTIMPL;
}

IFACEMETHODIMP FileDataObject::EnumFormatEtc(DWORD direction,
                                             IEnumFORMATETC** enumerator) {
  if (enumerator == nullptr) return E_POINTER;
  *enumerator = nullptr;
  if (direction != DATADIR_GET) return E_NOTIMPL;

  const FORMATETC formats[2] = {MakeFormat(kDropFiles),
                                MakeFormat(CF_UNICODETEXT)};
  return SHCreateStdEnumFmtEtc(2, formats, enumerator);
}

IFACEMETHODIMP FileDataObject::DAdvise(FORMATETC*, DWORD, IAdviseSink*,
                                       DWORD*) {
  return OLE_E_ADVISENOTSUPPORTED;
}

IFACEMETHODIMP FileDataObject::DUnadvise(DWORD) {
  return OLE_E_ADVISENOTSUPPORTED;
}

IFACEMETHODIMP FileDataObject::EnumDAdvise(IEnumSTATDATA**) {
  return OLE_E_ADVISENOTSUPPORTED;
}

}  // namespace nv::platform
