#include "platform/crash.h"

#include "platform/paths.h"

#include <windows.h>
#include <dbghelp.h>

#include <cwchar>
#include <filesystem>
#include <string>

namespace nv::platform {
namespace {

std::wstring g_crash_dir;

LONG WINAPI CrashFilter(EXCEPTION_POINTERS* exception) {
  if (g_crash_dir.empty()) {
    return EXCEPTION_EXECUTE_HANDLER;
  }

  wchar_t path[MAX_PATH * 2]{};
  swprintf_s(path, L"%s\\crash_%lu.dmp", g_crash_dir.c_str(),
             GetCurrentProcessId());

  HANDLE file = CreateFileW(path, GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                            FILE_ATTRIBUTE_NORMAL, nullptr);
  if (file != INVALID_HANDLE_VALUE) {
    MINIDUMP_EXCEPTION_INFORMATION info{};
    info.ThreadId = GetCurrentThreadId();
    info.ExceptionPointers = exception;
    info.ClientPointers = FALSE;

    MiniDumpWriteDump(GetCurrentProcess(), GetCurrentProcessId(), file,
                      MiniDumpNormal, &info, nullptr, nullptr);
    CloseHandle(file);
  }

  return EXCEPTION_EXECUTE_HANDLER;
}

}  // namespace

void InstallCrashHandler() {
  const std::filesystem::path dir = DataDir() / L"crashes";
  std::error_code ec;
  std::filesystem::create_directories(dir, ec);
  g_crash_dir = dir.wstring();

  SetUnhandledExceptionFilter(CrashFilter);
}

}  // namespace nv::platform
