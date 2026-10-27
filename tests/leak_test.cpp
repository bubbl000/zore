// M1-4 leak / stability harness.
//
// Exercises the same core paths the UI uses (sync decode, thumbnail service
// with memory + disk cache, async decode service, cancellation) in repeated
// rounds and compares Debug-CRT heap checkpoints. AddressSanitizer on Windows
// has no LeakSanitizer, so this is the leak counterpart to the ASan run.

#include "core/decode_service.h"
#include "core/format_router.h"
#include "core/thumbnail_service.h"
#include "platform/leak_check.h"

#include <windows.h>

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cwchar>
#include <cwctype>
#include <filesystem>
#include <functional>
#include <string>
#include <vector>

namespace {

constexpr UINT kDecodeDone = WM_APP + 11;
constexpr UINT kThumbDone = WM_APP + 12;

struct PumpState {
  int decoded = 0;
  int thumbs = 0;
  int thumb_failures = 0;
  int decode_errors = 0;
};

std::string ToUtf8(const std::wstring& text) {
  if (text.empty()) return {};
  const int size = WideCharToMultiByte(CP_UTF8, 0, text.c_str(),
                                       static_cast<int>(text.size()), nullptr, 0,
                                       nullptr, nullptr);
  if (size <= 0) return {};
  std::string out(static_cast<size_t>(size), '\0');
  WideCharToMultiByte(CP_UTF8, 0, text.c_str(), static_cast<int>(text.size()),
                      out.data(), size, nullptr, nullptr);
  return out;
}

LRESULT CALLBACK LeakTestWndProc(HWND hwnd, UINT message, WPARAM wparam,
                                 LPARAM lparam) {
  auto* state =
      reinterpret_cast<PumpState*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
  if (message == kDecodeDone) {
    auto* completion = reinterpret_cast<nv::DecodeCompletion*>(lparam);
    if (state != nullptr) ++state->decoded;
    delete completion;
    return 0;
  }
  if (message == kThumbDone) {
    auto* completion = reinterpret_cast<nv::ThumbnailCompletion*>(lparam);
    if (state != nullptr) {
      ++state->thumbs;
      if (!completion->image) ++state->thumb_failures;
    }
    delete completion;
    return 0;
  }
  return DefWindowProcW(hwnd, message, wparam, lparam);
}

HWND CreateMessageWindow(PumpState* state) {
  WNDCLASSW wc{};
  wc.lpfnWndProc = LeakTestWndProc;
  wc.hInstance = GetModuleHandleW(nullptr);
  wc.lpszClassName = L"ZoreViewLeakTestWindow";
  RegisterClassW(&wc);
  HWND hwnd = CreateWindowExW(0, wc.lpszClassName, L"", 0, 0, 0, 0, 0,
                              HWND_MESSAGE, nullptr, wc.hInstance, nullptr);
  if (hwnd != nullptr) {
    SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(state));
  }
  return hwnd;
}

void DrainMessages() {
  MSG msg;
  while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
    TranslateMessage(&msg);
    DispatchMessageW(&msg);
  }
}

bool PumpUntil(PumpState& state, const std::function<bool(const PumpState&)>& done,
               int timeout_ms) {
  const ULONGLONG deadline = GetTickCount64() + static_cast<ULONGLONG>(timeout_ms);
  for (;;) {
    DrainMessages();
    if (done(state)) return true;
    if (GetTickCount64() > deadline) return false;
    MsgWaitForMultipleObjects(0, nullptr, FALSE, 5, QS_ALLINPUT);
  }
}

std::vector<std::filesystem::path> CollectImages(
    const std::filesystem::path& root, size_t limit) {
  std::vector<std::filesystem::path> files;
  std::error_code ec;
  if (!std::filesystem::exists(root, ec)) return files;

  for (const auto& entry : std::filesystem::recursive_directory_iterator(
           root, std::filesystem::directory_options::skip_permission_denied,
           ec)) {
    if (ec) break;
    if (!entry.is_regular_file(ec)) continue;
    std::wstring extension = entry.path().extension().wstring();
    std::ranges::transform(extension, extension.begin(), [](wchar_t c) {
      return static_cast<wchar_t>(std::towlower(c));
    });
    if (!nv::FormatRouter::IsSupportedExtension(extension)) continue;
    std::wstring path_text = entry.path().wstring();
    std::ranges::transform(path_text, path_text.begin(), [](wchar_t c) {
      return static_cast<wchar_t>(std::towlower(c));
    });
    // Corrupt payload samples are covered by zoreview_formattests; including
    // them here would only add expected decode/thumbnail failures.
    if (path_text.find(L"\\corrupt\\") != std::wstring::npos) continue;
    files.push_back(entry.path());
    if (files.size() >= limit) break;
  }
  std::ranges::sort(files);
  return files;
}

bool RunRound(HWND hwnd, PumpState& state,
              const std::vector<std::filesystem::path>& images,
              const std::filesystem::path& cache_dir) {
  bool ok = true;
  state.decode_errors = 0;
  state.thumb_failures = 0;

  // 1. Synchronous probe/decode, as the UI's disk-cache miss path does.
  for (const auto& path : images) {
    (void)nv::FormatRouter::Probe(path);
    nv::DecodeResult decoded = nv::FormatRouter::Decode(path);
    if (!decoded.ok()) {
      ++state.decode_errors;
      std::printf("  decode skipped: %s\n", ToUtf8(path.wstring()).c_str());
    }
  }

  // 2. Thumbnail service: the first round populates the disk cache, later
  //    rounds read it back, so both paths are covered.
  {
    const int expected = static_cast<int>(images.size());
    state.thumbs = 0;
    nv::ThumbnailService thumbs(hwnd, kThumbDone, 128, 1, 64ull * 1024 * 1024,
                                cache_dir);
    for (size_t i = 0; i < images.size(); ++i) {
      (void)thumbs.Request(i, images[i]);
    }
    const bool completed = PumpUntil(
        state, [expected](const PumpState& s) { return s.thumbs >= expected; },
        30000);
    if (!completed) ok = false;
  }

  // 3. Async decode service: request one image at a time and drain, then a
  //    prefetch batch and a cancellation.
  {
    nv::DecodeService decode(hwnd, kDecodeDone, 2);
    for (size_t i = 0; i < std::min<size_t>(images.size(), 4); ++i) {
      const int before = state.decoded;
      decode.Request(images[i]);
      if (!PumpUntil(
              state, [before](const PumpState& s) { return s.decoded > before; },
              15000)) {
        ok = false;
      }
    }

    if (images.size() >= 2) {
      std::vector<std::filesystem::path> batch(images.begin(),
                                              images.begin() + 2);
      decode.Prefetch(batch);
      PumpUntil(state, [](const PumpState&) { return false; }, 100);
    }

    decode.Request(images.front());
    decode.CancelAll();
    PumpUntil(state, [](const PumpState&) { return false; }, 50);
    decode.Shutdown();
  }

  std::printf("  thumbs=%d/%zu thumb_failures=%d decode_errors=%d\n",
              state.thumbs, images.size(), state.thumb_failures,
              state.decode_errors);
  return ok;
}

}  // namespace

int wmain(int argc, wchar_t** argv) {
  std::filesystem::path images_dir = L"..\\测试文件夹\\images";
  int rounds = 5;

  for (int i = 1; i + 1 < argc; ++i) {
    const std::wstring arg = argv[i];
    if (arg == L"--images") {
      images_dir = argv[i + 1];
    } else if (arg == L"--rounds") {
      rounds = std::max(1, static_cast<int>(std::wcstol(argv[i + 1], nullptr, 10)));
    }
  }

  nv::platform::InitializeLeakTracking();
  std::printf("leak tracking: %s\n",
              nv::platform::LeakTrackingEnabled() ? "enabled (Debug CRT)"
                                                  : "disabled (Release/ASan)");

  const auto images = CollectImages(images_dir, 12);
  std::printf("images: %zu from %s\n", images.size(),
              ToUtf8(images_dir.wstring()).c_str());
  if (images.empty()) {
    std::fprintf(stderr, "no supported images found under %s\n",
                 ToUtf8(images_dir.wstring()).c_str());
    return 2;
  }

  PumpState state;
  HWND hwnd = CreateMessageWindow(&state);
  if (hwnd == nullptr) {
    std::fprintf(stderr, "CreateWindowExW failed: %lu\n", GetLastError());
    return 2;
  }

  wchar_t temp_path[MAX_PATH]{};
  GetTempPathW(MAX_PATH, temp_path);
  const std::filesystem::path cache_dir =
      std::filesystem::path(temp_path) /
      (L"zoreview-leak-" + std::to_wstring(GetCurrentProcessId()));

  bool failed = false;
  for (int round = 1; round <= rounds; ++round) {
    const auto before = nv::platform::SnapshotHeap();
    const bool round_ok = RunRound(hwnd, state, images, cache_dir);
    const auto after = nv::platform::SnapshotHeap();
    std::printf("round %d/%d%s\n", round, rounds, round_ok ? "" : " (FAILED)");
    const auto growth = nv::platform::ReportHeapGrowth("delta", before, after);
    // The first rounds may allocate process-global caches lazily; report them
    // but only treat a growing live set across later rounds as a leak.
    if (round >= 3 && (growth.blocks > 4 || growth.bytes > 64 * 1024)) {
      failed = true;
    }
    if (!round_ok) failed = true;
  }

  DestroyWindow(hwnd);
  std::error_code ec;
  std::filesystem::remove_all(cache_dir, ec);

  std::printf("result: %s\n", failed ? "FAILED" : "ok");
  return failed ? 1 : 0;
}
