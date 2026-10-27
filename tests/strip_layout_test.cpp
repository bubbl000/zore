// Layout and hit-testing checks for the thumbnail strip. No D3D device is
// created: these are the parts that are easy to get wrong (DPI scaling,
// scrolling, virtualized ranges) and cheap to pin down without a window.

#include "render/thumbnail_strip.h"

#include "tests/color_tests.h"
#include "tests/exif_tests.h"
#include "tests/folder_watch_tests.h"
#include "tests/settings_tests.h"
#include "tests/ui_layout_tests.h"
#include "tests/ui_tests.h"
#include "tests/window_session_tests.h"

#include <cmath>
#include <cstdio>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>

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

void CheckNear(float actual, float expected, float tolerance,
               const char* name) {
  const bool ok = std::fabs(actual - expected) <= tolerance;
  std::printf("%s %s (actual=%.2f expected=%.2f)\n", ok ? "ok  " : "FAIL",
              name, actual, expected);
  if (!ok) ++g_failures;
}

std::shared_ptr<nv::Image> MakeThumbnail() {
  auto image = std::make_shared<nv::Image>();
  image->info.width = 128;
  image->info.height = 80;
  image->info.stride = 128 * 4;
  image->info.format = nv::PixelFormat::kBgra8;
  image->pixels.resize(static_cast<size_t>(image->info.stride) * 80);
  return image;
}

}  // namespace

int main() {
  using nv::render::ThumbnailStrip;

  std::vector<std::filesystem::path> items;
  for (int i = 0; i < 40; ++i) {
    items.emplace_back(L"item" + std::to_wstring(i) + L".png");
  }

  ThumbnailStrip strip;
  Check(!strip.visible(), "empty strip is hidden");
  CheckNear(strip.height(), 76.0f, 0.01f, "base height is 76");

  strip.SetItems(items);
  Check(strip.visible(), "items make the strip visible");
  Check(strip.item_count() == 40, "item count");
  strip.SetDpiScale(1.0f);
  strip.SetViewWidth(1000.0f);
  strip.ResetScroll();
  strip.SetCurrent(0);

  // Client height 800 at scale 1: strip occupies y=724..800, thumbs y=732..792.
  size_t index = ThumbnailStrip::kNone;
  Check(strip.HitTest(40.0f, 762.0f, 1000.0f, 800.0f, &index) && index == 0,
        "hit first thumbnail");
  Check(!strip.HitTest(40.0f, 700.0f, 1000.0f, 800.0f, &index),
        "miss above the strip");
  Check(!strip.HitTest(40.0f, 820.0f, 1000.0f, 800.0f, &index),
        "miss below the client area");
  // Item 14 spans x=962..1022, so its visible part is still clickable.
  Check(strip.HitTest(990.0f, 762.0f, 1000.0f, 800.0f, &index) &&
            index == 14,
        "hit partially visible item");

  strip.ScrollBy(200.0f);
  CheckNear(strip.scroll(), 200.0f, 0.01f, "scroll clamps to available range");
  std::vector<size_t> visible;
  strip.VisibleIndices(0, &visible);
  Check(!visible.empty() && visible.front() == 2,
        "visible range starts at first fully visible item");

  strip.EnsureVisible(20);
  CheckNear(strip.scroll(), 448.0f, 0.01f, "ensure-visible centers selection");
  Check(strip.HitTest(950.0f, 762.0f, 1000.0f, 800.0f, &index) && index == 20,
        "selected item is visible after ensure-visible");

  // 150% DPI: every metric scales, so the geometry must follow.
  strip.SetDpiScale(1.5f);
  CheckNear(strip.height(), 114.0f, 0.01f, "height scales with DPI");
  strip.ResetScroll();
  strip.SetViewWidth(1000.0f);
  Check(strip.HitTest(66.0f, 743.0f, 1000.0f, 800.0f, &index) && index == 0,
        "hit at 150% DPI");
  Check(!strip.HitTest(66.0f, 690.0f, 1000.0f, 800.0f, &index),
        "miss above strip at 150% DPI");

  // Virtualization: one screen plus margin, not all 40 items.
  strip.SetDpiScale(1.0f);
  strip.ResetScroll();
  visible.clear();
  strip.VisibleIndices(2, &visible);
  Check(visible.front() == 0 && visible.back() <= 19,
        "visible range is bounded");
  Check(visible.size() < items.size(), "visible range is virtualized");

  strip.SetCurrent(ThumbnailStrip::kNone);
  Check(strip.current() == ThumbnailStrip::kNone,
        "kNone clears the selection");
  strip.SetCurrent(1000);
  Check(strip.current() == items.size() - 1, "current index clamps to the list");
  strip.SetCurrent(0);

  strip.SetThumbnail(0, MakeThumbnail());
  Check(strip.HasThumbnail(0), "thumbnail stored");
  strip.SetItems(items);
  Check(strip.HasThumbnail(0), "identical item list keeps thumbnails");

  std::vector<std::filesystem::path> fewer(items.begin(), items.end() - 1);
  strip.SetItems(fewer);
  Check(!strip.HasThumbnail(0), "new item list clears thumbnails");

  g_failures += RunColorTests();
  g_failures += nv::tests::RunUiTests();
  g_failures += nv::tests::RunUiLayoutTests();
  g_failures += nv::tests::RunExifTests();
  g_failures += nv::tests::RunSettingsTests();
  g_failures += nv::tests::RunWindowSessionTests();
  g_failures += nv::tests::RunFolderWatchTests();

  if (g_failures != 0) {
    std::printf("%d check(s) failed\n", g_failures);
    return 1;
  }
  std::printf("all strip layout checks passed\n");
  return 0;
}
