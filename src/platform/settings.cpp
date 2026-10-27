#include "platform/settings.h"

#include "platform/i18n.h"
#include "platform/log.h"
#include "platform/paths.h"

#include <windows.h>

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cwchar>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <system_error>
#include <vector>

namespace nv::platform {
namespace {

constexpr wchar_t kGeneral[] = L"general";
constexpr wchar_t kView[] = L"view";
constexpr wchar_t kWindowSection[] = L"window";
// Pre-S2 layout: everything lived in one section.
constexpr wchar_t kLegacy[] = L"ui";

constexpr int kMinWindowWidth = 640;
constexpr int kMinWindowHeight = 480;

bool EqualsNoCase(std::wstring_view a, std::wstring_view b) {
  if (a.size() != b.size()) return false;
  return _wcsnicmp(a.data(), b.data(), a.size()) == 0;
}

std::wstring ReadString(const std::filesystem::path& path,
                        const wchar_t* section, const wchar_t* key,
                        const wchar_t* fallback) {
  wchar_t buffer[512]{};
  GetPrivateProfileStringW(section, key, fallback, buffer,
                           static_cast<DWORD>(std::size(buffer)),
                           path.c_str());
  return buffer;
}

int ReadInt(const std::filesystem::path& path, const wchar_t* section,
            const wchar_t* key, int fallback) {
  return static_cast<int>(GetPrivateProfileIntW(section, key, fallback,
                                                path.c_str()));
}

double ReadZoomStep(const std::filesystem::path& path,
                    const wchar_t* section) {
  const std::wstring text = ReadString(path, section, L"zoom_step", L"1.15");
  const double parsed = _wtof(text.c_str());
  if (IsAllowedZoomStep(parsed)) return parsed;
  return 1.15;
}

// A settings file that is empty, has no sections, or contains raw NUL bytes
// without a UTF-16 BOM cannot be read reliably. It is preserved next to the new
// file so the user can recover hand-edited values (規劃/22/07 §3.2).
bool FileLooksUnreadable(const std::filesystem::path& path) {
  std::error_code ec;
  const uintmax_t size = std::filesystem::file_size(path, ec);
  if (ec || size == 0) return true;

  std::ifstream stream(path, std::ios::binary);
  if (!stream) return true;
  std::vector<char> head(64 * 1024);
  stream.read(head.data(), static_cast<std::streamsize>(head.size()));
  head.resize(static_cast<size_t>(stream.gcount()));
  if (head.size() >= 2) {
    const unsigned char b0 = static_cast<unsigned char>(head[0]);
    const unsigned char b1 = static_cast<unsigned char>(head[1]);
    if ((b0 == 0xFF && b1 == 0xFE) || (b0 == 0xFE && b1 == 0xFF)) {
      return false;  // profile API understands UTF-16 with a BOM
    }
  }
  for (const char byte : head) {
    if (byte == '\0') return true;
  }

  std::vector<wchar_t> names(8192, L'\0');
  const DWORD count = GetPrivateProfileSectionNamesW(
      names.data(), static_cast<DWORD>(names.size()), path.c_str());
  return count == 0;
}

std::wstring TimestampSuffix() {
  SYSTEMTIME time{};
  GetLocalTime(&time);
  wchar_t buffer[32]{};
  std::swprintf(buffer, std::size(buffer), L"%04u%02u%02u-%02u%02u%02u",
                time.wYear, time.wMonth, time.wDay, time.wHour, time.wMinute,
                time.wSecond);
  return buffer;
}

std::wstring BackupPath(const std::filesystem::path& path) {
  return path.wstring() + L".bak-" + TimestampSuffix();
}

// Reads the pre-S2 [ui] keys when [general] has no version. Missing keys keep
// the S2 defaults, so an old install upgrades to exactly its previous behavior.
bool ReadLegacy(const std::filesystem::path& path, Settings* settings) {
  wchar_t names[4096]{};
  const DWORD count = GetPrivateProfileSectionNamesW(
      names, static_cast<DWORD>(std::size(names)), path.c_str());
  if (count == 0) return false;

  bool found = false;
  for (const wchar_t* name = names; *name != L'\0'; name += std::wcslen(name) + 1) {
    if (EqualsNoCase(name, kLegacy)) {
      found = true;
      break;
    }
  }
  if (!found) return false;

  settings->dark_theme =
      ReadInt(path, kLegacy, L"dark_theme", settings->dark_theme ? 1 : 0) != 0;
  settings->theme = settings->dark_theme ? ThemeMode::kDark : ThemeMode::kLight;
  settings->checkerboard =
      ReadInt(path, kLegacy, L"checkerboard", settings->checkerboard ? 1 : 0) !=
      0;

  std::wstring language = ReadString(path, kLegacy, L"language", L"");
  settings->language = language;

  std::wstring value = ReadString(path, kLegacy, L"thumbstrip_mode", L"auto");
  if (EqualsNoCase(value, L"pinned") || EqualsNoCase(value, L"auto") ||
      EqualsNoCase(value, L"off")) {
    settings->thumbstrip_mode = value;
  }
  value = ReadString(path, kLegacy, L"sort_mode", L"name");
  if (EqualsNoCase(value, L"name") || EqualsNoCase(value, L"date") ||
      EqualsNoCase(value, L"size")) {
    settings->sort_mode = value;
  }
  settings->birdseye = ReadInt(path, kLegacy, L"birdseye", 0) != 0;
  settings->topmost = ReadInt(path, kLegacy, L"topmost", 0) != 0;
  settings->keep_rotation =
      ReadInt(path, kLegacy, L"keep_rotation", 1) != 0;
  settings->zoom_step = ReadZoomStep(path, kLegacy);
  return true;
}

void RemoveLegacySection(const std::filesystem::path& path) {
  const wchar_t* keys[] = {L"dark_theme",  L"checkerboard", L"language",
                           L"thumbstrip_mode", L"birdseye",  L"topmost",
                           L"sort_mode",   L"zoom_step",    L"keep_rotation"};
  for (const wchar_t* key : keys) {
    WritePrivateProfileStringW(kLegacy, key, nullptr, path.c_str());
  }
  WritePrivateProfileStringW(kLegacy, nullptr, nullptr, path.c_str());
}

void Normalize(Settings* settings) {
  settings->checker_opacity =
      std::clamp(settings->checker_opacity, 0, 100);

  if (!(EqualsNoCase(settings->thumbstrip_mode, L"pinned") ||
        EqualsNoCase(settings->thumbstrip_mode, L"auto") ||
        EqualsNoCase(settings->thumbstrip_mode, L"off"))) {
    settings->thumbstrip_mode = L"auto";
  }
  if (!(EqualsNoCase(settings->sort_mode, L"name") ||
        EqualsNoCase(settings->sort_mode, L"date") ||
        EqualsNoCase(settings->sort_mode, L"size"))) {
    settings->sort_mode = L"name";
  }
  if (!(EqualsNoCase(settings->wheel, L"zoom") ||
        EqualsNoCase(settings->wheel, L"page"))) {
    settings->wheel = L"zoom";
  }
  if (!(EqualsNoCase(settings->fullscreen_bg, L"theme") ||
        EqualsNoCase(settings->fullscreen_bg, L"black") ||
        EqualsNoCase(settings->fullscreen_bg, L"opacity") ||
        EqualsNoCase(settings->fullscreen_bg, L"dim"))) {
    settings->fullscreen_bg = L"theme";
  }
  if (!(EqualsNoCase(settings->folder_wrap, L"loop") ||
        EqualsNoCase(settings->folder_wrap, L"next") ||
        EqualsNoCase(settings->folder_wrap, L"ask"))) {
    settings->folder_wrap = L"loop";
  }
  if (!IsAllowedZoomStep(settings->zoom_step)) settings->zoom_step = 1.15;

  if (settings->window_w < kMinWindowWidth ||
      settings->window_w > 16384) {
    settings->window_w = 1280;
  }
  if (settings->window_h < kMinWindowHeight ||
      settings->window_h > 16384) {
    settings->window_h = 800;
  }
  if (settings->window_x < -32768 || settings->window_x > 32768) {
    settings->window_x = 0;
  }
  if (settings->window_y < -32768 || settings->window_y > 32768) {
    settings->window_y = 0;
  }
  // Info window (規劃/26 §2.1): an invalid/absent position is treated as "never
  // stored", so the window is centred instead of appearing off screen.
  if (settings->info_window_x < -32768 || settings->info_window_x > 32768 ||
      settings->info_window_y < -32768 || settings->info_window_y > 32768) {
    settings->info_window_x = 0;
    settings->info_window_y = 0;
    settings->info_window_saved = false;
  }

  if (!settings->language.empty()) {
    nv::i18n::Language language = nv::i18n::Language::kEnglish;
    if (!nv::i18n::ParseLanguage(settings->language, &language)) {
      settings->language.clear();
    } else {
      settings->language = nv::i18n::LanguageCode(language);
    }
  }

  settings->dark_theme = ResolveDarkTheme(settings->theme);
}

}  // namespace

const std::vector<double>& AllowedZoomSteps() {
  static const std::vector<double> kSteps = {1.05, 1.10, 1.15, 1.25};
  return kSteps;
}

bool IsAllowedZoomStep(double step) {
  for (const double allowed : AllowedZoomSteps()) {
    if (std::fabs(step - allowed) < 0.001) return true;
  }
  return false;
}

const wchar_t* ThemeModeName(ThemeMode mode) {
  switch (mode) {
    case ThemeMode::kLight: return L"light";
    case ThemeMode::kSystem: return L"system";
    case ThemeMode::kDark: break;
  }
  return L"dark";
}

bool ParseThemeMode(std::wstring_view text, ThemeMode* mode) {
  ThemeMode parsed = ThemeMode::kDark;
  if (EqualsNoCase(text, L"dark")) {
    parsed = ThemeMode::kDark;
  } else if (EqualsNoCase(text, L"light")) {
    parsed = ThemeMode::kLight;
  } else if (EqualsNoCase(text, L"system")) {
    parsed = ThemeMode::kSystem;
  } else {
    return false;
  }
  if (mode != nullptr) *mode = parsed;
  return true;
}

const wchar_t* WheelModeName(WheelMode mode) {
  return mode == WheelMode::kPage ? L"page" : L"zoom";
}

bool ParseWheelMode(std::wstring_view text, WheelMode* mode) {
  if (EqualsNoCase(text, L"zoom")) {
    if (mode) *mode = WheelMode::kZoom;
    return true;
  }
  if (EqualsNoCase(text, L"page")) {
    if (mode) *mode = WheelMode::kPage;
    return true;
  }
  return false;
}

const wchar_t* FullscreenBackgroundName(FullscreenBackground value) {
  switch (value) {
    case FullscreenBackground::kBlack: return L"black";
    case FullscreenBackground::kDim: return L"opacity";
    case FullscreenBackground::kTheme: break;
  }
  return L"theme";
}

bool ParseFullscreenBackground(std::wstring_view text,
                               FullscreenBackground* value) {
  if (EqualsNoCase(text, L"theme")) {
    if (value) *value = FullscreenBackground::kTheme;
    return true;
  }
  if (EqualsNoCase(text, L"black")) {
    if (value) *value = FullscreenBackground::kBlack;
    return true;
  }
  if (EqualsNoCase(text, L"opacity") || EqualsNoCase(text, L"dim")) {
    if (value) *value = FullscreenBackground::kDim;
    return true;
  }
  return false;
}

const wchar_t* FolderWrapName(FolderWrap value) {
  switch (value) {
    case FolderWrap::kNext: return L"next";
    case FolderWrap::kAsk: return L"ask";
    case FolderWrap::kLoop: break;
  }
  return L"loop";
}

bool ParseFolderWrap(std::wstring_view text, FolderWrap* value) {
  if (EqualsNoCase(text, L"loop")) {
    if (value) *value = FolderWrap::kLoop;
    return true;
  }
  if (EqualsNoCase(text, L"next")) {
    if (value) *value = FolderWrap::kNext;
    return true;
  }
  if (EqualsNoCase(text, L"ask")) {
    if (value) *value = FolderWrap::kAsk;
    return true;
  }
  return false;
}

bool SystemPrefersLightTheme() {
  DWORD value = 0;
  DWORD bytes = static_cast<DWORD>(sizeof(value));
  const LSTATUS status = RegGetValueW(
      HKEY_CURRENT_USER,
      L"Software\\Microsoft\\Windows\\CurrentVersion\\Themes\\Personalize",
      L"AppsUseLightTheme", RRF_RT_REG_DWORD, nullptr, &value, &bytes);
  return status == ERROR_SUCCESS && value != 0;
}

bool ResolveDarkTheme(ThemeMode mode) {
  switch (mode) {
    case ThemeMode::kLight: return false;
    case ThemeMode::kSystem: return !SystemPrefersLightTheme();
    case ThemeMode::kDark: break;
  }
  return true;
}

std::wstring SettingsFilePath() {
  const std::filesystem::path dir = DataDir();
  std::error_code ec;
  std::filesystem::create_directories(dir, ec);
  return (dir / kSettingsFileName).wstring();
}

Settings LoadSettingsFromFile(const std::filesystem::path& path) {
  Settings settings;

  std::error_code ec;
  const bool exists = std::filesystem::exists(path, ec) && !ec;
  if (exists && FileLooksUnreadable(path)) {
    const std::wstring backup = BackupPath(path);
    if (MoveFileExW(path.wstring().c_str(), backup.c_str(),
                    MOVEFILE_REPLACE_EXISTING)) {
      LogWarn("settings: unreadable file backed up to " +
              std::filesystem::path(backup).filename().string());
    } else {
      LogError("settings: unreadable file could not be backed up");
    }
    settings.recovered = true;
  }

  const int version = ReadInt(path, kGeneral, L"version", 0);
  const bool needs_migration = version < kSettingsVersion;

  if (needs_migration && ReadLegacy(path, &settings)) {
    settings.migrated = true;
  }

  // Tolerate a partially written/hand-edited file: every read falls back to its
  // default, so a broken section cannot make the reader fail.
  settings.language =
      ReadString(path, kGeneral, L"language", settings.language.c_str());
  ThemeMode theme = settings.theme;
  ParseThemeMode(ReadString(path, kGeneral, L"theme", ThemeModeName(theme)),
                 &theme);
  settings.theme = theme;
  settings.checkerboard =
      ReadInt(path, kGeneral, L"checker", settings.checkerboard ? 1 : 0) != 0;
  settings.checker_opacity =
      ReadInt(path, kGeneral, L"checker_opacity", settings.checker_opacity);
  settings.thumbstrip_mode = ReadString(path, kGeneral, L"strip_mode",
                                        settings.thumbstrip_mode.c_str());
  settings.birdseye =
      ReadInt(path, kGeneral, L"birdseye", settings.birdseye ? 1 : 0) != 0;
  settings.topmost =
      ReadInt(path, kGeneral, L"always_on_top", settings.topmost ? 1 : 0) != 0;
  settings.sort_mode =
      ReadString(path, kGeneral, L"sort_mode", settings.sort_mode.c_str());
  settings.remember_window =
      ReadInt(path, kGeneral, L"remember_window",
              settings.remember_window ? 1 : 0) != 0;
  settings.single_instance =
      ReadInt(path, kGeneral, L"single_instance",
              settings.single_instance ? 1 : 0) != 0;

  settings.wheel = ReadString(path, kView, L"wheel", settings.wheel.c_str());
  // A migrated file may have used [ui] zoom_step; keep it when [view] is absent.
  if (ReadString(path, kView, L"zoom_step", L"").empty()) {
    settings.zoom_step = ReadZoomStep(path, kLegacy);
  } else {
    settings.zoom_step = ReadZoomStep(path, kView);
  }
  settings.pixel_mode =
      ReadInt(path, kView, L"pixel_mode", settings.pixel_mode ? 1 : 0) != 0;
  settings.keep_rotation =
      ReadInt(path, kView, L"keep_rotation",
              settings.keep_rotation ? 1 : 0) != 0;
  settings.fullscreen_bg = ReadString(path, kView, L"fullscreen_bg",
                                      settings.fullscreen_bg.c_str());
  settings.folder_wrap =
      ReadString(path, kView, L"folder_wrap", settings.folder_wrap.c_str());

  settings.window_x = ReadInt(path, kWindowSection, L"x", settings.window_x);
  settings.window_y = ReadInt(path, kWindowSection, L"y", settings.window_y);
  settings.window_w = ReadInt(path, kWindowSection, L"w", settings.window_w);
  settings.window_h = ReadInt(path, kWindowSection, L"h", settings.window_h);
  settings.window_maximized =
      ReadInt(path, kWindowSection, L"maximized", 0) != 0;
  settings.window_saved = ReadInt(path, kWindowSection, L"saved", 0) != 0;
  settings.info_window_visible =
      ReadInt(path, kWindowSection, L"info_visible",
              settings.info_window_visible ? 1 : 0) != 0;
  settings.info_window_x =
      ReadInt(path, kWindowSection, L"info_x", settings.info_window_x);
  settings.info_window_y =
      ReadInt(path, kWindowSection, L"info_y", settings.info_window_y);
  settings.info_window_saved =
      ReadInt(path, kWindowSection, L"info_saved",
              settings.info_window_saved ? 1 : 0) != 0;

  Normalize(&settings);

  if (settings.migrated) {
    LogInfo("settings: migrated legacy [ui] keys");
    if (SaveSettingsToFile(settings, path)) {
      RemoveLegacySection(path);
    }
  } else if (settings.recovered) {
    LogInfo("settings: recovered with defaults");
    SaveSettingsToFile(settings, path);
  }

  return settings;
}

bool SaveSettingsToFile(const Settings& input,
                        const std::filesystem::path& path) {
  Settings settings = input;
  settings.dark_theme = ResolveDarkTheme(settings.theme);

  std::error_code ec;
  std::filesystem::create_directories(path.parent_path(), ec);

  const std::wstring target = path.wstring();
  const std::wstring temp = target + L".tmp";
  DeleteFileW(temp.c_str());
  if (GetFileAttributesW(target.c_str()) != INVALID_FILE_ATTRIBUTES &&
      !CopyFileW(target.c_str(), temp.c_str(), FALSE)) {
    LogError("settings: could not stage settings.ini.tmp");
    return false;
  }

  bool ok = true;
  const auto set = [&ok, &temp](const wchar_t* section, const wchar_t* key,
                                const std::wstring& value) {
    if (WritePrivateProfileStringW(section, key, value.c_str(),
                                   temp.c_str()) == FALSE) {
      ok = false;
    }
  };
  const auto set_int = [&set](const wchar_t* section, const wchar_t* key,
                              int value) {
    set(section, key, std::to_wstring(value));
  };
  const auto set_double = [&set](const wchar_t* section, const wchar_t* key,
                                 double value) {
    wchar_t buffer[32]{};
    std::swprintf(buffer, std::size(buffer), L"%.2f", value);
    set(section, key, buffer);
  };

  set(kGeneral, L"version", std::to_wstring(kSettingsVersion));
  set(kGeneral, L"language", settings.language);
  set(kGeneral, L"theme", ThemeModeName(settings.theme));
  set_int(kGeneral, L"checker", settings.checkerboard ? 1 : 0);
  set_int(kGeneral, L"checker_opacity", settings.checker_opacity);
  set(kGeneral, L"strip_mode", settings.thumbstrip_mode);
  set_int(kGeneral, L"birdseye", settings.birdseye ? 1 : 0);
  set_int(kGeneral, L"always_on_top", settings.topmost ? 1 : 0);
  set(kGeneral, L"sort_mode", settings.sort_mode);
  set_int(kGeneral, L"remember_window", settings.remember_window ? 1 : 0);
  set_int(kGeneral, L"single_instance", settings.single_instance ? 1 : 0);

  set(kView, L"wheel", settings.wheel);
  set_double(kView, L"zoom_step", settings.zoom_step);
  set_int(kView, L"pixel_mode", settings.pixel_mode ? 1 : 0);
  set_int(kView, L"keep_rotation", settings.keep_rotation ? 1 : 0);
  set(kView, L"fullscreen_bg", settings.fullscreen_bg);
  set(kView, L"folder_wrap", settings.folder_wrap);

  set_int(kWindowSection, L"x", settings.window_x);
  set_int(kWindowSection, L"y", settings.window_y);
  set_int(kWindowSection, L"w", settings.window_w);
  set_int(kWindowSection, L"h", settings.window_h);
  set_int(kWindowSection, L"maximized", settings.window_maximized ? 1 : 0);
  set_int(kWindowSection, L"saved", settings.window_saved ? 1 : 0);
  set_int(kWindowSection, L"info_visible",
          settings.info_window_visible ? 1 : 0);
  set_int(kWindowSection, L"info_x", settings.info_window_x);
  set_int(kWindowSection, L"info_y", settings.info_window_y);
  set_int(kWindowSection, L"info_saved", settings.info_window_saved ? 1 : 0);

  // The profile API caches writes; flush before moving the file or the last
  // keys can be missing from the replaced file.
  WritePrivateProfileStringW(nullptr, nullptr, nullptr, temp.c_str());

  bool replaced = false;
  if (ok) {
    for (int attempt = 0; attempt < 3 && !replaced; ++attempt) {
      if (MoveFileExW(temp.c_str(), target.c_str(),
                      MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        replaced = true;
      } else {
        Sleep(30);
      }
    }
  }
  if (!replaced) {
    DeleteFileW(temp.c_str());
    LogError("settings: save failed (settings.ini left unchanged)");
    return false;
  }
  return true;
}

Settings LoadSettings() {
  return LoadSettingsFromFile(std::filesystem::path(SettingsFilePath()));
}

bool SaveSettings(const Settings& settings) {
  return SaveSettingsToFile(settings,
                            std::filesystem::path(SettingsFilePath()));
}

Settings DefaultsForRestore(const Settings& current) {
  Settings defaults;
  // 規劃/22/07 坑 12: restoring defaults keeps the language, the window
  // placement and the association state (the latter is not part of Settings).
  defaults.language = current.language;
  defaults.window_x = current.window_x;
  defaults.window_y = current.window_y;
  defaults.window_w = current.window_w;
  defaults.window_h = current.window_h;
  defaults.window_maximized = current.window_maximized;
  defaults.window_saved = current.window_saved;
  defaults.info_window_visible = current.info_window_visible;
  defaults.info_window_x = current.info_window_x;
  defaults.info_window_y = current.info_window_y;
  defaults.info_window_saved = current.info_window_saved;
  return defaults;
}

}  // namespace nv::platform
