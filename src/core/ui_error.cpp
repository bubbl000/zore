#include "core/ui_error.h"

#include <windows.h>

#include <algorithm>
#include <cwctype>

namespace nv {
namespace {

constexpr HRESULT kCodecNotFound = static_cast<HRESULT>(0x88982F50L);

std::wstring Lower(std::wstring text) {
  std::ranges::transform(text, text.begin(), [](wchar_t ch) {
    return static_cast<wchar_t>(std::towlower(ch));
  });
  return text;
}

i18n::StringId MessageFor(ErrorKind kind) {
  switch (kind) {
    case ErrorKind::kFileMissing:
      return i18n::StringId::kErrorFileMissing;
    case ErrorKind::kAccessDenied:
      return i18n::StringId::kErrorAccessDenied;
    case ErrorKind::kUnsupported:
      return i18n::StringId::kErrorUnsupported;
    case ErrorKind::kCorrupt:
      return i18n::StringId::kErrorCorrupt;
    case ErrorKind::kCodecMissing:
      return i18n::StringId::kErrorCodecMissing;
    case ErrorKind::kOutOfMemory:
      return i18n::StringId::kErrorOutOfMemory;
    case ErrorKind::kSaveFailed:
      return i18n::StringId::kErrorSaveImage;
    case ErrorKind::kPrintFailed:
      return i18n::StringId::kErrorOpenImage;
    case ErrorKind::kShellIntegration:
      return i18n::StringId::kErrorOperationFailed;
    case ErrorKind::kCancelled:
    case ErrorKind::kPreviewFailed:
    case ErrorKind::kUnknown:
      return i18n::StringId::kErrorOpenImage;
  }
  return i18n::StringId::kErrorOperationFailed;
}

i18n::StringId HintFor(ErrorKind kind) {
  switch (kind) {
    case ErrorKind::kFileMissing:
      return i18n::StringId::kErrorFileMissingHint;
    case ErrorKind::kAccessDenied:
      return i18n::StringId::kErrorAccessDeniedHint;
    case ErrorKind::kUnsupported:
      return i18n::StringId::kErrorUnsupportedHint;
    case ErrorKind::kCorrupt:
      return i18n::StringId::kErrorCorruptHint;
    case ErrorKind::kCodecMissing:
      return i18n::StringId::kErrorCodecMissingHint;
    case ErrorKind::kOutOfMemory:
      return i18n::StringId::kErrorOutOfMemoryHint;
    case ErrorKind::kSaveFailed:
      return i18n::StringId::kErrorSaveFailedHint;
    case ErrorKind::kPrintFailed:
    case ErrorKind::kPreviewFailed:
    case ErrorKind::kShellIntegration:
    case ErrorKind::kCancelled:
    case ErrorKind::kUnknown:
      break;
  }
  return i18n::StringId::kErrorOperationFailedHint;
}

}  // namespace

const char* ErrorKindName(ErrorKind kind) {
  switch (kind) {
    case ErrorKind::kFileMissing:
      return "file_missing";
    case ErrorKind::kAccessDenied:
      return "access_denied";
    case ErrorKind::kUnsupported:
      return "unsupported";
    case ErrorKind::kCorrupt:
      return "corrupt";
    case ErrorKind::kCodecMissing:
      return "codec_missing";
    case ErrorKind::kOutOfMemory:
      return "out_of_memory";
    case ErrorKind::kSaveFailed:
      return "save_failed";
    case ErrorKind::kPrintFailed:
      return "print_failed";
    case ErrorKind::kPreviewFailed:
      return "preview_failed";
    case ErrorKind::kShellIntegration:
      return "shell";
    case ErrorKind::kCancelled:
      return "cancelled";
    case ErrorKind::kUnknown:
      return "unknown";
  }
  return "unknown";
}

bool ParseErrorKind(const std::wstring& text, ErrorKind* kind) {
  if (kind == nullptr) return false;
  const std::wstring value = Lower(text);
  static const struct {
    const wchar_t* name;
    ErrorKind kind;
  } kKinds[] = {
      {L"missing", ErrorKind::kFileMissing},
      {L"file_missing", ErrorKind::kFileMissing},
      {L"access", ErrorKind::kAccessDenied},
      {L"access_denied", ErrorKind::kAccessDenied},
      {L"unsupported", ErrorKind::kUnsupported},
      {L"corrupt", ErrorKind::kCorrupt},
      {L"codec", ErrorKind::kCodecMissing},
      {L"codec_missing", ErrorKind::kCodecMissing},
      {L"oom", ErrorKind::kOutOfMemory},
      {L"out_of_memory", ErrorKind::kOutOfMemory},
      {L"save", ErrorKind::kSaveFailed},
      {L"save_failed", ErrorKind::kSaveFailed},
      {L"shell", ErrorKind::kShellIntegration},
      {L"unknown", ErrorKind::kUnknown},
  };
  for (const auto& entry : kKinds) {
    if (value == entry.name) {
      *kind = entry.kind;
      return true;
    }
  }
  return false;
}

const char* ErrorActionName(ErrorAction action) {
  switch (action) {
    case ErrorAction::kRetry:
      return "retry";
    case ErrorAction::kCopyPath:
      return "copy_path";
    case ErrorAction::kOpenWith:
      return "open_with";
    case ErrorAction::kReveal:
      return "reveal";
    case ErrorAction::kDetails:
      return "details";
    case ErrorAction::kNone:
      return "none";
  }
  return "none";
}

ErrorKind ErrorKindForHresult(long hr) {
  const HRESULT result = static_cast<HRESULT>(hr);
  if (result == kCodecNotFound) return ErrorKind::kCodecMissing;
  if (result == E_OUTOFMEMORY ||
      result == HRESULT_FROM_WIN32(ERROR_NOT_ENOUGH_MEMORY) ||
      result == HRESULT_FROM_WIN32(ERROR_OUTOFMEMORY)) {
    return ErrorKind::kOutOfMemory;
  }
  if (result == E_ACCESSDENIED ||
      result == HRESULT_FROM_WIN32(ERROR_ACCESS_DENIED) ||
      result == HRESULT_FROM_WIN32(ERROR_SHARING_VIOLATION) ||
      result == HRESULT_FROM_WIN32(ERROR_LOCK_VIOLATION)) {
    return ErrorKind::kAccessDenied;
  }
  if (result == HRESULT_FROM_WIN32(ERROR_FILE_NOT_FOUND) ||
      result == HRESULT_FROM_WIN32(ERROR_PATH_NOT_FOUND) ||
      result == HRESULT_FROM_WIN32(ERROR_INVALID_NAME) ||
      result == HRESULT_FROM_WIN32(ERROR_BAD_PATHNAME)) {
    return ErrorKind::kFileMissing;
  }
  // WINCODEC_ERR_* (0x88982F0x): bad header/image data/unsupported format.
  if ((static_cast<uint32_t>(result) & 0xFFFFFF00u) == 0x88982F00u) {
    return ErrorKind::kCorrupt;
  }
  if (result == HRESULT_FROM_WIN32(ERROR_DISK_FULL) ||
      result == HRESULT_FROM_WIN32(ERROR_HANDLE_DISK_FULL)) {
    return ErrorKind::kSaveFailed;
  }
  return ErrorKind::kUnknown;
}

uint32_t DefaultErrorActions(ErrorKind kind) {
  switch (kind) {
    case ErrorKind::kFileMissing:
      return ErrorAction::kCopyPath | ErrorAction::kReveal;
    case ErrorKind::kAccessDenied:
      return ErrorAction::kCopyPath | ErrorAction::kReveal;
    case ErrorKind::kUnsupported:
      return ErrorAction::kOpenWith | ErrorAction::kCopyPath;
    case ErrorKind::kCorrupt:
      return ErrorAction::kRetry | ErrorAction::kOpenWith |
             ErrorAction::kCopyPath;
    case ErrorKind::kCodecMissing:
      return ErrorAction::kOpenWith | ErrorAction::kCopyPath;
    case ErrorKind::kOutOfMemory:
      return static_cast<uint32_t>(ErrorAction::kRetry);
    case ErrorKind::kSaveFailed:
      return ErrorAction::kRetry | ErrorAction::kCopyPath;
    case ErrorKind::kPrintFailed:
      return static_cast<uint32_t>(ErrorAction::kRetry);
    case ErrorKind::kShellIntegration:
      return static_cast<uint32_t>(ErrorAction::kNone);
    case ErrorKind::kPreviewFailed:
    case ErrorKind::kCancelled:
    case ErrorKind::kUnknown:
      return ErrorAction::kRetry | ErrorAction::kCopyPath;
  }
  return static_cast<uint32_t>(ErrorAction::kNone);
}

UiError MakeUiError(ErrorKind kind, std::filesystem::path path,
                    std::wstring detail, uint32_t actions,
                    i18n::StringId message_id, i18n::StringId hint_id) {
  UiError error;
  error.kind = kind;
  error.path = std::move(path);
  error.detail = std::move(detail);
  error.actions = actions != 0 ? actions : DefaultErrorActions(kind);
  error.message_id = message_id != i18n::StringId::kCount ? message_id
                                                          : MessageFor(kind);
  error.hint_id =
      hint_id != i18n::StringId::kCount ? hint_id : HintFor(kind);
  return error;
}

UiError ClassifyDecodeError(const Error& error,
                            const std::filesystem::path& path,
                            ImageFormat format) {
  const auto with_code = [&error](UiError ui) {
    ui.code = static_cast<uint32_t>(error.hr) != 0
                  ? static_cast<uint32_t>(error.hr)
                  : static_cast<uint32_t>(error.code);
    return ui;
  };
  const ErrorKind by_hr = ErrorKindForHresult(error.hr);
  if (by_hr == ErrorKind::kCodecMissing) {
    return with_code(MakeUiError(ErrorKind::kCodecMissing, path, {}));
  }
  if (by_hr == ErrorKind::kOutOfMemory) {
    return with_code(MakeUiError(ErrorKind::kOutOfMemory, path, {}));
  }
  // A file that vanished between listing and decoding is "missing", not
  // "corrupt"; the same goes for a path the user has no rights to.
  std::error_code ec;
  if (!std::filesystem::exists(path, ec)) {
    return with_code(MakeUiError(ErrorKind::kFileMissing, path, {}));
  }
  if (by_hr == ErrorKind::kAccessDenied) {
    return with_code(MakeUiError(ErrorKind::kAccessDenied, path, {}));
  }
  if (format == ImageFormat::kUnknown) {
    return with_code(MakeUiError(ErrorKind::kUnsupported, path, {}));
  }
  std::wstring detail;
  if (!error.message.empty()) detail = std::wstring(error.message.begin(),
                                                     error.message.end());
  return with_code(MakeUiError(ErrorKind::kCorrupt, path, std::move(detail)));
}

UiError ClassifyOperationError(ErrorKind fallback, const Error& error,
                               std::filesystem::path path,
                               i18n::StringId message_id) {
  const ErrorKind by_hr = ErrorKindForHresult(error.hr);
  ErrorKind kind = fallback;
  // Only a few failures are more specific than the command that failed:
  // access denied and out-of-memory change the user-facing hint.
  if (by_hr == ErrorKind::kAccessDenied || by_hr == ErrorKind::kOutOfMemory) {
    kind = by_hr;
  }
  std::wstring detail;
  if (!error.message.empty()) {
    detail = std::wstring(error.message.begin(), error.message.end());
  }
  UiError ui =
      MakeUiError(kind, std::move(path), std::move(detail), 0, message_id);
  ui.code = static_cast<uint32_t>(error.hr) != 0
                ? static_cast<uint32_t>(error.hr)
                : static_cast<uint32_t>(error.code);
  return ui;
}

}  // namespace nv
