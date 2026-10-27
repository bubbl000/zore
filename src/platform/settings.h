#pragma once

#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

namespace nv::platform {

// Theme selection (規劃/22/07 §3.1). `system` follows the Windows app theme
// (AppsUseLightTheme) and is re-read on WM_SETTINGCHANGE("ImmersiveColorSet").
enum class ThemeMode {
  kDark,
  kLight,
  kSystem,
};

enum class WheelMode {
  kZoom,
  kPage,
};

enum class FullscreenBackground {
  kTheme,
  kBlack,
  kDim,
};

enum class FolderWrap {
  kLoop,
  kNext,
  kAsk,
};

// settings.ini schema version written under [general] version. Missing or older
// versions are migrated from the pre-S2 [ui] section.
inline constexpr int kSettingsVersion = 1;
inline constexpr wchar_t kSettingsFileName[] = L"settings.ini";

struct Settings {
  // --- [general] -----------------------------------------------------------
  // "zh" / "en"; empty means "follow the Windows display language" and is
  // resolved by nv::i18n at startup.
  std::wstring language;
  ThemeMode theme = ThemeMode::kDark;
  // Resolved effective theme. Not persisted: it is derived from `theme` (and
  // the system setting for kSystem) on load/apply so render/draw code keeps
  // using one bool.
  bool dark_theme = true;
  bool checkerboard = true;
  // 0-100; the checkerboard is blended towards the window background.
  int checker_opacity = 60;
  // "auto" | "pinned" | "off". Auto (fade in on hover) is the S2 default
  // (規劃/22/08 §3.1); pinned subtracts the rail from the image area.
  std::wstring thumbstrip_mode = L"auto";
  bool birdseye = false;
  bool topmost = false;
  // "name" | "date" | "size"
  std::wstring sort_mode = L"name";
  bool remember_window = true;
  bool single_instance = false;

  // --- [view] --------------------------------------------------------------
  // "zoom" | "page"
  std::wstring wheel = L"zoom";
  // Keyboard zoom step (1.05/1.10/1.15/1.25); unknown values fall back to 1.15.
  double zoom_step = 1.15;
  // Nearest-neighbour magnification for pixel-art inspection.
  bool pixel_mode = false;
  // Keep the view rotation when opening the next image (規劃/22/05 §3.5).
  bool keep_rotation = true;
  // "theme" | "black" | "opacity" (older files may say "dim"; both parse).
  std::wstring fullscreen_bg = L"theme";
  // "loop" | "next" | "ask"
  std::wstring folder_wrap = L"loop";

  // --- [window] ------------------------------------------------------------
  int window_x = 0;
  int window_y = 0;
  int window_w = 1280;
  int window_h = 800;
  bool window_maximized = false;
  // True once a placement has been stored, so a fresh install still centres
  // instead of restoring the 0,0 default (規劃/22/07 §3.2).
  bool window_saved = false;

  // v3.0 (規劃/26 §2.1): the standalone info window. `visible` is the user's
  // intent (not the temporary hide while minimised/fullscreen), so it survives a
  // restart and is restored once an image is up. The schema version stays 1:
  // missing keys fall back to hidden + centred (規劃/26 §4).
  bool info_window_visible = false;
  int info_window_x = 0;
  int info_window_y = 0;
  // True once the info window position has been stored; without it the window is
  // centred in the owner's work area instead of restoring 0,0.
  bool info_window_saved = false;

  // Runtime bookkeeping (never written).
  // The file existed but had to be backed up and recreated from defaults.
  bool recovered = false;
  // The legacy [ui] section was migrated to the S2 keys.
  bool migrated = false;
};

// settings.ini lives in DataDir(), so a portable copy and an installed copy
// never share state (規劃/22/02 §3.3). Creates the directory when needed.
std::wstring SettingsFilePath();

// Loads settings.ini, migrating the pre-S2 [ui] keys, validating every value and
// recovering from an unreadable/empty file (the broken file is kept as
// settings.ini.bak-<timestamp>). Never throws.
Settings LoadSettings();

// Atomic save: copies the existing file to settings.ini.tmp (so unknown keys and
// ordering survive), writes every known key, flushes the profile cache and
// replaces the original with MoveFileEx. Returns false and keeps the previous
// file untouched when the replacement fails.
bool SaveSettings(const Settings& settings);

// --- Shared path/test variants (規劃/22/07 §7) -----------------------------
Settings LoadSettingsFromFile(const std::filesystem::path& path);
bool SaveSettingsToFile(const Settings& settings,
                        const std::filesystem::path& path);

// --- Value parsing/formatting helpers --------------------------------------
const wchar_t* ThemeModeName(ThemeMode mode);
bool ParseThemeMode(std::wstring_view text, ThemeMode* mode);
const wchar_t* WheelModeName(WheelMode mode);
bool ParseWheelMode(std::wstring_view text, WheelMode* mode);
const wchar_t* FullscreenBackgroundName(FullscreenBackground value);
bool ParseFullscreenBackground(std::wstring_view text,
                               FullscreenBackground* value);
const wchar_t* FolderWrapName(FolderWrap value);
bool ParseFolderWrap(std::wstring_view text, FolderWrap* value);

// True when Windows is configured for the light app theme. Failures report
// false (dark), matching Windows' own default.
bool SystemPrefersLightTheme();
// Applies the system theme for ThemeMode::kSystem, otherwise the explicit one.
bool ResolveDarkTheme(ThemeMode mode);

// Allowed zoom steps, in display order.
const std::vector<double>& AllowedZoomSteps();
bool IsAllowedZoomStep(double step);

// Restore-defaults scope (規劃/22/07 坑 12): language, window placement and
// file associations are preserved.
Settings DefaultsForRestore(const Settings& current);

}  // namespace nv::platform
