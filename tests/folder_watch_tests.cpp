// v2.0 S2 / 22/10: folder wrap, sibling peeking, rescan and the watch state
// machine. Everything here is pure/temporary-directory logic, so the checks do
// not depend on a window, D3D or real timing.

#include "tests/folder_watch_tests.h"

#include "core/folder_navigator.h"
#include "core/folder_watcher.h"
#include "platform/dir_notify.h"

#include <windows.h>

#include <atomic>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>
#include <system_error>
#include <vector>

namespace nv::tests {
namespace {

int g_failures = 0;

void Check(bool condition, const char* name) {
  if (condition) {
    std::printf("ok   %s\n", name);
  } else {
    std::printf("FAIL %s\n", name);
    ++g_failures;
  }
}

void TouchFile(const std::filesystem::path& path) {
  std::ofstream out(path, std::ios::binary | std::ios::trunc);
  out << "not a real image, only the extension matters here";
}

std::filesystem::path MakeTempRoot() {
  wchar_t buffer[MAX_PATH]{};
  GetTempPathW(MAX_PATH, buffer);
  const std::filesystem::path root =
      std::filesystem::path(buffer) /
      (L"nv-watch-tests-" + std::to_wstring(GetCurrentProcessId()));
  std::error_code ec;
  std::filesystem::remove_all(root, ec);
  std::filesystem::create_directories(root, ec);
  return root;
}

void TestWrapStrategy() {
  using nv::StepAction;
  using nv::WrapStrategy;

  WrapStrategy strategy = WrapStrategy::kLoop;
  Check(nv::ParseWrapStrategy(L"loop", &strategy) &&
            strategy == WrapStrategy::kLoop,
        "ParseWrapStrategy accepts loop");
  Check(nv::ParseWrapStrategy(L"next", &strategy) &&
            strategy == WrapStrategy::kNext,
        "ParseWrapStrategy accepts next");
  Check(nv::ParseWrapStrategy(L"ask", &strategy) &&
            strategy == WrapStrategy::kAsk,
        "ParseWrapStrategy accepts ask");
  Check(!nv::ParseWrapStrategy(L"bogus", &strategy),
        "ParseWrapStrategy rejects unknown text");
  Check(std::wstring(nv::WrapStrategyName(WrapStrategy::kAsk)) == L"ask",
        "WrapStrategyName round-trips");

  Check(nv::PlanFolderStep(1, 3, +1, WrapStrategy::kLoop, false) ==
            StepAction::kMove,
        "a middle step never peeks at siblings");
  Check(nv::PlanFolderStep(2, 3, +1, WrapStrategy::kLoop, true) ==
            StepAction::kLoop,
        "loop wraps at the end");
  Check(nv::PlanFolderStep(0, 3, -1, WrapStrategy::kLoop, true) ==
            StepAction::kLoop,
        "loop wraps at the start");
  Check(nv::PlanFolderStep(2, 3, +1, WrapStrategy::kNext, true) ==
            StepAction::kAdjacent,
        "next enters the adjacent folder when one has images");
  Check(nv::PlanFolderStep(2, 3, +1, WrapStrategy::kNext, false) ==
            StepAction::kLoop,
        "next falls back to loop at the last image folder");
  Check(nv::PlanFolderStep(0, 1, +1, WrapStrategy::kAsk, true) ==
            StepAction::kAskAdjacent,
        "ask offers the adjacent folder");
  Check(nv::PlanFolderStep(0, 1, +1, WrapStrategy::kAsk, false) ==
            StepAction::kAskLoop,
        "ask falls back to the in-folder wrap prompt");
  Check(nv::PlanFolderStep(0, 3, -1, WrapStrategy::kNext, true) ==
            StepAction::kAdjacent,
        "prev works symmetrically");
}

void TestSiblingPeeking(const std::filesystem::path& root) {
  std::error_code ec;
  const auto one = root / L"1";
  const auto empty = root / L"2empty";
  const auto three = root / L"3";
  const auto ten = root / L"10";
  std::filesystem::create_directories(one, ec);
  std::filesystem::create_directories(empty, ec);
  std::filesystem::create_directories(three, ec);
  std::filesystem::create_directories(ten, ec);
  TouchFile(one / L"a.png");
  TouchFile(three / L"c.png");
  TouchFile(ten / L"b.png");
  TouchFile(root / L"stray.txt");

  const auto siblings = nv::ListSiblingFolders(root);
  Check(siblings.size() == 4, "only directories are siblings (files excluded)");
  Check(siblings.size() == 4 && siblings[0].filename() == L"1" &&
            siblings[1].filename() == L"2empty" &&
            siblings[2].filename() == L"3" &&
            siblings[3].filename() == L"10",
        "siblings use Explorer's natural (numeric) order");

  Check(nv::FolderHasSupportedImages(one), "folder with a png has images");
  Check(!nv::FolderHasSupportedImages(empty),
        "a folder with no supported extension is skipped");
  Check(!nv::FolderHasSupportedImages(root),
        "the parent itself is not counted as its own sibling folder");

  size_t probed = 0;
  const auto next = nv::FindAdjacentImageFolder(one, +1, 200, &probed);
  Check(next.has_value() && next->filename() == L"3",
        "peeking forwards skips the empty folder");
  Check(probed == 2, "peeking counts both inspected folders");
  const auto prev = nv::FindAdjacentImageFolder(three, -1);
  Check(prev.has_value() && prev->filename() == L"1",
        "peeking backwards finds the previous image folder");
  Check(!nv::FindAdjacentImageFolder(ten, +1).has_value(),
        "peeking past the last sibling returns nothing");
  Check(!nv::FindAdjacentImageFolder(root / L"10", 0).has_value(),
        "direction zero is invalid");

  // A tiny probe budget must give up rather than scan everything.
  size_t limited = 0;
  Check(!nv::FindAdjacentImageFolder(one, +1, 1, &limited).has_value() &&
            limited == 1,
        "the probe budget bounds a wide sibling list");
}

void TestFolderOrder(const std::filesystem::path& root) {
  using namespace nv;
  std::error_code ec;
  const auto folder = root / L"order";
  std::filesystem::create_directories(folder, ec);
  // v3.0 (規劃/26 §2.5): one order only — Explorer's natural name order.
  TouchFile(folder / L"img10.png");
  TouchFile(folder / L"img2.png");
  TouchFile(folder / L"B.png");
  TouchFile(folder / L"a.png");
  TouchFile(folder / L"notes.txt");  // unsupported, never listed

  const FolderList list = BuildFolderListFromFolder(folder);
  Check(list.size() == 4, "the order folder lists four supported images");
  Check(list.size() == 4 && list.files[0].filename() == L"a.png" &&
            list.files[1].filename() == L"B.png" &&
            list.files[2].filename() == L"img2.png" &&
            list.files[3].filename() == L"img10.png",
        "images use Explorer's natural order (digits by value, case-insensitive)");

  const FolderList from_file = BuildFolderList(folder / L"img10.png");
  Check(from_file.index == 3, "the open file is located by name in the new order");
}

void TestRescan(const std::filesystem::path& root) {
  using namespace nv;
  const auto folder = root / L"1";
  FolderList list = BuildFolderListFromFolder(folder);
  Check(list.size() == 1 && list.index == 0, "baseline folder list");

  TouchFile(folder / L"d.png");
  Check(RescanFolder(list, folder / L"a.png"),
        "rescan re-locates the open file after an external add");
  Check(list.size() == 2, "rescan sees the new file");
  Check(list.current().filename() == L"a.png",
        "rescan keeps the open file as the current entry");

  std::error_code ec;
  std::filesystem::remove(folder / L"a.png", ec);
  Check(!RescanFolder(list, folder / L"a.png"),
        "rescan reports that the open file disappeared");
  Check(!list.empty(), "the remaining file is still listed");
  Check(list.index < list.size(), "the index stays in range when the file is gone");

  std::filesystem::remove(folder / L"d.png", ec);
  Check(!RescanFolder(list, folder / L"a.png") && list.empty(),
        "an emptied folder rescans to an empty list");
}

// --- V3-05 (規劃/25 §8): ReadDirectoryChangesW notifier ----------------------
// These checks use a real message-only window and a real temporary folder, so
// they cover the shipped kernel path; only the 2 s poll is replaced by a short
// pump. They are self-contained and add no product code.

std::atomic<int> g_notify_messages{0};

LRESULT CALLBACK NotifyProbeProc(HWND hwnd, UINT message, WPARAM wparam,
                                 LPARAM lparam) {
  if (message == nv::platform::DirNotifier::kMessage) {
    g_notify_messages.fetch_add(1, std::memory_order_relaxed);
    return 0;
  }
  return DefWindowProcW(hwnd, message, wparam, lparam);
}

HWND MakeNotifyProbe() {
  constexpr wchar_t kClass[] = L"ZoreView.DirNotifyTest";
  HINSTANCE instance = GetModuleHandleW(nullptr);
  WNDCLASSEXW window_class{};
  window_class.cbSize = sizeof(window_class);
  window_class.lpfnWndProc = NotifyProbeProc;
  window_class.hInstance = instance;
  window_class.lpszClassName = kClass;
  RegisterClassExW(&window_class);  // "already registered" is fine
  return CreateWindowExW(0, kClass, L"", 0, 0, 0, 0, 0, HWND_MESSAGE, nullptr,
                         instance, nullptr);
}

// Pumps the thread's posted messages until `target` arrive or `timeout_ms`
// passes; returns the count seen and, via `elapsed_ms`, the time it took.
int PumpNotify(int target, int timeout_ms, ULONGLONG* elapsed_ms = nullptr) {
  const ULONGLONG start = GetTickCount64();
  MSG message{};
  for (;;) {
    while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE)) {
      TranslateMessage(&message);
      DispatchMessageW(&message);
    }
    const int seen = g_notify_messages.load(std::memory_order_relaxed);
    const ULONGLONG elapsed = GetTickCount64() - start;
    if (seen >= target || elapsed >= static_cast<ULONGLONG>(timeout_ms)) {
      if (elapsed_ms != nullptr) *elapsed_ms = elapsed;
      return seen;
    }
    Sleep(5);
  }
}

void TestDirNotifier(const std::filesystem::path& root) {
  using nv::platform::DirNotifier;
  std::error_code ec;
  const std::filesystem::path folder = root / L"notify";
  std::filesystem::create_directories(folder, ec);

  const HWND probe = MakeNotifyProbe();
  Check(probe != nullptr, "the notify probe window was created");

  DirNotifier notifier;
  Check(notifier.Start(probe, folder),
        "DirNotifier::Start opens a local folder");
  Check(notifier.running(), "the notifier thread runs after a successful Start");
  Check(notifier.folder() == folder, "the notifier remembers its folder");

  // One external add: one merged message, inside the old 2 s poll latency.
  g_notify_messages.store(0, std::memory_order_relaxed);
  ULONGLONG elapsed = 0;
  TouchFile(folder / L"one.bmp");
  Check(PumpNotify(1, 3000, &elapsed) == 1,
        "one external add posts exactly one notification");
  Check(elapsed < 1500,
        "the notification arrives far inside the 2 s poll latency");
  Check(PumpNotify(2, 600) == 1, "a single add does not repeat the message");

  // 100 rapid writes inside one quiet window: at most two messages (規劃/25 §8
  // acceptance), never one per file.
  g_notify_messages.store(0, std::memory_order_relaxed);
  for (int i = 0; i < 100; ++i) {
    std::ofstream out(folder / (L"burst-" + std::to_wstring(i) + L".bmp"),
                      std::ios::binary | std::ios::trunc);
    out << 'x';
  }
  const int burst = PumpNotify(1, 3000);
  Check(burst >= 1 && burst <= 2,
        "100 rapid writes coalesce into at most two notifications");
  Check(PumpNotify(3, 600) <= 2, "the burst produces no third notification");

  notifier.Stop();
  Check(!notifier.running(), "Stop() joins the notifier thread");

  // Start() failure is the fallback trigger (the app then keeps polling). A
  // missing folder is the cheapest deterministic failure to reproduce.
  DirNotifier missing;
  Check(!missing.Start(probe, root / L"no-such-folder"),
        "Start fails on a missing folder so the caller keeps polling");
  Check(!missing.running() && !missing.error_text().empty(),
        "a failed Start leaves nothing running and reports the reason");

  // Handle soak (坑 77): 20 open/close cycles must not grow the handle count.
  DWORD handles_before = 0;
  GetProcessHandleCount(GetCurrentProcess(), &handles_before);
  DirNotifier loop;
  for (int i = 0; i < 20; ++i) {
    loop.Start(probe, folder);
    loop.Stop();
  }
  DWORD handles_after = 0;
  GetProcessHandleCount(GetCurrentProcess(), &handles_after);
  Check(handles_after <= handles_before + 2,
        "20 start/stop cycles do not leak handles");

  if (probe != nullptr) DestroyWindow(probe);
}

nv::FolderSnapshot BaseSnapshot() {  nv::FolderSnapshot snapshot;
  snapshot.folder_key = L"c:\\pics";
  snapshot.current_key = L"c:\\pics\\a.png";
  snapshot.dir_write_time = 100;
  snapshot.file_count = 5;
  snapshot.current = nv::FileSnapshot{true, 10, 200};
  return snapshot;
}

void TestWatcher() {
  using nv::WatchEventType;
  nv::FolderWatcher watcher;
  watcher.Reset(BaseSnapshot(), 0.0);
  Check(watcher.initialized(), "Reset installs a baseline");

  Check(watcher.Update(BaseSnapshot(), 2000.0).type == WatchEventType::kNone,
        "an unchanged folder reports nothing");

  nv::FolderSnapshot modified = BaseSnapshot();
  modified.current.size = 11;
  modified.current.write_time = 201;
  Check(watcher.Update(modified, 4000.0).type ==
            WatchEventType::kCurrentModified,
        "content changes are reported even though the directory timestamp is unchanged");
  Check(watcher.Update(modified, 6000.0).type == WatchEventType::kNone,
        "the same modification is reported only once");

  nv::FolderSnapshot added = modified;
  added.dir_write_time = 101;
  added.file_count = 6;
  Check(watcher.Update(added, 8000.0).type == WatchEventType::kFolderChanged,
        "a new file changes the directory timestamp");
  // Within the 2 s minimum the event waits instead of being lost.
  nv::FolderSnapshot added_again = added;
  added_again.dir_write_time = 102;
  Check(watcher.Update(added_again, 8100.0).type == WatchEventType::kNone,
        "the 2 s minimum suppresses a second immediate rescan");
  Check(watcher.rescan_deferred(),
        "a suppressed rescan is marked so a single notification can retry it");
  Check(watcher.Update(added_again, 10001.0).type ==
            WatchEventType::kFolderChanged,
        "the suppressed change is retried after the minimum interval");
  Check(!watcher.rescan_deferred(), "an applied rescan clears the retry marker");

  // Navigation re-baselines silently: the new file's size/mtime must not look
  // like a modification of the previous image.
  nv::FolderWatcher navigated;
  navigated.Reset(BaseSnapshot(), 0.0);
  nv::FolderSnapshot next_file = BaseSnapshot();
  next_file.current_key = L"c:\\pics\\b.png";
  next_file.current = nv::FileSnapshot{true, 40, 500};
  Check(navigated.Update(next_file, 1000.0).type == WatchEventType::kNone,
        "switching files re-baselines instead of reporting a modification");

  // Removal.
  nv::FolderWatcher removed_watcher;
  removed_watcher.Reset(BaseSnapshot(), 0.0);
  nv::FolderSnapshot removed = BaseSnapshot();
  removed.current = nv::FileSnapshot{};
  removed.dir_write_time = 300;
  Check(removed_watcher.Update(removed, 2000.0).type ==
            WatchEventType::kCurrentRemoved,
        "a missing current file is reported as removed");

  // Suppression: our own rename/save adopts the new state without an event.
  nv::FolderWatcher suppressed;
  suppressed.Reset(BaseSnapshot(), 0.0);
  suppressed.Suppress(1000.0);
  Check(suppressed.suppressed(2000.0), "suppression covers the window");
  Check(suppressed.Update(added, 2000.0).type == WatchEventType::kNone,
        "an own write inside the suppression window is ignored");
  Check(suppressed.Update(added, 5000.0).type == WatchEventType::kNone,
        "the suppressed state became the new baseline");
  Check(!suppressed.suppressed(5000.0), "suppression expires");

  // Remote drives have no file count; a directory timestamp change still works.
  nv::FolderWatcher remote;
  nv::FolderSnapshot base = BaseSnapshot();
  base.file_count.reset();
  remote.Reset(base, 0.0);
  nv::FolderSnapshot remote_added = base;
  remote_added.dir_write_time = 120;
  Check(remote.Update(remote_added, 3000.0).type ==
            WatchEventType::kFolderChanged,
        "remote snapshots detect changes from the directory timestamp alone");
}

}  // namespace

int RunFolderWatchTests() {
  const std::filesystem::path root = MakeTempRoot();
  TestWrapStrategy();
  TestSiblingPeeking(root);
  TestFolderOrder(root);
  TestRescan(root);
  TestWatcher();
  TestDirNotifier(root);
  std::error_code ec;
  std::filesystem::remove_all(root, ec);
  if (g_failures != 0) {
    std::printf("%d folder/watch check(s) failed\n", g_failures);
  }
  return g_failures;
}

}  // namespace nv::tests
