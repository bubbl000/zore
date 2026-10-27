// v2.0 S3 (plan 22/15): pure window-placement geometry plus the
// single-instance mutex name, payload and message-only receiver. No visible
// window and no user state are touched.

#include "tests/window_session_tests.h"

#include "core/disk_cache.h"
#include "platform/single_instance.h"
#include "platform/window_placement.h"

#include <windows.h>

#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <thread>
#include <vector>

namespace nv::tests {
namespace {

using nv::platform::IntRect;

int g_failures = 0;

void Check(bool ok, const char* name) {
  std::printf("%s %s\n", ok ? "ok  " : "FAIL", name);
  if (!ok) ++g_failures;
}

bool RectsEqual(const IntRect& actual, const IntRect& expected) {
  return actual.left == expected.left && actual.top == expected.top &&
         actual.right == expected.right && actual.bottom == expected.bottom;
}

void CheckRect(const IntRect& actual, const IntRect& expected,
               const char* name) {
  const bool ok = RectsEqual(actual, expected);
  std::printf("%s %s (actual=%d,%d,%d,%d expected=%d,%d,%d,%d)\n",
              ok ? "ok  " : "FAIL", name, actual.left, actual.top, actual.right,
              actual.bottom, expected.left, expected.top, expected.right,
              expected.bottom);
  if (!ok) ++g_failures;
}

std::wstring UniqueName(const wchar_t* prefix) {
  return std::wstring(prefix) + L"." +
         std::to_wstring(GetCurrentProcessId()) + L"." +
         std::to_wstring(GetTickCount64());
}

void TestNames() {
  const std::wstring portable =
      nv::platform::InstanceMutexName(L"C:\\Portable\\ZoreView");
  const std::wstring portable_trailing =
      nv::platform::InstanceMutexName(L"c:\\portable\\zoreview\\");
  const std::wstring installed = nv::platform::InstanceMutexName(
      L"C:\\Users\\user\\AppData\\Local\\ZoreView");
  Check(portable.rfind(L"Local\\ZoreView.Single.", 0) == 0,
        "mutex name is in the Local namespace");
  Check(portable == portable_trailing,
        "mutex name normalises case and a trailing separator");
  Check(portable != installed, "portable and installed names differ");
  Check(nv::platform::InstanceReceiverClass(L"C:\\Portable\\ZoreView") !=
            nv::platform::InstanceReceiverClass(
                L"C:\\Users\\user\\AppData\\Local\\ZoreView"),
        "receiver class names differ per data directory");
}

void TestPayload() {
  const std::wstring path = L"C:\\pics\\\u4e2d\u6587 \u56fe.jpg";
  const std::vector<char> payload = nv::platform::EncodeInstancePayload(path);
  Check(!payload.empty(), "payload encodes");
  std::wstring decoded;
  Check(nv::platform::DecodeInstancePayload(payload.data(),
                                            static_cast<DWORD>(payload.size()),
                                            &decoded),
        "payload decodes");
  Check(decoded == path, "Unicode path round-trips");

  const std::vector<char> empty = nv::platform::EncodeInstancePayload(L"");
  decoded = L"unchanged";
  Check(!empty.empty() &&
            nv::platform::DecodeInstancePayload(
                empty.data(), static_cast<DWORD>(empty.size()), &decoded),
            "empty payload decodes as activation");
  Check(decoded.empty(), "empty payload clears the path");

  std::vector<char> wrong_magic = payload;
  wrong_magic[0] = 'X';
  Check(!nv::platform::DecodeInstancePayload(
            wrong_magic.data(), static_cast<DWORD>(wrong_magic.size()),
            &decoded),
        "wrong magic is rejected");

  std::vector<char> wrong_version = payload;
  wrong_version[4] = 2;
  Check(!nv::platform::DecodeInstancePayload(
            wrong_version.data(), static_cast<DWORD>(wrong_version.size()),
            &decoded),
        "unknown version is rejected");

  Check(!nv::platform::DecodeInstancePayload(
            payload.data(), static_cast<DWORD>(payload.size() - 1), &decoded),
        "truncated payload is rejected");

  std::vector<char> bad_chars = payload;
  const DWORD huge = 0xFFFFFFFFu;
  std::memcpy(bad_chars.data() + 8, &huge, sizeof(huge));
  Check(!nv::platform::DecodeInstancePayload(
            bad_chars.data(), static_cast<DWORD>(bad_chars.size()), &decoded),
        "out-of-range character count is rejected");
  Check(!nv::platform::DecodeInstancePayload(nullptr, 0, &decoded),
        "null payload is rejected");
}

void TestPlacementGeometry() {
  const std::vector<IntRect> primary{{0, 0, 1920, 1040}};
  Check(nv::platform::PlacementVisible({100, 100, 500, 400}, primary),
        "on-screen placement is visible");
  Check(!nv::platform::PlacementVisible({2500, 1200, 3000, 1600}, primary),
        "off-screen placement is rejected");
  Check(!nv::platform::PlacementVisible({-50, 100, 50, 300}, primary),
        "a 50 px sliver is rejected (needs 100x100)");
  Check(!nv::platform::PlacementVisible({100, 100, 100, 400}, primary),
        "a zero-width placement is rejected");

  const std::vector<IntRect> dual{{-1920, 0, 0, 1080}, {0, 0, 1920, 1040}};
  Check(nv::platform::PlacementVisible({-1800, 100, -1000, 700}, dual),
        "negative monitor coordinates are valid");

  CheckRect(nv::platform::ClampPlacement({-100, -50, 1400, 750}, primary),
            {0, 0, 1500, 800}, "a partially off-screen rect is moved inside");
  CheckRect(nv::platform::ClampPlacement({1700, 900, 2500, 1400}, primary),
            {1120, 540, 1920, 1040}, "the bottom-right corner is clamped");
  CheckRect(nv::platform::ClampPlacement({0, 0, 3000, 2000}, primary),
            {0, 0, 1920, 1040}, "an oversized rect matches the work area");
  CheckRect(nv::platform::ClampPlacement({5000, 5000, 6000, 6000}, primary),
            {920, 40, 1920, 1040}, "a rect on no monitor snaps to the nearest");
  CheckRect(nv::platform::ClampPlacement({-1800, 100, -1000, 700}, dual),
            {-1800, 100, -1000, 700},
            "a rect on the secondary monitor is unchanged");
  CheckRect(nv::platform::ClampPlacement({-10, 100, 100, 400}, primary),
            {0, 100, 640, 580}, "the minimum size is enforced");

  CheckRect(nv::platform::CenterInWorkArea({100, 50, 1100, 850}, 800, 600),
            {200, 150, 1000, 750}, "centering uses the work area");
  CheckRect(nv::platform::CenterInWorkArea({0, 0, 1920, 1040}, 4000, 3000),
            {0, 0, 1920, 1040}, "centering clamps an oversized default");

  Check(nv::platform::ShouldSavePlacement(true, false, false),
        "normal windows save their placement");
  Check(!nv::platform::ShouldSavePlacement(false, false, false),
        "remember-window off does not save");
  Check(!nv::platform::ShouldSavePlacement(true, true, false),
        "fullscreen does not overwrite the placement");
  Check(!nv::platform::ShouldSavePlacement(true, false, true),
        "minimized does not overwrite the placement");
}

void TestMutex() {
  const std::wstring name = UniqueName(L"Local\\ZoreView.Test.Single");
  nv::platform::SingleInstanceLock first;
  bool already = true;
  Check(first.Acquire(name, &already) && !already,
        "the first lock creates the mutex");
  nv::platform::SingleInstanceLock second;
  bool already_second = false;
  Check(second.Acquire(name, &already_second) && already_second,
        "a second lock reports the existing instance");
  second.Release();
  first.Release();
  nv::platform::SingleInstanceLock third;
  bool already_third = true;
  Check(third.Acquire(name, &already_third) && !already_third,
        "releasing every handle removes the mutex");
}

void TestReceiver() {
  const std::wstring class_name = UniqueName(L"ZoreView.Test.Receiver");
  nv::platform::InstanceReceiver receiver;
  Check(receiver.Create(class_name), "the message-only receiver is created");
  Check(nv::platform::InstanceReceiver::Find(class_name) == receiver.hwnd(),
        "the receiver is found as a child of HWND_MESSAGE");

  std::wstring got;
  int calls = 0;
  receiver.SetHandler([&](const std::wstring& path) {
    got = path;
    ++calls;
  });
  std::wstring error;
  const std::wstring path = L"C:\\pics\\a.jpg";
  Check(nv::platform::ForwardInstanceMessage(receiver.hwnd(), path, 1000,
                                             &error),
        "forwarding to the receiver succeeds");
  Check(calls == 1 && got == path, "the receiver delivers the forwarded path");

  Check(nv::platform::ForwardInstanceMessage(receiver.hwnd(), L"", 1000, &error),
        "an empty forward succeeds");
  Check(calls == 2 && got.empty(), "an empty forward means activate");

  // A request received before the main window exists must be kept and
  // delivered when the handler is installed (規劃/22/15 坑 7).
  const std::wstring late_class = UniqueName(L"ZoreView.Test.LateReceiver");
  nv::platform::InstanceReceiver late;
  Check(late.Create(late_class), "a second receiver can exist");
  Check(nv::platform::ForwardInstanceMessage(late.hwnd(), path, 1000, &error),
        "a request is forwarded before the handler exists");
  std::wstring late_got;
  int late_calls = 0;
  late.SetHandler([&](const std::wstring& value) {
    late_got = value;
    ++late_calls;
  });
  Check(late_calls == 1 && late_got == path,
        "the pending request is delivered when the handler is installed");

  Check(!nv::platform::ForwardInstanceMessage(nullptr, path, 100, &error),
        "forwarding without a window fails");
  Check(!nv::platform::InstanceReceiver::Find(
            L"ZoreView.Test.NoSuchReceiver.00000000"),
        "an unknown receiver class is not found");

  receiver.Destroy();
  Check(nv::platform::InstanceReceiver::Find(class_name) == nullptr,
        "destroying the receiver removes it");
}

void TestDiskCacheConcurrency() {
  wchar_t temp[MAX_PATH]{};
  GetTempPathW(MAX_PATH, temp);
  const std::filesystem::path dir =
      std::filesystem::path(temp) /
      (L"nv-cache-test-" + std::to_wstring(GetCurrentProcessId()) + L"-" +
       std::to_wstring(GetTickCount64()));
  std::error_code ec;
  std::filesystem::remove_all(dir, ec);
  nv::DiskCache cache(dir / L"images", 512ull * 1024 * 1024);
  const std::filesystem::path source = dir / L"source.jpg";

  const auto make_image = [](uint32_t width, uint32_t height,
                             uint8_t fill) {
    auto image = std::make_shared<nv::Image>();
    image->info.width = width;
    image->info.height = height;
    image->info.stride = width * 4;
    image->info.format = nv::PixelFormat::kBgra8;
    image->pixels.assign(static_cast<size_t>(image->info.stride) * height, fill);
    return image;
  };
  const auto small = make_image(4, 4, 0x20);
  const auto large = make_image(8, 8, 0x80);

  // Two writers race for the same key. Every write stages under a unique
  // tmp-<pid>-<tick>-<seq> name and publishes with MoveFileEx, so the final
  // file is always one complete image (規劃/22/15 §6/坑 13).
  std::thread first([&] {
    for (int i = 0; i < 12; ++i) cache.Save(source, *small, false);
  });
  std::thread second([&] {
    for (int i = 0; i < 12; ++i) cache.Save(source, *large, false);
  });
  first.join();
  second.join();

  const auto loaded = cache.Load(source);
  Check(loaded != nullptr && !loaded->Empty(),
        "a concurrent cache write leaves a complete image");
  Check(loaded != nullptr &&
            ((loaded->info.width == 4 && loaded->info.height == 4) ||
             (loaded->info.width == 8 && loaded->info.height == 8)),
        "the loaded image is one of the two complete writes");

  // A truncated final file must be treated as a miss and removed.
  std::filesystem::path cached;
  for (const auto& item : std::filesystem::directory_iterator(dir / L"images")) {
    if (item.path().extension() == L".nvimg") cached = item.path();
  }
  Check(!cached.empty(), "the cache file exists");
  {
    std::ofstream file(cached, std::ios::binary | std::ios::trunc);
    file << "broken";
  }
  Check(cache.Load(source) == nullptr, "a corrupt cache file is a miss");
  Check(!std::filesystem::exists(cached), "a corrupt cache file is removed");

  std::filesystem::remove_all(dir, ec);
}

}  // namespace

int RunWindowSessionTests() {
  TestNames();
  TestPayload();
  TestPlacementGeometry();
  TestMutex();
  TestReceiver();
  TestDiskCacheConcurrency();
  return g_failures;
}

}  // namespace nv::tests
