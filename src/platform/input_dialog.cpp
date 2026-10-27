#include "platform/input_dialog.h"

#include "platform/dpi.h"
#include "platform/i18n.h"

#include <windows.h>
#include <imm.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cwctype>
#include <string>

namespace nv::platform {
namespace {

constexpr wchar_t kInputClass[] = L"ZoreView.TextInput";
constexpr int kEditId = 1000;

struct DialogState {
  HWND owner = nullptr;
  HWND window = nullptr;
  HWND edit = nullptr;
  WNDPROC edit_proc = nullptr;
  HFONT font = nullptr;
  std::wstring label;
  std::wstring initial;
  std::wstring result;
  bool accepted = false;
  float scale = 1.0f;
};

HFONT CreateDialogFont(float scale) {
  NONCLIENTMETRICSW metrics{};
  metrics.cbSize = sizeof(metrics);
  if (!SystemParametersInfoW(SPI_GETNONCLIENTMETRICS, sizeof(metrics),
                             &metrics, 0)) {
    return nullptr;
  }
  LOGFONTW font = metrics.lfMessageFont;
  const float system_scale =
      GetDpiForSystem() == 0 ? 1.0f : static_cast<float>(GetDpiForSystem()) / 96.0f;
  const float ratio = system_scale > 0.0f ? scale / system_scale : 1.0f;
  const int height = std::max(
      1, static_cast<int>(std::lround(std::abs(font.lfHeight) * ratio)));
  font.lfHeight = -height;
  font.lfWidth = 0;
  return CreateFontIndirectW(&font);
}

std::wstring Trim(std::wstring text) {
  size_t begin = 0;
  while (begin < text.size() && std::iswspace(text[begin]) != 0) ++begin;
  size_t end = text.size();
  while (end > begin && std::iswspace(text[end - 1]) != 0) --end;
  return text.substr(begin, end - begin);
}

void Accept(DialogState* state) {
  if (state == nullptr || state->edit == nullptr) return;
  const int length = GetWindowTextLengthW(state->edit);
  std::wstring text(static_cast<size_t>(length) + 1, L'\0');
  GetWindowTextW(state->edit, text.data(), length + 1);
  text.resize(static_cast<size_t>(length));
  text = Trim(std::move(text));
  if (text.empty()) {
    MessageBeep(MB_ICONWARNING);
    SetFocus(state->edit);
    return;
  }
  state->result = std::move(text);
  state->accepted = true;
  DestroyWindow(state->window);
}

bool ImeComposing(HWND hwnd) {
  HIMC context = ImmGetContext(hwnd);
  if (context == nullptr) return false;
  const LONG length = ImmGetCompositionStringW(context, GCS_COMPSTR, nullptr, 0);
  ImmReleaseContext(hwnd, context);
  return length > 0;
}

LRESULT CALLBACK EditProc(HWND hwnd, UINT message, WPARAM wparam, LPARAM lparam) {
  auto* state =
      reinterpret_cast<DialogState*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
  if (message == WM_KEYDOWN && state != nullptr && !ImeComposing(hwnd)) {
    if (wparam == VK_RETURN) {
      SendMessageW(GetParent(hwnd), WM_COMMAND, MAKEWPARAM(IDOK, BN_CLICKED), 0);
      return 0;
    }
    if (wparam == VK_ESCAPE) {
      SendMessageW(GetParent(hwnd), WM_COMMAND,
                   MAKEWPARAM(IDCANCEL, BN_CLICKED), 0);
      return 0;
    }
  }
  if (state != nullptr && state->edit_proc != nullptr) {
    return CallWindowProcW(state->edit_proc, hwnd, message, wparam, lparam);
  }
  return DefWindowProcW(hwnd, message, wparam, lparam);
}

LRESULT CALLBACK DialogProc(HWND hwnd, UINT message, WPARAM wparam,
                            LPARAM lparam) {
  auto* state =
      reinterpret_cast<DialogState*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));

  switch (message) {
    case WM_CREATE: {
      auto* create = reinterpret_cast<CREATESTRUCTW*>(lparam);
      state = static_cast<DialogState*>(create->lpCreateParams);
      SetWindowLongPtrW(hwnd, GWLP_USERDATA,
                        reinterpret_cast<LONG_PTR>(state));
      if (state == nullptr) return -1;
      state->window = hwnd;

      const float scale = state->scale;
      const int width = DpiPixels(460.0f, scale);
      const int height = DpiPixels(170.0f, scale);
      state->font = CreateDialogFont(scale);
      const HFONT font = state->font != nullptr
                             ? state->font
                             : static_cast<HFONT>(GetStockObject(DEFAULT_GUI_FONT));

      HWND label = CreateWindowExW(
          0, L"STATIC", state->label.c_str(), WS_CHILD | WS_VISIBLE,
          DpiPixels(16.0f, scale), DpiPixels(16.0f, scale),
          width - DpiPixels(32.0f, scale), DpiPixels(22.0f, scale), hwnd,
          nullptr, GetModuleHandleW(nullptr), nullptr);
      state->edit = CreateWindowExW(
          WS_EX_CLIENTEDGE, L"EDIT", state->initial.c_str(),
          WS_CHILD | WS_VISIBLE | WS_TABSTOP | ES_AUTOHSCROLL,
          DpiPixels(16.0f, scale), DpiPixels(42.0f, scale),
          width - DpiPixels(32.0f, scale), DpiPixels(28.0f, scale), hwnd,
          reinterpret_cast<HMENU>(static_cast<INT_PTR>(kEditId)),
          GetModuleHandleW(nullptr), nullptr);
      HWND ok = CreateWindowExW(
          0, L"BUTTON", nv::i18n::Text(nv::i18n::StringId::kButtonOk),
          WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_DEFPUSHBUTTON,
          width - DpiPixels(196.0f, scale), height - DpiPixels(46.0f, scale),
          DpiPixels(84.0f, scale), DpiPixels(30.0f, scale), hwnd,
          reinterpret_cast<HMENU>(static_cast<INT_PTR>(IDOK)),
          GetModuleHandleW(nullptr), nullptr);
      HWND cancel = CreateWindowExW(
          0, L"BUTTON", nv::i18n::Text(nv::i18n::StringId::kButtonCancel),
          WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_PUSHBUTTON,
          width - DpiPixels(100.0f, scale), height - DpiPixels(46.0f, scale),
          DpiPixels(84.0f, scale), DpiPixels(30.0f, scale), hwnd,
          reinterpret_cast<HMENU>(static_cast<INT_PTR>(IDCANCEL)),
          GetModuleHandleW(nullptr), nullptr);

      for (HWND control : {label, state->edit, ok, cancel}) {
        if (control != nullptr) {
          SendMessageW(control, WM_SETFONT,
                       reinterpret_cast<WPARAM>(font), TRUE);
        }
      }

      if (state->edit != nullptr) {
        state->edit_proc = reinterpret_cast<WNDPROC>(SetWindowLongPtrW(
            state->edit, GWLP_WNDPROC,
            reinterpret_cast<LONG_PTR>(EditProc)));
        SetWindowLongPtrW(state->edit, GWLP_USERDATA,
                          reinterpret_cast<LONG_PTR>(state));
        SetFocus(state->edit);
        SendMessageW(state->edit, EM_SETSEL, 0, -1);
      }
      return 0;
    }

    case WM_COMMAND:
      if (LOWORD(wparam) == IDOK) {
        Accept(state);
        return 0;
      }
      if (LOWORD(wparam) == IDCANCEL) {
        DestroyWindow(hwnd);
        return 0;
      }
      break;

    case WM_CLOSE:
      DestroyWindow(hwnd);
      return 0;

    case WM_DESTROY:
      if (state != nullptr && state->font != nullptr) {
        DeleteObject(state->font);
        state->font = nullptr;
      }
      PostQuitMessage(0);
      return 0;

    default:
      break;
  }
  return DefWindowProcW(hwnd, message, wparam, lparam);
}

void EnsureClass() {
  static bool registered = false;
  if (registered) return;
  WNDCLASSEXW window_class{};
  window_class.cbSize = sizeof(window_class);
  window_class.style = CS_HREDRAW | CS_VREDRAW;
  window_class.lpfnWndProc = DialogProc;
  window_class.hInstance = GetModuleHandleW(nullptr);
  window_class.hCursor = LoadCursorW(nullptr, IDC_ARROW);
  window_class.hbrBackground =
      reinterpret_cast<HBRUSH>(static_cast<INT_PTR>(COLOR_3DFACE + 1));
  window_class.lpszClassName = kInputClass;
  RegisterClassExW(&window_class);
  registered = true;
}

}  // namespace

std::optional<std::wstring> PromptForText(HWND owner, const std::wstring& title,
                                          const std::wstring& label,
                                          const std::wstring& initial) {
  EnsureClass();

  const float scale = DpiScaleForWindow(owner);

  RECT owner_rect{};
  if (owner != nullptr) {
    GetWindowRect(owner, &owner_rect);
  } else {
    owner_rect.right = GetSystemMetrics(SM_CXSCREEN);
    owner_rect.bottom = GetSystemMetrics(SM_CYSCREEN);
  }
  const int width = DpiPixels(460.0f, scale);
  const int height = DpiPixels(170.0f, scale);
  const int x = owner_rect.left + (owner_rect.right - owner_rect.left - width) / 2;
  const int y = owner_rect.top + (owner_rect.bottom - owner_rect.top - height) / 2;

  DialogState state;
  state.owner = owner;
  state.label = label;
  state.initial = initial;
  state.scale = scale;

  if (owner != nullptr) EnableWindow(owner, FALSE);
  HWND window = CreateWindowExW(
      WS_EX_DLGMODALFRAME | WS_EX_CONTROLPARENT, kInputClass, title.c_str(),
      WS_POPUP | WS_CAPTION | WS_SYSMENU, x, y, width, height, owner, nullptr,
      GetModuleHandleW(nullptr), &state);

  if (window == nullptr) {
    if (owner != nullptr) EnableWindow(owner, TRUE);
    return std::nullopt;
  }

  ShowWindow(window, SW_SHOW);
  UpdateWindow(window);
  SetForegroundWindow(window);
  if (state.edit != nullptr) SetFocus(state.edit);

  MSG message{};
  while (GetMessageW(&message, nullptr, 0, 0) > 0) {
    // Give Tab/Shift+Tab and the default button their standard dialog
    // behavior; without this the modal input box was keyboard-trapped.
    if (IsDialogMessageW(window, &message)) continue;
    TranslateMessage(&message);
    DispatchMessageW(&message);
  }

  if (owner != nullptr) {
    EnableWindow(owner, TRUE);
    SetForegroundWindow(owner);
  }
  if (!state.accepted) return std::nullopt;
  return state.result;
}

}  // namespace nv::platform
