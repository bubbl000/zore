// Automated checks for v2.0 S3 / 22/19 drag-out:
//   * FileDataObject (CF_HDROP + CF_UNICODETEXT) payloads and format queries
//   * FileDropSource copy-only / Escape / button-release state machine
//   * the 4 DIP drag-out trigger policy (image needs Ctrl, rail does not)
//
// No window or mouse drag is needed, so this runs headless like the other
// harnesses. DoDragDrop itself is exercised by the GUI dry-run hook and the
// manual checklist in docs/DRAG-OUT.md.

#include "platform/file_data_object.h"
#include "platform/file_drop_source.h"
#include "ui/drag_threshold.h"

#include <windows.h>
#include <oleidl.h>
#include <shellapi.h>
#include <shlobj.h>

#include <cstdio>
#include <cstring>
#include <string>

namespace {

int g_failures = 0;

void Check(bool condition, const char* name) {
  std::printf("%s %s\n", condition ? "ok  " : "FAIL", name);
  if (!condition) ++g_failures;
}

FORMATETC Format(CLIPFORMAT format) {
  FORMATETC value{};
  value.cfFormat = format;
  value.dwAspect = DVASPECT_CONTENT;
  value.lindex = -1;
  value.tymed = TYMED_HGLOBAL;
  return value;
}

// Reads the single wide path out of a CF_HDROP payload and verifies the header
// fields and the double NUL terminator.
bool ReadHdrop(HGLOBAL memory, std::wstring* path, bool* wide, bool* double_nul,
               size_t* offset) {
  if (memory == nullptr) return false;
  const auto* header = static_cast<const DROPFILES*>(GlobalLock(memory));
  if (header == nullptr) return false;
  *wide = header->fWide != FALSE;
  *offset = header->pFiles;
  const auto* bytes = reinterpret_cast<const BYTE*>(header);
  const auto* text = reinterpret_cast<const wchar_t*>(bytes + header->pFiles);
  const wchar_t* end = text;
  while (*end != L'\0') ++end;
  *path = std::wstring(text, static_cast<size_t>(end - text));
  *double_nul = end[1] == L'\0';
  GlobalUnlock(memory);
  return true;
}

void TestDataObject() {
  const std::wstring path = L"C:\\pictures\\drag sample \u56fe.png";
  nv::platform::FileDataObject* data = nv::platform::FileDataObject::Create(path);
  Check(data != nullptr, "the data object is created");

  // QueryInterface / refcount
  IDataObject* as_data = nullptr;
  Check(data->QueryInterface(IID_IDataObject, reinterpret_cast<void**>(&as_data)) ==
                S_OK &&
            as_data == data,
        "QueryInterface returns IDataObject");
  if (as_data != nullptr) as_data->Release();
  void* bogus = nullptr;
  Check(data->QueryInterface(IID_IDropSource, &bogus) == E_NOINTERFACE,
        "QueryInterface rejects unrelated interfaces");

  // Formats
  FORMATETC drop = Format(static_cast<CLIPFORMAT>(CF_HDROP));
  FORMATETC text = Format(static_cast<CLIPFORMAT>(CF_UNICODETEXT));
  Check(data->QueryGetData(&drop) == S_OK, "CF_HDROP is offered");
  Check(data->QueryGetData(&text) == S_OK, "CF_UNICODETEXT is offered");
  FORMATETC png = Format(static_cast<CLIPFORMAT>(CF_BITMAP));
  Check(data->QueryGetData(&png) == DV_E_FORMATETC, "a bitmap format is refused");
  FORMATETC wrong_index = drop;
  wrong_index.lindex = 0;
  Check(data->QueryGetData(&wrong_index) == DV_E_FORMATETC,
        "an indexed request is refused");
  FORMATETC memory_format = drop;
  memory_format.tymed = TYMED_ISTREAM;
  Check(data->QueryGetData(&memory_format) == DV_E_FORMATETC,
        "a non-HGLOBAL medium is refused");

  IEnumFORMATETC* enumerator = nullptr;
  Check(data->EnumFormatEtc(DATADIR_GET, &enumerator) == S_OK &&
            enumerator != nullptr,
        "DATADIR_GET enumerates the two formats");
  if (enumerator != nullptr) {
    FORMATETC items[4]{};
    ULONG fetched = 0;
    enumerator->Next(4, items, &fetched);
    Check(fetched == 2, "exactly two formats are enumerated");
    Check(fetched == 2 && items[0].cfFormat == CF_HDROP &&
              items[1].cfFormat == CF_UNICODETEXT,
          "the formats are CF_HDROP then CF_UNICODETEXT");
    Check(fetched == 2 && items[0].tymed == TYMED_HGLOBAL &&
              items[0].dwAspect == DVASPECT_CONTENT && items[0].lindex == -1,
          "the enumerated CF_HDROP matches the copy contract");
    Check(enumerator->Next(1, items, &fetched) == S_FALSE && fetched == 0,
          "the enumeration ends after two entries");
    enumerator->Release();
  }
  IEnumFORMATETC* set_enumerator = nullptr;
  Check(data->EnumFormatEtc(DATADIR_SET, &set_enumerator) == E_NOTIMPL,
        "DATADIR_SET is not supported (read-only source)");

  // CF_HDROP payload
  STGMEDIUM medium{};
  Check(data->GetData(&drop, &medium) == S_OK, "GetData(CF_HDROP) succeeds");
  std::wstring parsed;
  bool wide = false;
  bool double_nul = false;
  size_t offset = 0;
  Check(ReadHdrop(medium.hGlobal, &parsed, &wide, &double_nul, &offset),
        "the CF_HDROP payload can be read back");
  Check(wide, "the CF_HDROP payload is wide (fWide=TRUE)");
  Check(offset == sizeof(DROPFILES),
        "pFiles points just past the DROPFILES header");
  Check(parsed == path, "the CF_HDROP payload carries the exact path");
  Check(double_nul, "the CF_HDROP path list ends with a double NUL");
  if (medium.hGlobal != nullptr) GlobalFree(medium.hGlobal);

  // CF_UNICODETEXT payload
  STGMEDIUM text_medium{};
  Check(data->GetData(&text, &text_medium) == S_OK,
        "GetData(CF_UNICODETEXT) succeeds");
  Check(text_medium.tymed == TYMED_HGLOBAL && text_medium.hGlobal != nullptr,
        "the text medium is an HGLOBAL");
  if (text_medium.hGlobal != nullptr) {
    const auto* value = static_cast<const wchar_t*>(GlobalLock(text_medium.hGlobal));
    Check(value != nullptr && path == value,
          "the text payload carries the exact path");
    if (value != nullptr) GlobalUnlock(text_medium.hGlobal);
    GlobalFree(text_medium.hGlobal);
  }

  // Each GetData call owns its own allocation.
  STGMEDIUM again{};
  Check(data->GetData(&drop, &again) == S_OK &&
            again.hGlobal != nullptr,
        "GetData can be called again");
  if (again.hGlobal != nullptr) GlobalFree(again.hGlobal);

  // GetDataHere / SetData / advise are unsupported.
  STGMEDIUM here{};
  Check(data->GetDataHere(&drop, &here) == E_NOTIMPL, "GetDataHere is not supported");
  STGMEDIUM incoming{};
  Check(data->SetData(&drop, &incoming, FALSE) == E_NOTIMPL,
        "SetData is refused (we are a source)");
  DWORD connection = 0;
  Check(data->DAdvise(&drop, 0, nullptr, &connection) == OLE_E_ADVISENOTSUPPORTED,
        "DAdvise is not supported");

  // Null arguments must not crash.
  Check(data->GetData(nullptr, &medium) == E_INVALIDARG,
        "GetData(null) returns E_INVALIDARG");
  Check(data->QueryGetData(nullptr) == E_INVALIDARG,
        "QueryGetData(null) returns E_INVALIDARG");

  data->Release();
}

void TestDropSource() {
  nv::platform::FileDropSource* source = nv::platform::FileDropSource::Create();
  Check(source != nullptr, "the drop source is created");
  IDropSource* as_source = nullptr;
  Check(source->QueryInterface(IID_IDropSource,
                               reinterpret_cast<void**>(&as_source)) == S_OK &&
            as_source == source,
        "QueryInterface returns IDropSource");
  if (as_source != nullptr) as_source->Release();

  Check(source->QueryContinueDrag(TRUE, MK_LBUTTON) == DRAGDROP_S_CANCEL,
        "Escape cancels the drag");
  Check(source->QueryContinueDrag(FALSE, 0) == DRAGDROP_S_DROP,
        "releasing the left button drops");
  Check(source->QueryContinueDrag(FALSE, MK_LBUTTON) == S_OK,
        "holding the left button keeps dragging");
  Check(source->QueryContinueDrag(FALSE, MK_RBUTTON) == DRAGDROP_S_DROP,
        "without the left button the drag completes");
  Check(source->GiveFeedback(DROPEFFECT_COPY) == DRAGDROP_S_USEDEFAULTCURSORS,
        "feedback uses the system copy cursor");
  Check(source->GiveFeedback(DROPEFFECT_NONE) == DRAGDROP_S_USEDEFAULTCURSORS,
        "feedback uses the system no-drop cursor");
  source->Release();
}

void TestThreshold() {
  using nv::ui::DragOutRequest;
  using nv::ui::ExceedsDragThreshold;
  using nv::ui::ShouldStartDragOut;

  Check(!ExceedsDragThreshold(2.0f, 2.0f, 4.0f),
        "a 2.8 px move is below the 4 DIP threshold");
  Check(ExceedsDragThreshold(4.0f, 0.0f, 4.0f),
        "exactly 4 px meets the threshold");
  Check(ExceedsDragThreshold(0.0f, 5.0f, 4.0f),
        "5 px vertically meets the threshold");
  Check(ExceedsDragThreshold(3.0f, 3.0f, 4.0f),
        "a 4.24 px diagonal exceeds the threshold");

  DragOutRequest request;
  request.image_area = true;
  request.ctrl = true;
  request.dx = 10.0f;
  request.threshold = 4.0f;
  Check(ShouldStartDragOut(request),
        "Ctrl + image drag starts a file drag");

  request.ctrl = false;
  Check(!ShouldStartDragOut(request),
        "plain image drag stays a pan (no file drag)");

  request.ctrl = true;
  request.dx = 1.0f;
  Check(!ShouldStartDragOut(request),
        "Ctrl + a tiny move is still a click (no drag)");

  // V3.1-06 (§06b): the rail is no longer a drag-out source.
  DragOutRequest strip;
  strip.strip_item = true;
  strip.dx = 10.0f;
  strip.threshold = 4.0f;
  Check(!ShouldStartDragOut(strip),
        "a rail drag never starts a file drag (V3.1-06)");
  strip.ctrl = true;
  Check(!ShouldStartDragOut(strip),
        "Ctrl does not re-enable the rail drag either");

  DragOutRequest neither;
  neither.dx = 100.0f;
  Check(!ShouldStartDragOut(neither),
        "a press outside the image and rail never starts a drag");

  DragOutRequest scaled;
  scaled.image_area = true;
  scaled.ctrl = true;
  scaled.dx = 6.0f;
  scaled.threshold = 5.0f;
  Check(ShouldStartDragOut(scaled),
        "the threshold follows the DPI-scaled value");
  scaled.dx = 4.0f;
  Check(!ShouldStartDragOut(scaled),
        "a 4 px move is below a 5 px scaled threshold");

  // V3.1-06: leaving the window by 16 DIP (after the 4 DIP movement threshold)
  // starts a file drag without Ctrl.
  DragOutRequest leave;
  leave.image_area = true;
  leave.dx = 40.0f;
  leave.outside_dip = 15.0f;
  Check(!ShouldStartDragOut(leave),
        "15 DIP outside the window is still a pan");
  leave.outside_dip = 16.0f;
  Check(ShouldStartDragOut(leave),
        "16 DIP outside the window starts a file drag");
  leave.dx = 1.0f;
  Check(!ShouldStartDragOut(leave),
        "leaving the window needs the 4 DIP movement threshold too");
  leave.dx = 40.0f;
  leave.outside_dip = 0.0f;
  Check(!ShouldStartDragOut(leave),
        "a large move inside the window stays a pan");
}

}  // namespace

int main() {
  // OLE is not required for direct IDataObject calls, but the drop source tests
  // compare against OLE return codes only.
  TestDataObject();
  TestDropSource();
  TestThreshold();
  if (g_failures != 0) {
    std::printf("%d drag-out check(s) failed\n", g_failures);
    return 1;
  }
  std::printf("all drag-out checks passed\n");
  return 0;
}
