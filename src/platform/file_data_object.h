#pragma once

#include <windows.h>
#include <objidl.h>
#include <oleidl.h>

#include <atomic>
#include <filesystem>
#include <string>

namespace nv::platform {

// Read-only drag source data for one file (規劃/22/19 §3.2).
//
// Formats:
//   CF_HDROP       DROPFILES header + wide absolute path + double NUL
//   CF_UNICODETEXT the same path as plain text
//
// The object never accepts SetData: we are a source, not a drop target. Every
// GetData call allocates a fresh HGLOBAL because the caller owns it.
class FileDataObject final : public IDataObject {
 public:
  // Returns a new object with one reference for the caller.
  static FileDataObject* Create(const std::filesystem::path& path);

  // Exposed for tests: one CF_HDROP payload. Caller owns the HGLOBAL.
  static HGLOBAL CreateHdrop(const std::wstring& path);
  // Exposed for tests: one CF_UNICODETEXT payload. Caller owns the HGLOBAL.
  static HGLOBAL CreateUnicodeText(const std::wstring& path);

  // IUnknown
  IFACEMETHODIMP QueryInterface(REFIID riid, void** result) override;
  IFACEMETHODIMP_(ULONG) AddRef() override;
  IFACEMETHODIMP_(ULONG) Release() override;

  // IDataObject
  IFACEMETHODIMP GetData(FORMATETC* format, STGMEDIUM* medium) override;
  IFACEMETHODIMP GetDataHere(FORMATETC* format, STGMEDIUM* medium) override;
  IFACEMETHODIMP QueryGetData(FORMATETC* format) override;
  IFACEMETHODIMP GetCanonicalFormatEtc(FORMATETC* format,
                                       FORMATETC* output) override;
  IFACEMETHODIMP SetData(FORMATETC* format, STGMEDIUM* medium,
                         BOOL release) override;
  IFACEMETHODIMP EnumFormatEtc(DWORD direction,
                               IEnumFORMATETC** enumerator) override;
  IFACEMETHODIMP DAdvise(FORMATETC* format, DWORD flags, IAdviseSink* sink,
                         DWORD* connection) override;
  IFACEMETHODIMP DUnadvise(DWORD connection) override;
  IFACEMETHODIMP EnumDAdvise(IEnumSTATDATA** enumerator) override;

 private:
  explicit FileDataObject(std::filesystem::path path);
  ~FileDataObject();

  FileDataObject(const FileDataObject&) = delete;
  FileDataObject& operator=(const FileDataObject&) = delete;

  static bool MatchesFormat(const FORMATETC& format);

  std::atomic<ULONG> refs_{1};
  std::wstring path_;
};

}  // namespace nv::platform
