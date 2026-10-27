// 「另存为」：资源管理器式系统对话框 + **内嵌**选项区（质量滑条 / 文件大小）。
//
// 为什么不是 IFileSaveDialog：`IFileDialogCustomize` 只能加下拉/编辑框/按钮/文本，
// **没有滑条**（旧实现只能放一个质量下拉）。经典 `GetSaveFileNameW` + `OFN_EXPLORER`
// 是资源管理器式的（左侧位置栏、右侧图标列表、新建文件夹），而 `OFN_ENABLETEMPLATE`
// 允许把一个子对话框模板**内嵌**到它底部 —— 滑条就真的存在。
//
// 做法：模板只是空白
// WS_CHILD 面板，控件在 WM_INITDIALOG 里用 CreateWindowEx 建（文案跟随 i18n）；
// 给对话框补 WS_THICKFRAME 并子类化做边框判定；文件类型下拉没有变更通知 → 250 ms
// 定时器轮询，刷新"文件大小"并按格式显示/隐藏质量整组。
//
// 注意：`OFN_EXPLORER` 得到的是**经典**资源管理器式对话框（Vista 之前的布局），
// 不是 IFileSaveDialog 的现代视图。这是换取滑条的代价，用户明确选了这个外观。
#include "platform/save_dialog.h"

#include "platform/encoder_probe.h"
#include "platform/i18n.h"
#include "platform/log.h"

#include <commctrl.h>
#include <commdlg.h>
#include <windows.h>
#include <windowsx.h>

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cwchar>
#include <filesystem>
#include <string>
#include <vector>

namespace nv::platform {
namespace {

// Must match src/app/version.rc.
constexpr int kTemplateId = 203;
constexpr int kQualityId = 1004;
constexpr int kQualityLabelId = 1010;
constexpr int kQualityValueId = 1005;
constexpr int kSizeLabelId = 1011;
constexpr int kSizeValueId = 1006;
constexpr int kRecommendedQuality = 90;

struct PanelState {
  OPENFILENAMEW* ofn = nullptr;
  std::vector<ImageSaveFormat> formats;
  bool has_original = false;
  ImageSaveFormat source_format = ImageSaveFormat::kPng;
  bool source_unmodified = false;
  uint64_t source_bytes = 0;
  int quality = kRecommendedQuality;
  bool byte_copy = false;
  // Size estimate (规划/22/14 §4.2): a strided proxy encode scaled back up.
  const Image* image = nullptr;
  ImageSaveFormat last_format = ImageSaveFormat::kPng;
  int last_quality = -1;
  uint64_t estimate_bytes = 0;
  bool estimate_exact = false;
  bool script_accept = false;
  bool script_cancel = false;
};

// NV_SAVEAS test hook: `type=jpg;quality=40;original=1;accept=1` — the grammar the
// v3.1-06c script already used. Type/quality are applied before the dialog is shown;
// accept/cancel press the real buttons afterwards, so the test exercises the
// production path instead of a private shortcut.
struct DialogScript {
  std::wstring type;
  int quality = -1;
  bool original = false;
  bool accept = false;
  bool cancel = false;
};

DialogScript ReadDialogScript() {
  DialogScript script;
  wchar_t buffer[512]{};
  const DWORD got = GetEnvironmentVariableW(L"NV_SAVEAS", buffer, 512);
  if (got == 0 || got >= 512) return script;
  const std::wstring text(buffer);
  size_t start = 0;
  while (start <= text.size()) {
    const size_t semi = text.find(L';', start);
    const std::wstring token = text.substr(
        start, semi == std::wstring::npos ? std::wstring::npos : semi - start);
    const size_t eq = token.find(L'=');
    if (eq != std::wstring::npos) {
      const std::wstring key = token.substr(0, eq);
      const std::wstring value = token.substr(eq + 1);
      if (key == L"type") {
        script.type = value;
      } else if (key == L"quality") {
        script.quality = std::clamp(_wtoi(value.c_str()), 1, 100);
      } else if (key == L"original") {
        script.original = value == L"1";
      } else if (key == L"accept") {
        script.accept = value == L"1";
      } else if (key == L"cancel") {
        script.cancel = value == L"1";
      }
    }
    if (semi == std::wstring::npos) break;
    start = semi + 1;
  }
  return script;
}

PanelState* g_panel_state = nullptr;

bool IsLossy(ImageSaveFormat format) {
  return format == ImageSaveFormat::kJpeg || format == ImageSaveFormat::kWebp;
}

ImageSaveFormat SelectedFormat(const PanelState& state) {
  const DWORD index = state.ofn != nullptr ? state.ofn->nFilterIndex : 1;
  if (state.has_original && index <= 1) return state.source_format;
  const size_t real = index - (state.has_original ? 2u : 1u);
  if (state.formats.empty() || real >= state.formats.size()) {
    return state.formats.empty() ? ImageSaveFormat::kPng : state.formats.front();
  }
  return state.formats[real];
}

bool UsesByteCopy(const PanelState& state) {
  return state.has_original && state.source_unmodified &&
         (state.ofn == nullptr || state.ofn->nFilterIndex <= 1);
}

HWND MakeChild(HWND panel, const wchar_t* cls, const wchar_t* text, DWORD style,
               int x, int y, int w, int h, int id) {
  HWND child = CreateWindowExW(
      0, cls, text, WS_CHILD | WS_VISIBLE | style, x, y, w, h, panel,
      reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)), GetModuleHandleW(nullptr),
      nullptr);
  if (child != nullptr) {
    SendMessageW(child, WM_SETFONT, SendMessageW(panel, WM_GETFONT, 0, 0), TRUE);
  }
  return child;
}

std::wstring FormatBytes(uint64_t bytes) {
  wchar_t buffer[64]{};
  if (bytes >= 1024ull * 1024ull) {
    std::swprintf(buffer, 64, L"%.1f MB",
                  static_cast<double>(bytes) / (1024.0 * 1024.0));
  } else if (bytes >= 1024ull) {
    std::swprintf(buffer, 64, L"%.0f KB", static_cast<double>(bytes) / 1024.0);
  } else {
    std::swprintf(buffer, 64, L"%llu B",
                  static_cast<unsigned long long>(bytes));
  }
  return buffer;
}

// 体积估算（规划/22/14 §4.2）：把画面按步长抽样缩到 <=480 px，用同一个编码器编到
// 内存，再按像素比放大。JPEG/WebP 误差 ~10%，可在毫秒级完成；无损格式的压缩率跟细节
// 强相关，缩略图比例不可靠，因此只显示"—"。
uint64_t EstimateBytes(PanelState& state, ImageSaveFormat format, int quality) {
  state.estimate_exact = false;
  if (state.byte_copy) {
    state.estimate_exact = state.source_bytes != 0;
    return state.source_bytes;
  }
  if (state.image == nullptr || state.image->Empty()) return 0;
  if (format != ImageSaveFormat::kJpeg && format != ImageSaveFormat::kWebp) return 0;
  const uint32_t width = state.image->info.width;
  const uint32_t height = state.image->info.height;
  if (width == 0 || height == 0) return 0;
  // `Image::pixels` is frame 0; `frames` is populated only for animations.
  const uint8_t* bgra = state.image->pixels.empty() ? nullptr
                                                   : state.image->pixels.data();
  if (bgra == nullptr) return 0;
  const uint32_t long_edge = std::max(width, height);
  const uint32_t step = std::max(1u, long_edge / 1024u);
  const uint32_t pw = std::max(1u, width / step);
  const uint32_t ph = std::max(1u, height / step);
  const uint32_t src_stride = state.image->info.stride != 0
                                  ? state.image->info.stride
                                  : width * 4u;
  // Box-average each block (max 4x4 samples) instead of nearest: nearest keeps the
  // full high-frequency energy and makes the proxy compress far worse than the
  // real downscale (measured 1.7x too high on a 24 MP photo).
  std::vector<uint8_t> proxy(static_cast<size_t>(pw) * ph * 4u, 0);
  const uint32_t taps = std::min(4u, std::max(1u, step));
  const uint32_t tap_step = std::max(1u, step / taps);
  for (uint32_t y = 0; y < ph; ++y) {
    uint8_t* dst = proxy.data() + static_cast<size_t>(y) * pw * 4u;
    for (uint32_t x = 0; x < pw; ++x) {
      uint32_t sum[4] = {0, 0, 0, 0};
      uint32_t count = 0;
      for (uint32_t ty = 0; ty < taps; ++ty) {
        const uint32_t sy = std::min(height - 1, y * step + ty * tap_step);
        const uint8_t* row = bgra + static_cast<size_t>(sy) * src_stride;
        for (uint32_t tx = 0; tx < taps; ++tx) {
          const uint32_t sx = std::min(width - 1, x * step + tx * tap_step);
          const uint8_t* src = row + static_cast<size_t>(sx) * 4u;
          for (int c = 0; c < 4; ++c) sum[c] += src[c];
          ++count;
        }
      }
      uint8_t* out = dst + static_cast<size_t>(x) * 4u;
      for (int c = 0; c < 4; ++c) {
        out[c] = static_cast<uint8_t>(sum[c] / count);
      }
    }
  }
  std::vector<uint8_t> encoded;
  std::wstring error;
  if (!EncodeBgraToMemory(proxy.data(), pw, ph, pw * 4u, format, quality, &encoded,
                          &error)) {
    return 0;
  }
  const double ratio = static_cast<double>(width) * height /
                       (static_cast<double>(pw) * ph);
  const uint64_t total =
      static_cast<uint64_t>(static_cast<double>(encoded.size()) * ratio);
  char line[192]{};
  std::snprintf(line, sizeof(line),
                "saveas: estimate bytes=%llu proxy=%ux%u q=%d",
                static_cast<unsigned long long>(total), pw, ph, quality);
  LogInfo(line);
  return total;
}

void RefreshPanel(HWND panel, PanelState& state) {
  if (HWND trackbar = GetDlgItem(panel, kQualityId)) {
    const int pos = static_cast<int>(SendMessageW(trackbar, TBM_GETPOS, 0, 0));
    state.quality = std::clamp(pos, 1, 100);
  }
  const ImageSaveFormat format = SelectedFormat(state);
  state.byte_copy = UsesByteCopy(state);
  // 非有损格式或字节复制时，质量整组**隐藏**（不是置灰）。
  const bool lossy = IsLossy(format) && !state.byte_copy;
  const int quality_controls[] = {kQualityId, kQualityLabelId, kQualityValueId};
  for (const int id : quality_controls) {
    if (HWND control = GetDlgItem(panel, id)) {
      ShowWindow(control, lossy ? SW_SHOW : SW_HIDE);
    }
  }
  if (HWND value = GetDlgItem(panel, kQualityValueId)) {
    const std::wstring text = std::to_wstring(state.quality);
    SetWindowTextW(value, text.c_str());
  }
  // Recompute only when the inputs changed: the 250 ms timer runs constantly.
  if (format != state.last_format || state.quality != state.last_quality ||
      state.byte_copy != state.estimate_exact) {
    state.estimate_bytes = EstimateBytes(state, format, state.quality);
    state.last_format = format;
    state.last_quality = state.quality;
  }
  if (HWND size = GetDlgItem(panel, kSizeValueId)) {
    std::wstring text = L"—";
    if (state.estimate_bytes != 0) {
      text = (state.estimate_exact ? L"" : L"≈ ") +
             FormatBytes(state.estimate_bytes);
    }
    SetWindowTextW(size, text.c_str());
  }
}

// 边框拖拽：只加 WS_THICKFRAME 拖不动（系统会重设样式），所以自己判定边框。
constexpr int kResizeBorder = 6;

LRESULT CALLBACK DialogSubclass(HWND dialog, UINT message, WPARAM wparam,
                                LPARAM lparam, UINT_PTR, DWORD_PTR) {
  switch (message) {
    case WM_NCHITTEST: {
      RECT box{};
      GetWindowRect(dialog, &box);
      const int x = GET_X_LPARAM(lparam);
      const int y = GET_Y_LPARAM(lparam);
      const bool left = x >= box.left && x < box.left + kResizeBorder;
      const bool right = x <= box.right && x > box.right - kResizeBorder;
      const bool top = y >= box.top && y < box.top + kResizeBorder;
      const bool bottom = y <= box.bottom && y > box.bottom - kResizeBorder;
      if (top && left) return HTTOPLEFT;
      if (top && right) return HTTOPRIGHT;
      if (bottom && left) return HTBOTTOMLEFT;
      if (bottom && right) return HTBOTTOMRIGHT;
      if (left) return HTLEFT;
      if (right) return HTRIGHT;
      if (top) return HTTOP;
      if (bottom) return HTBOTTOM;
      break;
    }
    case WM_GETMINMAXINFO: {
      auto* info = reinterpret_cast<MINMAXINFO*>(lparam);
      if (info != nullptr) {
        info->ptMinTrackSize.x = 560;
        info->ptMinTrackSize.y = 420;
        return 0;
      }
      break;
    }
    default:
      break;
  }
  return DefSubclassProc(dialog, message, wparam, lparam);
}

void EnableResize(HWND panel) {
  HWND dialog = GetParent(panel);
  if (dialog == nullptr) return;
  const LONG_PTR style = GetWindowLongPtrW(dialog, GWL_STYLE);
  SetWindowLongPtrW(dialog, GWL_STYLE, style | WS_THICKFRAME | WS_MAXIMIZEBOX);
  SetWindowPos(dialog, nullptr, 0, 0, 0, 0,
               SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE |
                   SWP_FRAMECHANGED);
  const BOOL subclassed = SetWindowSubclass(dialog, &DialogSubclass, 1, 0);
  RECT box{};
  GetWindowRect(dialog, &box);
  const LONG_PTR applied = GetWindowLongPtrW(dialog, GWL_STYLE);
  char line[256]{};
  std::snprintf(line, sizeof(line),
                "save: resize style=%lld subclass=%d thick=%d size=%ldx%ld",
                static_cast<long long>(applied),
                subclassed ? 1 : 0,
                (applied & WS_THICKFRAME) ? 1 : 0,
                box.right - box.left, box.bottom - box.top);
  LogInfo(line);
}

UINT_PTR CALLBACK PanelProc(HWND panel, UINT message, WPARAM wparam,
                            LPARAM lparam) {
  (void)wparam;
  (void)lparam;
  PanelState* state = g_panel_state;
  switch (message) {
    case WM_INITDIALOG: {
      if (state == nullptr) return FALSE;
      MakeChild(panel, L"STATIC",
                nv::i18n::Text(nv::i18n::StringId::kSaveCardQuality), SS_LEFT, 8,
                6, 70, 14, kQualityLabelId);
      MakeChild(panel, L"STATIC", L"90", SS_LEFT, 78, 6, 30, 14, kQualityValueId);
      HWND trackbar = MakeChild(panel, TRACKBAR_CLASSW, L"",
                                TBS_HORZ | TBS_AUTOTICKS | WS_TABSTOP, 112, 2, 220,
                                26, kQualityId);
      if (trackbar != nullptr) {
        SendMessageW(trackbar, TBM_SETRANGE, TRUE, MAKELPARAM(1, 100));
        SendMessageW(trackbar, TBM_SETTICFREQ, 10, 0);
        SendMessageW(trackbar, TBM_SETPOS, TRUE, state->quality);
      }
      MakeChild(panel, L"STATIC",
                nv::i18n::Text(nv::i18n::StringId::kSaveCardEstimate), SS_LEFT, 8,
                32, 100, 14, kSizeLabelId);
      MakeChild(panel, L"STATIC", L"—", SS_LEFT, 112, 32, 100, 14, kSizeValueId);
      EnableResize(panel);
      RefreshPanel(panel, *state);
      SetTimer(panel, 1, 250, nullptr);
      // NV_SAVEAS accept/cancel: press the real buttons once the dialog settled.
      if (state->script_accept || state->script_cancel) {
        SetTimer(panel, 2, 400, nullptr);
      }
      return TRUE;
    }
    case WM_HSCROLL:
      if (state != nullptr) RefreshPanel(panel, *state);
      return TRUE;
    case WM_NOTIFY: {
      // The OFN hook is the template dialog proc, so CDN_INITDONE arrives as
      // WM_NOTIFY/OFNOTIFY *after* comdlg32 has laid the dialog out - and after
      // it re-applied its own style, which is what dropped WS_THICKFRAME and
      // left the window unresizable.
      auto* notify = reinterpret_cast<OFNOTIFY*>(lparam);
      if (notify != nullptr && notify->hdr.code == CDN_INITDONE &&
          state != nullptr) {
        EnableResize(panel);
        return TRUE;
      }
      break;
    }
    case WM_TIMER:
      if (state != nullptr) {
        if (wparam == 2) {
          KillTimer(panel, 2);
          HWND dialog = GetParent(panel);
          if (dialog != nullptr) {
            const WPARAM button = state->script_cancel ? IDCANCEL : IDOK;
            PostMessageW(dialog, WM_COMMAND, button, 0);
            LogInfo(state->script_cancel ? "saveas: scripted cancel"
                                        : "saveas: scripted accept");
          }
          return TRUE;
        }
        RefreshPanel(panel, *state);
      }
      return TRUE;
    default:
      break;
  }
  return FALSE;
}

}  // namespace

SaveAsOutcome ShowSaveAsDialog(const SaveAsRequest& request) {
  SaveAsOutcome outcome;
  if (request.image == nullptr || request.formats.empty()) return outcome;
  if (request.default_format_index >= request.formats.size()) return outcome;

  PanelState state;
  state.formats = request.formats;
  state.source_format = request.source_format;
  state.source_unmodified = request.source_unmodified;
  state.source_bytes = request.source_bytes;
  state.quality = std::clamp(request.initial_quality, 1, 100);
  state.image = request.image.get();

  // "原格式" is only offered when the source can be byte-copied verbatim.
  state.has_original = request.source_unmodified && !request.source_path.empty() &&
                       EncoderAvailable(request.source_format);
  std::wstring filter;
  if (state.has_original) {
    const wchar_t* ext = SaveFormatExtension(request.source_format);
    filter += nv::i18n::Text(nv::i18n::StringId::kSaveCardOriginal);
    filter += L" (*";
    filter += ext;
    filter += L")";
    filter.push_back(L'\0');
    filter += L"*";
    filter += ext;
    filter.push_back(L'\0');
  }
  filter += request.filter;

  // Pass the FULL path, not just the leaf name: with OFN_EXPLORER the shell keeps a
  // per-app "last folder" MRU and may start there instead of lpstrInitialDir, which
  // silently sent saves to a stale directory.
  std::wstring file_name = request.suggested_path.wstring();
  if (file_name.empty()) file_name = L"image";
  file_name.resize(1024, L'\0');

  const std::wstring title = request.title;
  const std::wstring default_extension =
      SaveFormatExtension(request.formats[request.default_format_index]);
  OPENFILENAMEW ofn{};
  ofn.lStructSize = sizeof(ofn);
  ofn.hwndOwner = static_cast<HWND>(request.owner);
  ofn.lpstrFilter = filter.c_str();
  ofn.nFilterIndex = static_cast<DWORD>(request.default_format_index) +
                     (state.has_original ? 2u : 1u);
  ofn.lpstrFile = file_name.data();
  ofn.nMaxFile = static_cast<DWORD>(file_name.size());
  ofn.lpstrInitialDir = request.suggested_path.parent_path().empty()
                            ? nullptr
                            : request.suggested_path.parent_path().c_str();
  ofn.lpstrTitle = title.empty() ? nullptr : title.c_str();
  // lpstrDefExt must skip the leading dot and stay alive for the whole call.
  ofn.lpstrDefExt = default_extension.c_str() + 1;
  ofn.Flags = OFN_EXPLORER | OFN_OVERWRITEPROMPT | OFN_PATHMUSTEXIST |
              OFN_ENABLETEMPLATE | OFN_ENABLEHOOK | OFN_NOCHANGEDIR;
  ofn.lpTemplateName = MAKEINTRESOURCEW(kTemplateId);
  ofn.lpfnHook = &PanelProc;
  ofn.hInstance = GetModuleHandleW(nullptr);

  // NV_SAVEAS: type/quality must be applied *before* GetSaveFileNameW (changing
  // nFilterIndex afterwards has no effect), accept/cancel are pressed later.
  const DialogScript script = ReadDialogScript();
  if (!script.type.empty()) {
    for (size_t i = 0; i < state.formats.size(); ++i) {
      const wchar_t* ext = SaveFormatExtension(state.formats[i]);
      if (ext != nullptr && _wcsicmp(ext + 1, script.type.c_str()) == 0) {
        ofn.nFilterIndex = static_cast<DWORD>(i) + (state.has_original ? 2u : 1u);
        break;
      }
    }
  }
  if (script.original && state.has_original) ofn.nFilterIndex = 1;
  if (script.quality > 0) state.quality = script.quality;
  state.script_accept = script.accept;
  state.script_cancel = script.cancel;

  state.ofn = &ofn;
  g_panel_state = &state;
  const BOOL accepted = GetSaveFileNameW(&ofn);
  g_panel_state = nullptr;
  if (!accepted) {
    LogInfo("saveas: cancelled");
    return outcome;
  }

  const ImageSaveFormat selected = SelectedFormat(state);
  outcome.accepted = true;
  outcome.format = selected;
  outcome.quality = state.quality;
  outcome.byte_copy = state.byte_copy;
  std::filesystem::path path(file_name.c_str());
  // The name must match the bytes: a byte copy keeps the SOURCE extension, an
  // encode uses the chosen container's. (Mismatching them made the copy land on
  // a path the caller never asked for.)
  const wchar_t* extension = state.byte_copy && !request.source_path.empty()
                                  ? request.source_path.extension().c_str()
                                  : SaveFormatExtension(selected);
  if (extension != nullptr && extension[0] != L'\0') {
    path.replace_extension(extension);
  }
  outcome.path = path;
  // The outcome carries the estimate the panel showed (the byte-copy case is
  // exact, everything else is the proxy figure the dialog displayed).
  outcome.estimated_bytes = state.estimate_bytes;
  char line[512]{};
  std::snprintf(line, sizeof(line),
                "saveas: accepted format=%s quality=%d estimate=%llu byte_copy=%d",
                SaveFormatToken(selected), outcome.quality,
                static_cast<unsigned long long>(outcome.estimated_bytes),
                outcome.byte_copy ? 1 : 0);
  LogInfo(line);
  return outcome;
}

}  // namespace nv::platform
