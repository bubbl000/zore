// v2.0 S2: settings model (atomic write, migration, recovery, validation) and
// the pure settings-window layout (規劃/22/07 §7).
//
// Everything here works in a private temp directory: the real settings.ini is
// never touched, so the checks are safe in Release, Debug and ASan builds.

#include "tests/settings_tests.h"

#include "platform/settings.h"
#include "ui/metrics.h"
#include "ui/settings_layout.h"

#include <windows.h>

#include <cmath>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

namespace nv::tests {
namespace {

int g_failures = 0;

void Check(bool ok, const char* name) {
  std::printf("%s %s\n", ok ? "ok  " : "FAIL", name);
  if (!ok) ++g_failures;
}

void CheckInt(int actual, int expected, const char* name) {
  const bool ok = actual == expected;
  std::printf("%s %s (actual=%d expected=%d)\n", ok ? "ok  " : "FAIL", name,
              actual, expected);
  if (!ok) ++g_failures;
}

void CheckText(const std::wstring& actual, const wchar_t* expected,
               const char* name) {
  const bool ok = actual == expected;
  std::printf("%s %s\n", ok ? "ok  " : "FAIL", name);
  if (!ok) {
    std::printf("     actual=\"%ls\" expected=\"%ls\"\n", actual.c_str(),
                expected);
    ++g_failures;
  }
}

void CheckNear(float actual, float expected, const char* name,
               float epsilon = 0.5f) {
  const bool ok = std::fabs(actual - expected) <= epsilon;
  std::printf("%s %s (actual=%.2f expected=%.2f)\n", ok ? "ok  " : "FAIL",
              name, actual, expected);
  if (!ok) ++g_failures;
}

std::filesystem::path MakeTempDir() {
  wchar_t buffer[MAX_PATH]{};
  GetTempPathW(static_cast<DWORD>(std::size(buffer)), buffer);
  const std::filesystem::path dir =
      std::filesystem::path(buffer) /
      (L"nv-settings-test-" +
       std::to_wstring(static_cast<unsigned>(GetCurrentProcessId())) + L"-" +
       std::to_wstring(static_cast<unsigned>(GetTickCount64() % 100000)));
  std::error_code ec;
  std::filesystem::remove_all(dir, ec);
  std::filesystem::create_directories(dir, ec);
  return dir;
}

void WriteBytes(const std::filesystem::path& path, const void* data,
                size_t size) {
  std::ofstream file(path, std::ios::binary | std::ios::trunc);
  file.write(static_cast<const char*>(data), static_cast<std::streamsize>(size));
}

void WriteText(const std::filesystem::path& path, const char* text) {
  WriteBytes(path, text, std::strlen(text));
}

int BakCount(const std::filesystem::path& dir) {
  int count = 0;
  std::error_code ec;
  for (const auto& entry : std::filesystem::directory_iterator(dir, ec)) {
    const std::wstring name = entry.path().filename().wstring();
    if (name.find(L".bak-") != std::wstring::npos) ++count;
  }
  return count;
}

void CheckDefaults(const nv::platform::Settings& s, const char* prefix) {
  CheckInt(static_cast<int>(s.theme), 0, prefix);
  Check(s.checkerboard, "checkerboard defaults on");
  CheckInt(s.checker_opacity, 60, "checker opacity defaults to 60");
  CheckText(s.thumbstrip_mode, L"auto", "strip defaults to auto (22/08)");
  CheckText(s.sort_mode, L"name", "sort defaults to name");
  CheckText(s.wheel, L"zoom", "wheel defaults to zoom");
  CheckText(s.fullscreen_bg, L"theme", "fullscreen background defaults to theme");
  CheckText(s.folder_wrap, L"loop", "wrap defaults to loop");
  Check(s.remember_window, "remember window defaults on");
  Check(!s.single_instance, "single instance defaults off");
  Check(!s.pixel_mode, "pixel mode defaults off");
  Check(s.keep_rotation, "keep rotation defaults on");
  Check(std::fabs(s.zoom_step - 1.15) < 0.001, "zoom step defaults to 1.15");
  CheckInt(s.window_w, 1280, "window width default");
  CheckInt(s.window_h, 800, "window height default");
  Check(!s.window_saved, "no placement saved by default");
}

void TestDefaultsAndRoundTrip(const std::filesystem::path& dir) {
  const std::filesystem::path path = dir / L"roundtrip.ini";
  Check(!std::filesystem::exists(path), "roundtrip starts without a file");
  const nv::platform::Settings missing =
      nv::platform::LoadSettingsFromFile(path);
  CheckDefaults(missing, "missing file uses dark theme");
  CheckText(missing.language, L"", "language defaults to follow-system");

  nv::platform::Settings settings;
  settings.language = L"en";
  settings.theme = nv::platform::ThemeMode::kLight;
  settings.dark_theme = false;
  settings.checkerboard = false;
  settings.checker_opacity = 35;
  settings.thumbstrip_mode = L"auto";
  settings.birdseye = true;
  settings.topmost = true;
  settings.sort_mode = L"date";
  settings.remember_window = false;
  settings.single_instance = true;
  settings.wheel = L"page";
  settings.zoom_step = 1.10;
  settings.pixel_mode = true;
  settings.keep_rotation = false;
  settings.fullscreen_bg = L"black";
  settings.folder_wrap = L"ask";
  settings.window_x = 120;
  settings.window_y = 80;
  settings.window_w = 1600;
  settings.window_h = 900;
  settings.window_maximized = true;
  settings.window_saved = true;
  settings.info_window_visible = true;
  settings.info_window_x = 440;
  settings.info_window_y = 220;
  settings.info_window_saved = true;

  Check(nv::platform::SaveSettingsToFile(settings, path), "settings saved");
  Check(!std::filesystem::exists(path.wstring() + L".tmp"),
        "atomic save leaves no .tmp behind");
  Check(std::filesystem::exists(path), "settings.ini exists after save");

  const nv::platform::Settings loaded =
      nv::platform::LoadSettingsFromFile(path);
  CheckText(loaded.language, L"en", "language round-trips");
  CheckInt(static_cast<int>(loaded.theme), 1, "light theme round-trips");
  Check(!loaded.dark_theme, "light theme resolves dark_theme=false");
  Check(!loaded.checkerboard, "checkerboard round-trips");
  CheckInt(loaded.checker_opacity, 35, "checker opacity round-trips");
  CheckText(loaded.thumbstrip_mode, L"auto", "strip mode round-trips");
  Check(loaded.birdseye, "birdseye round-trips");
  Check(loaded.topmost, "topmost round-trips");
  CheckText(loaded.sort_mode, L"date", "sort mode round-trips");
  Check(!loaded.remember_window, "remember window round-trips");
  Check(loaded.single_instance, "single instance round-trips");
  CheckText(loaded.wheel, L"page", "wheel mode round-trips");
  Check(std::fabs(loaded.zoom_step - 1.10) < 0.001, "zoom step round-trips");
  Check(loaded.pixel_mode, "pixel mode round-trips");
  Check(!loaded.keep_rotation, "keep rotation round-trips");
  CheckText(loaded.fullscreen_bg, L"black", "fullscreen background round-trips");
  CheckText(loaded.folder_wrap, L"ask", "folder wrap round-trips");
  CheckInt(loaded.window_x, 120, "window x round-trips");
  CheckInt(loaded.window_y, 80, "window y round-trips");
  CheckInt(loaded.window_w, 1600, "window width round-trips");
  CheckInt(loaded.window_h, 900, "window height round-trips");
  Check(loaded.window_maximized, "maximized round-trips");
  Check(loaded.window_saved, "placement marker round-trips");
  Check(loaded.info_window_visible, "info window visibility round-trips");
  CheckInt(loaded.info_window_x, 440, "info window x round-trips");
  CheckInt(loaded.info_window_y, 220, "info window y round-trips");
  Check(loaded.info_window_saved, "info window placement marker round-trips");

  // Unknown keys survive a save: the file is copied before the profile API
  // rewrites the known values (規劃/22/07 §3.2). Re-saving must also keep the
  // file loadable, which is what the reload below asserts.
  nv::platform::Settings second = loaded;
  second.theme = nv::platform::ThemeMode::kDark;
  Check(nv::platform::SaveSettingsToFile(second, path), "second save");
  Check(nv::platform::LoadSettingsFromFile(path).dark_theme,
        "rewritten theme is dark again");
}

void TestInvalidValues(const std::filesystem::path& dir) {
  const std::filesystem::path path = dir / L"invalid.ini";
  WriteText(path,
            "[general]\nversion=1\ntheme=purple\nchecker=7\n"
            "checker_opacity=999\nstrip_mode=banana\nsort_mode=colour\n"
            "language=fr\n"
            "[view]\nwheel=spiral\nzoom_step=2.00\nfullscreen_bg=neon\n"
            "folder_wrap=bounce\n"
            "[window]\nx=-999999\ny=999999\nw=10\nh=999999\n"
            "info_visible=1\ninfo_x=-999999\ninfo_y=999999\n"
            "info_saved=1\n");
  const nv::platform::Settings s = nv::platform::LoadSettingsFromFile(path);
  CheckInt(static_cast<int>(s.theme), 0, "invalid theme falls back to dark");
  Check(s.checkerboard, "non-zero checker flag stays on");
  CheckInt(s.checker_opacity, 100, "checker opacity clamps to 100");
  CheckText(s.thumbstrip_mode, L"auto", "invalid strip falls back");
  CheckText(s.sort_mode, L"name", "invalid sort falls back");
  CheckText(s.language, L"", "unsupported language falls back to system");
  CheckText(s.wheel, L"zoom", "invalid wheel falls back");
  Check(std::fabs(s.zoom_step - 1.15) < 0.001, "invalid zoom step falls back");
  CheckText(s.fullscreen_bg, L"theme", "invalid fullscreen background falls back");
  CheckText(s.folder_wrap, L"loop", "invalid wrap falls back");
  CheckInt(s.window_x, 0, "out-of-range x falls back");
  CheckInt(s.window_y, 0, "out-of-range y falls back");
  CheckInt(s.window_w, 1280, "too-small width falls back");
  CheckInt(s.window_h, 800, "too-large height falls back");
  // v3.0 (規劃/26 §2.1/§4): an invalid info position is dropped so the window is
  // centred instead of restored off screen; the visibility flag stays.
  CheckInt(s.info_window_x, 0, "invalid info x falls back");
  CheckInt(s.info_window_y, 0, "invalid info y falls back");
  Check(!s.info_window_saved, "invalid info position clears the marker");
  Check(s.info_window_visible, "the info visibility flag survives");
}

void TestMigration(const std::filesystem::path& dir) {
  const std::filesystem::path path = dir / L"legacy.ini";
  WriteText(path,
            "[ui]\ndark_theme=0\ncheckerboard=0\nlanguage=en\n"
            "thumbstrip_mode=auto\nsort_mode=date\nbirdseye=1\ntopmost=1\n"
            "keep_rotation=0\nzoom_step=1.10\n");
  const nv::platform::Settings s = nv::platform::LoadSettingsFromFile(path);
  Check(s.migrated, "legacy [ui] section is migrated");
  CheckInt(static_cast<int>(s.theme), 1, "legacy dark_theme=0 becomes light");
  Check(!s.checkerboard, "legacy checkerboard carried over");
  CheckText(s.language, L"en", "legacy language carried over");
  CheckText(s.thumbstrip_mode, L"auto", "legacy strip mode carried over");
  CheckText(s.sort_mode, L"date", "legacy sort carried over");
  Check(s.birdseye && s.topmost, "legacy toggles carried over");
  Check(!s.keep_rotation, "legacy keep_rotation carried over");
  Check(std::fabs(s.zoom_step - 1.10) < 0.001, "legacy zoom step carried over");

  CheckInt(GetPrivateProfileIntW(L"general", L"version", 0, path.c_str()), 1,
           "migration writes schema version 1");
  wchar_t legacy[32]{};
  GetPrivateProfileStringW(L"ui", L"dark_theme", L"GONE", legacy,
                           static_cast<DWORD>(std::size(legacy)), path.c_str());
  CheckText(legacy, L"GONE", "legacy keys are removed after migration");

  const nv::platform::Settings again =
      nv::platform::LoadSettingsFromFile(path);
  Check(!again.migrated, "second load does not migrate again");
  CheckText(again.language, L"en", "migrated values survive a reload");
}

void TestRecovery(const std::filesystem::path& dir) {
  const std::filesystem::path path = dir / L"broken.ini";
  const char bytes[] = {'[', 'g', 'e', 'n', 'e', 'r', 'a', 'l', ']', '\n',
                        'v', 'e', 'r', 's', 'i', 'o', 'n', '=', '1', '\0',
                        'x', '\n'};
  WriteBytes(path, bytes, sizeof(bytes));
  const nv::platform::Settings s = nv::platform::LoadSettingsFromFile(path);
  Check(s.recovered, "unreadable file is reported as recovered");
  CheckDefaults(s, "recovered settings use defaults");
  Check(BakCount(dir) == 1, "exactly one .bak-* backup is kept");
  Check(std::filesystem::exists(path), "a fresh settings.ini is written");

  const std::filesystem::path empty = dir / L"empty.ini";
  WriteBytes(empty, "", 0);
  const nv::platform::Settings e = nv::platform::LoadSettingsFromFile(empty);
  Check(e.recovered, "empty file is recovered");
  Check(BakCount(dir) == 2, "empty file also gets a backup");
}

void TestRestoreDefaults() {
  nv::platform::Settings current;
  current.language = L"en";
  current.theme = nv::platform::ThemeMode::kLight;
  current.checker_opacity = 10;
  current.zoom_step = 1.25;
  current.single_instance = true;
  current.window_x = 300;
  current.window_y = 200;
  current.window_w = 1500;
  current.window_h = 950;
  current.window_saved = true;
  current.info_window_visible = true;
  current.info_window_x = 420;
  current.info_window_y = 260;
  current.info_window_saved = true;

  const nv::platform::Settings defaults =
      nv::platform::DefaultsForRestore(current);
  CheckText(defaults.language, L"en", "restore keeps the language");
  CheckInt(static_cast<int>(defaults.theme), 0, "restore resets the theme");
  CheckInt(defaults.checker_opacity, 60, "restore resets checker opacity");
  Check(std::fabs(defaults.zoom_step - 1.15) < 0.001,
        "restore resets the zoom step");
  Check(!defaults.single_instance, "restore resets single instance");
  CheckInt(defaults.window_x, 300, "restore keeps the window position");
  CheckInt(defaults.window_w, 1500, "restore keeps the window size");
  Check(defaults.window_saved, "restore keeps the placement marker");
  Check(defaults.info_window_visible, "restore keeps the info window state");
  CheckInt(defaults.info_window_x, 420, "restore keeps the info window x");
  CheckInt(defaults.info_window_y, 260, "restore keeps the info window y");
  Check(defaults.info_window_saved, "restore keeps the info placement marker");
}

nv::ui::SettingsRowSpec Row(nv::ui::SettingsRowKind kind, int options = 0,
                            float option_width = 120.0f) {
  nv::ui::SettingsRowSpec spec;
  spec.kind = kind;
  for (int i = 0; i < options; ++i) spec.option_widths.push_back(option_width);
  return spec;
}

void TestLayout() {
  using namespace nv::ui;
  SettingsLayoutInputs inputs;
  inputs.rows = {
      Row(SettingsRowKind::kGroup),
      Row(SettingsRowKind::kRadio, 3),
      Row(SettingsRowKind::kRadio, 3),
      Row(SettingsRowKind::kCheck),
      Row(SettingsRowKind::kSlider),
      Row(SettingsRowKind::kGroup),
      Row(SettingsRowKind::kRadio, 3),
      Row(SettingsRowKind::kCheck),
      Row(SettingsRowKind::kRadio, 3),
      Row(SettingsRowKind::kGroup),
      Row(SettingsRowKind::kCheck),
      Row(SettingsRowKind::kCheck),
      Row(SettingsRowKind::kCheck),
  };
  const Rect client = Rect::FromLTRB(0.0f, 0.0f, 950.0f, 700.0f);
  const SettingsLayout layout = ComputeSettingsLayout(inputs, 1.25f, client);

  CheckNear(layout.title.height(), 40.0f, "title is 32 DIP");
  CheckNear(layout.nav.width(), 200.0f, "nav column is 160 DIP");
  CheckNear(layout.footer.height(), 70.0f, "footer is 56 DIP");
  CheckNear(layout.content.left, layout.nav.right + 25.0f,
            "content starts after the nav column");
  CheckNear(layout.content.bottom, layout.footer.top,
            "content ends at the footer");
  CheckInt(static_cast<int>(layout.nav_rows.size()), 3, "three page rows");
  CheckNear(layout.nav_rows[0].height(), 50.0f, "nav row is 40 DIP");
  CheckNear(layout.nav_rows[0].top, layout.nav.top + 20.0f,
            "nav rows have a 16 DIP top padding");
  Check(layout.reset.left >= layout.footer.left &&
            layout.close.right <= layout.footer.right,
        "footer buttons stay inside the footer");
  Check(layout.reset.right < layout.close.left, "footer buttons do not overlap");

  CheckInt(static_cast<int>(layout.rows.size()), 13, "all rows are laid out");
  CheckNear(layout.rows[0].row.height(), 35.0f, "group row is 28 DIP");
  CheckNear(layout.rows[1].row.height(), 45.0f, "control row is 36 DIP");
  CheckNear(layout.rows[1].label.width(), 150.0f, "label column is 120 DIP");
  CheckInt(static_cast<int>(layout.rows[1].options.size()), 3,
           "radio options are laid out");
  Check(layout.rows[1].options[0].left < layout.rows[1].options[1].left &&
            layout.rows[1].options[1].left < layout.rows[1].options[2].left,
        "radio options are ordered");
  Check(layout.rows[1].options[2].right <= layout.body.right,
        "radio options stay inside the body");
  CheckNear(layout.rows[3].control.width(), 22.5f, "checkbox is 18 DIP");
  CheckNear(layout.rows[4].control.height(), 5.0f, "slider track is 4 DIP");

  // 2*25 padding + 3*35 groups + 10*45 rows = 605; the viewport is 590.
  CheckNear(layout.body_height, 605.0f, "body height follows the rows");
  CheckNear(layout.max_scroll, 15.0f, "max scroll is body minus viewport");

  inputs.scroll = 10000.0f;
  const SettingsLayout clamped = ComputeSettingsLayout(inputs, 1.25f, client);
  CheckNear(clamped.scroll, clamped.max_scroll, "scroll clamps to the range");
  CheckNear(clamped.body.top, clamped.content.top - clamped.max_scroll,
            "scrolled body moves up by the clamped offset");

  inputs.scroll = 0.0f;
  const SettingsLayout top = ComputeSettingsLayout(inputs, 1.25f, client);
  CheckInt(SettingsRowAt(top, top.rows[1].row.left + 5.0f,
                         top.rows[1].row.top + 5.0f),
           1, "hit-test finds the row under the pointer");
  CheckInt(SettingsRowAt(top, top.content.right - 1.0f,
                         top.rows[0].row.top + 1.0f),
           -1, "hit-test is limited to the viewport");
  CheckInt(SettingsNavAt(top, top.nav_rows[2].left + 4.0f,
                         top.nav_rows[2].top + 4.0f),
           2, "hit-test finds the nav page");
  CheckInt(SettingsNavAt(top, top.content.left + 4.0f,
                         top.nav_rows[0].top + 4.0f),
           -1, "nav hit-test does not leak into the content");

  // Association grid: 39 extensions, four columns.
  const AssociationLayout assoc =
      ComputeAssociationLayout(1.25f, top.content, 39, 4, 0.0f);
  CheckInt(static_cast<int>(assoc.cells.size()), 39, "39 association cells");
  CheckNear(assoc.cells[1].rect.width(), assoc.cells[0].rect.width(),
            "cells share a column width");
  Check(assoc.cells[3].rect.right <= assoc.list.right,
        "the fourth column stays inside the list");
  Check(assoc.cells[4].rect.top > assoc.cells[0].rect.top,
        "cells wrap to the next row");
  Check(assoc.max_scroll >= 0.0f, "association scroll range is valid");
  CheckInt(AssociationCellAt(assoc, assoc.cells[7].rect.left + 2.0f,
                             assoc.cells[7].rect.top + 2.0f),
           7, "association hit-test finds the cell");
}

}  // namespace

int RunSettingsTests() {
  g_failures = 0;
  const std::filesystem::path dir = MakeTempDir();
  TestDefaultsAndRoundTrip(dir);
  TestInvalidValues(dir);
  TestMigration(dir);
  TestRecovery(dir);
  TestRestoreDefaults();
  TestLayout();
  std::error_code ec;
  std::filesystem::remove_all(dir, ec);
  return g_failures;
}

}  // namespace nv::tests
