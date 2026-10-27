#pragma once

#include "core/error.h"
#include "core/format_router.h"
#include "platform/i18n.h"

#include <cstdint>
#include <filesystem>
#include <string>

namespace nv {

// One user-facing error, classified from a decoder/HRESULT/Win32 result
// (規劃/22/11 §3). The technical detail is kept separately so the card never
// shows a raw HRESULT in its main text.
enum class ErrorKind {
  kFileMissing = 0,
  kAccessDenied,
  kUnsupported,
  kCorrupt,
  kCodecMissing,
  kOutOfMemory,
  kSaveFailed,
  kPrintFailed,
  kPreviewFailed,
  kShellIntegration,
  kCancelled,
  kUnknown,
};

const char* ErrorKindName(ErrorKind kind);
bool ParseErrorKind(const std::wstring& text, ErrorKind* kind);

// Buttons offered by the error card. Bit flags so a classifier can combine them.
enum class ErrorAction : uint32_t {
  kNone = 0,
  kRetry = 1u << 0,
  kCopyPath = 1u << 1,
  kOpenWith = 1u << 2,
  kReveal = 1u << 3,
  kDetails = 1u << 4,
};

constexpr uint32_t operator|(ErrorAction a, ErrorAction b) {
  return static_cast<uint32_t>(a) | static_cast<uint32_t>(b);
}
constexpr uint32_t operator|(uint32_t a, ErrorAction b) {
  return a | static_cast<uint32_t>(b);
}
constexpr uint32_t operator|(ErrorAction a, uint32_t b) {
  return static_cast<uint32_t>(a) | b;
}
constexpr bool HasErrorAction(uint32_t actions, ErrorAction action) {
  return (actions & static_cast<uint32_t>(action)) != 0;
}

const char* ErrorActionName(ErrorAction action);

struct UiError {
  ErrorKind kind = ErrorKind::kUnknown;
  // Internal code (route/decode base + HRESULT). Only logs and the detail line.
  uint32_t code = 0;
  std::filesystem::path path;
  std::wstring detail;
  uint32_t actions = 0;
  // Human text; defaults to the per-kind entry when kCount is passed.
  i18n::StringId message_id = i18n::StringId::kCount;
  i18n::StringId hint_id = i18n::StringId::kCount;

  bool empty() const { return kind == ErrorKind::kUnknown && path.empty() &&
                              detail.empty(); }
  bool has_action(ErrorAction action) const {
    return HasErrorAction(actions, action);
  }
};

// Builds an error with the per-kind message/hint and the default action set.
// `message_id`/`hint_id` may override the kind defaults (e.g. a delete failure
// keeps "Could not delete" while still using the generic operation hint).
UiError MakeUiError(ErrorKind kind, std::filesystem::path path = {},
                    std::wstring detail = {}, uint32_t actions = 0,
                    i18n::StringId message_id = i18n::StringId::kCount,
                    i18n::StringId hint_id = i18n::StringId::kCount);

// Classifies a decode/probe failure. Checks the file system first (a deleted
// file is kFileMissing, not kCorrupt), then the HRESULT, then the format.
UiError ClassifyDecodeError(const Error& error,
                            const std::filesystem::path& path,
                            ImageFormat format);

// Classifies a file operation (save/delete/rename/wallpaper) with `fallback`
// when the HRESULT does not say anything more specific.
UiError ClassifyOperationError(ErrorKind fallback, const Error& error,
                               std::filesystem::path path = {},
                               i18n::StringId message_id =
                                   i18n::StringId::kCount);

// Win32/HRESULT -> kind. Returns kUnknown when nothing is recognised.
ErrorKind ErrorKindForHresult(long hr);

// Default buttons per kind (規劃/22/11 §3.1).
uint32_t DefaultErrorActions(ErrorKind kind);

}  // namespace nv
