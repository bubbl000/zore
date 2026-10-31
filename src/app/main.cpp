// ZoreView M1: Win32 shell + WIC/FreeImage decode + D3D11/D2D display
// + bottom thumbnail strip with asynchronous previews.

#include "core/cache_key.h"
#include "core/animation_controller.h"
#include "core/decode_service.h"
#include "core/disk_cache.h"
#include "core/disk_cache_writer.h"
#include "core/display_tier.h"
#include "core/exif.h"
#include "core/exif_service.h"
#include "core/folder_navigator.h"
#include "core/folder_watcher.h"
#include "core/format_router.h"
#include "core/format_summary.h"
#include "core/icc_profile.h"
#include "core/image_cache.h"
#include "core/image_transform.h"
#include "core/preview_plan.h"
#include "core/thumbnail_service.h"
#include "core/ui_error.h"
#include "core/version.h"
#include "platform/clipboard.h"
#include "platform/crash.h"
#include "platform/dir_notify.h"
#include "platform/dpi.h"
#include "platform/drag_drop.h"
#include "platform/encoder_probe.h"
#include "platform/file_association.h"
#include "platform/file_data_object.h"
#include "platform/file_drop_source.h"
#include "platform/file_ops.h"
#include "platform/i18n.h"
#include "platform/image_io.h"
#include "platform/save_dialog.h"
#include "platform/input_dialog.h"
#include "platform/installer.h"
#include "platform/log.h"
#include "shell/thumbnail_registry.h"
#include "platform/paths.h"
#include "platform/save_service.h"
#include "platform/screenshot.h"
#include "platform/settings.h"
#include "platform/single_instance.h"
#include "platform/shell_integration.h"
#include "platform/theme.h"
#include "platform/window_placement.h"
#include "render/overlay_layer.h"
#include "render/renderer.h"
#include "render/thumbnail_strip.h"
#include "render/view_transform.h"
#include "render/zoom_math.h"
#include "ui/command.h"
#include "ui/drag_threshold.h"
#include "ui/info_window.h"
#include "ui/metrics.h"
#include "ui/minimap.h"
#include "ui/settings_window.h"
#include "ui/ui_root.h"
#include "ui/window_state.h"

#include <windows.h>
#include <windowsx.h>
#include <commdlg.h>
#include <dwmapi.h>
#include <psapi.h>
#include <shellapi.h>
#include <shlobj.h>
#include <shobjidl.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cwchar>
#include <cwctype>
#include <filesystem>
#include <fstream>
#include <future>
#include <memory>
#include <optional>
#include <string>
#include <unordered_set>
#include <utility>
#include <vector>

// Shared text/file helpers (defined below the anonymous namespace). Declaring
// them here keeps them callable from the window procedure.
std::string ToUtf8(const std::wstring& text);
void WriteUtf8(const std::filesystem::path& output_path,
               const std::string& text);

namespace {

constexpr wchar_t kWindowClass[] = L"ZoreView.MainWindow";
constexpr int kDefaultWidth = 1280;
constexpr int kDefaultHeight = 800;
constexpr UINT kDecodeDone = WM_APP + 1;
constexpr UINT kThumbnailDone = WM_APP + 2;
// Background EXIF read finished (規劃/22/06): lparam owns an ExifCompletion*.
constexpr UINT kExifDone = WM_APP + 3;
// Two-stage loading (規劃/22/13): lparam owns a DecodeCompletion* whose
// `preview` flag is set. Runs on its own queue/thread, never behind a full
// decode.
constexpr UINT kPreviewDone = WM_APP + 4;
// v2.0 S3 / 22/14: background save completion. lparam owns a SaveOutcome*.
constexpr UINT kSaveDone = WM_APP + 5;
// V3.1-06 (§06d): a copy confirmation is posted, not shown inline, so it lands in
// the message-loop turn *after* the context menu closed. Painting it in the same
// turn put the toast under the closing popup (the layer order was the second
// half of the bug; ui_root now draws toasts after the menu).
constexpr UINT kDeferredToast = WM_APP + 9;
constexpr UINT_PTR kScreenshotTimer = 1;
constexpr UINT_PTR kTestActionTimer = 2;
constexpr UINT_PTR kAnimationTimer = 3;
constexpr UINT_PTR kLayoutDumpTimer = 4;
constexpr UINT_PTR kToastTimer = 5;
constexpr UINT_PTR kClickHitTimer = 6;
// Polls the caption press for release (the system sends no button-up for a
// zero-movement caption click, see the WM_NCLBUTTONDOWN handler).
constexpr UINT_PTR kTitleClickTimer = 7;
// The "unsupported format" note under the empty-state title is transient
// (規劃/22/03 §4.4); this timer returns the card to its base state.
constexpr UINT_PTR kEmptyNoteTimer = 8;
// Queued --simulate-drop paths, one per tick (folder scan + decode must finish
// before the next drop is applied).
constexpr UINT_PTR kTestDropTimer = 9;
// --send-keys: one queued key chord per tick, posted as a real WM_KEYDOWN.
constexpr UINT_PTR kSendKeysTimer = 10;
// Loading indicator (規劃/22/11 §4.4): 400 ms delay before it appears, then an
// animation tick that only repaints while it is visible.
constexpr UINT_PTR kLoadingDelayTimer = 11;
constexpr UINT_PTR kLoadingTickTimer = 12;
// --force-confirm: opens the delete confirmation once the first image is up.
constexpr UINT_PTR kConfirmTimer = 13;
// --force-loading <ms>: hides the forced indicator after the requested time.
constexpr UINT_PTR kLoadingEndTimer = 14;
// --save-as <file>: writes the current view once the queued keys (if any) have
// been applied; used by tests/run_rotation_check.ps1 for the 90-degree export.
constexpr UINT_PTR kSaveAsTimer = 15;
// --info-scroll <notches>: applies the scroll offset once the window is open, so
// both --layout-dump and --info-screenshot capture the scrolled state.
constexpr UINT_PTR kInfoScrollTimer = 16;
// --open-settings / --settings-click / --settings-dump: drives the settings
// window after the first frame (規劃/22/07 §7).
constexpr UINT_PTR kSettingsTimer = 17;
// Auto-fading rail (規劃/22/08): fade tick, hide grace period, tooltip delay and
// the deterministic --hover-thumb hook.
constexpr UINT_PTR kStripFadeTimer = 18;
constexpr UINT_PTR kStripHideTimer = 19;
constexpr UINT_PTR kStripTooltipTimer = 20;
constexpr UINT_PTR kHoverTestTimer = 21;
// v2.0 S2 / 22/10: folder polling and the deterministic watch-test hooks.
constexpr UINT_PTR kWatchTimer = 22;
constexpr UINT_PTR kDumpAtTimer = 23;
constexpr UINT_PTR kQuitTimer = 24;
constexpr UINT_PTR kMinimizeTimer = 25;
// Idle fade of the animation pill (規劃/22/12 §3.1): only runs while an
// animated image is loaded and repaints only when the opacity actually changes.
constexpr UINT_PTR kAnimPillTimer = 26;
// --restore-at: restores a window minimised by --minimize-at so the animation
// resume path can be asserted (規劃/22/12 §6).
constexpr UINT_PTR kRestoreTimer = 27;
// Preview badge fade-out after the full frame arrives (規劃/22/13 §3.5).
constexpr UINT_PTR kPreviewBadgeTimer = 28;
// --save-options <file>: test hook that opens the real system save dialog once
// the first image is on screen (the hook keeps its 1200 ms delay, 坑 271).
constexpr UINT_PTR kSaveOptionsTimer = 29;
// v3.0 (規劃/26 §2.7): fullscreen reveal strip. Both the 160 ms fade and the 3 s
// idle deadline share this id; 31 is the first free slot (renderer owns 30,
// see 規劃/22/08 坑 81 / 22/18 坑 290).
constexpr UINT_PTR kFsRevealTimer = 31;
// --fullscreen (v3.0, 規劃/26 §2.7): enter fullscreen shortly after the first
// frame so the reveal screenshots and checks do not have to click first.
constexpr UINT_PTR kFullscreenTestTimer = 32;
// v3.0 (規劃/26 §2.1): standalone info window test drivers. 33-35 keep the next
// free slots after the fullscreen hook (the renderer owns 30).
constexpr UINT_PTR kInfoWindowTimer = 33;
constexpr UINT_PTR kInfoScreenshotTimer = 34;
constexpr UINT_PTR kInfoKeyTimer = 35;
constexpr UINT kEmptyNoteLifetimeMs = 3000;
// Toasts disappear on their own; the timer is re-armed while more are queued
// (規劃/22/11 §3.3).
constexpr UINT kToastLifetimeMs = 2500;
// The loading indicator only appears when a decode is still running after this
// delay, so a fast 4K image never flashes it (規劃/22/11 §4.4).
constexpr UINT kLoadingDelayMs = 400;
// Auto-fading rail (規劃/22/08 §3.5; V3.1-09 retimed): 220 ms fade both ways,
// 180 ms grace before hiding (was 400 - users read that pause as "slow"), 500 ms
// before the hover tooltip appears.
constexpr UINT kStripFadeMs = 220;
constexpr UINT kStripHideDelayMs = 180;
constexpr UINT kStripTooltipDelayMs = 500;
constexpr size_t kThumbMargin = 2;
// V3.1-07 S3: the strip thumbnail is also the default transition frame, and
// 160 px is the size the replica uses for that job (`規劃/v3.1/07` §1.1/§3 S3).
// The rail draws a slot at 60 DIP, so this only raises the decoded detail — no
// layout depends on it. The thumbnail disk cache is keyed by this value
// (`e160v1`, thumbnail_service.cpp), so the old 128 px tree is simply unused.
constexpr uint32_t kThumbEdge = 160;
// Prefetching full-resolution neighbours of an 8K frame costs ~127 MB each.
// Above this decoded size the adjacent images are decoded on demand instead,
// which keeps the 8K memory peak to the current frame only.
constexpr uint64_t kPrefetchMaxCurrentBytes = 64ull * 1024 * 1024;
// Disk-cache reads happen on the UI thread (127 MB for 8K) and would keep a
// second full frame alive. Frames above this size are always decoded on the
// worker instead; the decode is ~80 ms, faster than the file read anyway.
constexpr uint64_t kDiskCacheMaxFrameBytes = 64ull * 1024 * 1024;
// V3-04 display tiers (規劃/25 §7.1–§7.5). V3-04b finished the two pieces that
// made a tier unsafe as the default: the §7.6 upgrade to a full frame (>= 100 %,
// pixel mode, and every export) and the §7.7 proxy rule (never bake a 1/4 copy
// of a tier). With those in place the chain is on by default; `off` keeps the
// pre-V3-04 path byte-for-byte for A/B and rollback (規劃/25 §7.10).
enum class DisplayCacheMode { kOff = 0, kOn = 1, kForce = 2 };
DisplayCacheMode g_display_cache_mode = DisplayCacheMode::kOn;
// --cache-trace: one `cache=...` line per load (規劃/25 §7.8).
bool g_cache_trace = false;

// Right-click menu commands (v3.0, 規劃/26 §2.9). The ids are opaque to the UI
// module; the native menu's open/language ids disappeared with the native menu.
enum : UINT {
  kContextSaveAs = 2001,
  kContextCopyImage,
  kContextCopyPath,
  kContextRename,
  kContextDelete,
  kContextWallpaper,
  kContextReveal,
  kContextSettings,
};

struct WindowState {
  nv::render::Renderer renderer;
  nv::render::ThumbnailStrip strip;
  nv::ImageCache cache;
  std::unique_ptr<nv::DiskCache> disk_cache;
  std::unique_ptr<nv::DiskCacheWriter> cache_writer;
  std::unique_ptr<nv::ThumbnailService> thumbs;
  nv::FolderList folder;
  std::unique_ptr<nv::DecodeService> decode;
  std::filesystem::path path;
  std::shared_ptr<nv::Image> current;
  // Playback state machine (規劃/22/12). The app owns the frame timer and the
  // renderer upload; the controller owns index/loops/state.
  nv::AnimationController anim;
  // Two-stage loading (規劃/22/13): while a slow full decode is in flight the
  // canvas shows a preview and the title/info report the *target* size. The
  // preview itself never enters the memory/disk caches (坑 6).
  bool preview_active = false;
  bool preview_failed = false;
  std::filesystem::path preview_path;
  uint32_t preview_target_width = 0;
  uint32_t preview_target_height = 0;
  std::string preview_source;  // "cache" or "embedded"
  bool dragging = false;
  POINT last_mouse{};
  // v3.0 (規劃/26 §2.4): dragging the bird's eye frame. Kept apart from the
  // picture pan above so a press on the panel can never start an image drag and
  // the two can never both capture the mouse.
  bool minimap_dragging = false;
  POINT minimap_last_mouse{};
  // V3-04 display tier (規劃/25 §7.1–§7.5): which tier the current frame is at
  // (0 = full resolution), where it came from, and the files whose full frame
  // this session already produced. Rule §7.3-3: once a full frame existed, a
  // later load of the same file must not go back to the blurry tier.
  uint32_t display_edge = 0;
  bool current_is_tier = false;
  const char* cache_source = "none";
  std::unordered_set<std::wstring> full_frames;
  // V3-04b (§7.6): the file whose full frame a 100 % view already asked for.
  // Kept apart from g_pending_path because that one is cleared by loads,
  // cancellation and the completion handler, while this must suppress a second
  // upgrade request until the first one's completion is applied.
  std::filesystem::path upgrade_in_flight;
  // V3.1-02: a frame smaller than the requested tier, shown while that tier
  // decodes so every key press changes the screen. Display-only: it is never
  // cached, never persisted, never the source of an export or an upgrade
  // (EnsureFullForExport decodes synchronously either way).
  bool transition_active = false;
  uint32_t transition_edge = 0;
  std::string transition_source;  // "small-tier" or "thumb"
  double transition_ms = 0.0;     // request -> transition on screen
  // V3.1-04a: how the current RAW frame was produced, and the long edge of the
  // camera-embedded JPEG when that preview *is* the final frame. The preview
  // edge is 1920 or the camera's own size, i.e. not on the tier ladder.
  std::string raw_mode = "none";  // none | preview | half | full | deferred | tier
  uint32_t preview_edge = 0;
  // V3.1-08: the format of `path` and its size on disk, captured at load time.
  // The early-upgrade policy reads both on every paint while a tier is on
  // screen, so re-probing the header (Detect) or stat()-ing the file there would
  // be a per-frame syscall / header read.
  nv::ImageFormat format = nv::ImageFormat::kUnknown;
  uint64_t file_bytes = 0;
};

std::unique_ptr<WindowState> g_state;
// V3.1-02: a transition frame is display-only. Any real frame (cache hit, decode
// completion, forced export decode) ends it; leaving the flag set would let a
// later paint believe the placeholder is the requested frame.
void ClearTransitionFrame() {
  if (g_state == nullptr) return;
  g_state->transition_active = false;
  g_state->transition_edge = 0;
  g_state->transition_source.clear();
}
// v2.0 custom-drawn chrome. g_ui owns the DWrite context, the cached layout and
// the interaction stack; the window procedure only forwards messages to it.
std::unique_ptr<nv::ui::UiRoot> g_ui;
// v2.0 S2 settings window (規劃/22/07). Kept alive for the process lifetime: its
// WM_DESTROY callback must never delete the object that is running it.
std::unique_ptr<nv::ui::SettingsWindow> g_settings_window;
bool g_settings_window_open = false;
nv::ui::UiInputs g_ui_inputs;
nv::ui::WindowStateMachine g_window_state;
bool g_borderless = true;
nv::ui::StripMode g_strip_mode = nv::ui::StripMode::kAuto;
// True when --strip= overrides the saved mode for this session only.
bool g_strip_override = false;
// --- v2.0 S2: auto-fading thumbnail rail (規劃/22/08) ----------------------
// The rail fades between 0 and 1 over 160 ms; only the opacity animates, the
// content rectangle changes once when the fade starts/ends (坑 1/2).
float g_strip_opacity = 0.0f;
float g_strip_target = 0.0f;
// While set, the pointer is inside the rail/hot zone; the hide timer may fire
// after the kStripHideDelayMs grace period (規劃/22/08 §3.5).
double g_strip_hide_at_ms = 0.0;
double g_strip_fade_tick_ms = 0.0;
// V3.1-09: when the fade-out began, so `ui: strip=hidden` can report how long
// the fade actually took (the dump's opacity is a single instant and the log
// timestamps only have 1 s resolution).
double g_strip_hide_start_ms = 0.0;
// --- v3.0 (規劃/26 §2.7): fullscreen reveal strip ---------------------------
// Two buttons float in the top-right corner. They fade in over 160 ms when the
// pointer reaches that corner, and fade out 3 s after the last move inside it.
float g_fs_reveal_opacity = 0.0f;
float g_fs_reveal_target = 0.0f;
double g_fs_reveal_hide_at_ms = 0.0;
double g_fs_reveal_tick_ms = 0.0;
constexpr double kFsRevealIdleMs = 3000.0;
constexpr double kFsRevealFadeMs = 160.0;
// --- arrow reveal (mouse left the window, or five idle seconds) -------------
// The two circles slide apart by kArrowSlideDip while their opacity goes 1 -> 0
// over kArrowFadeMs. Any pointer movement, wheel, click or key brings them back.
constexpr UINT kArrowIdleMs = 5000;
constexpr double kArrowFadeMs = 200.0;
constexpr float kArrowSlideDip = 16.0f;
float g_arrow_opacity = 1.0f;
float g_arrow_target = 1.0f;
double g_arrow_fade_tick_ms = 0.0;
double g_arrow_last_activity_ms = 0.0;
// Set while the pointer is over the client area (WM_MOUSELEAVE clears it).
bool g_arrow_pointer_inside = true;
// --- v2.0 S2 / 22/12: animation frame pill --------------------------------
// The pill dims to 35% after two idle seconds and returns to full opacity on
// the next mouse move; only the opacity animates (規劃/22/12 §3.1).
float g_anim_pill_opacity = 1.0f;
double g_anim_pill_last_move_ms = 0.0;
double g_anim_pill_tick_ms = 0.0;
bool g_anim_pill_timer_running = false;
// Tracks the pill's visibility so RefreshUi only arms/disarms the fade timer on
// an actual transition (per-frame RefreshUi must not wake a dimmed pill).
bool g_anim_pill_active = false;
// --- v2.0 S3 / 22/13: two-stage loading ------------------------------------
// Preview state lives in WindowState; these are the timers/hooks around it.
float g_preview_badge_opacity = 0.0f;
double g_preview_badge_tick_ms = 0.0;
// Start of the current load, used to report the preview latency in the log.
double g_preview_request_start_ms = 0.0;
double g_preview_shown_ms = 0.0;
// A preview produced before the D3D device exists (first command-line image:
// LoadPath runs before Renderer::Initialize on purpose, M1-2). It is applied
// once the renderer can upload it instead of being thrown away.
struct PendingPreview {
  std::shared_ptr<nv::Image> image;
  std::filesystem::path path;
  std::string source;
  uint32_t target_width = 0;
  uint32_t target_height = 0;
  double shown_ms = 0.0;
};
std::optional<PendingPreview> g_pending_preview;
// --force-preview-fail: makes the full decode of a previewed file fail once, so
// "failure keeps the preview" can be asserted without a corrupt sample.
bool g_force_preview_fail = false;
// --preview-zoom <percent>: zooms the preview deterministically, which lets a
// test prove that the replacement keeps the manual view (規劃/22/13 §6).
int g_test_preview_zoom_percent = 0;
// --prefetch=off (V3-04b, 規劃/25 §7.13-3): disables the neighbour prefetch for
// this run so a navigation has to decode. Without it the second image of a
// session is almost always a cache hit, which would leave the tier *decode*
// path (and its single header pass) untested.
bool g_prefetch_enabled = true;

// --- V3.1-01 (規劃/v3.1/01-切换体验-方案.md §V3.1-01) ----------------------
// A navigation burst is two or more loads within 400 ms; it ends 400 ms after
// the last one. During a burst one tier prefetch may run beside the current
// decode (DecodeService::SetBurstMode), which is what restores "every key press
// shows something" on a 22 MB photo (200 ms interval, ~260 ms tier decode).
// Outside a burst the load chain is exactly the V3-04 one.
constexpr UINT_PTR kBurstSettleTimer = 36;
constexpr double kBurstWindowMs = 400.0;
bool g_nav_burst = false;
double g_last_nav_ms = 0.0;
int g_nav_streak = 0;
// --prefetch-parallel=off (V3.1-01 rollback): keeps the widened burst gate off,
// so the only change left is the unconditional chain re-arm (01 A).
bool g_prefetch_parallel = true;
// V3.1-01 A: 50 ms debounce for the unconditional neighbour re-arm.
std::filesystem::path g_prefetch_armed_path;
double g_prefetch_armed_ms = 0.0;

// V3.1-06 §06e: rename overlay state. The EDIT is a borderless top-level window
// (a WS_CHILD control cannot paint above the D3D swap chain), placed over the
// card's input box; `g_rename_error` is the line under it.
HWND g_rename_edit = nullptr;
HFONT g_rename_font = nullptr;
HBRUSH g_rename_brush = nullptr;
std::wstring g_rename_error;
// --rename-input: the text the overlay starts with (test hook; the real entry is
// the user typing into the EDIT).
std::wstring g_test_rename_input;
bool g_rename_has_test_input = false;

// Places the rename EDIT over the card's input box (hidden while it is closed).
// Layout rectangles are client pixels; the EDIT is a top-level window, so both
// its position and its size go through the client-to-screen mapping. The 2 DIP
// inset keeps the card's accent border visible around it.
void PositionRenameEdit(HWND hwnd) {
  if (g_rename_edit == nullptr || g_ui == nullptr) return;
  const nv::ui::RenameCardLayout& card = g_ui->rename_card();
  if (!card.visible || card.input.empty()) {
    ShowWindow(g_rename_edit, SW_HIDE);
    return;
  }
  POINT origin{0, 0};
  ClientToScreen(hwnd, &origin);
  const int inset = static_cast<int>(std::lround(2.0 * g_ui->dpi_scale()));
  const int pad =
      std::max(6, static_cast<int>(std::lround(6.0 * g_ui->dpi_scale())));
  const int x =
      origin.x + static_cast<int>(std::lround(card.input.left)) + pad;
  const int y =
      origin.y + static_cast<int>(std::lround(card.input.top)) + inset;
  const int w = std::max(
      10, static_cast<int>(std::lround(card.input.width())) - pad * 2);
  const int h = std::max(
      10, static_cast<int>(std::lround(card.input.height())) - inset * 2);
  SetWindowPos(g_rename_edit, HWND_TOP, x, y, w, h,
               SWP_NOACTIVATE | SWP_SHOWWINDOW);
  // The padding above *is* the text inset; keeping the control's own margins at
  // zero stops it from adding a second, unscaled one.
  SendMessageW(g_rename_edit, EM_SETMARGINS, EC_LEFTMARGIN | EC_RIGHTMARGIN, 0);
}

// --- V3.1-02 / V3.1-07 S3: transition frames --------------------------------
// A *smaller* frame that already exists (a smaller tier in memory, or the
// strip's thumbnail) is shown as a placeholder, then replaced by the tier when
// it lands.
//   off    never show one (rollback, 01 §V3.1-02);
//   auto   **every** load whose target tier missed the memory/disk cache shows
//          one, burst or not — this is the replica's "always blur first"
//          behaviour (規劃/v3.1/07 §3 S3). Default since S3;
//   sharp  the V3.1-02 rule: burst only, and no placeholder while the target
//          tier is already decoding ("in flight") — kept as the A/B rollback;
//   force  the V3.1-02 test hook: always try, never consult burst or in-flight
//          state. Since S3 `auto` no longer consults them either, `force` is now
//          equivalent to `auto`; it is kept so existing test runs still work.
enum class TransitionMode { kOff, kAuto, kSharp, kForce };
TransitionMode g_transition_mode = TransitionMode::kAuto;

// Both spellings are documented and accepted: `--transition sharp` and
// `--transition=sharp` (規劃/v3.1/07 §3 S3-5, 回滚). An unknown value keeps the
// V3.1-02 fallback (auto), so a typo never silently disables the feature.
TransitionMode ParseTransitionMode(const std::wstring& value) {
  if (value == L"off") return TransitionMode::kOff;
  if (value == L"sharp") return TransitionMode::kSharp;
  if (value == L"force") return TransitionMode::kForce;
  return TransitionMode::kAuto;
}

const char* TransitionModeName(TransitionMode mode) {
  switch (mode) {
    case TransitionMode::kOff:
      return "off";
    case TransitionMode::kSharp:
      return "sharp";
    case TransitionMode::kForce:
      return "force";
    case TransitionMode::kAuto:
    default:
      return "auto";
  }
}

// Evidence for the dump: how many transitions this run showed and when the
// last one appeared.
int g_transition_count = 0;
// V3.1-07 S3b: the display edge the *in-flight* load asked for (0 = the format
// has no tier, i.e. RAW). The asynchronous thumbnail completion below builds the
// same placeholder the load itself would have shown had the frame been cached.
uint32_t g_pending_edge = 0;

// --- V3.1-07 S1: thumbnail pre-read -----------------------------------------
// A transition frame can only be shown from a 160 px frame that already exists.
// The rail produces those for *visible* slots only, so a hidden rail (the
// default `--strip=auto`) leaves a freshly opened folder with nothing, and a
// 5-9/s page-through looks dead. Pre-read the neighbours once the view is at
// rest — a burst almost always starts from a resting image, so by the first key
// the next few frames are already on disk (規劃/v3.1/07 §3 S1; the replica uses
// ±6).
//
// The full version (2026-10-27) adds the three pieces the minimal one left out
// (see `PrewarmThumbnails`/`PumpPrewarmStep`): direction (±6 towards the way the
// user is paging first), one request per kThumbPrewarmStepMs, and the 24-entry
// queue cap in ThumbnailService. The throttle deliberately covers *only* this
// plan: the per-load request for the file being opened bypasses it (坑 335: one
// 160 px frame already costs 200-300 ms, so pacing the frame the user is waiting
// for would only delay it).
constexpr UINT_PTR kThumbPrewarmTimer = 37;
constexpr UINT_PTR kThumbPrewarmStepTimer = 38;
// Prev/next circle reveal: one timer drives the 200 ms fade/slide, the other
// fires five seconds after the last input.
constexpr UINT_PTR kArrowFadeTimer = 39;
constexpr UINT_PTR kArrowIdleTimer = 40;
constexpr int kThumbPrewarmRadius = 6;
constexpr UINT kThumbPrewarmDelayMs = 400;
constexpr UINT kThumbPrewarmStepMs = 40;
// `--thumb-prewarm=off` (V3.1-07 S1 rollback): no ±6 pre-read. The per-load
// request for the file being opened stays — it is what makes a *cold* first
// press show a frame at all, and it is bounded to one item (skipped in a burst).
bool g_thumb_prewarm = true;
// Paths the pre-read asked for: their completions are logged as `thumbs: prewarm`
// so the rail's own "a hidden rail requests no previews" check keeps measuring
// the rail (main.cpp SyncStripThumbnails) instead of this queue.
std::unordered_set<std::wstring> g_prewarm_paths;
// The ordered plan the pre-read walks, as folder indices (current first, then
// the paging direction 1..6, then the other side). Consumed one entry per
// kThumbPrewarmStepMs tick; folder rebuilds and bursts drop it.
std::vector<size_t> g_prewarm_plan;
size_t g_prewarm_cursor = 0;
int g_prewarm_queued = 0;
double g_prewarm_started_ms = 0.0;
// V3.1-07 S1-3: which way the user is paging (+1 forward, -1 backward, 0 not
// known yet). The pre-read fills that side first, so a burst that keeps going
// the same way finds its next six frames already on disk (the replica's
// index_worker does the same by direction, 規劃/v3.1/07 §1.1).
int g_nav_direction = 0;
// S4 reuses the same record: how many navigations in a row went the same way
// (1 = the direction just changed or was just learned). The neighbour prefetch
// depth is min(streak, g_prefetch_max_depth), i.e. "3+ in a row -> 3 deep"
// (規劃/v3.1/07 §3 S4).
int g_nav_dir_streak = 0;
// V3.1-07 S4: how deep the neighbour prefetch walks the paging direction (1..3),
// `--prefetch-depth=1` caps it for A/B. RAW neighbours are prefetched at the fit
// tier (LibRaw half_size) unless `--prefetch-raw=off` restores the pre-S4 rule.
constexpr int kPrefetchMaxDepth = 3;
int g_prefetch_max_depth = kPrefetchMaxDepth;
bool g_prefetch_raw_neighbours = true;

// The one place that interprets a navigation: keeps the S1 pre-read direction
// and the S4 depth-streak in sync. `direction == 0` means "a fresh open, no
// direction" (forward-first, the pre-S4 behaviour).
void NoteNavigationDirection(int direction) {
  if (direction == 0) {
    g_nav_direction = 0;
    g_nav_dir_streak = 0;
    return;
  }
  if (direction == g_nav_direction) {
    ++g_nav_dir_streak;
  } else {
    g_nav_direction = direction;
    g_nav_dir_streak = 1;
  }
}

// --- V3.1-04a: the RAW embedded preview as the final frame -----------------
// A 23 MB CR3 carries a 1620x1080 JPEG that decodes in ~80 ms; the LibRaw full
// decode of 5999x3999 costs ~800 ms and a 91.5 MB texture upload, and at fit
// (1060x708 content) the result is downscaled again. When the preview is big
// enough for the content rectangle it *is* the final frame; 1:1, pixel mode and
// exports still go through EnsureFullForExport's full decode.
enum class RawPreviewMode { kOff, kPreview };
RawPreviewMode g_raw_preview_mode = RawPreviewMode::kPreview;
// V3.1-07 S2: while the embedded preview is the final frame, also store its
// pixels as the fit display tier so a revisit is a cache hit (規劃/v3.1/07 §3
// S2, put-from-preview). `--preview-cache=off` disables exactly that put; it
// does not touch the preview path itself (`--raw-preview=off`).
bool g_preview_cache = true;
// Set while a RAW load is waiting for its embedded preview: no main decode is
// in flight, so OnPreviewDone decides between "preview is final" and a
// half-size request.
std::filesystem::path g_raw_deferred_path;
// Hover tooltip: 500 ms over one slot, hidden by movement/scroll/click/mode.
int g_strip_hover_index = -1;
bool g_strip_tooltip_visible = false;
// Pointer position when the current hover began; moving more than 4 DIP inside
// the same slot restarts the 500 ms countdown (規劃/22/08 §3.3).
POINT g_strip_hover_origin{};
// --hover-thumb <index>: deterministic tooltip check without composing a drag.
int g_test_hover_thumb = -1;
// --hover-hotzone: post a move into the bottom 8 DIP so the auto rail's reveal
// path (fade + requests) runs without a real pointer.
bool g_test_hover_hotzone = false;
// --hover-cycle: reveal through the hot zone, then leave; used to assert both
// the 160 ms fade in and the 400 ms grace + fade out in one run.
bool g_test_hover_cycle = false;
// v3.0 (規劃/26 §2.10): the reveal area now includes the bottom bar. Two extra
// hooks exercise it — a plain move onto the bar, and a held drag that crosses
// the bar and must NOT reveal anything.
bool g_test_hover_bottom = false;
bool g_test_drag_to_bottom = false;
// --hover-fs (v3.0, 規劃/26 §2.7): post a move into the fullscreen reveal hot
// zone so the 160 ms fade + dump can be asserted without a real pointer.
bool g_test_hover_fs = false;
// --fullscreen: start in fullscreen (see kFullscreenTestTimer).
bool g_test_fullscreen = false;
int g_test_hover_phase = 0;
int g_test_hover_attempts = 0;
// The hover hooks own the layout dump: it is scheduled only after the synthetic
// move (or the end of the reveal/hide cycle), so a slow ASan start cannot make
// the fixed 900 ms dump run before the reveal.
bool g_hover_owns_dump = false;
// --- v2.0 S2 / 22/10: folder watching and folder wrap ---------------------
nv::FolderWatcher g_watcher;
UINT g_watch_interval_ms =
    static_cast<UINT>(nv::FolderWatcher::kDefaultIntervalMs);
// The directory count is only sampled every N polls on local drives (22/10
// §3.1); the directory timestamp is checked every time.
int g_watch_poll_count = 0;
bool g_watch_pause_logged = false;
// V3-05 (規劃/25 §8): local fixed disks are watched with ReadDirectoryChangesW
// (single thread + single handle, 250 ms quiet-period merge) instead of the 2 s
// poll. g_watch_uses_notify says which source is currently driving rescans:
// false = the legacy poll timer. Remote paths, --watch-poll and a failed
// Start() all keep the poll, unchanged.
nv::platform::DirNotifier g_dir_notify;
bool g_watch_uses_notify = false;
// A change arrived while the window was minimised or a modal owned the UI; the
// deferred tick retries every kWatchDeferredRetryMs so it is not lost on resume.
bool g_watch_notify_deferred = false;
constexpr UINT kWatchDeferredRetryMs = 500;
// --watch-poll: force the fallback even on a local disk (test hook for the
// "Start() failed / remote" branch).
bool g_watch_force_poll = false;
// The folder the current source was selected for. While it matches the folder
// on screen the source is left alone, so a rescan's SyncWatchBaseline neither
// restarts the watch nor drops a queued notification.
std::filesystem::path g_watch_source_folder;
// Sibling-folder enumeration is cached for 2 s so pressing the edge repeatedly
// does not rescan a directory with hundreds of entries (22/10 §4.2).
struct SiblingCache {
  std::filesystem::path parent;
  std::vector<std::filesystem::path> folders;
  double expires_ms = 0.0;
  bool valid = false;
};
SiblingCache g_sibling_cache;
// Folder-edge prompt state (the confirm card is shared with delete).
bool g_wrap_remember = false;
bool g_wrap_forward = true;
// Empty when the next/previous sibling has no images: the prompt then offers
// to wrap inside the current folder.
std::filesystem::path g_wrap_target;
// Deterministic watch-test hooks (see tests/run_watch_check.ps1).
int g_test_dump_at_ms = 0;
int g_test_quit_at_ms = 0;
int g_test_minimize_at_ms = 0;
// --restore-at: brings the window back so tests can assert that a minimised
// animation resumes only when it was playing (規劃/22/12 §6).
int g_test_restore_at_ms = 0;
// --force-anim: parks the pill in a deterministic state for screenshots and
// dumps (like --force-error for the error card). Test-only; not product UI.
enum class ForceAnimState {
  kNone = 0,
  kFirstFrame,
  kPaused,
  kEnded,
  kTruncated,
};
ForceAnimState g_force_anim = ForceAnimState::kNone;
// --force-quality fast|good|pixel and --proxy on|off (規劃/22/18 §3.5). -1
// keeps the automatic policy / the default proxy switch.
int g_force_quality = nv::render::kQualityAuto;
int g_force_proxy = -1;
bool g_window_active = true;
std::filesystem::path g_layout_dump_path;
bool g_start_maximized = false;
bool g_start_fullscreen = false;

// Fullscreen hides the frame, so the previous style and rectangle are restored
// when it ends (規劃/22/01 §4.3: remember the rectangle *and* the maximized
// state, never trust one of them alone).
struct SavedPlacement {
  RECT rect{};
  LONG_PTR style = 0;
  bool maximized = false;
  bool valid = false;
};
SavedPlacement g_saved_placement;
nv::platform::Settings g_settings;
HBRUSH g_background_brush = nullptr;
std::filesystem::path g_screenshot_path;
std::filesystem::path g_bench_startup_path;
int g_screenshot_delay_ms = 1500;
int g_test_click_index = -1;
int g_test_scroll_notches = 0;
std::wstring g_test_rename_to;
bool g_test_delete_current = false;
bool g_test_open_rename_dialog = false;
// --click-hit <part>: synthesized clicks executed in order, so the automated
// checks can drive the chrome without composing DPI-scaled mouse events
// (規劃/22/02 §7; avoids the DPI pitfalls of 坑 139).
std::vector<std::wstring> g_test_click_hits;
// --send-keys <seq>: chords posted through the real WM_KEYDOWN path
// (規劃/22/09 §7). One chord per tick; the tick is 350 ms by default so a
// panel opened by a key is painted before the next one, and configurable via
// --send-keys-interval so a navigation burst (V3.1-01, 200 ms) can be driven
// without the screen-capture harness.
// (規劃/22/09 §7 step 9). Simulated modifiers stand in for GetKeyState, which a
// posted message cannot fake.
std::vector<std::wstring> g_test_keys;
// --send-keys-interval <ms>: the tick between queued chords (default 350). A
// V3.1 navigation burst is one key per 200 ms, which this hook could not drive
// before.
int g_test_keys_interval_ms = 350;
// True between posting a simulated key-down/up pair. While it is set the key
// handler uses only the simulated modifiers, so a physically held Ctrl/Shift
// (the user typing in another window while a test runs) cannot change what the
// queued chord means.
bool g_test_sending_key = false;

// --- v2.0 S3 / 22/19: drag-out ---------------------------------------------
// One left-button press that may become a file drag. The path is captured at
// button-down: the dragged item is the one under the cursor, never a later
// `current`.
struct DragOutPress {
  bool active = false;
  bool on_strip = false;
  bool ctrl = false;
  POINT anchor{};
  std::filesystem::path path;
  size_t strip_index = nv::render::ThumbnailStrip::kNone;
};
DragOutPress g_drag_press;
// The Ctrl+click hint is a once-per-session teaching moment; dragging out works
// without ever showing it.
bool g_drag_hint_shown = false;
// --simulate-drag-out / --simulate-strip-drag: run the same start path without
// DoDragDrop so an automated check can assert which file was chosen. Real drags
// always go through OLE.
bool g_test_drag_dry_run = false;
bool g_test_drag_current = false;
int g_test_drag_strip_index = -1;
// V3.1-06 (§06b): the dry-run drag of the picture is a *move* from the content
// centre, so the same leave-the-window rule decides it. By default the move
// lands 20 DIP beyond the right edge (the gesture that now drags out); an
// explicit "<dx>,<dy>" is a delta from the centre instead.
float g_test_drag_dx = 0.0f;
float g_test_drag_dy = 0.0f;
bool g_test_drag_use_edge = true;
bool g_test_drag_ctrl = false;
// Evidence for the dump: which source started the drag and whether the pan was
// recentred before it (V3.1-06).
std::string g_last_drag_source;
bool g_last_drag_recentered = false;
// Set only while DoDragDrop is running: our own drop target must ignore a file
// dropped back onto the window it came from (規劃/22/19 坑 12).
std::filesystem::path g_self_drag_path;
// --send-keys-at <ms>: when the queued chords start. The default 1200 ms waits
// for the first frame; a two-stage test needs the chords earlier, while only the
// preview is on screen.
int g_test_keys_start_ms = 1200;
// v3.0 (規劃/26 §2.9): --context-menu <x,y> posts WM_CONTEXTMENU at a client
// point during the 1200 ms test-action tick. The right-click menu is an
// in-window popup, so its geometry and keyboard path need a deterministic
// opener that does not depend on the physical pointer.
bool g_test_context_menu = false;
POINT g_test_context_menu_client{};
// Client-pixel anchor of the open right-click menu (0,0 when it is closed).
POINT g_context_menu_anchor{};
// --info-scroll <notches>: applied when --layout-dump writes, so the scrolled
// card geometry can be asserted deterministically.
int g_test_info_scroll = 0;
int g_test_info_scroll_attempts = 0;
// v3.0 (規劃/26 §2.1): standalone info window test drivers.
bool g_test_open_info_window = false;
bool g_test_info_geometry_valid = false;
POINT g_test_info_geometry{};
std::filesystem::path g_test_info_screenshot;
// --info-key <escape|i>: key delivered to the info window (0 = no hook).
unsigned g_test_info_key = 0;
int g_info_open_attempts = 0;
int g_info_shot_attempts = 0;
int g_info_key_attempts = 0;
struct TestModifiers {
  bool ctrl = false;
  bool shift = false;
  bool alt = false;
};
TestModifiers g_test_modifiers;
// --send-keys waits for the first frame's image to be on screen: navigation is
// disabled without a picture, so posting Right too early would only log
// "blocked". The dump (if any) is scheduled after the last chord instead of on
// a fixed delay.
bool g_keys_wait_image = false;
bool g_keys_owns_dump = false;
int g_keys_wait_attempts = 0;
int g_language_override = -1;  // -1 = settings/system, 0 = zh, 1 = en
// --rotation <0|90|180|270>: deterministic initial view rotation for the
// automated angle screenshots and the reset-rotation menu check.
int g_initial_rotation_steps = 0;
// --save-as <file>: test hook; saves the current view after the queued keys.
std::filesystem::path g_test_save_as;

// --- v2.0 S3 / 22/14: save-as enhancement ----------------------------------
// V3.1-06 (§06c) moved the target/format/quality into the system dialog
// (platform/save_dialog.*). The app only keeps the async encode worker: it runs
// on SaveService's thread so an 8K save cannot freeze the window.
std::unique_ptr<nv::platform::SaveService> g_save_service;
bool g_save_busy = false;
// --save-to <file> [--save-quality N]: synchronous save hook that also covers
// the WebP/GIF containers (規劃/22/14 §6).
std::filesystem::path g_test_save_to;
int g_test_save_quality = 0;
// --save-options <file>: test hook; opens the real system save dialog once the
// first image is on screen so scripts can drive it with NV_SAVEAS.
std::filesystem::path g_test_save_options;

// --- v2.0 S1: startup empty state and OLE drag/drop (規劃/22/03) -----------
// g_empty_base survives a transient unsupported-format note; the visible kind
// is derived from it, the open image and the decode-in-flight path.
nv::ui::EmptyKind g_empty_base = nv::ui::EmptyKind::kEmpty;
bool g_empty_note = false;
std::wstring g_empty_note_text;
// Set while a decode is queued/running so the card does not flash behind a
// loading image (規劃/22/03 坑 5).
std::filesystem::path g_pending_path;
bool g_drag_over = false;
nv::platform::DropTargetPtr g_drop_target;
const nv::FormatSummary* g_format_summary = nullptr;
// --simulate-drop <path> (repeatable) and --simulate-drag-over: test hooks that
// exercise the same OnDroppedPaths()/hover state as the real OLE target.
std::vector<std::filesystem::path> g_test_drops;
bool g_test_drag_over = false;
// --simulate-ole-drop <path>: calls the real IDropTarget methods with a
// CF_HDROP data object built by the shell, so ReadPaths() and the hover
// transitions are exercised without composing a mouse drag.
std::filesystem::path g_test_ole_drop;

// --- v2.0 S2: EXIF info panel (規劃/22/06) ---------------------------------
// Metadata is read on one background worker; the UI thread only ever applies a
// completion that still matches the open file (path + mtime), so a quick sweep
// can never show the previous picture's tags.
std::unique_ptr<nv::ExifService> g_exif;
nv::ExifData g_exif_data;
std::wstring g_exif_key;
// v3.0 (規劃/26 §2.1): the info card is its own top-level window. The object
// outlives every open (規劃/22/07 坑 218); `g_info_closing_with_app` suppresses
// the "user closed it" write while the whole application is shutting down.
std::unique_ptr<nv::ui::InfoWindow> g_info_window;
bool g_info_closing_with_app = false;
// Actual ShowWindow state of the info window, so the lifecycle sync only acts
// (and logs) when it really changes.
bool g_info_shown = false;
// A previous session ended with the info window open: reopen it once an image is
// up (a window without a file would only show an empty card).
bool g_info_restore_pending = false;

// --- v2.0 S1: errors, confirmation and loading (規劃/22/11) ----------------
// One error card at a time; the overlay stack owns its open/closed state so
// Escape has a single implementation. g_error supplies the content.
nv::UiError g_error;
enum class ConfirmKind { kNone, kDelete, kWrapNext, kWrapPrev };
ConfirmKind g_confirm = ConfirmKind::kNone;
// 0 = idle, 1 = waiting for the 400 ms delay, 2 = visible.
int g_loading_state = 0;
double g_load_start_ms = 0.0;
float g_loading_phase = 0.0f;
// Test hooks: a forced error/toast/loading state, used by
// tests/run_error_ui_check.ps1 so every presentation is deterministic.
bool g_force_error = false;
nv::ErrorKind g_force_error_kind = nv::ErrorKind::kCorrupt;
// A forced error is presented for the whole test run: the first decode may
// finish after it and must not dismiss it.
bool g_force_error_sticky = false;
std::vector<std::wstring> g_force_toasts;
bool g_test_force_confirm = false;
bool g_force_loading = false;
int g_force_loading_ms = 0;
// --force-loading with a duration below the 400 ms delay models a fast decode:
// the indicator must never appear for the whole run.
bool g_suppress_loading = false;
std::filesystem::path g_forced_error_path;
// Fatal startup failures cannot use the D2D error card (the device is what
// failed); they fall back to a GDI text frame instead of a MessageBox so the
// product has no modal system dialogs (規劃/22/11 §5).
std::wstring g_fatal_error_text;

// --- v2.0 S2: settings window (規劃/22/07) ---------------------------------
// Test hooks: open the window, run a sequence of control activations, then
// dump/capture. The actions run on a timer so the window is painted first.
bool g_test_open_settings = false;
nv::ui::SettingsPage g_test_settings_page = nv::ui::SettingsPage::kGeneral;
std::vector<std::wstring> g_test_settings_actions;
std::filesystem::path g_test_settings_dump;
std::filesystem::path g_test_settings_screenshot;
int g_test_settings_action_index = 0;
// Single instance (規劃/22/15): the mutex handle and the message-only receiver
// that accepts a forwarded path while the main window is still being created.
std::unique_ptr<nv::platform::SingleInstanceLock> g_instance_lock;
std::unique_ptr<nv::platform::InstanceReceiver> g_instance_receiver;
HWND g_main_window = nullptr;
// --move-window x,y,w,h: test hook that resizes the window and then sends the
// WM_EXITSIZEMOVE the system would send when the user let go of the frame.
bool g_test_move_window_valid = false;
RECT g_test_move_window_rect{};

// Forward declarations for the UI helpers defined below the state-building
// code; keeping them here avoids a second declaration per helper.
void RefreshUi(HWND hwnd, bool invalidate);
void UpdateAnimPillTimer(HWND hwnd);
bool AnimPillVisible();
bool AnimHasControls();
void RequestVisibleThumbnails();
void InvalidateStrip(HWND hwnd);
nv::ui::Rect StripRect();
bool SaveSettingsNow();
void ShowToast(HWND hwnd, const std::wstring& text, bool danger = false);
void ShowError(HWND hwnd, const nv::UiError& error);
void CloseError(HWND hwnd);
void ShowErrorToast(HWND hwnd, const std::wstring& text,
                    const std::wstring& detail = {});
void DeleteCurrent(HWND hwnd);
// v3.0 (規劃/26 §2.9): commands of the self-drawn right-click menu. They are
// defined further down with the save/file helpers, so ExecuteMenuCommand needs
// the declarations here.
void SaveAsDialog(HWND hwnd,
                  const std::filesystem::path& suggested_path = {});
void CopyCurrentImage(HWND hwnd);
void CopyCurrentPath(HWND hwnd);
// V3.1-06 (§06d): shows a toast in the next message-loop turn (after a context
// menu closed), not in the same paint.
void ShowToastAfterMenu(HWND hwnd, nv::i18n::StringId id);
void RenameCurrent(HWND hwnd);
// V3.1-06 (§06e): in-window rename overlay (replaces the Win32 PromptForText).
void OpenRenameOverlay(HWND hwnd);
void CancelRenameOverlay(HWND hwnd);
void CommitRenameOverlay(HWND hwnd);
bool RenameOverlayOpen();
// V3.1-04a: decides what a RAW load does with its camera-embedded preview
// (promote it to the final frame, or ask for a half-size decode).
void HandleRawPreviewDecision(HWND hwnd, const std::filesystem::path& path,
                              const std::shared_ptr<nv::Image>& image);
void SetCurrentWallpaper(HWND hwnd);
void RevealCurrent(HWND hwnd);
void OpenSettingsWindow(HWND hwnd);
void CloseConfirm(HWND hwnd, bool run_primary);
// Folder-edge prompt for the "ask" wrap policy (規劃/22/07 §3.1).
void ShowWrapConfirm(HWND hwnd, bool forward);
void ShowWrapPrompt(HWND hwnd, bool forward,
                    const std::filesystem::path& target);
void EnterAdjacentFolder(HWND hwnd, const std::filesystem::path& folder,
                         bool last);
void EnterFolderWithLast(HWND hwnd, const std::filesystem::path& folder,
                         bool open_last);
// Returns false only when the poll was skipped because the UI is paused
// (minimised / hidden / modal), so the notify path can retry the change later.
bool PollFolderWatch(HWND hwnd);
void SyncWatchBaseline();
// V3-05 (規劃/25 §8): picks ReadDirectoryChangesW or the legacy 2 s poll for
// the folder on screen, and keeps the notifier in sync with navigation.
void UpdateWatchSource();
void WriteLayoutDump(HWND hwnd, bool destroy);
// V3.1-07 S1: drops the thumbnail pre-read plan (folder rebuild / burst / done).
void ClearPrewarmPlan(HWND hwnd);
// --- v2.0 S3 / 22/13: two-stage loading -----------------------------------
bool PreviewActive();
uint32_t TargetImageWidth();
uint32_t TargetImageHeight();
void ResetPreviewState(HWND hwnd);
// Returns 0 = no preview, 1 = queued until the renderer exists, 2 = shown.
int TryShowPreview(HWND hwnd, const std::filesystem::path& path,
                   std::shared_ptr<nv::Image> image, const char* source,
                   uint32_t target_width, uint32_t target_height,
                   double shown_ms);
void ApplyPendingPreview(HWND hwnd);
void StartPreviewBadgeFade(HWND hwnd);
void OnPreviewDone(HWND hwnd, LPARAM lparam);
bool BlockExportWhilePreview(HWND hwnd);
// V3-04b (規劃/25 §7.6): the export pixels must come from the file, never from
// a display tier. Replaces the tier with a synchronous full decode before
// Save/Save-as/Copy/Wallpaper. `mode == kScreen` only forces the decode when
// the tier cannot supply the current view's pixels (>= 100 % / pixel mode),
// because a fit-scale capture is exactly what the user sees. Returns false
// when the export must not proceed (the caller shows the reason).
enum class ExportPixels { kFile, kScreen };
bool EnsureFullForExport(HWND hwnd, ExportPixels mode);
// V3-04b (規劃/25 §7.6): while a tier is on screen, a view that wants real
// pixels (>= 100 % of the file, or pixel mode) asks for the full frame once and
// keeps showing the tier until it arrives.
void MaybeUpgradeToFullFrame(HWND hwnd);
// Client pixels per file pixel (tier corrected); see the definition.
float ViewFileScale();
// The display tier a load made right now would ask for, from the content
// rectangle alone (V3.1-07 S2 uses it for the put-from-preview entry and the
// RAW revisit lookup).
uint32_t FitDisplayTierEdge();
// The view scale a §7.6 upgrade should end at; 0 = none in flight.
extern float g_upgrade_target_scale;
// Writes one decoded frame into the disk cache, honouring the 64 MB rule.
void SaveDecodedToDiskCache(const std::filesystem::path& path,
                            const std::shared_ptr<nv::Image>& image,
                            uint32_t edge);
void ActivateErrorAction(HWND hwnd, int index);
void ActivateErrorDefault(HWND hwnd);
void BeginLoading(HWND hwnd, bool immediate);
void CancelLoading(HWND hwnd);
void ShowLoadingNow(HWND hwnd);
void ExecuteMenuCommand(HWND hwnd, int command);
bool HandleTitleSegmentClick(HWND hwnd, float x, float y);
void ActivateEmptyAction(HWND hwnd, int action);
nv::ui::CommandContext BuildCommandContext();
void DispatchCommand(HWND hwnd, nv::ui::CommandId id);
bool ParseKeyToken(const std::wstring& token, UINT* virtual_key,
                   TestModifiers* modifiers);
// EXIF info window (規劃/22/06, v3.0 規劃/26 §2.1).
void RequestExifForPath(const std::filesystem::path& path);
bool InfoWindowVisible();
bool OpenInfoWindow(HWND hwnd);
void CloseInfoWindow(HWND hwnd);
// Closes the card as part of shutting the application down: the user's "it was
// open" intent is kept so the next launch restores it (規劃/26 §2.1).
void CloseInfoWindowForAppClose();
void ToggleInfoPanel(HWND hwnd);
void SyncInfoWindowVisibility(HWND hwnd, bool owner_minimized = false);
void UpdateInfoWindowContent(HWND hwnd);
void OnExifDone(HWND hwnd, LPARAM lparam);
void CopyInfoAll(HWND hwnd);
void CopyInfoLine(HWND hwnd, int index);
std::wstring InfoPanelText();

// --- Startup timing --------------------------------------------------------
// Phase timings are process uptime, so they include image loading and are what
// the user actually waits for. --bench-startup writes them to a file so
// tests/run_perf.ps1 can read first-frame/first-image numbers directly instead
// of inferring them from WaitForInputIdle (which also sees decode messages).

double ProcessUptimeMs() {
  FILETIME creation{};
  FILETIME exit{};
  FILETIME kernel{};
  FILETIME user{};
  if (!GetProcessTimes(GetCurrentProcess(), &creation, &exit, &kernel, &user)) {
    return 0.0;
  }
  ULARGE_INTEGER created;
  created.LowPart = creation.dwLowDateTime;
  created.HighPart = creation.dwHighDateTime;

  FILETIME now_filetime{};
  GetSystemTimePreciseAsFileTime(&now_filetime);
  ULARGE_INTEGER now;
  now.LowPart = now_filetime.dwLowDateTime;
  now.HighPart = now_filetime.dwHighDateTime;

  return static_cast<double>(now.QuadPart - created.QuadPart) / 10000.0;
}

double NowMs() {
  static const LARGE_INTEGER frequency = [] {
    LARGE_INTEGER value{};
    QueryPerformanceFrequency(&value);
    return value;
  }();
  LARGE_INTEGER now{};
  QueryPerformanceCounter(&now);
  return static_cast<double>(now.QuadPart) * 1000.0 /
         static_cast<double>(frequency.QuadPart);
}

// M1-3 diagnostics: working set / private bytes / peak, so the 8K test and
// big-folder test can assert memory from the log alone.
void LogMemoryUsage(const char* tag) {
  PROCESS_MEMORY_COUNTERS_EX counters{};
  if (!GetProcessMemoryInfo(GetCurrentProcess(),
                            reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&counters),
                            sizeof(counters))) {
    return;
  }
  const auto mb = [](SIZE_T bytes) {
    return static_cast<double>(bytes) / (1024.0 * 1024.0);
  };
  char line[192]{};
  std::snprintf(line, sizeof(line),
                "memory: %s ws_mb=%.1f private_mb=%.1f peak_ws_mb=%.1f "
                "peak_private_mb=%.1f",
                tag, mb(counters.WorkingSetSize), mb(counters.PrivateUsage),
                mb(counters.PeakWorkingSetSize), mb(counters.PeakPagefileUsage));
  nv::platform::LogInfo(line);
}

std::vector<std::pair<std::string, double>> g_startup_phases;
bool g_gdi_paint_reported = false;
bool g_first_image_reported = false;

void ReportStartupPhase(const char* name) {
  const double ms = ProcessUptimeMs();
  g_startup_phases.emplace_back(name, ms);
  char line[128]{};
  std::snprintf(line, sizeof(line), "startup: %s=%.1fms", name, ms);
  nv::platform::LogInfo(line);

  if (g_bench_startup_path.empty()) return;
  std::ofstream out(g_bench_startup_path, std::ios::trunc);
  if (!out) return;
  for (const auto& [phase, value] : g_startup_phases) {
    out << phase << ',' << value << '\n';
  }
}

std::string Narrow(const std::wstring& text) {
  if (text.empty()) return {};
  const int size = WideCharToMultiByte(CP_UTF8, 0, text.data(),
                                       static_cast<int>(text.size()), nullptr, 0,
                                       nullptr, nullptr);
  if (size <= 0) return {};
  std::string out(static_cast<size_t>(size), '\0');
  WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()),
                      out.data(), size, nullptr, nullptr);
  return out;
}

std::wstring Widen(const std::string& text) {
  if (text.empty()) return {};
  const int size = MultiByteToWideChar(CP_UTF8, 0, text.data(),
                                       static_cast<int>(text.size()), nullptr, 0);
  if (size <= 0) return {};
  std::wstring out(static_cast<size_t>(size), L'\0');
  MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()),
                      out.data(), size);
  return out;
}

void EnablePerMonitorDpi() {
  SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
}

// OPENFILENAMEW filters are NUL-separated with a double NUL terminator, so
// they are rebuilt whenever the UI language can change.
std::wstring BuildOpenFilter() {
  std::wstring filter = nv::i18n::Text(nv::i18n::StringId::kFilterImages);
  filter.push_back(L'\0');
  filter +=
      L"*.jpg;*.jpeg;*.jpe;*.jfif;*.png;*.apng;*.bmp;*.dib;*.gif;*.webp;"
      L"*.tif;*.tiff;*.ico;*.cur;*.psd;*.psb;*.heic;*.heif;*.hif;*.avif;"
      L"*.avifs;*.svg;*.tga;*.targa;*.pcx;*.pbm;*.pgm;*.ppm;*.pnm;*.xbm;"
      L"*.xpm;*.cr2;*.cr3;*.nef;*.arw;*.raf;*.orf;*.rw2;*.pef;*.srw;*.dng";
  filter.push_back(L'\0');
  filter += nv::i18n::Text(nv::i18n::StringId::kFilterAllFiles);
  filter.push_back(L'\0');
  filter += L"*.*";
  filter.push_back(L'\0');
  filter.push_back(L'\0');
  return filter;
}

BOOL CALLBACK CollectWorkArea(HMONITOR monitor, HDC /*dc*/, LPRECT /*rect*/,
                              LPARAM context) {
  auto* areas = reinterpret_cast<std::vector<nv::platform::IntRect>*>(context);
  MONITORINFO info{};
  info.cbSize = sizeof(info);
  if (GetMonitorInfoW(monitor, &info)) {
    areas->push_back({info.rcWork.left, info.rcWork.top, info.rcWork.right,
                      info.rcWork.bottom});
  }
  return TRUE;
}

// Work areas of every monitor, in physical pixels. Negative coordinates are
// normal for a monitor left of / above the primary (規劃/22/15 坑 15).
std::vector<nv::platform::IntRect> MonitorWorkAreas() {
  std::vector<nv::platform::IntRect> areas;
  EnumDisplayMonitors(nullptr, nullptr, &CollectWorkArea,
                      reinterpret_cast<LPARAM>(&areas));
  if (areas.empty()) {
    RECT work{};
    if (SystemParametersInfoW(SPI_GETWORKAREA, 0, &work, 0)) {
      areas.push_back({work.left, work.top, work.right, work.bottom});
    }
  }
  return areas;
}

void CenterWindow(HWND hwnd, int width, int height) {
  RECT work{};
  if (!SystemParametersInfoW(SPI_GETWORKAREA, 0, &work, 0)) {
    const int screen_w = GetSystemMetrics(SM_CXSCREEN);
    const int screen_h = GetSystemMetrics(SM_CYSCREEN);
    work = {0, 0, screen_w, screen_h};
  }
  const nv::platform::IntRect centered = nv::platform::CenterInWorkArea(
      {work.left, work.top, work.right, work.bottom}, width, height);
  SetWindowPos(hwnd, nullptr, centered.left, centered.top,
               centered.right - centered.left, centered.bottom - centered.top,
               SWP_NOZORDER | SWP_NOACTIVATE);
}

void UpdateTitle(HWND hwnd) {
  if (!g_state) return;
  wchar_t title[512]{};
  const auto& r = g_state->renderer;
  if (g_state->path.empty() || r.image_width() == 0) {
    swprintf_s(title, L"%s  %s", nv::i18n::Text(nv::i18n::StringId::kAppTitle), nv::kVersion);
  } else {
    const std::wstring filename = g_state->path.filename().wstring();
    // Two-stage loading: the title reports the file's real size, never the
    // preview's (規劃/22/13 坑 5).
    const uint32_t width = TargetImageWidth();
    const uint32_t height = TargetImageHeight();
    if (!g_state->folder.files.empty()) {
      swprintf_s(title, L"%s \u2014 %s  (%ux%u, %.0f%%) [%zu/%zu]", nv::i18n::Text(nv::i18n::StringId::kAppTitle),
                 filename.c_str(), width, height,
                 r.zoom() * 100.0f, g_state->folder.index + 1,
                 g_state->folder.size());
    } else {
      swprintf_s(title, L"%s \u2014 %s  (%ux%u, %.0f%%)", nv::i18n::Text(nv::i18n::StringId::kAppTitle),
                 filename.c_str(), width, height, r.zoom() * 100.0f);
    }
  }
  if (g_state->current && g_state->current->frames.size() > 1) {
    wchar_t suffix[96]{};
    const std::string& source = g_state->current->info.source;
    const wchar_t* label = source == "apng" ? L"APNG"
                           : source == "webp" ? L"WebP"
                                              : L"GIF";
    const nv::i18n::StringId state_id =
        g_state->anim.playing() ? nv::i18n::StringId::kAnimationPlaying
        : g_state->anim.ended() ? nv::i18n::StringId::kAnimEnded
                                : nv::i18n::StringId::kAnimationPaused;
    swprintf_s(suffix,
               nv::i18n::Text(nv::i18n::StringId::kAnimationSuffixFormat), label,
               g_state->current->frames.size(),
               nv::i18n::Text(state_id));
    wcsncat_s(title, 512, suffix, _TRUNCATE);
  }
  SetWindowTextW(hwnd, title);
  // Native menus/dialogs read the window name; tell UI Automation that it
  // changed so screen readers announce loading and navigation results.
  NotifyWinEvent(EVENT_OBJECT_NAMECHANGE, hwnd, OBJID_WINDOW, CHILDID_SELF);
}

const char* HitIdName(nv::ui::HitId id) {
  switch (id) {
    case nv::ui::HitId::kNone:
      return "none";
    case nv::ui::HitId::kTitleMenu:
      return "title.menu";
    case nv::ui::HitId::kTitleFullscreen:
      return "title.fullscreen";
    case nv::ui::HitId::kTitleMinimize:
      return "title.minimize";
    case nv::ui::HitId::kTitleMaximize:
      return "title.maximize";
    case nv::ui::HitId::kTitleClose:
      return "title.close";
    case nv::ui::HitId::kBottomInfo:
      return "bottom.info";
    case nv::ui::HitId::kBottomPercent:
      return "bottom.percent";
    case nv::ui::HitId::kBottomIndex:
      return "bottom.index";
    case nv::ui::HitId::kBottomFit:
      return "bottom.fit";
    case nv::ui::HitId::kBottomActual:
      return "bottom.actual";
    case nv::ui::HitId::kBottomRotateLeft:
      return "bottom.rotate_left";
    case nv::ui::HitId::kBottomRotateRight:
      return "bottom.rotate_right";
    case nv::ui::HitId::kBottomMore:
      return "bottom.more";
    case nv::ui::HitId::kStrip:
      return "strip";
    case nv::ui::HitId::kImage:
      return "image";
    case nv::ui::HitId::kMenuPanel:
      return "menu.panel";
    case nv::ui::HitId::kEmptyPrimary:
      return "empty.primary";
    case nv::ui::HitId::kEmptyOpenFolder:
      return "empty.open_folder";
    case nv::ui::HitId::kEmptyAllFormats:
      return "empty.all_formats";
    case nv::ui::HitId::kFormatsPanel:
      return "formats.panel";
    case nv::ui::HitId::kFormatsClose:
      return "formats.close";
    case nv::ui::HitId::kArrowLeft:
      return "arrows.left";
    case nv::ui::HitId::kArrowRight:
      return "arrows.right";
    case nv::ui::HitId::kAnimPrev:
      return "anim.prev";
    case nv::ui::HitId::kAnimNext:
      return "anim.next";
    case nv::ui::HitId::kAnimToggle:
      return "anim.toggle";
    case nv::ui::HitId::kErrorAction0:
      return "error.0";
    case nv::ui::HitId::kErrorAction1:
      return "error.1";
    case nv::ui::HitId::kErrorAction2:
      return "error.2";
    case nv::ui::HitId::kErrorClose:
      return "error.close";
    case nv::ui::HitId::kConfirmPrimary:
      return "confirm.primary";
    case nv::ui::HitId::kConfirmSecondary:
      return "confirm.secondary";
    case nv::ui::HitId::kConfirmRemember:
      return "confirm.remember";
    case nv::ui::HitId::kConfirmPanel:
      return "confirm.panel";
    // V3.1-06 (§06e): the rename overlay's controls.
    case nv::ui::HitId::kRenameOk:
      return "rename.ok";
    case nv::ui::HitId::kRenameCancel:
      return "rename.cancel";
    case nv::ui::HitId::kRenamePanel:
      return "rename.panel";
    case nv::ui::HitId::kMinimap:
      return "minimap";
  }
  return "none";
}

std::wstring FormatFileSize(uint64_t bytes) {
  if (bytes == 0) return {};
  wchar_t buffer[64]{};
  constexpr double kKb = 1024.0;
  constexpr double kMb = kKb * 1024.0;
  constexpr double kGb = kMb * 1024.0;
  const double value = static_cast<double>(bytes);
  if (value >= kGb) {
    swprintf_s(buffer, L"%.1f GB", value / kGb);
  } else if (value >= kMb) {
    swprintf_s(buffer, L"%.1f MB", value / kMb);
  } else if (value >= kKb) {
    swprintf_s(buffer, L"%.0f KB", value / kKb);
  } else {
    swprintf_s(buffer, L"%llu B", static_cast<unsigned long long>(bytes));
  }
  return buffer;
}

uint64_t CurrentFileSize() {
  if (!g_state || g_state->path.empty()) return 0;
  std::error_code error;
  const auto size = std::filesystem::file_size(g_state->path, error);
  return error ? 0 : static_cast<uint64_t>(size);
}

// --- Popup menu commands (規劃/22/02 §3.3) ---------------------------------
// One id space for the zoom menu and the more panel. The ids are opaque to the
// UI module; only the app knows what they mean.
enum MenuCommand : int {
  kCmdNone = 0,
  kCmdZoomFit,
  kCmdZoom50,
  kCmdZoom100,
  kCmdZoom200,
  kCmdZoom400,
  kCmdToggleBirdseye,
  kCmdToggleThumbnails,
};

bool NearZoom(float zoom, float expected) {
  return std::fabs(zoom - expected) < 0.005f;
}

nv::ui::MenuItem MenuRow(nv::i18n::StringId id, nv::ui::MenuItemKind kind,
                         int command, bool checked, bool enabled = true) {
  nv::ui::MenuItem item;
  item.label = nv::i18n::Text(id);
  item.kind = kind;
  item.command = command;
  item.checked = checked;
  item.enabled = enabled;
  return item;
}

// v3.0 (規劃/26 §2.9): a group separator of the right-click menu. It owns a
// compact row (10 DIP) and is never selectable.
nv::ui::MenuItem MenuSeparator() {
  nv::ui::MenuItem item;
  item.kind = nv::ui::MenuItemKind::kSeparator;
  item.enabled = false;
  return item;
}

// Builds the rows of the open popup. The app is the single source of truth for
// command availability, labels and check marks; the UI only draws them.
std::vector<nv::ui::MenuItem> BuildMenuItems() {
  using nv::ui::MenuItemKind;
  std::vector<nv::ui::MenuItem> items;
  if (g_ui == nullptr || g_state == nullptr) return items;

  const nv::ui::OverlayKind popup = g_ui->overlay().popup();
  if (popup == nv::ui::OverlayKind::kContext) {
    // v3.0 (規劃/26 §2.9): 8 commands + 3 separators. "Open file" and the
    // language submenu are gone (the empty state, drag-in and Ctrl+O still open
    // images; language lives in Settings). The native menu showed shortcut
    // hints; the custom menu does not, matching the zoom/more menus.
    const bool has_image = g_state->renderer.image_width() != 0;
    items.push_back(MenuRow(nv::i18n::StringId::kMenuSaveAs,
                            MenuItemKind::kCommand, kContextSaveAs, false,
                            has_image));
    items.push_back(MenuRow(nv::i18n::StringId::kMenuCopyImage,
                            MenuItemKind::kCommand, kContextCopyImage, false,
                            has_image));
    items.push_back(MenuRow(nv::i18n::StringId::kMenuCopyPath,
                            MenuItemKind::kCommand, kContextCopyPath, false,
                            has_image));
    items.push_back(MenuSeparator());
    items.push_back(MenuRow(nv::i18n::StringId::kMenuRename,
                            MenuItemKind::kCommand, kContextRename, false,
                            has_image));
    items.push_back(MenuRow(nv::i18n::StringId::kMenuDelete,
                            MenuItemKind::kCommand, kContextDelete, false,
                            has_image));
    items.push_back(MenuSeparator());
    items.push_back(MenuRow(nv::i18n::StringId::kMenuWallpaper,
                            MenuItemKind::kCommand, kContextWallpaper, false,
                            has_image));
    items.push_back(MenuRow(nv::i18n::StringId::kMenuReveal,
                            MenuItemKind::kCommand, kContextReveal, false,
                            has_image));
    items.push_back(MenuSeparator());
    items.push_back(MenuRow(nv::i18n::StringId::kSettingsMenu,
                            MenuItemKind::kCommand, kContextSettings, false,
                            true));
    return items;
  }

  if (popup == nv::ui::OverlayKind::kPercent) {
    const auto& renderer = g_state->renderer;
    const bool have_image = renderer.image_width() != 0;
    // §7.6: the radio must mark the zoom the *file* is shown at; a tier on
    // screen makes the renderer's own zoom look like 100 %.
    const float zoom = ViewFileScale();
    items.push_back(MenuRow(nv::i18n::StringId::kUiFitToWindow,
                            MenuItemKind::kRadio, kCmdZoomFit, renderer.fit(),
                            have_image));
    const struct {
      int command;
      int percent;
    } kZoomSteps[] = {{kCmdZoom50, 50},
                      {kCmdZoom100, 100},
                      {kCmdZoom200, 200},
                      {kCmdZoom400, 400}};
    for (const auto& step : kZoomSteps) {
      wchar_t label[16]{};
      swprintf_s(label, L"%d%%", step.percent);
      nv::ui::MenuItem item;
      item.label = label;
      item.kind = MenuItemKind::kRadio;
      item.command = step.command;
      item.checked = !renderer.fit() &&
                     NearZoom(zoom, static_cast<float>(step.percent) / 100.0f);
      item.enabled = have_image;
      items.push_back(std::move(item));
    }
    return items;
  }

  if (popup == nv::ui::OverlayKind::kMore) {
    items.push_back(MenuRow(nv::i18n::StringId::kUiBirdseye,
                            MenuItemKind::kToggle, kCmdToggleBirdseye,
                            g_settings.birdseye));
    // V3.1-09: "智能缩略图" is a plain on/off switch - checked means the rail
    // exists (auto fade-in on hover). `pinned` still reads as "on".
    items.push_back(MenuRow(nv::i18n::StringId::kUiThumbnails,
                            MenuItemKind::kToggle, kCmdToggleThumbnails,
                            g_strip_mode != nv::ui::StripMode::kOff));

    // v3.0 (規劃/26 §2.5): the sort group left the panel as well — the folder
    // has exactly one order now (Explorer's natural name order, see
    // core/folder_navigator.cpp). That is also why there is no separator left:
    // two switches have nothing to separate.
  }
  return items;
}

// --- EXIF info panel (規劃/22/06) ------------------------------------------
// The canonic text of the panel: four groups, fixed row order, one em dash for
// a value the camera (or the file) did not write. Keeping the text in one place
// means the drawn panel, "copy all" and the layout dump can never disagree.

std::wstring InfoValueOrDash(const std::wstring& value) {
  return value.empty() ? nv::i18n::Text(nv::i18n::StringId::kInfoNone) : value;
}

std::wstring AbsoluteDisplayPath() {
  if (g_state == nullptr) return std::wstring();
  std::error_code ec;
  const std::filesystem::path absolute =
      std::filesystem::absolute(g_state->path, ec);
  if (ec) return g_state->path.wstring();
  return absolute.lexically_normal().wstring();
}

// Path rows must not blow up the card width: keep the drive and the file name,
// elide the middle (規劃/22/06 坑 18). The clipboard always gets the full path.
std::wstring MiddleElide(const std::wstring& text, size_t max_chars) {
  if (text.size() <= max_chars || max_chars < 8) return text;
  const size_t head = max_chars / 2;
  const size_t tail = max_chars - head - 3;
  return text.substr(0, head) + L"\u2026" + text.substr(text.size() - tail);
}

// Fixed "YYYY-MM-DD HH:MM:SS" in local time; independent of the system locale
// so screenshots and assertions do not change with the machine.
std::wstring FormatFileTimestamp(const std::filesystem::path& path) {
  WIN32_FILE_ATTRIBUTE_DATA data{};
  if (!GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &data)) {
    return std::wstring();
  }
  FILETIME local{};
  if (!FileTimeToLocalFileTime(&data.ftLastWriteTime, &local)) {
    return std::wstring();
  }
  SYSTEMTIME parts{};
  if (!FileTimeToSystemTime(&local, &parts)) return std::wstring();
  wchar_t buffer[64]{};
  swprintf_s(buffer, L"%04u-%02u-%02u %02u:%02u:%02u", parts.wYear, parts.wMonth,
             parts.wDay, parts.wHour, parts.wMinute, parts.wSecond);
  return buffer;
}

std::wstring FormatUppercaseExtension(const std::filesystem::path& path) {
  std::wstring format = path.extension().wstring();
  if (format.size() > 1 && format[0] == L'.') format.erase(0, 1);
  std::ranges::transform(format, format.begin(), [](wchar_t c) {
    return static_cast<wchar_t>(std::towupper(c));
  });
  return format;
}

std::wstring CurrentImageDimensionText() {
  if (g_state == nullptr || g_state->renderer.image_width() == 0) {
    return std::wstring();
  }
  const auto& renderer = g_state->renderer;
  wchar_t buffer[64]{};
  // Preview or full frame: the row always shows the file's size (規劃/22/13
  // 坑 5).
  swprintf_s(buffer, L"%u \u00D7 %u", TargetImageWidth(),
             TargetImageHeight());
  std::wstring text = buffer;
  // The view rotation is display-only, so the row keeps the source size and
  // appends the current angle (規劃/22/05 §3.6).
  if (renderer.rotation() != 0) {
    wchar_t with_view[128]{};
    swprintf_s(with_view,
               nv::i18n::Text(nv::i18n::StringId::kInfoViewRotationFormat),
               text.c_str(), nv::render::RotationDegrees(renderer.rotation()));
    text = with_view;
  }
  return text;
}

std::wstring ExifValue(nv::ExifId id) {
  return g_exif_data.Get(id);
}

// "Canon" + "Canon EOS R5" must not become "Canon Canon EOS R5".
std::wstring CameraModelText() {
  const std::wstring make = ExifValue(nv::ExifId::kMake);
  const std::wstring model = ExifValue(nv::ExifId::kModel);
  if (make.empty()) return model;
  if (model.empty()) return make;
  if (model.size() >= make.size() &&
      _wcsnicmp(model.c_str(), make.c_str(), make.size()) == 0) {
    return model;
  }
  return make + L" " + model;
}

std::wstring GpsText() {
  const std::wstring latitude = ExifValue(nv::ExifId::kGpsLatitude);
  const std::wstring longitude = ExifValue(nv::ExifId::kGpsLongitude);
  const std::wstring altitude = ExifValue(nv::ExifId::kGpsAltitude);
  if (!latitude.empty() && !longitude.empty()) {
    std::wstring text = latitude + L", " + longitude;
    if (!altitude.empty()) text += L" \u00B7 " + altitude;
    return text;
  }
  return latitude + longitude + altitude;
}

std::wstring ColorProfileText() {
  if (g_state == nullptr || g_state->current == nullptr) return std::wstring();
  const nv::ImageInfo& info = g_state->current->info;
  if (info.icc_profile.empty()) return std::wstring();
  std::wstring description =
      Widen(nv::IccProfileDescription(info.icc_profile.data(),
                                      info.icc_profile.size()));
  if (description.empty()) {
    description = nv::i18n::Text(nv::i18n::StringId::kInfoNone);
  }
  if (!info.icc_applied) return description;
  wchar_t buffer[256]{};
  swprintf_s(buffer, nv::i18n::Text(nv::i18n::StringId::kInfoIccConvertedFormat),
             description.c_str());
  return buffer;
}

std::wstring DepthText() {
  if (g_state == nullptr || g_state->current == nullptr) return std::wstring();
  const nv::ImageInfo& info = g_state->current->info;
  wchar_t depth[64]{};
  swprintf_s(depth, nv::i18n::Text(nv::i18n::StringId::kInfoDepthFormat), 8u);
  std::wstring text = depth;
  text += L" / ";
  text += nv::i18n::Text(info.has_alpha
                             ? nv::i18n::StringId::kInfoAlphaYes
                             : nv::i18n::StringId::kInfoAlphaNone);
  return text;
}

nv::ui::InfoPanelStrings BuildInfoStrings() {
  nv::ui::InfoPanelStrings info;
  if (g_state == nullptr) return info;
  info.title = nv::i18n::Text(nv::i18n::StringId::kUiInfoTitle);
  info.copy_all = nv::i18n::Text(nv::i18n::StringId::kInfoCopyAll);
  info.close = nv::i18n::Text(nv::i18n::StringId::kInfoClose);
  if (g_state->path.empty()) return info;

  auto& lines = info.lines;
  lines.reserve(32);
  const auto header = [&lines](nv::i18n::StringId id) {
    nv::ui::InfoLine line;
    line.header = true;
    line.label = nv::i18n::Text(id);
    lines.push_back(std::move(line));
  };
  const auto row = [&lines](nv::i18n::StringId id, std::wstring value) {
    nv::ui::InfoLine line;
    line.label = nv::i18n::Text(id);
    line.value = InfoValueOrDash(value);
    lines.push_back(std::move(line));
  };

  // File group: everything is available immediately, without decoding.
  header(nv::i18n::StringId::kInfoGroupFile);
  row(nv::i18n::StringId::kInfoName, g_state->path.filename().wstring());
  row(nv::i18n::StringId::kInfoPath,
      MiddleElide(AbsoluteDisplayPath(), 56));
  row(nv::i18n::StringId::kInfoFileSize, FormatFileSize(CurrentFileSize()));
  row(nv::i18n::StringId::kInfoModified,
      FormatFileTimestamp(g_state->path));

  // Image group: comes from the decoded frame (available once the image is up).
  header(nv::i18n::StringId::kInfoGroupImage);
  row(nv::i18n::StringId::kInfoFormat,
      FormatUppercaseExtension(g_state->path));
  row(nv::i18n::StringId::kInfoDimensions, CurrentImageDimensionText());
  row(nv::i18n::StringId::kInfoDepth, DepthText());
  wchar_t frames[32]{};
  const uint32_t frame_count =
      g_state->current != nullptr ? g_state->current->info.frame_count : 1;
  swprintf_s(frames, L"%u", frame_count == 0 ? 1 : frame_count);
  row(nv::i18n::StringId::kInfoFrames, frames);

  // Camera group: empty until the background EXIF read completes. The rows
  // stay in place (showing an em dash) so the panel height never jumps.
  header(nv::i18n::StringId::kInfoGroupCamera);
  row(nv::i18n::StringId::kInfoCamera, CameraModelText());
  row(nv::i18n::StringId::kInfoLens, ExifValue(nv::ExifId::kLens));
  std::wstring taken = ExifValue(nv::ExifId::kDateTimeOriginal);
  if (taken.empty()) taken = ExifValue(nv::ExifId::kDateTime);
  row(nv::i18n::StringId::kInfoTaken, taken);
  row(nv::i18n::StringId::kInfoExposure, ExifValue(nv::ExifId::kExposureTime));
  row(nv::i18n::StringId::kInfoAperture, ExifValue(nv::ExifId::kFNumber));
  row(nv::i18n::StringId::kInfoIso, ExifValue(nv::ExifId::kIso));
  row(nv::i18n::StringId::kInfoFocal, ExifValue(nv::ExifId::kFocalLength));
  row(nv::i18n::StringId::kInfoBias, ExifValue(nv::ExifId::kExposureBias));
  row(nv::i18n::StringId::kInfoFlash, ExifValue(nv::ExifId::kFlash));
  row(nv::i18n::StringId::kInfoMetering, ExifValue(nv::ExifId::kMeteringMode));
  row(nv::i18n::StringId::kInfoWhiteBalance,
      ExifValue(nv::ExifId::kWhiteBalance));
  row(nv::i18n::StringId::kInfoGps, GpsText());

  // Color group: the embedded profile and whether it was converted to sRGB.
  header(nv::i18n::StringId::kInfoGroupColor);
  row(nv::i18n::StringId::kInfoIcc, ColorProfileText());
  return info;
}

// "label: value" for every row that has a value; group headers are skipped
// (規劃/22/06 坑 14).
std::wstring InfoPanelText() {
  const nv::ui::InfoPanelStrings info = BuildInfoStrings();
  std::wstring text;
  for (const nv::ui::InfoLine& line : info.lines) {
    if (line.header) continue;
    if (line.value.empty() || line.value == nv::i18n::Text(
                                                nv::i18n::StringId::kInfoNone)) {
      continue;
    }
    wchar_t row[1024]{};
    swprintf_s(row, nv::i18n::Text(nv::i18n::StringId::kInfoValueFormat),
               line.label.c_str(), line.value.c_str());
    if (!text.empty()) text += L"\r\n";
    text += row;
  }
  return text;
}

// Copies one row's value (the row context menu) or the whole sheet.
void CopyInfoLine(HWND hwnd, int index) {
  const nv::ui::InfoPanelStrings info = BuildInfoStrings();
  if (index < 0 || static_cast<size_t>(index) >= info.lines.size()) return;
  const nv::ui::InfoLine& line = info.lines[index];
  if (line.header || line.value.empty() ||
      line.value == nv::i18n::Text(nv::i18n::StringId::kInfoNone)) {
    return;
  }
  std::wstring error;
  if (!nv::platform::CopyTextToClipboard(hwnd, line.value, &error)) {
    ShowErrorToast(hwnd, nv::i18n::Text(nv::i18n::StringId::kErrorCopyPath),
                   error);
    return;
  }
  nv::platform::LogInfo("ui: info_copy=row " + std::to_string(index));
  ShowToast(hwnd, nv::i18n::Text(nv::i18n::StringId::kInfoCopied));
}

void CopyInfoAll(HWND hwnd) {
  const std::wstring text = InfoPanelText();
  if (text.empty()) return;
  std::wstring error;
  if (!nv::platform::CopyTextToClipboard(hwnd, text, &error)) {
    ShowErrorToast(hwnd, nv::i18n::Text(nv::i18n::StringId::kErrorCopyPath),
                   error);
    return;
  }
  nv::platform::LogInfo("ui: info_copy=all lines=" +
                        std::to_string(std::count(text.begin(), text.end(),
                                                  L'\n') +
                                       1));
  ShowToast(hwnd, nv::i18n::Text(nv::i18n::StringId::kInfoCopiedAll));
}

// Asks the worker for the current file's tags when the card is (or becomes)
// visible. Cached results still arrive through kExifDone, so there is one path.
void RequestExifForPath(const std::filesystem::path& path) {
  if (!g_exif || path.empty()) return;
  const std::wstring key = nv::ExifService::MakeKey(path);
  if (key == g_exif_key) return;  // already showing this file's tags
  g_exif->Request(path);
}

void OnExifDone(HWND hwnd, LPARAM lparam) {
  std::unique_ptr<nv::ExifCompletion> completion(
      reinterpret_cast<nv::ExifCompletion*>(lparam));
  if (!completion || g_state == nullptr) return;
  if (g_state->path.empty() || completion->path != g_state->path) {
    nv::platform::LogInfo("exif: stale result dropped");
    return;
  }
  g_exif_data = std::move(completion->data);
  g_exif_key = completion->key;
  size_t values = 0;
  for (const nv::ExifField& field : g_exif_data.fields) {
    if (!field.value.empty()) ++values;
  }
  char line[128]{};
  std::snprintf(line, sizeof(line), "exif: done ok=%d fields=%zu",
                g_exif_data.ok ? 1 : 0, values);
  nv::platform::LogInfo(line);
  if (InfoWindowVisible()) UpdateInfoWindowContent(hwnd);
}

// --- v3.0 (規劃/26 §2.1): standalone info window ---------------------------
// The card lives in its own top-level window, so visibility is a property of
// that window (plus the persisted user intent) instead of the overlay stack.
bool InfoWindowVisible() {
  return g_info_window != nullptr && g_info_window->IsOpen();
}

namespace {

void PersistInfoWindowPlacement(const RECT& rect) {
  g_settings.info_window_x = rect.left;
  g_settings.info_window_y = rect.top;
  g_settings.info_window_saved = true;
  SaveSettingsNow();
}

// The window told us the user closed it (button, Escape or system close).
void OnInfoWindowClosed(HWND hwnd) {
  (void)hwnd;
  nv::platform::LogInfo(g_info_closing_with_app ? "ui: info_window=closed (app)"
                                                : "ui: info_window=closed");
  g_info_shown = false;
  if (!g_info_closing_with_app) {
    // Closing is the user's intent: the next launch starts hidden.
    g_settings.info_window_visible = false;
    SaveSettingsNow();
  }
}

}  // namespace

bool OpenInfoWindow(HWND hwnd) {
  if (InfoWindowVisible()) return true;
  if (g_state == nullptr || g_state->path.empty()) return false;
  if (!g_info_window) g_info_window = std::make_unique<nv::ui::InfoWindow>();

  nv::ui::InfoWindow::Host host;
  host.on_close = [hwnd] { OnInfoWindowClosed(hwnd); };
  host.on_moved = [](int x, int y) {
    g_settings.info_window_x = x;
    g_settings.info_window_y = y;
    g_settings.info_window_saved = true;
    SaveSettingsNow();
    nv::platform::LogInfo("ui: info_window=moved " + std::to_string(x) + "," +
                          std::to_string(y));
  };
  host.on_copy_all = [hwnd] { CopyInfoAll(hwnd); };
  host.on_copy_row = [hwnd](int index) { CopyInfoLine(hwnd, index); };

  const bool opened = g_info_window->Open(
      hwnd, BuildInfoStrings(), g_settings.dark_theme,
      nv::platform::DpiScaleForWindow(hwnd), g_settings.info_window_saved,
      g_settings.info_window_x, g_settings.info_window_y, std::move(host));
  if (!opened) {
    nv::platform::LogError("ui: info_window open failed");
    return false;
  }
  g_settings.info_window_visible = true;
  g_info_shown = true;
  PersistInfoWindowPlacement(g_info_window->WindowRect());
  g_info_restore_pending = false;
  RECT rect = g_info_window->WindowRect();
  nv::platform::LogInfo("ui: info_window=open " + std::to_string(rect.left) +
                        "," + std::to_string(rect.top) + " " +
                        std::to_string(rect.right - rect.left) + "x" +
                        std::to_string(rect.bottom - rect.top));
  // The tags are read once the card exists; the rows already show placeholders.
  RequestExifForPath(g_state->path);
  // --info-geometry: deterministic move so the persistence path is testable
  // without a real drag (same trick as --move-window).
  if (g_test_info_geometry_valid) {
    g_info_window->MoveTo(g_test_info_geometry.x, g_test_info_geometry.y);
    nv::platform::LogInfo("test: info_window moved");
  }
  return true;
}

void CloseInfoWindow(HWND hwnd) {
  if (!InfoWindowVisible()) return;
  g_info_closing_with_app = false;
  g_info_window->Close();
  if (hwnd != nullptr) RefreshUi(hwnd, false);
}

void CloseInfoWindowForAppClose() {
  if (!InfoWindowVisible()) return;
  g_info_closing_with_app = true;
  nv::platform::LogInfo("ui: info_window=app_close");
  g_info_window->Close();
}

void ToggleInfoPanel(HWND hwnd) {
  if (InfoWindowVisible()) {
    CloseInfoWindow(hwnd);
  } else if (!OpenInfoWindow(hwnd)) {
    return;
  }
  nv::platform::LogInfo(std::string("ui: info=") +
                        (InfoWindowVisible() ? "1" : "0"));
  RefreshUi(hwnd, false);
}

// Keeps the window in step with the main window: hidden while the owner is
// minimised or fullscreen, shown again afterwards if the user had it open
// (規劃/26 §2.1 lifecycle). ShowWindow(..., SW_SHOWNOACTIVATE) never steals the
// keyboard from the main window, and the log only fires on an actual change.
void SyncInfoWindowVisibility(HWND hwnd, bool owner_minimized) {
  if (!InfoWindowVisible()) return;
  const bool wanted = g_settings.info_window_visible && g_state != nullptr &&
                      !g_state->path.empty() && !g_window_state.fullscreen() &&
                      !owner_minimized &&
                      (hwnd == nullptr || IsIconic(hwnd) == FALSE);
  if (wanted == g_info_shown) return;
  g_info_shown = wanted;
  ShowWindow(g_info_window->hwnd(), wanted ? SW_SHOWNOACTIVATE : SW_HIDE);
  nv::platform::LogInfo(std::string("ui: info_window_visible=") +
                        (wanted ? "1" : "0"));
}

void UpdateInfoWindowContent(HWND hwnd) {
  (void)hwnd;
  if (!InfoWindowVisible()) return;
  g_info_window->SetContent(BuildInfoStrings());
}


// --- v2.0 S2: thumbnail rail modes, fade and tooltip (規劃/22/08) ----------
// Pinned paints at full opacity and reserves space; auto overlays the image and
// fades between 0 and 1 (160 ms) when the pointer reaches the bottom hot zone
// (8 DIP) or the faded-in rail; off paints and requests nothing.
float EffectiveStripOpacity() {
  // v3.0 (規劃/26 §2.7): in fullscreen every non-off mode is reveal-only, so
  // the fade factor is the only source there (pinned no longer forces 1).
  if (g_window_state.fullscreen()) {
    return g_strip_mode == nv::ui::StripMode::kOff ? 0.0f : g_strip_opacity;
  }
  if (g_strip_mode == nv::ui::StripMode::kPinned) return 1.0f;
  if (g_strip_mode == nv::ui::StripMode::kOff) return 0.0f;
  return g_strip_opacity;
}

bool StripShown() {
  if (g_window_state.fullscreen()) {
    return g_strip_mode != nv::ui::StripMode::kOff && g_strip_opacity > 0.001f;
  }
  return g_strip_mode == nv::ui::StripMode::kPinned ||
         (g_strip_mode == nv::ui::StripMode::kAuto && g_strip_opacity > 0.001f);
}

// v3.0 (規劃/26 §2.7): the rail is reveal-driven when the mode is auto, and in
// fullscreen for every mode except off (pitfall 146 is reversed for that).
bool StripRevealEnabled() {
  if (g_strip_mode == nv::ui::StripMode::kOff) return false;
  return g_strip_mode == nv::ui::StripMode::kAuto ||
         g_window_state.fullscreen();
}

// "Reduce motion" / animations disabled: the fade collapses to one frame
// (規劃/22/08 §3.1).
bool StripAnimationsEnabled() {
  BOOL enabled = TRUE;
  return SystemParametersInfoW(SPI_GETCLIENTAREAANIMATION, 0, &enabled, 0) != 0 &&
         enabled != FALSE;
}

void HideStripTooltip(HWND hwnd) {
  const bool was_visible = g_strip_tooltip_visible;
  g_strip_tooltip_visible = false;
  if (hwnd != nullptr) KillTimer(hwnd, kStripTooltipTimer);
  if (was_visible && hwnd != nullptr) InvalidateStrip(hwnd);
}

// Puts the visual state back in sync with the mode: pinned is opaque at once,
// auto starts hidden, off has nothing to paint.
void ResetStripVisualState(HWND hwnd) {
  // Fullscreen is reveal-only, so even a pinned rail starts hidden there.
  g_strip_opacity = (g_strip_mode == nv::ui::StripMode::kPinned &&
                     !g_window_state.fullscreen())
                        ? 1.0f
                        : 0.0f;
  g_strip_target = g_strip_opacity;
  g_strip_hide_at_ms = 0.0;
  g_strip_fade_tick_ms = NowMs();
  g_strip_hover_index = -1;
  HideStripTooltip(hwnd);
  if (hwnd != nullptr) {
    KillTimer(hwnd, kStripFadeTimer);
    KillTimer(hwnd, kStripHideTimer);
  }
  RequestVisibleThumbnails();
}

void ShowStrip(HWND hwnd) {
  if (!StripRevealEnabled()) return;
  g_strip_hide_at_ms = 0.0;
  KillTimer(hwnd, kStripHideTimer);
  if (g_strip_target >= 1.0f) return;
  g_strip_target = 1.0f;
  nv::platform::LogInfo("ui: strip=show");
  if (!StripAnimationsEnabled()) {
    // Reduced motion: reveal in one frame, exactly the same end state.
    g_strip_opacity = 1.0f;
    RequestVisibleThumbnails();
    RefreshUi(hwnd, true);
    InvalidateStrip(hwnd);
    return;
  }
  g_strip_fade_tick_ms = NowMs();
  SetTimer(hwnd, kStripFadeTimer, 16, nullptr);
  // The rail is about to become visible: ask for the previews it will paint.
  RequestVisibleThumbnails();
}

void ScheduleStripHide(HWND hwnd) {
  if (!StripRevealEnabled()) return;
  if (g_strip_opacity <= 0.001f || g_strip_hide_at_ms > 0.0) return;
  g_strip_hide_at_ms = NowMs() + kStripHideDelayMs;
  SetTimer(hwnd, kStripHideTimer, kStripHideDelayMs, nullptr);
}

void TickStripFade(HWND hwnd) {
  if (!StripRevealEnabled()) {
    KillTimer(hwnd, kStripFadeTimer);
    return;
  }
  const double now = NowMs();
  if (g_strip_hide_at_ms > 0.0 && now >= g_strip_hide_at_ms) {
    g_strip_hide_at_ms = 0.0;
    g_strip_target = 0.0f;
    KillTimer(hwnd, kStripHideTimer);
    HideStripTooltip(hwnd);
  }
  const double dt = std::max(0.0, now - g_strip_fade_tick_ms);
  g_strip_fade_tick_ms = now;
  // Elapsed-time based so the fade takes 160 ms regardless of timer jitter.
  const float step = static_cast<float>(dt / static_cast<double>(kStripFadeMs));
  const bool was_shown = g_strip_opacity > 0.001f;
  if (g_strip_opacity < g_strip_target) {
    g_strip_opacity = std::min(g_strip_target, g_strip_opacity + step);
  } else if (g_strip_opacity > g_strip_target) {
    g_strip_opacity = std::max(g_strip_target, g_strip_opacity - step);
  }
  const bool now_shown = g_strip_opacity > 0.001f;
  // Once the rail is visible enough to read, ask for its previews; the request
  // path itself is idempotent and skips items that already have a bitmap.
  if (g_strip_opacity > 0.05f && g_strip_target > 0.0f) {
    RequestVisibleThumbnails();
  }
  if (was_shown != now_shown) {
    // One inset change per transition, never per frame (規劃/22/08 坑 1).
    RefreshUi(hwnd, true);
  } else {
    InvalidateStrip(hwnd);
  }
  if (g_strip_opacity <= 0.001f) {
    g_strip_opacity = 0.0f;
    if (g_state && g_state->thumbs) g_state->thumbs->CancelPending();
    KillTimer(hwnd, kStripFadeTimer);
    // V3.1-09 evidence: the fade-out duration (kStripFadeMs plus timer jitter).
    char hidden[128]{};
    std::snprintf(hidden, sizeof(hidden), "ui: strip=hidden fade_ms=%.0f",
                  g_strip_hide_start_ms > 0.0 ? NowMs() - g_strip_hide_start_ms
                                              : 0.0);
    g_strip_hide_start_ms = 0.0;
    nv::platform::LogInfo(hidden);
  } else if (g_strip_opacity >= 0.999f) {
    g_strip_opacity = 1.0f;
    KillTimer(hwnd, kStripFadeTimer);
  }
}

// --- v3.0 (規劃/26 §2.7): fullscreen reveal --------------------------------
// The two buttons live in the top-right corner. The rectangles are computed by
// the layout for the whole fullscreen session (they host the hot zone); this
// block only animates the opacity and owns the 3 s idle deadline.
nv::ui::Rect FullscreenRevealHotRect() {
  if (g_ui == nullptr) return {};
  const nv::ui::LayoutFrame& layout = g_ui->layout();
  if (layout.fs_exit.empty() || layout.fs_close.empty()) return {};
  const float pad = nv::ui::metrics::kFullscreenHotPad * layout.dpi_scale;
  return nv::ui::Rect::FromLTRB(layout.fs_exit.left - pad,
                                layout.fs_exit.top - pad,
                                layout.fs_close.right + pad,
                                layout.fs_close.bottom + pad);
}

void ShowFullscreenReveal(HWND hwnd) {
  if (!g_window_state.fullscreen() || g_ui == nullptr) return;
  g_fs_reveal_hide_at_ms = NowMs() + kFsRevealIdleMs;
  if (g_fs_reveal_target >= 1.0f) return;
  g_fs_reveal_target = 1.0f;
  nv::platform::LogInfo("ui: fs_reveal=show");
  if (!StripAnimationsEnabled()) {
    g_fs_reveal_opacity = 1.0f;
    RefreshUi(hwnd, false);
    return;
  }
  g_fs_reveal_tick_ms = NowMs();
  SetTimer(hwnd, kFsRevealTimer, 16, nullptr);
}

void ResetFullscreenReveal(HWND hwnd) {
  g_fs_reveal_opacity = 0.0f;
  g_fs_reveal_target = 0.0f;
  g_fs_reveal_hide_at_ms = 0.0;
  if (hwnd != nullptr) KillTimer(hwnd, kFsRevealTimer);
}

void TickFullscreenReveal(HWND hwnd) {
  if (!g_window_state.fullscreen()) {
    ResetFullscreenReveal(hwnd);
    RefreshUi(hwnd, false);
    return;
  }
  const double now = NowMs();
  if (g_fs_reveal_hide_at_ms > 0.0 && now >= g_fs_reveal_hide_at_ms) {
    g_fs_reveal_hide_at_ms = 0.0;
    if (g_fs_reveal_target > 0.0f) {
      g_fs_reveal_target = 0.0f;
      nv::platform::LogInfo("ui: fs_reveal=hide");
    }
  }
  const double dt = std::max(0.0, now - g_fs_reveal_tick_ms);
  g_fs_reveal_tick_ms = now;
  const float step = static_cast<float>(dt / kFsRevealFadeMs);
  const bool was_visible = g_fs_reveal_opacity > 0.001f;
  if (g_fs_reveal_opacity < g_fs_reveal_target) {
    g_fs_reveal_opacity = std::min(g_fs_reveal_target, g_fs_reveal_opacity + step);
  } else if (g_fs_reveal_opacity > g_fs_reveal_target) {
    g_fs_reveal_opacity = std::max(g_fs_reveal_target, g_fs_reveal_opacity - step);
  }
  const bool visible = g_fs_reveal_opacity > 0.001f;
  // The rectangles exist regardless of opacity; crossing 0 only changes whether
  // they are painted/hit-testable, so a plain input refresh is enough.
  RefreshUi(hwnd, was_visible != visible);
  if (g_fs_reveal_opacity <= 0.001f && g_fs_reveal_target <= 0.0f) {
    g_fs_reveal_opacity = 0.0f;
    g_fs_reveal_hide_at_ms = 0.0;
    KillTimer(hwnd, kFsRevealTimer);
  } else if (g_fs_reveal_opacity >= 0.999f && g_fs_reveal_target >= 1.0f) {
    // Fully visible: the fade is done, but the idle deadline still has to fire.
    // Re-arm for the remaining time instead of killing the timer (a killed
    // timer would leave the pair on screen forever).
    g_fs_reveal_opacity = 1.0f;
    const double remaining = std::max(0.0, g_fs_reveal_hide_at_ms - now);
    SetTimer(hwnd, kFsRevealTimer,
             static_cast<UINT>(std::max(16.0, remaining)), nullptr);
  }
}


// --- arrow reveal ----------------------------------------------------------
void ArrowLog(const char* state, const char* reason, double extra_ms) {
  char line[160]{};
  if (extra_ms > 0.0) {
    std::snprintf(line, sizeof(line), "ui: arrows=%s reason=%s ms=%.0f", state,
                  reason, extra_ms);
  } else {
    std::snprintf(line, sizeof(line), "ui: arrows=%s reason=%s", state, reason);
  }
  nv::platform::LogInfo(line);
}

// Restarts the five-second idle deadline. Cheap enough for every WM_MOUSEMOVE:
// SetTimer on the same id just moves the deadline.
void ArmArrowIdle(HWND hwnd) {
  g_arrow_last_activity_ms = NowMs();
  if (hwnd != nullptr) SetTimer(hwnd, kArrowIdleTimer, kArrowIdleMs, nullptr);
}

void StartArrowFade(HWND hwnd) {
  if (hwnd == nullptr) return;
  g_arrow_fade_tick_ms = NowMs();
  SetTimer(hwnd, kArrowFadeTimer, 16, nullptr);
}

// Any input: show the circles again and restart the idle countdown.
void ShowArrows(HWND hwnd, const char* reason) {
  const bool was_gone = g_arrow_target <= 0.0f || g_arrow_opacity <= 0.999f;
  g_arrow_pointer_inside = true;
  ArmArrowIdle(hwnd);
  if (!was_gone) return;
  g_arrow_target = 1.0f;
  ArrowLog("show", reason, 0.0);
  StartArrowFade(hwnd);
}

// The pointer left the client area, or it has been still for five seconds.
void HideArrows(HWND hwnd, const char* reason) {
  if (g_arrow_target <= 0.0f) return;
  g_arrow_target = 0.0f;
  ArrowLog("hide", reason, 0.0);
  StartArrowFade(hwnd);
}

void TickArrowFade(HWND hwnd) {
  const double now = NowMs();
  const double dt = std::max(0.0, now - g_arrow_fade_tick_ms);
  g_arrow_fade_tick_ms = now;
  const float step = static_cast<float>(dt / kArrowFadeMs);
  const bool was_visible = g_arrow_opacity > 0.001f;
  if (g_arrow_opacity < g_arrow_target) {
    g_arrow_opacity = std::min(g_arrow_target, g_arrow_opacity + step);
  } else if (g_arrow_opacity > g_arrow_target) {
    g_arrow_opacity = std::max(g_arrow_target, g_arrow_opacity - step);
  }
  const bool visible = g_arrow_opacity > 0.001f;
  // The rectangles always exist; crossing zero only changes painting and hit
  // testing, so a plain input refresh is enough (same rule as the fs reveal).
  RefreshUi(hwnd, was_visible != visible);
  InvalidateRect(hwnd, nullptr, FALSE);
  if (g_arrow_opacity <= 0.001f && g_arrow_target <= 0.0f) {
    g_arrow_opacity = 0.0f;
    KillTimer(hwnd, kArrowFadeTimer);
    ArrowLog("hidden", "fade", 0.0);
  } else if (g_arrow_opacity >= 0.999f && g_arrow_target >= 1.0f) {
    g_arrow_opacity = 1.0f;
    KillTimer(hwnd, kArrowFadeTimer);
  }
}

// Restarts the 500 ms tooltip countdown whenever the hovered slot changes. The
// pointer coordinate is already client-relative.
void SyncStripHover(HWND hwnd, float x, float y) {
  if (!g_state) return;
  const nv::ui::Rect strip = StripRect();
  int index = -1;
  if (!strip.empty() && StripShown()) {
    size_t hit = nv::render::ThumbnailStrip::kNone;
    if (g_state->strip.HitTest(x, y, strip.width(), strip.bottom, &hit)) {
      index = static_cast<int>(hit);
    }
  }
  if (index == g_strip_hover_index) {
    // Same slot: a small settle still counts as "resting". Moving more than
    // 4 DIP restarts the countdown, but a visible tooltip is not flickered.
    if (index >= 0 && !g_strip_tooltip_visible && g_ui != nullptr) {
      const int slack =
          static_cast<int>(std::lround(4.0f * g_ui->dpi_scale()));
      if (std::abs(static_cast<int>(x) - g_strip_hover_origin.x) > slack ||
          std::abs(static_cast<int>(y) - g_strip_hover_origin.y) > slack) {
        g_strip_hover_origin = POINT{static_cast<LONG>(x),
                                     static_cast<LONG>(y)};
        KillTimer(hwnd, kStripTooltipTimer);
        SetTimer(hwnd, kStripTooltipTimer, kStripTooltipDelayMs, nullptr);
      }
    }
    return;
  }
  g_strip_hover_index = index;
  g_strip_hover_origin = POINT{static_cast<LONG>(x), static_cast<LONG>(y)};
  nv::platform::LogInfo("ui: strip_hover=" + std::to_string(index));
  HideStripTooltip(hwnd);
  if (index >= 0) SetTimer(hwnd, kStripTooltipTimer, kStripTooltipDelayMs, nullptr);
}

void ShowStripTooltip(HWND hwnd) {
  if (!g_state || g_strip_hover_index < 0 || !StripShown()) return;
  if (static_cast<size_t>(g_strip_hover_index) >= g_state->folder.files.size()) {
    return;
  }
  g_strip_tooltip_visible = true;
  nv::platform::LogInfo("ui: strip_tooltip=" +
                        Narrow(g_state->folder.files[static_cast<size_t>(
                            g_strip_hover_index)].filename().wstring()));
  InvalidateStrip(hwnd);
}

// Test hooks move the real pointer as well as posting the message: owning the
// cursor keeps TrackMouseEvent(TME_LEAVE) from immediately firing a spurious
// WM_MOUSELEAVE (the pointer would otherwise still be wherever the terminal is)
// and clearing the hover it is meant to test.
void MoveCursorToClient(HWND hwnd, int x, int y) {
  POINT point{x, y};
  if (ClientToScreen(hwnd, &point)) SetCursorPos(point.x, point.y);
}

// --- Startup empty state (規劃/22/03) --------------------------------------
// The card is visible exactly when nothing owns the image area: no image and no
// decode in flight. A folder that was opened but has no supported image is a
// distinct state; an unsupported drop is a transient note over the base state.
nv::ui::EmptyKind VisibleEmptyKind() {
  if (g_state == nullptr) return nv::ui::EmptyKind::kEmpty;
  if (!g_state->path.empty() && g_state->renderer.image_width() > 0) {
    return nv::ui::EmptyKind::kNone;
  }
  if (!g_pending_path.empty()) return nv::ui::EmptyKind::kNone;
  if (g_empty_note) return nv::ui::EmptyKind::kUnsupported;
  return g_empty_base == nv::ui::EmptyKind::kNone ? nv::ui::EmptyKind::kEmpty
                                                  : g_empty_base;
}

std::wstring JoinExtensions(const std::vector<std::wstring>& extensions) {
  std::wstring text;
  for (const std::wstring& extension : extensions) {
    if (!text.empty()) text += L' ';
    text += extension;
  }
  return text;
}

// Fills g_ui_inputs.empty / .formats from the derived format summary. Labels
// and formatted counts are built here; wrapping happens in UiRoot because only
// it can measure text at the current DPI (規劃/22/03 §3.3).
void BuildEmptyInputs() {
  nv::ui::EmptyUiInputs& empty = g_ui_inputs.empty;
  empty = nv::ui::EmptyUiInputs{};
  nv::ui::FormatsUiInputs& formats = g_ui_inputs.formats;
  formats = nv::ui::FormatsUiInputs{};

  const nv::ui::EmptyKind kind = VisibleEmptyKind();
  empty.kind = kind;
  empty.visible = kind != nv::ui::EmptyKind::kNone;
  empty.drag_hint = nv::i18n::Text(nv::i18n::StringId::kEmptyDragHint);
  if (!empty.visible) return;

  using nv::i18n::StringId;
  const bool folder = g_empty_base == nv::ui::EmptyKind::kFolder;
  empty.title = nv::i18n::Text(folder ? StringId::kEmptyFolderTitle
                                      : StringId::kEmptyTitle);
  empty.formats_prefix = nv::i18n::Text(StringId::kEmptyFormatsPrefix);
  empty.primary = nv::i18n::Text(StringId::kEmptyChooseImage);
  empty.open_folder = nv::i18n::Text(StringId::kEmptyOpenFolder);
  empty.all_formats = nv::i18n::Text(StringId::kEmptyAllFormats);

  if (kind == nv::ui::EmptyKind::kUnsupported && !g_empty_note_text.empty()) {
    empty.note = true;
    empty.note_text = g_empty_note_text;
  } else if (folder) {
    empty.note = true;
    empty.note_text = nv::i18n::Text(StringId::kEmptyFolderHint);
  }

  if (g_format_summary == nullptr) return;
  empty.formats = g_format_summary->highlights;
  wchar_t buffer[128]{};
  swprintf_s(buffer, nv::i18n::Text(StringId::kEmptyFormatsMore),
             g_format_summary->extension_count);
  empty.formats_more = buffer;

  // "All formats" card: one row per family, unavailable families keep their
  // extensions visible with the reason instead of lying about support.
  formats.title = nv::i18n::Text(StringId::kEmptyFormatsTitle);
  formats.close = nv::i18n::Text(StringId::kEmptyFormatsClose);
  swprintf_s(buffer, nv::i18n::Text(StringId::kEmptyFormatsHint),
             g_format_summary->extension_count);
  formats.footer = buffer;
  formats.rows.reserve(g_format_summary->families.size());
  for (const nv::FormatFamilyInfo& family : g_format_summary->families) {
    nv::ui::FormatPanelRow row;
    row.name = family.name;
    row.extensions = JoinExtensions(family.extensions);
    if (!family.available) {
      row.note = nv::i18n::Text(family.needs_system_codec
                                    ? StringId::kEmptyFormatsNeedCodec
                                    : StringId::kEmptyFormatsNotBuilt);
    }
    formats.rows.push_back(std::move(row));
  }
}

// --- Error presentation helpers (規劃/22/11) -------------------------------
// Which buttons an error offers: the classifier's action mask filtered by what
// the current error can actually act on, in a fixed visual order and capped at
// the card's three action slots.
bool ErrorActionAvailable(const nv::UiError& error,
                          nv::ErrorAction action) {
  switch (action) {
    case nv::ErrorAction::kRetry:
      return error.kind == nv::ErrorKind::kSaveFailed || !error.path.empty();
    case nv::ErrorAction::kCopyPath:
    case nv::ErrorAction::kOpenWith:
    case nv::ErrorAction::kReveal:
      return !error.path.empty();
    case nv::ErrorAction::kDetails:
    case nv::ErrorAction::kNone:
      return false;
  }
  return false;
}

std::vector<nv::ErrorAction> ErrorActionsFor(const nv::UiError& error) {
  std::vector<nv::ErrorAction> actions;
  for (const nv::ErrorAction action :
       {nv::ErrorAction::kRetry, nv::ErrorAction::kOpenWith,
        nv::ErrorAction::kCopyPath, nv::ErrorAction::kReveal}) {
    if (!nv::HasErrorAction(error.actions, action)) continue;
    if (!ErrorActionAvailable(error, action)) continue;
    actions.push_back(action);
    if (actions.size() >= nv::ui::kErrorActionCount) break;
  }
  return actions;
}

nv::i18n::StringId ErrorActionLabel(nv::ErrorAction action) {
  using nv::i18n::StringId;
  switch (action) {
    case nv::ErrorAction::kRetry:
      return StringId::kErrorActionRetry;
    case nv::ErrorAction::kCopyPath:
      return StringId::kErrorActionCopyPath;
    case nv::ErrorAction::kOpenWith:
      return StringId::kErrorActionOpenWith;
    case nv::ErrorAction::kReveal:
      return StringId::kErrorActionReveal;
    case nv::ErrorAction::kDetails:
    case nv::ErrorAction::kNone:
      return StringId::kErrorActionClose;
  }
  return StringId::kErrorActionClose;
}

// Technical detail line: the decoder/HRESULT message when there is one, plus the
// file name so the card identifies what failed without duplicating the path
// into the main text.
std::wstring ErrorDetailText() {
  std::wstring detail = g_error.detail;
  if (detail.empty() && !g_error.path.empty()) {
    detail = g_error.path.filename().wstring();
  }
  if (detail.size() > 160) detail.resize(160);
  return detail;
}

void BuildErrorInputs() {
  nv::ui::ErrorUiInputs& error = g_ui_inputs.error;
  error = nv::ui::ErrorUiInputs{};
  if (g_ui == nullptr || !g_ui->overlay().error_open() || g_error.empty()) {
    return;
  }
  error.visible = true;
  error.title = nv::i18n::Text(g_error.message_id);
  error.message = nv::i18n::Text(g_error.hint_id);
  error.detail = ErrorDetailText();
  const std::vector<nv::ErrorAction> actions = ErrorActionsFor(g_error);
  error.actions.reserve(actions.size());
  int retry_index = -1;
  for (size_t i = 0; i < actions.size(); ++i) {
    error.actions.push_back(nv::i18n::Text(ErrorActionLabel(actions[i])));
    if (actions[i] == nv::ErrorAction::kRetry) {
      retry_index = static_cast<int>(i);
    }
  }
  error.close = nv::i18n::Text(nv::i18n::StringId::kErrorActionClose);
  error.default_action =
      retry_index >= 0 ? retry_index
                       : (actions.empty() ? -1 : 0);
}

// --- V3.1-06 §06e: the rename overlay's labels and validation line ----------
void BuildRenameInputs() {
  nv::ui::RenameUiInputs& rename = g_ui_inputs.rename;
  rename = nv::ui::RenameUiInputs{};
  if (g_ui == nullptr || g_state == nullptr ||
      !g_ui->overlay().IsModal(nv::ui::OverlayKind::kRename)) {
    return;
  }
  rename.visible = true;
  rename.title = nv::i18n::Text(nv::i18n::StringId::kDialogRename);
  rename.hint = nv::i18n::Text(nv::i18n::StringId::kRenamePrompt);
  rename.ok = nv::i18n::Text(nv::i18n::StringId::kButtonOk);
  rename.cancel = nv::i18n::Text(nv::i18n::StringId::kButtonCancel);
  rename.error = g_rename_error;
}

void BuildConfirmInputs() {
  nv::ui::ConfirmUiInputs& confirm = g_ui_inputs.confirm;
  confirm = nv::ui::ConfirmUiInputs{};
  if (g_ui == nullptr || g_confirm == ConfirmKind::kNone ||
      !g_ui->overlay().IsModal(nv::ui::OverlayKind::kConfirm)) {
    return;
  }
  confirm.visible = true;
  confirm.secondary = nv::i18n::Text(nv::i18n::StringId::kButtonCancel);
  if (g_confirm == ConfirmKind::kDelete) {
    if (!g_state || g_state->path.empty()) return;
    confirm.title = nv::i18n::Text(nv::i18n::StringId::kDialogDelete);
    wchar_t body[1024]{};
    swprintf_s(body, nv::i18n::Text(nv::i18n::StringId::kConfirmDeleteBody),
               g_state->path.filename().wstring().c_str());
    confirm.body = body;
    confirm.primary = nv::i18n::Text(nv::i18n::StringId::kButtonDelete);
    return;
  }
  // Folder-edge prompt for the "ask" wrap policy (規劃/22/10 §4.3). The target
  // folder name is shown when a neighbouring folder was found; otherwise the
  // prompt offers the in-folder wrap (loop fallback).
  const bool forward = g_confirm == ConfirmKind::kWrapNext;
  confirm.title = nv::i18n::Text(forward ? nv::i18n::StringId::kConfirmWrapEndTitle
                                         : nv::i18n::StringId::kConfirmWrapStartTitle);
  if (!g_wrap_target.empty()) {
    wchar_t body[1024]{};
    swprintf_s(
        body,
        nv::i18n::Text(forward
                           ? nv::i18n::StringId::kConfirmWrapNextFolderFormat
                           : nv::i18n::StringId::kConfirmWrapPrevFolderFormat),
        g_wrap_target.filename().wstring().c_str());
    confirm.body = body;
    confirm.primary = nv::i18n::Text(nv::i18n::StringId::kConfirmWrapEnter);
  } else {
    confirm.body = nv::i18n::Text(
        forward ? nv::i18n::StringId::kConfirmWrapEndBody
                : nv::i18n::StringId::kConfirmWrapStartBody);
    confirm.primary = nv::i18n::Text(nv::i18n::StringId::kConfirmWrapContinue);
  }
  confirm.secondary =
      nv::i18n::Text(nv::i18n::StringId::kConfirmWrapStay);
  confirm.danger = false;
  confirm.remember_visible = true;
  confirm.remember_checked = g_wrap_remember;
  confirm.remember =
      nv::i18n::Text(nv::i18n::StringId::kConfirmWrapRemember);
}

void CloseWrapState() {
  g_wrap_remember = false;
  g_wrap_target.clear();
}

// Fills the five title segments and the capability flags from the current
// state. Numbers and text are formatted here so the UI module stays free of
// i18n and file-system concerns.
void BuildUiInputs() {
  if (g_ui == nullptr || g_state == nullptr) return;
  const auto& renderer = g_state->renderer;

  nv::ui::UiInputs& inputs = g_ui_inputs;
  inputs = nv::ui::UiInputs{};
  inputs.has_image = !g_state->path.empty() && renderer.image_width() > 0;
  inputs.maximized = g_window_state.maximized();
  inputs.window_active = g_window_active;
  inputs.fs_reveal_opacity = g_fs_reveal_opacity;
  inputs.strip_mode = g_strip_mode;
  // No picture means no rail in any mode: the empty card owns the content area
  // (規劃/22/08 §3.1).
  inputs.strip_visible = inputs.has_image && g_state->strip.visible() &&
                         g_strip_mode != nv::ui::StripMode::kOff;
  inputs.strip_shown = inputs.strip_visible && StripShown();
  inputs.strip_opacity = EffectiveStripOpacity();
  // Tooltip anchor: the hovered slot's cell, measured from the same live rail
  // geometry that draws it.
  if (g_strip_tooltip_visible && inputs.strip_shown &&
      g_strip_hover_index >= 0 &&
      static_cast<size_t>(g_strip_hover_index) < g_state->folder.files.size()) {
    const nv::ui::Rect strip = StripRect();
    const D2D1_RECT_F cell = g_state->strip.CellRect(
        static_cast<size_t>(g_strip_hover_index), strip.top);
    if (cell.right > cell.left) {
      inputs.strip_tooltip =
          g_state->folder.files[static_cast<size_t>(g_strip_hover_index)]
              .filename()
              .wstring();
      inputs.strip_tooltip_anchor = nv::ui::Rect::FromLTRB(
          cell.left, cell.top, cell.right, cell.bottom);
    }
  }
  // Previous/next circles: only with a picture and more than one file in the
  // folder, or when the wrap policy can leave the folder (規劃/22/09 §5.1).
  nv::WrapStrategy wrap = nv::WrapStrategy::kLoop;
  (void)nv::ParseWrapStrategy(g_settings.folder_wrap, &wrap);
  // The reveal animation owns the last word: at opacity 0 the circles are gone
  // (and not clickable) even though the folder would allow them.
  inputs.arrows_visible =
      inputs.has_image &&
      (g_state->folder.size() > 1 || wrap != nv::WrapStrategy::kLoop) &&
      g_arrow_opacity > 0.001f;
  inputs.arrows_opacity = g_arrow_opacity;
  inputs.arrows_slide = (1.0f - g_arrow_opacity) * kArrowSlideDip;
  inputs.pill_visible = inputs.has_image && AnimPillVisible();
  inputs.anim.visible = inputs.pill_visible;
  inputs.anim.show_buttons = AnimHasControls();
  inputs.anim.playing = g_state->anim.playing();
  inputs.anim.opacity = g_anim_pill_opacity;
  if (inputs.anim.visible && g_state->current != nullptr) {
    const nv::Image& image = *g_state->current;
    std::wstring label;
    if (inputs.anim.show_buttons) {
      label = std::to_wstring(g_state->anim.index() + 1) + L" / " +
              std::to_wstring(image.frames.size());
      nv::i18n::StringId badge = nv::i18n::StringId::kCount;
      if (g_state->anim.ended()) {
        badge = nv::i18n::StringId::kAnimEnded;
      } else if (image.info.frames_truncated ||
                 g_force_anim == ForceAnimState::kTruncated) {
        badge = nv::i18n::StringId::kAnimTruncated;
      }
      if (badge != nv::i18n::StringId::kCount) {
        label += L" \u00B7 ";
        label += nv::i18n::Text(badge);
      }
    } else {
      label = nv::i18n::Text(nv::i18n::StringId::kAnimFirstFrame);
    }
    inputs.anim.label = label;
  }
  // S1: 百分比 (zoom menu), ⓘ (info panel), 更多▾ and the rotate pair are live.
  inputs.percent_available = true;
  inputs.info_available = true;
  inputs.rotate_available = true;  // layout still multiplies by has_image
  inputs.more_available = true;
  inputs.menu_items = BuildMenuItems();
  if (g_ui != nullptr &&
      g_ui->overlay().popup() == nv::ui::OverlayKind::kContext) {
    // v3.0 (規劃/26 §2.9): zero-size anchor at the click point; the UI opens the
    // panel below/right of it and flips near the client edges.
    inputs.menu_anchor = nv::ui::Rect::FromLTRB(
        static_cast<float>(g_context_menu_anchor.x),
        static_cast<float>(g_context_menu_anchor.y),
        static_cast<float>(g_context_menu_anchor.x),
        static_cast<float>(g_context_menu_anchor.y));
  }
  inputs.drag_over = g_drag_over;
  BuildEmptyInputs();
  BuildErrorInputs();
  BuildConfirmInputs();
  BuildRenameInputs();
  inputs.loading.visible = g_loading_state == 2;
  if (inputs.loading.visible) {
    inputs.loading.text =
        g_save_busy ? nv::i18n::Text(nv::i18n::StringId::kSaveSavingText)
                    : nv::i18n::Text(nv::i18n::StringId::kLoadingText);
    inputs.loading.progress = g_loading_phase;
  }

  inputs.strings.file_name = g_state->path.filename().wstring();
  if (inputs.strings.file_name.empty()) inputs.strings.file_name = nv::i18n::Text(nv::i18n::StringId::kAppTitle);
  if (inputs.has_image) {
    wchar_t dimension[64]{};
    swprintf_s(dimension, L"%u \u00D7 %u", TargetImageWidth(),
               TargetImageHeight());
    inputs.strings.dimension = dimension;
    wchar_t percent[32]{};
    // V3-04b (§7.6): while a tier is on screen the renderer's zoom is relative
    // to the tier, so the title must show the file-relative percentage; a fit
    // view of an 8K photo would otherwise read "100 %".
    swprintf_s(percent, L"%d%%",
               static_cast<int>(std::lround(ViewFileScale() * 100.0f)));
    inputs.strings.percent = percent;
  }
  // Two-stage loading badge (規劃/22/13 §3.5): shown while the preview is the
  // best frame, then faded out over 160 ms after the full frame arrives. The
  // failed state keeps the badge (with red text) instead of fading.
  inputs.preview.visible =
      inputs.has_image &&
      (PreviewActive() || g_preview_badge_opacity > 0.01f);
  if (inputs.preview.visible) {
    inputs.preview.failed = g_state->preview_active && g_state->preview_failed;
    inputs.preview.text = nv::i18n::Text(
        inputs.preview.failed ? nv::i18n::StringId::kPreviewBadgeFailed
                              : nv::i18n::StringId::kPreviewBadge);
    inputs.preview.opacity =
        PreviewActive() ? 1.0f : g_preview_badge_opacity;
  }
  inputs.strings.file_size = FormatFileSize(CurrentFileSize());
  if (!g_state->folder.files.empty()) {
    wchar_t index[64]{};
    swprintf_s(index, L"%zu / %zu", g_state->folder.index + 1,
               g_state->folder.size());
    inputs.strings.index = index;
  }

  // v3.0 (規劃/26 §2.4): bird's eye view. The panel exists while the setting is
  // on and a picture is on screen; the renderer only spends the derived-copy
  // VRAM in the same condition. Display width/height are the rotated size, so
  // the panel aspect already follows the view rotation.
  inputs.minimap.visible = g_settings.birdseye && inputs.has_image &&
                           renderer.display_width() > 0 &&
                           renderer.display_height() > 0;
  if (inputs.minimap.visible) {
    inputs.minimap.display_width = renderer.display_width();
    inputs.minimap.display_height = renderer.display_height();
    inputs.minimap.view_scale = renderer.zoom();
    float center_x = 0.0f;
    float center_y = 0.0f;
    renderer.ViewCenter(&center_x, &center_y);
    inputs.minimap.view_center_x = center_x;
    inputs.minimap.view_center_y = center_y;
  }
  g_state->renderer.SetMinimapEnabled(inputs.minimap.visible);
}

// Recomputes the layout and pushes the resulting insets into the renderer, so
// fit/zoom/pan only ever operate on the image area.
void RefreshUi(HWND hwnd, bool invalidate) {
  if (g_ui == nullptr) return;
  // GetClientRect is the authoritative client rectangle. The first WM_SIZE of a
  // borderless window still reports the framed size (DWM applies the new
  // non-client calculation afterwards without another WM_SIZE), so Paint()
  // re-syncs the layout against this same value before drawing.
  float width = 0.0f;
  float height = 0.0f;
  RECT client{};
  if (hwnd != nullptr && GetClientRect(hwnd, &client)) {
    width = static_cast<float>(client.right);
    height = static_cast<float>(client.bottom);
  } else if (g_state != nullptr) {
    width = static_cast<float>(g_state->renderer.client_width());
    height = static_cast<float>(g_state->renderer.client_height());
  }
  g_ui->SetClientSize(width, height);
  BuildUiInputs();
  g_ui->Update(g_ui_inputs);
  if (g_state != nullptr) {
    const nv::ui::LayoutFrame& layout = g_ui->layout();
    const float client_height =
        static_cast<float>(g_state->renderer.client_height());
    g_state->renderer.SetTopInset(layout.content.top);
    g_state->renderer.SetBottomInset(
        std::max(0.0f, client_height - layout.content.bottom));
    if (!layout.strip.empty()) {
      g_state->strip.SetViewWidth(layout.strip.width());
    }
    // v3.0 (規劃/26 §2.4): the bird's eye frame maps the renderer's effective
    // scale, and on a resize that scale is derived from the insets *this* layout
    // just produced (chicken and egg). A second pass converges: the insets do not
    // depend on the panel, so the second layout is the final one.
    if (g_ui_inputs.minimap.visible) {
      BuildUiInputs();
      g_ui->Update(g_ui_inputs);
    }
  }
  if (invalidate && hwnd != nullptr) InvalidateRect(hwnd, nullptr, TRUE);
  // V3.1-06 §06e: the rename EDIT is a separate window, so every layout pass that
  // could move the card (resize, DPI, strip mode) must re-place it.
  if (RenameOverlayOpen()) PositionRenameEdit(hwnd);
  // Arms/disarms the pill's idle fade only when the pill really appears or
  // disappears. Per-frame RefreshUi during playback must not wake a dimmed
  // control (規劃/22/12 §3.1/§6).
  const bool anim_visible = AnimPillVisible();
  if (anim_visible != g_anim_pill_active) {
    g_anim_pill_active = anim_visible;
    UpdateAnimPillTimer(hwnd);
  }
  // V3-04b (規劃/25 §7.6): every path that can change the view scale (keys,
  // wheel, percent menu, bottom bar, resize) ends up here, so this is the one
  // place that has to notice "the tier can no longer supply this view". It is
  // cheap: a flag check unless a tier is actually on screen.
  if (hwnd != nullptr && g_state != nullptr && g_state->current_is_tier) {
    MaybeUpgradeToFullFrame(hwnd);
  }
}

// --- Toast feedback (規劃/22/02 坑 15, 22/11 §3.3) -------------------------
// One line of feedback in the bottom stack. The overlay stack owns the queue;
// the timer only decides when the oldest entry expires.
void ShowToast(HWND hwnd, const std::wstring& text, bool danger) {
  if (g_ui == nullptr || text.empty()) return;
  g_ui->overlay().PushToast(text, danger);
  nv::platform::LogInfo(std::string("ui: toast=") + Narrow(text) +
                        (danger ? " danger=1" : ""));
  if (hwnd != nullptr) {
    SetTimer(hwnd, kToastTimer, kToastLifetimeMs, nullptr);
    RefreshUi(hwnd, false);
    InvalidateRect(hwnd, nullptr, FALSE);
  }
}

void ExpireToast(HWND hwnd) {
  if (g_ui == nullptr) return;
  // Resting the pointer on a toast pauses its expiry (規劃/22/11 §4.1). The
  // timer is re-armed briefly so leaving the toast resumes the countdown.
  POINT cursor{};
  if (GetCursorPos(&cursor) && ScreenToClient(hwnd, &cursor)) {
    if (g_ui->ToastAt(static_cast<float>(cursor.x),
                      static_cast<float>(cursor.y)) >= 0) {
      SetTimer(hwnd, kToastTimer, 300, nullptr);
      return;
    }
  }
  g_ui->overlay().PopToast();
  if (g_ui->overlay().toast_visible()) {
    SetTimer(hwnd, kToastTimer, kToastLifetimeMs, nullptr);
  } else {
    KillTimer(hwnd, kToastTimer);
  }
  RefreshUi(hwnd, false);
  InvalidateRect(hwnd, nullptr, FALSE);
}

// --- Error card (規劃/22/11 §4.2) ------------------------------------------
// One card, non-modal: a new error replaces the old one, opening another image
// or Escape dismisses it, and clicks outside its buttons reach the image.
void ShowError(HWND hwnd, const nv::UiError& error) {
  if (error.kind == nv::ErrorKind::kCancelled) {
    // A superseded decode is not a failure (規劃/22/11 坑 2).
    nv::platform::LogInfo("error: kind=cancelled (silent)");
    return;
  }
  g_error = error;
  if (g_ui != nullptr) {
    g_ui->overlay().SetErrorOpen(true);
    g_ui->overlay().ClosePopup();
  }
  CancelLoading(hwnd);
  char line[512]{};
  std::snprintf(line, sizeof(line), "error: kind=%s code=0x%08lX actions=0x%02lX",
                nv::ErrorKindName(error.kind),
                static_cast<unsigned long>(error.code),
                static_cast<unsigned long>(error.actions));
  std::string message = line;
  if (!error.path.empty()) {
    message += " path=" + Narrow(error.path.wstring());
  }
  if (!error.detail.empty()) {
    message += " detail=" + Narrow(error.detail);
  }
  nv::platform::LogError(message);
  RefreshUi(hwnd, false);
  InvalidateRect(hwnd, nullptr, TRUE);
}

void CloseError(HWND hwnd) {
  if (g_ui != nullptr) g_ui->overlay().SetErrorOpen(false);
  g_error = nv::UiError{};
  nv::platform::LogInfo("error: closed");
  RefreshUi(hwnd, false);
  InvalidateRect(hwnd, nullptr, TRUE);
}

// A new image that actually made it on screen dismisses the previous failure
// (規劃/22/11 坑 1). Unlike CloseError this is not logged as a user close.
void DismissErrorOnNewImage() {
  if (g_force_error_sticky) return;
  if (g_ui == nullptr) return;
  if (!g_ui->overlay().error_open()) return;
  g_ui->overlay().SetErrorOpen(false);
  g_error = nv::UiError{};
  nv::platform::LogInfo("error: cleared=new_image");
}

void ShowErrorToast(HWND hwnd, const std::wstring& text,
                    const std::wstring& detail) {
  if (!detail.empty()) {
    nv::platform::LogError("error: kind=toast detail=" + Narrow(detail));
  }
  ShowToast(hwnd, text, true);
}

// --- Loading indicator (規劃/22/11 §4.4) -----------------------------------
void ShowLoadingNow(HWND hwnd) {
  if (g_loading_state == 2) return;
  g_loading_state = 2;
  g_loading_phase = 0.0f;
  const double elapsed = g_load_start_ms > 0.0 ? NowMs() - g_load_start_ms : 0.0;
  char line[256]{};
  std::snprintf(line, sizeof(line), "load: shown path=%s after_ms=%.0f",
                g_pending_path.empty()
                    ? "-"
                    : Narrow(g_pending_path.filename().wstring()).c_str(),
                elapsed);
  nv::platform::LogInfo(line);
  if (hwnd != nullptr) {
    SetTimer(hwnd, kLoadingTickTimer, 40, nullptr);
    RefreshUi(hwnd, false);
    InvalidateRect(hwnd, nullptr, FALSE);
  }
}

void BeginLoading(HWND hwnd, bool immediate) {
  if (g_suppress_loading) return;
  if (g_loading_state != 0) return;
  g_loading_state = 1;
  g_load_start_ms = NowMs();
  if (immediate) {
    ShowLoadingNow(hwnd);
    return;
  }
  if (hwnd != nullptr) {
    SetTimer(hwnd, kLoadingDelayTimer, kLoadingDelayMs, nullptr);
  }
}

void CancelLoading(HWND hwnd) {
  if (g_force_loading) return;
  if (hwnd != nullptr) {
    KillTimer(hwnd, kLoadingDelayTimer);
    KillTimer(hwnd, kLoadingTickTimer);
  }
  if (g_loading_state == 2) nv::platform::LogInfo("load: hidden");
  if (g_loading_state == 0) return;
  g_loading_state = 0;
  g_loading_phase = 0.0f;
  if (hwnd != nullptr) {
    RefreshUi(hwnd, false);
    InvalidateRect(hwnd, nullptr, FALSE);
  }
}

// `on`/`off` words come from the i18n table so every toggle reports the same
// way ("鸟瞰图：开启" / "Bird's eye view: on").
std::wstring FormatToggle(nv::i18n::StringId id, bool on) {
  wchar_t buffer[160]{};
  swprintf_s(buffer,
             nv::i18n::Text(nv::i18n::StringId::kToastToggleFormat),
             nv::i18n::Text(id),
             nv::i18n::Text(on ? nv::i18n::StringId::kUiOn
                              : nv::i18n::StringId::kUiOff));
  return buffer;
}

// Applies the saved always-on-top flag. The settings page owns the toggle, the
// window manager the state (規劃/22/15).
void ApplyTopmost(HWND hwnd, bool topmost) {
  g_settings.topmost = topmost;
  if (hwnd != nullptr && !g_window_state.fullscreen()) {
    SetWindowPos(hwnd, topmost ? HWND_TOPMOST : HWND_NOTOPMOST, 0, 0, 0, 0,
                 SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
    nv::platform::LogInfo(std::string("ui: topmost=") + (topmost ? "1" : "0"));
  } else if (hwnd != nullptr) {
    // Fullscreen always runs non-topmost so the taskbar/task manager can
    // appear; the saved flag is applied again when fullscreen ends
    // (規劃/22/15 §4/坑 6).
    nv::platform::LogInfo(std::string("ui: topmost=") + (topmost ? "1" : "0") +
                          " (deferred: fullscreen)");
  } else {
    nv::platform::LogInfo(std::string("ui: topmost=") + (topmost ? "1" : "0"));
  }
}

// --- V3.1-08: zoom is file-relative everywhere ------------------------------
//
// V3-04b had to convert between "client pixels per tier pixel" (the renderer)
// and "per file pixel" (the user) with tier_long/file_long, and re-applied the
// ratio whenever the frame changed. Since the renderer now sizes its rectangle
// from the *logical* (file) size and stretches the bitmap into it, its zoom is
// already file-relative: 1.0 is always one file pixel per client pixel, for a
// 1280 tier, a 1920 RAW preview and a full frame alike. Every conversion that
// used to live here is therefore gone - keeping one of them would square the
// percentage.

// A zoom factor the user chose in file pixels (50/100/200/400 %, percent menu).
void SetZoomForFile(float file_scale) {
  if (g_state == nullptr) return;
  g_state->renderer.SetZoom(file_scale);
}

// The 1:1 command: one file pixel per client pixel, centred. This is the one
// command whose whole point is "centred at 1:1", so it re-centres explicitly
// (the upgrade path deliberately does not; see OnDecodeDone).
void ApplyActualSize() {
  if (g_state == nullptr) return;
  g_state->renderer.ActualSize();
  // ActualSize() centres against the bottom-right corner (a v2.0 assumption that
  // the content starts at 0,0); with a title bar inset the picture sits ~20 px
  // above the centre (坑 333), so centre it exactly here.
  g_state->renderer.CenterView();
}

// Runs one popup command. Every entry point (mouse, keyboard, test hook) goes
// through here so availability checks live in exactly one place.
void ExecuteMenuCommand(HWND hwnd, int command) {
  if (!g_state) return;
  const char* name = "none";
  switch (command) {
    case kCmdZoomFit:
      name = "zoom.fit";
      g_state->renderer.FitToWindow();
      break;
    case kCmdZoom50:
      name = "zoom.50";
      SetZoomForFile(0.5f);
      break;
    case kCmdZoom100:
      name = "zoom.100";
      SetZoomForFile(1.0f);
      break;
    case kCmdZoom200:
      name = "zoom.200";
      SetZoomForFile(2.0f);
      break;
    case kCmdZoom400:
      name = "zoom.400";
      SetZoomForFile(4.0f);
      break;
    case kCmdToggleBirdseye:
      name = "toggle.birdseye";
      g_settings.birdseye = !g_settings.birdseye;
      nv::platform::SaveSettings(g_settings);
      nv::platform::LogInfo(std::string("ui: toggle birdseye=") +
                            (g_settings.birdseye ? "1" : "0"));
      ShowToast(hwnd, FormatToggle(nv::i18n::StringId::kUiBirdseye,
                                   g_settings.birdseye));
      break;
    case kCmdToggleThumbnails: {
      name = "toggle.thumbstrip";
      // V3.1-09: the switch is now "智能缩略图" - on means the fade-in-on-hover
      // rail (auto), off means no rail at all. `pinned` stays reachable only
      // from the settings page (規劃/22/08 §3.2); a legacy pinned state counts
      // as "on" and switches off on the first click.
      g_strip_mode = g_strip_mode == nv::ui::StripMode::kOff
                         ? nv::ui::StripMode::kAuto
                         : nv::ui::StripMode::kOff;
      g_settings.thumbstrip_mode = nv::ui::StripModeName(g_strip_mode);
      nv::platform::SaveSettings(g_settings);
      ResetStripVisualState(hwnd);
      nv::platform::LogInfo(std::string("ui: toggle thumbstrip=") +
                            Narrow(g_settings.thumbstrip_mode));
      ShowToast(hwnd, FormatToggle(nv::i18n::StringId::kUiThumbnails,
                                   g_strip_mode != nv::ui::StripMode::kOff));
      break;
    }
    // v3.0 (規劃/26 §2.9): right-click menu commands. The popup is closed before
    // dispatch (both the mouse and the keyboard path do that); the shared footer
    // refreshes the caption/chrome like any other menu command.
    case kContextSaveAs:
      name = "context.save_as";
      SaveAsDialog(hwnd, g_state->path);
      break;
    case kContextCopyImage:
      name = "context.copy_image";
      CopyCurrentImage(hwnd);
      break;
    case kContextCopyPath:
      name = "context.copy_path";
      CopyCurrentPath(hwnd);
      break;
    case kContextRename:
      name = "context.rename";
      RenameCurrent(hwnd);
      break;
    case kContextDelete:
      name = "context.delete";
      DeleteCurrent(hwnd);
      break;
    case kContextWallpaper:
      name = "context.wallpaper";
      SetCurrentWallpaper(hwnd);
      break;
    case kContextReveal:
      name = "context.reveal";
      RevealCurrent(hwnd);
      break;
    case kContextSettings:
      name = "context.settings";
      OpenSettingsWindow(hwnd);
      break;
    default:
      return;
  }
  nv::platform::LogInfo(std::string("cmd: menu=") + name);
  // Fit scale (and therefore the title percentage) changes with the view
  // rotation and with any zoom selection, so the caption is always refreshed.
  UpdateTitle(hwnd);
  RefreshUi(hwnd, true);
  InvalidateRect(hwnd, nullptr, TRUE);
}

// --- Test hook: --click-hit ------------------------------------------------
// Drives one chrome control without composing DPI-scaled mouse events: the
// target rectangle comes from the same cached layout the drawing uses, so the
// click lands exactly where the control is painted at any DPI (規劃/22/02 §7).
const nv::ui::BottomPart* BottomPartByName(const std::wstring& name) {
  static const struct {
    const wchar_t* name;
    nv::ui::BottomPart part;
  } kParts[] = {
      {L"bottom.info", nv::ui::kPartInfo},
      {L"bottom.percent", nv::ui::kPartPercent},
      {L"bottom.index", nv::ui::kPartIndex},
      {L"bottom.fit", nv::ui::kPartFit},
      {L"bottom.actual", nv::ui::kPartActual},
      {L"bottom.rotate_left", nv::ui::kPartRotateLeft},
      {L"bottom.rotate_right", nv::ui::kPartRotateRight},
      {L"bottom.more", nv::ui::kPartMore},
  };
  for (const auto& entry : kParts) {
    if (name == entry.name) return &entry.part;
  }
  return nullptr;
}

void PerformTestClick(HWND hwnd, const std::wstring& name) {
  if (g_ui == nullptr) return;
  const auto center = [](const nv::ui::Rect& rect) {
    return POINT{static_cast<LONG>(std::lround((rect.left + rect.right) * 0.5f)),
                 static_cast<LONG>(std::lround((rect.top + rect.bottom) * 0.5f))};
  };
  const auto post_click = [hwnd](POINT point) {
    LPARAM position = MAKELPARAM(point.x, point.y);
    PostMessageW(hwnd, WM_MOUSEMOVE, 0, position);
    PostMessageW(hwnd, WM_LBUTTONDOWN, MK_LBUTTON, position);
    PostMessageW(hwnd, WM_LBUTTONUP, 0, position);
  };

  // v3.0 (規劃/26 §2.4): bird's eye view. `minimap` presses the middle of the
  // frame, `minimap.blank` the top-left of the photo (only blank at zoom > fit)
  // and `minimap.drag:dx,dy` posts a real press/move/release so the same
  // WM_ handlers run as with a mouse.
  if (name == L"minimap" || name == L"minimap.blank" ||
      name.rfind(L"minimap.drag:", 0) == 0) {
    const nv::ui::MinimapLayout& minimap = g_ui->layout().minimap;
    if (!minimap.visible) {
      nv::platform::LogInfo("test: click=" + Narrow(name) + " skipped=hidden");
      return;
    }
    POINT point = center(minimap.viewport.empty() ? minimap.image
                                                  : minimap.viewport);
    if (name == L"minimap.blank") {
      const float inset = 4.0f * g_ui->dpi_scale();
      point = POINT{
          static_cast<LONG>(std::lround(minimap.image.left + inset)),
          static_cast<LONG>(std::lround(minimap.image.top + inset))};
    }
    nv::platform::LogInfo("test: click=" + Narrow(name) + " x=" +
                          std::to_string(point.x) +
                          " y=" + std::to_string(point.y));
    constexpr size_t kDragPrefix = sizeof("minimap.drag:") - 1;
    if (name.rfind(L"minimap.drag:", 0) == 0) {
      const std::wstring delta = name.substr(kDragPrefix);
      const size_t comma = delta.find(L',');
      if (comma == std::wstring::npos) {
        nv::platform::LogInfo("test: click=" + Narrow(name) + " ignored");
        return;
      }
      const int dx = _wtoi(delta.substr(0, comma).c_str());
      const int dy = _wtoi(delta.substr(comma + 1).c_str());
      const LPARAM down = MAKELPARAM(point.x, point.y);
      const LPARAM move = MAKELPARAM(point.x + dx, point.y + dy);
      PostMessageW(hwnd, WM_MOUSEMOVE, 0, down);
      PostMessageW(hwnd, WM_LBUTTONDOWN, MK_LBUTTON, down);
      PostMessageW(hwnd, WM_MOUSEMOVE, MK_LBUTTON, move);
      PostMessageW(hwnd, WM_LBUTTONUP, 0, move);
      return;
    }
    post_click(point);
    return;
  }

  if (name == L"arrows.left" || name == L"arrows.right") {
    const nv::ui::Rect& rect = name == L"arrows.left"
                                   ? g_ui->layout().arrows_left
                                   : g_ui->layout().arrows_right;
    if (rect.empty()) {
      nv::platform::LogInfo("test: click=" + Narrow(name) + " skipped=hidden");
      return;
    }
    const POINT point = center(rect);
    nv::platform::LogInfo("test: click=" + Narrow(name) + " x=" +
                          std::to_string(point.x) +
                          " y=" + std::to_string(point.y));
    post_click(point);
    return;
  }

  if (name == L"anim.prev" || name == L"anim.next" ||
      name == L"anim.toggle") {
    const nv::ui::AnimPillLayout& pill = g_ui->anim_pill();
    const nv::ui::Rect& rect = name == L"anim.prev"   ? pill.prev
                              : name == L"anim.next"  ? pill.next
                                                      : pill.toggle;
    if (!pill.visible || !pill.interactive || rect.empty()) {
      nv::platform::LogInfo("test: click=" + Narrow(name) + " skipped=hidden");
      return;
    }
    const POINT point = center(rect);
    nv::platform::LogInfo("test: click=" + Narrow(name) + " x=" +
                          std::to_string(point.x) +
                          " y=" + std::to_string(point.y));
    post_click(point);
    return;
  }

  if (const nv::ui::BottomPart* part = BottomPartByName(name)) {
    const POINT point = center(g_ui->bottom().part[*part]);
    nv::platform::LogInfo("test: click=" + Narrow(name) + " x=" +
                          std::to_string(point.x) +
                          " y=" + std::to_string(point.y));
    post_click(point);
    return;
  }

  static const struct {
    const wchar_t* name;
    nv::ui::TitleSegment segment;
  } kSegments[] = {
      {L"title.file_name", nv::ui::kSegFileName},
      {L"title.dimension", nv::ui::kSegDimension},
      {L"title.file_size", nv::ui::kSegFileSize},
  };
  for (const auto& entry : kSegments) {
    if (name != entry.name) continue;
    const nv::ui::Rect& rect =
        g_ui->title().segment[static_cast<size_t>(entry.segment)];
    if (rect.empty()) {
      nv::platform::LogInfo("test: click=" + Narrow(name) + " skipped=hidden");
      return;
    }
    const POINT point = center(rect);
    nv::platform::LogInfo("test: click=" + Narrow(name) + " x=" +
                          std::to_string(point.x) +
                          " y=" + std::to_string(point.y));
    // The real gesture goes through the caption's modal move loop, which a
    // synthetic message cannot reproduce; the segment action is invoked
    // directly (it is the same function the click ends up calling).
    HandleTitleSegmentClick(hwnd, static_cast<float>(point.x),
                           static_cast<float>(point.y));
    return;
  }

  // Title-bar buttons (規劃/26 §2.8): the rectangle comes from the same cached
  // title layout the drawing and HitTest use, and the synthetic down/up runs
  // the normal WM_LBUTTONUP dispatch (same trick as bottom.more). Segments are
  // handled above because a real caption click is a different gesture.
  static const struct {
    const wchar_t* name;
    nv::ui::HitId id;
  } kTitleButtons[] = {
      {L"title.menu", nv::ui::HitId::kTitleMenu},
      {L"title.fullscreen", nv::ui::HitId::kTitleFullscreen},
      {L"title.minimize", nv::ui::HitId::kTitleMinimize},
      {L"title.maximize", nv::ui::HitId::kTitleMaximize},
      {L"title.close", nv::ui::HitId::kTitleClose},
  };
  for (const auto& entry : kTitleButtons) {
    if (name != entry.name) continue;
    const nv::ui::Rect& rect =
        g_ui->title().buttons[static_cast<size_t>(
            nv::ui::TitleButtonIndex(entry.id))];
    if (rect.empty()) {
      nv::platform::LogInfo("test: click=" + Narrow(name) + " skipped=hidden");
      return;
    }
    const POINT point = center(rect);
    nv::platform::LogInfo("test: click=" + Narrow(name) + " x=" +
                          std::to_string(point.x) +
                          " y=" + std::to_string(point.y));
    post_click(point);
    return;
  }

  // v3.0 (規劃/26 §2.7): the fullscreen reveal pair. Same rule as the title
  // buttons: the rectangle comes from the cached layout and a synthetic
  // down/up runs the normal dispatch.
  if (name == L"fullscreen.exit" || name == L"fullscreen.close") {
    const nv::ui::Rect& rect = name == L"fullscreen.exit"
                                   ? g_ui->layout().fs_exit
                                   : g_ui->layout().fs_close;
    if (rect.empty() || g_fs_reveal_opacity <= 0.001f) {
      nv::platform::LogInfo("test: click=" + Narrow(name) + " skipped=hidden");
      return;
    }
    const POINT point = center(rect);
    nv::platform::LogInfo("test: click=" + Narrow(name) + " x=" +
                          std::to_string(point.x) + " y=" +
                          std::to_string(point.y));
    post_click(point);
    return;
  }

  if (name == L"empty.all_formats") {
    const nv::ui::Rect& rect = g_ui->empty().all_formats;
    if (rect.empty()) {
      nv::platform::LogInfo("test: click=empty.all_formats skipped=hidden");
      return;
    }
    nv::platform::LogInfo("test: click=empty.all_formats x=" +
                          std::to_string(center(rect).x) +
                          " y=" + std::to_string(center(rect).y));
    ActivateEmptyAction(hwnd, 2);
    return;
  }

  if (name == L"formats.close") {
    if (g_ui->overlay().IsModal(nv::ui::OverlayKind::kFormats)) {
      g_ui->overlay().CloseModal();
      nv::platform::LogInfo("test: click=formats.close");
      nv::platform::LogInfo("cmd: formats=closed");
      RefreshUi(hwnd, true);
      InvalidateRect(hwnd, nullptr, FALSE);
    } else {
      nv::platform::LogInfo("test: click=formats.close skipped=closed");
    }
    return;
  }

  if (name.rfind(L"error.", 0) == 0) {
    const nv::ui::ErrorCardLayout& card = g_ui->error_card();
    nv::ui::Rect rect;
    if (name == L"error.close") {
      rect = card.close;
    } else {
      const int index = _wtoi(name.c_str() + 6);
      if (index >= 0 && static_cast<size_t>(index) < card.buttons.size()) {
        rect = card.buttons[static_cast<size_t>(index)];
      }
    }
    if (rect.empty()) {
      nv::platform::LogInfo("test: click=" + Narrow(name) + " skipped=hidden");
      return;
    }
    const POINT point = center(rect);
    nv::platform::LogInfo("test: click=" + Narrow(name) + " x=" +
                          std::to_string(point.x) +
                          " y=" + std::to_string(point.y));
    post_click(point);
    return;
  }

  if (name == L"confirm.primary" || name == L"confirm.secondary") {
    const nv::ui::ConfirmCardLayout& card = g_ui->confirm_card();
    const nv::ui::Rect& rect =
        name == L"confirm.primary" ? card.primary : card.secondary;
    if (rect.empty()) {
      nv::platform::LogInfo("test: click=" + Narrow(name) + " skipped=hidden");
      return;
    }
    const POINT point = center(rect);
    nv::platform::LogInfo("test: click=" + Narrow(name) + " x=" +
                          std::to_string(point.x) +
                          " y=" + std::to_string(point.y));
    post_click(point);
    return;
  }

  // Info window (v3.0, 規劃/26 §2.1): its two buttons and any row. The click is
  // posted to the info window itself with its own client coordinates, which is
  // exactly what a real click does. `info.copy_row.N` posts the real
  // WM_CONTEXTMENU there (copy that value), same as a right-click.
  if (name == L"info.close" || name == L"info.copy_all" ||
      name.rfind(L"info.copy_row.", 0) == 0 ||
      name.rfind(L"info.row.", 0) == 0) {
    if (!InfoWindowVisible()) {
      nv::platform::LogInfo("test: click=" + Narrow(name) + " skipped=hidden");
      return;
    }
    const bool copy_row = name.rfind(L"info.copy_row.", 0) == 0;
    const nv::ui::InfoPanelLayout& panel = g_info_window->layout();
    nv::ui::Rect rect;
    if (name == L"info.close") {
      rect = panel.close;
    } else if (name == L"info.copy_all") {
      rect = panel.copy_all;
    } else {
      const int index = _wtoi(name.c_str() + (copy_row ? 14 : 9));
      if (index >= 0 && static_cast<size_t>(index) < panel.line.size()) {
        rect = panel.line[static_cast<size_t>(index)];
      }
    }
    if (rect.empty()) {
      nv::platform::LogInfo("test: click=" + Narrow(name) + " skipped=hidden");
      return;
    }
    const HWND info = g_info_window->hwnd();
    const POINT client = center(rect);
    POINT screen = client;
    ClientToScreen(info, &screen);
    nv::platform::LogInfo("test: click=" + Narrow(name) + " x=" +
                          std::to_string(screen.x) +
                          " y=" + std::to_string(screen.y));
    if (copy_row) {
      PostMessageW(info, WM_CONTEXTMENU, reinterpret_cast<WPARAM>(info),
                   MAKELPARAM(screen.x, screen.y));
    } else {
      const LPARAM position = MAKELPARAM(client.x, client.y);
      PostMessageW(info, WM_MOUSEMOVE, 0, position);
      PostMessageW(info, WM_LBUTTONDOWN, MK_LBUTTON, position);
      PostMessageW(info, WM_LBUTTONUP, 0, position);
    }
    return;
  }

  // V3.1-06 (§06e): the rename overlay's two buttons. The input box is an EDIT
  // window, so there is deliberately no "rename.input" synthetic click.
  if (name == L"rename.ok" || name == L"rename.cancel") {
    const nv::ui::RenameCardLayout& card = g_ui->rename_card();
    const nv::ui::Rect& rect =
        name == L"rename.ok" ? card.primary : card.secondary;
    if (rect.empty()) {
      nv::platform::LogInfo("test: click=" + Narrow(name) + " skipped=hidden");
      return;
    }
    const POINT point = center(rect);
    nv::platform::LogInfo("test: click=" + Narrow(name) + " x=" +
                          std::to_string(point.x) +
                          " y=" + std::to_string(point.y));
    post_click(point);
    return;
  }

  if (name.rfind(L"menu.", 0) == 0) {
    const int row = _wtoi(name.c_str() + 5);
    const nv::ui::MenuLayout& menu = g_ui->menu();
    if (!menu.visible || row < 0 ||
        static_cast<size_t>(row) >= menu.rows.size()) {
      nv::platform::LogInfo("test: click=" + Narrow(name) + " skipped=no_menu");
      return;
    }
    const POINT point = center(menu.rows[static_cast<size_t>(row)]);
    nv::platform::LogInfo("test: click=" + Narrow(name) + " x=" +
                          std::to_string(point.x) +
                          " y=" + std::to_string(point.y));
    post_click(point);
    return;
  }

  nv::platform::LogError("test: click=" + Narrow(name) + " unknown target");
}

// --- Test hook: --send-keys ------------------------------------------------
// Parses one chord token: "Right", "Ctrl+0", "Alt+Enter", "PgUp", "+" ... The
// posted WM_KEYDOWN then runs the same CommandForKey()/DispatchCommand() path
// as a real key press (規劃/22/09 §7 step 9).
bool ParseKeyToken(const std::wstring& token, UINT* virtual_key,
                   TestModifiers* modifiers) {
  if (virtual_key == nullptr || modifiers == nullptr) return false;
  *modifiers = TestModifiers{};
  *virtual_key = 0;

  std::wstring text = token;
  const size_t begin = text.find_first_not_of(L" \t");
  const size_t end = text.find_last_not_of(L" \t");
  if (begin == std::wstring::npos) return false;
  text = text.substr(begin, end - begin + 1);
  std::ranges::transform(text, text.begin(), [](wchar_t ch) {
    return static_cast<wchar_t>(std::towlower(ch));
  });
  if (text.empty()) return false;

  std::vector<std::wstring> parts;
  if (text != L"+") {
    size_t start = 0;
    while (true) {
      const size_t plus = text.find(L'+', start);
      if (plus == std::wstring::npos) {
        parts.push_back(text.substr(start));
        break;
      }
      parts.push_back(text.substr(start, plus - start));
      start = plus + 1;
    }
  } else {
    parts.push_back(text);
  }

  std::wstring key;
  for (const std::wstring& part : parts) {
    if (part == L"ctrl" || part == L"control") {
      modifiers->ctrl = true;
    } else if (part == L"shift") {
      modifiers->shift = true;
    } else if (part == L"alt") {
      modifiers->alt = true;
    } else {
      if (!key.empty()) return false;
      key = part;
    }
  }
  if (key.empty()) return false;

  if (key.size() == 1) {
    const wchar_t ch = key[0];
    if (ch >= L'a' && ch <= L'z') {
      *virtual_key = static_cast<UINT>(std::towupper(ch));
      return true;
    }
    if (ch >= L'0' && ch <= L'9') {
      *virtual_key = static_cast<UINT>(ch);
      return true;
    }
    switch (ch) {
      case L'+':
      case L'=':
        *virtual_key = VK_OEM_PLUS;
        return true;
      case L'-':
        *virtual_key = VK_OEM_MINUS;
        return true;
      case L',':
        *virtual_key = VK_OEM_COMMA;
        return true;
      case L'.':
        *virtual_key = VK_OEM_PERIOD;
        return true;
      default:
        return false;
    }
  }

  static const struct {
    const wchar_t* name;
    UINT key;
  } kNames[] = {
      {L"f2", VK_F2},
      {L"f11", VK_F11},
      {L"f5", VK_F5},
      {L"esc", VK_ESCAPE},
      {L"escape", VK_ESCAPE},
      {L"left", VK_LEFT},
      {L"right", VK_RIGHT},
      {L"up", VK_UP},
      {L"down", VK_DOWN},
      {L"home", VK_HOME},
      {L"end", VK_END},
      {L"pgup", VK_PRIOR},
      {L"pageup", VK_PRIOR},
      {L"pgdn", VK_NEXT},
      {L"pagedown", VK_NEXT},
      {L"space", VK_SPACE},
      {L"enter", VK_RETURN},
      {L"return", VK_RETURN},
      {L"delete", VK_DELETE},
      {L"del", VK_DELETE},
      {L"tab", VK_TAB},
      {L"plus", VK_OEM_PLUS},
      {L"minus", VK_OEM_MINUS},
      {L"comma", VK_OEM_COMMA},
      {L"period", VK_OEM_PERIOD},
      {L"add", VK_ADD},
      {L"subtract", VK_SUBTRACT},
      {L"i", 'I'},
      {L"t", 'T'},
      {L"c", 'C'},
  };
  for (const auto& entry : kNames) {
    if (key == entry.name) {
      *virtual_key = entry.key;
      return true;
    }
  }
  return false;
}

// Client rectangle of the thumbnail rail (empty when it is hidden). The strip
// itself only knows "bottom of the client", so it is fed this rectangle's
// bottom edge instead of the window bottom.
nv::ui::Rect StripRect() {
  if (g_ui == nullptr) return {};
  const nv::ui::Rect& strip = g_ui->layout().strip;
  if (g_strip_mode == nv::ui::StripMode::kOff) return {};
  return strip;
}

// --- v3.0 (規劃/26 §2.4): bird's eye drag -----------------------------------
// A press on the panel either grabs the visible frame (press inside it) or
// re-centres the view on the pressed picture point (press on the blank margin)
// and then keeps dragging from there. Only PanBy() is used: zoom is never
// touched, so dragging the frame can not make the picture grow - the 0.9
// pitfall is avoided by construction rather than by a guard.
void BeginMinimapDrag(HWND hwnd, float x, float y) {
  if (g_ui == nullptr || g_state == nullptr) return;
  const nv::ui::MinimapLayout& layout = g_ui->layout().minimap;
  if (!layout.visible) return;

  float display_x = 0.0f;
  float display_y = 0.0f;
  const bool on_photo =
      nv::ui::MinimapDisplayPoint(layout, x, y, &display_x, &display_y);
  const bool on_frame =
      !layout.viewport.empty() && layout.viewport.Contains(x, y);
  if (on_photo && !on_frame) {
    const float center_x =
        (layout.view_display.left + layout.view_display.right) * 0.5f;
    const float center_y =
        (layout.view_display.top + layout.view_display.bottom) * 0.5f;
    const float scale = g_state->renderer.zoom();
    g_state->renderer.PanBy((center_x - display_x) * scale,
                            (center_y - display_y) * scale);
    RefreshUi(hwnd, false);
    nv::platform::LogInfo("ui: minimap center");
  }

  g_state->minimap_dragging = true;
  g_state->minimap_last_mouse =
      POINT{static_cast<LONG>(x), static_cast<LONG>(y)};
  SetCapture(hwnd);
  nv::platform::LogInfo("ui: minimap drag=start");
  InvalidateRect(hwnd, nullptr, TRUE);
}

void ApplyWindowFrame(HWND hwnd) {
  if (!g_borderless || hwnd == nullptr) return;
  // Win11 rounds the window; maximized/fullscreen must not round or the
  // desktop shows through the corners (規劃/22/01 坑 4). Win10 ignores this.
  const DWORD preference =
      (g_window_state.maximized() || g_window_state.fullscreen())
          ? DWMWCP_DONOTROUND
          : DWMWCP_ROUND;
  DwmSetWindowAttribute(hwnd, DWMWA_WINDOW_CORNER_PREFERENCE, &preference,
                        sizeof(preference));
}

void ApplyWindowModeUi(HWND hwnd, nv::ui::WindowMode mode) {
  char line[96]{};
  std::snprintf(line, sizeof(line), "window: state=%s fullscreen=%d maximized=%d",
                nv::ui::WindowModeName(mode),
                g_window_state.fullscreen() ? 1 : 0,
                g_window_state.maximized() ? 1 : 0);
  nv::platform::LogInfo(line);
  ApplyWindowFrame(hwnd);
  RefreshUi(hwnd, true);
}

void EnterFullscreen(HWND hwnd) {
  if (g_window_state.fullscreen()) return;
  g_saved_placement.valid = true;
  g_saved_placement.maximized = IsZoomed(hwnd) != FALSE;
  g_saved_placement.style = GetWindowLongPtrW(hwnd, GWL_STYLE);
  GetWindowRect(hwnd, &g_saved_placement.rect);

  const nv::ui::WindowMode mode = g_window_state.ToggleFullscreen();
  g_ui->overlay().SetFullscreen(true);
  if (g_state) g_state->renderer.SetFullscreen(true);

  MONITORINFO monitor{};
  monitor.cbSize = sizeof(monitor);
  if (GetMonitorInfoW(MonitorFromWindow(hwnd, MONITOR_DEFAULTTONEAREST),
                      &monitor)) {
    SetWindowLongPtrW(hwnd, GWL_STYLE,
                      WS_POPUP | WS_VISIBLE | WS_CLIPCHILDREN);
    SetWindowPos(hwnd, HWND_TOP, monitor.rcMonitor.left, monitor.rcMonitor.top,
                 monitor.rcMonitor.right - monitor.rcMonitor.left,
                 monitor.rcMonitor.bottom - monitor.rcMonitor.top,
                 SWP_FRAMECHANGED | SWP_SHOWWINDOW);
  }
  if (g_settings.topmost) {
    // Never keep the always-on-top band while fullscreen (規劃/22/15 坑 6).
    SetWindowPos(hwnd, HWND_NOTOPMOST, 0, 0, 0, 0,
                 SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
    nv::platform::LogInfo("ui: topmost=0 (fullscreen)");
  }
  ApplyWindowModeUi(hwnd, mode);
  // v3.0 (規劃/26 §2.7): the rail becomes reveal-only in fullscreen, and the
  // exit buttons show for 3 s so they are discoverable even without moving the
  // pointer first.
  ResetStripVisualState(hwnd);
  ResetFullscreenReveal(hwnd);
  ShowFullscreenReveal(hwnd);
  // The info card hides in fullscreen and returns when fullscreen ends
  // (規劃/26 §2.1 lifecycle).
  SyncInfoWindowVisibility(hwnd);
}

void ExitFullscreen(HWND hwnd) {
  if (!g_window_state.fullscreen()) return;
  const nv::ui::WindowMode mode = g_window_state.ExitFullscreen();
  g_ui->overlay().SetFullscreen(false);
  if (g_state) g_state->renderer.SetFullscreen(false);

  if (g_saved_placement.valid) {
    SetWindowLongPtrW(hwnd, GWL_STYLE, g_saved_placement.style);
    SetWindowPos(hwnd, HWND_TOP, g_saved_placement.rect.left,
                 g_saved_placement.rect.top,
                 g_saved_placement.rect.right - g_saved_placement.rect.left,
                 g_saved_placement.rect.bottom - g_saved_placement.rect.top,
                 SWP_FRAMECHANGED | SWP_SHOWWINDOW);
    if (g_saved_placement.maximized) ShowWindow(hwnd, SW_MAXIMIZE);
    g_saved_placement.valid = false;
  }
  if (g_settings.topmost) {
    // The setting stayed on during fullscreen; restore the z-order band.
    SetWindowPos(hwnd, HWND_TOPMOST, 0, 0, 0, 0,
                 SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
    nv::platform::LogInfo("ui: topmost=1 (fullscreen exit)");
  }
  ApplyWindowModeUi(hwnd, mode);
  // Leaving fullscreen: back to the normal rail rules and no revealed buttons.
  ResetFullscreenReveal(hwnd);
  ResetStripVisualState(hwnd);
  SyncInfoWindowVisibility(hwnd);
}

void ToggleFullscreen(HWND hwnd) {
  if (g_window_state.fullscreen()) {
    ExitFullscreen(hwnd);
  } else {
    EnterFullscreen(hwnd);
  }
}

// Test hook (--info-scroll): one-shot scroll of the info window body. Returns
// false while the window is not open yet so the timer can retry.
bool ApplyTestInfoScroll(HWND hwnd) {
  (void)hwnd;
  if (g_test_info_scroll == 0) return true;
  if (!InfoWindowVisible()) return false;
  g_info_window->ScrollByRows(g_test_info_scroll);
  nv::platform::LogInfo("test: info_scroll=" +
                        std::to_string(g_test_info_scroll));
  g_test_info_scroll = 0;
  return true;
}

// Test hook (--layout-dump): writes the cached layout so the automated checks
// can assert window behaviour without reading pixels. --dump-at uses the same
// writer but keeps the process alive for the watch checks.
void WriteLayoutDump(HWND hwnd, bool destroy) {
  if (g_layout_dump_path.empty() || g_ui == nullptr) return;
  // --info-scroll normally runs on its own timer; applying it here too makes a
  // dump taken before that timer still deterministic (the flag is one-shot).
  ApplyTestInfoScroll(hwnd);
  RefreshUi(hwnd, false);
  const nv::ui::LayoutFrame& layout = g_ui->layout();
  const nv::ui::TitleLayout& title = g_ui->title();
  RECT window{};
  RECT client{};
  GetWindowRect(hwnd, &window);
  GetClientRect(hwnd, &client);

  std::ofstream out(g_layout_dump_path, std::ios::trunc);
  if (!out) return;
  const auto rect_line = [](const char* name, const nv::ui::Rect& rect) {
    char buffer[160]{};
    std::snprintf(buffer, sizeof(buffer), "%s=%.0f,%.0f,%.0f,%.0f\n", name,
                  rect.left, rect.top, rect.right, rect.bottom);
    return std::string(buffer);
  };
  out << "version=" << Narrow(nv::kVersion) << '\n';
  out << "dpi=" << g_ui->dpi_scale() << '\n';
  out << "borderless=" << (g_borderless ? 1 : 0) << '\n';
  out << "state=" << nv::ui::WindowModeName(g_window_state.mode()) << '\n';
  out << "fullscreen=" << (g_window_state.fullscreen() ? 1 : 0) << '\n';
  out << "maximized=" << (g_window_state.maximized() ? 1 : 0) << '\n';
  // v3.0 (規劃/26 §2.7): the fullscreen reveal strip is the only chrome there.
  out << "fs_reveal=" << (g_fs_reveal_opacity > 0.001f ? 1 : 0) << '\n';
  out << "fs_reveal_opacity="
      << static_cast<int>(std::lround(g_fs_reveal_opacity * 100.0f)) << '\n';
  out << "has_image=" << (g_ui_inputs.has_image ? 1 : 0) << '\n';
  out << "arrows_visible=" << (layout.arrows_visible ? 1 : 0) << '\n';
  {
    char arrow_state[64]{};
    std::snprintf(arrow_state, sizeof(arrow_state), "arrows_opacity=%.2f",
                  g_arrow_opacity);
    out << arrow_state << '\n';
  }
  if (g_state != nullptr) {
    // V3-04b (§7.6): `zoom`/`zoom_percent` are file-relative so a tier cannot
    // masquerade as 100 %; `zoom_raw` keeps the renderer value for the tests
    // that compare it against the bitmap size.
    char zoom[48]{};
    std::snprintf(zoom, sizeof(zoom), "zoom=%.3f",
                  static_cast<double>(ViewFileScale()));
    out << zoom << '\n';
    char zoom_raw[48]{};
    std::snprintf(zoom_raw, sizeof(zoom_raw), "zoom_raw=%.3f",
                  static_cast<double>(g_state->renderer.zoom()));
    out << zoom_raw << '\n';
    out << "fit=" << (g_state->renderer.fit() ? 1 : 0) << '\n';
    out << "zoom_min=" << g_state->renderer.min_zoom() << '\n';
    out << "zoom_max=" << g_state->renderer.max_zoom() << '\n';
    out << "zoom_percent=" << nv::render::ZoomPercent(ViewFileScale())
        << '\n';
    // 22/18 §3.5: interpolation state, so a check can prove the static frame
    // upgraded and that --force-quality / --proxy took effect.
    out << "quality="
        << nv::render::DrawQualityName(g_state->renderer.quality()) << '\n';
    out << "quality_last="
        << nv::render::DrawQualityName(
               g_state->renderer.last_drawn_quality())
        << '\n';
    out << "quality_forced="
        << (g_state->renderer.forced_quality() == nv::render::kQualityAuto
                ? "auto"
                : nv::render::DrawQualityName(nv::render::DrawQualityFromValue(
                      g_state->renderer.forced_quality())))
        << '\n';
    out << "interactive=" << (g_state->renderer.interactive() ? 1 : 0) << '\n';
    out << "animating=" << (g_state->renderer.animating() ? 1 : 0) << '\n';
    out << "proxy_enabled=" << (g_state->renderer.proxy_enabled() ? 1 : 0)
        << '\n';
    out << "proxy_active=" << (g_state->renderer.proxy_active() ? 1 : 0)
        << '\n';
    out << "proxy_ready=" << (g_state->renderer.proxy_ready() ? 1 : 0)
        << '\n';
    // V3-04: display-tier evidence. `display_edge=0` + `current_is_tier=0`
    // means the frame on screen is the full-resolution decode; a tier hit is
    // `current_is_tier=1` with the tier edge and where it came from.
    out << "display_edge=" << g_state->display_edge << '\n';
    out << "current_is_tier=" << (g_state->current_is_tier ? 1 : 0) << '\n';
    // §7.7: the renderer's own view of "this bitmap is a tier" — the input the
    // proxy policy refuses to bake from.
    out << "source_is_tier=" << (g_state->renderer.source_is_tier() ? 1 : 0)
        << '\n';
    out << "cache_source=" << g_state->cache_source << '\n';
    // V3.1-01: navigation-burst evidence for the switch-speed check.
    out << "nav_burst=" << (g_nav_burst ? 1 : 0) << '\n';
    out << "burst_streak=" << g_nav_streak << '\n';
    // V3.1-07 S4: the direction record the pre-read and the neighbour prefetch
    // share, and the two prefetch levers.
    out << "nav_direction=" << g_nav_direction << '\n';
    out << "nav_dir_streak=" << g_nav_dir_streak << '\n';
    out << "prefetch_depth=" << g_prefetch_max_depth << '\n';
    out << "prefetch_raw=" << (g_prefetch_raw_neighbours ? 1 : 0) << '\n';
    // V3.1-02: a placeholder frame is on screen (and where it came from).
    out << "transition_shown=" << (g_state->transition_active ? 1 : 0) << '\n';
    // V3.1-07 S3: which rule was active (auto = every load, sharp = the 02
    // rollback), so an A/B run can prove which behaviour produced the video.
    out << "transition_mode=" << TransitionModeName(g_transition_mode) << '\n';
    out << "transition_edge=" << g_state->transition_edge << '\n';
    out << "transition_source=" << g_state->transition_source << '\n';
    {
      char line[64]{};
      std::snprintf(line, sizeof(line), "transition_frame_ms=%.0f",
                    g_state->transition_ms);
      out << line << '\n';
    }
    out << "transition_count=" << g_transition_count << '\n';
    // V3.1-04a: how the RAW frame was produced and the preview edge when the
    // camera-embedded JPEG is the final frame.
    out << "raw_mode=" << g_state->raw_mode << '\n';
    out << "preview_edge=" << g_state->preview_edge << '\n';
    // V3.1-07 S2: the `--preview-cache=off` rollback state, next to the RAW
    // evidence it gates.
    out << "preview_cache=" << (g_preview_cache ? 1 : 0) << '\n';
  }
  out << "strip_mode=" << Narrow(nv::ui::StripModeName(g_strip_mode)) << '\n';
  out << "strip_counts=" << (layout.strip_counts_in_content ? 1 : 0) << '\n';
  out << "strip_shown=" << (g_ui_inputs.strip_shown ? 1 : 0) << '\n';
  {
    char opacity[48]{};
    std::snprintf(opacity, sizeof(opacity), "strip_opacity=%.3f",
                  static_cast<double>(EffectiveStripOpacity()));
    out << opacity << '\n';
  }
  out << "strip_interactive=" << (layout.strip_interactive ? 1 : 0) << '\n';
  out << "strip_hover=" << g_strip_hover_index << '\n';
  out << "strip_tooltip=" << Narrow(g_ui_inputs.strip_tooltip) << '\n';
  out << "strip_tooltip_above="
      << ((g_ui->strip_tooltip().visible && g_ui->strip_tooltip().above) ? 1 : 0)
      << '\n';
  out << "birdseye=" << (g_settings.birdseye ? 1 : 0) << '\n';
  // v3.0 (規劃/26 §2.4): bird's eye view. `minimap=1` only when the panel is
  // actually placed (setting on, picture on screen, window large enough).
  out << "minimap=" << (layout.minimap.visible ? 1 : 0) << '\n';
  if (layout.minimap.visible) {
    out << rect_line("minimap_panel", layout.minimap.panel);
    out << rect_line("minimap_canvas", layout.minimap.canvas);
    out << rect_line("minimap_image", layout.minimap.image);
    out << rect_line("minimap_viewport", layout.minimap.viewport);
    out << rect_line("minimap_view_display", layout.minimap.view_display);
    char minimap_scale[48]{};
    std::snprintf(minimap_scale, sizeof(minimap_scale),
                  "minimap_scale=%.4f",
                  static_cast<double>(layout.minimap.display_per_canvas));
    out << minimap_scale << '\n';
    out << "minimap_photo="
        << ((g_state != nullptr && g_state->renderer.minimap_photo() != nullptr)
                ? 1
                : 0)
        << '\n';
    out << "minimap_baked="
        << ((g_state != nullptr && g_state->renderer.minimap_ready()) ? 1 : 0)
        << '\n';
  }
  out << "topmost=" << (g_settings.topmost ? 1 : 0) << '\n';
  out << "rotate_available=" << (g_ui_inputs.rotate_available ? 1 : 0) << '\n';
  // Animation frame pill (規劃/22/12 §7).
  out << "anim_visible=" << (g_ui->anim_pill().visible ? 1 : 0) << '\n';
  out << "anim_buttons=" << (g_ui->anim_pill().interactive ? 1 : 0) << '\n';
  out << "anim_state="
      << (g_state != nullptr ? nv::AnimStateName(g_state->anim.state())
                             : "stopped")
      << '\n';
  out << "anim_frame=" << (g_state != nullptr ? g_state->anim.index() + 1 : 0)
      << '\n';
  out << "anim_frames="
      << (g_state != nullptr && g_state->current != nullptr
              ? g_state->current->frames.size()
              : 0)
      << '\n';
  out << "anim_ended=" << (g_state != nullptr && g_state->anim.ended() ? 1 : 0)
      << '\n';
  out << "anim_truncated="
      << (g_state != nullptr && g_state->current != nullptr &&
                  (g_state->current->info.frames_truncated ||
                   g_force_anim == ForceAnimState::kTruncated)
              ? 1
              : 0)
      << '\n';
  out << "anim_first_frame_only="
      << (g_ui->anim_pill().visible && !g_ui->anim_pill().interactive ? 1 : 0)
      << '\n';
  {
    char opacity[48]{};
    std::snprintf(opacity, sizeof(opacity), "anim_opacity=%.3f",
                  static_cast<double>(g_anim_pill_opacity));
    out << opacity << '\n';
  }
  out << "anim_label=" << Narrow(g_ui->anim_strings().label) << '\n';
  // Two-stage loading (規劃/22/13 §7): a check can assert the preview state and
  // the badge while the full decode is still running, and that the replacement
  // kept the view.
  out << "preview_active=" << (PreviewActive() ? 1 : 0) << '\n';
  out << "preview_failed="
      << (g_state != nullptr && g_state->preview_failed ? 1 : 0) << '\n';
  out << "preview_source="
      << (g_state != nullptr ? g_state->preview_source : std::string()) << '\n';
  out << "preview_target="
      << (g_state != nullptr ? g_state->preview_target_width : 0) << 'x'
      << (g_state != nullptr ? g_state->preview_target_height : 0) << '\n';
  {
    char shown[64]{};
    std::snprintf(shown, sizeof(shown), "preview_ms=%.0f", g_preview_shown_ms);
    out << shown << '\n';
  }
  out << "preview_badge_visible="
      << (g_ui->preview_badge().visible ? 1 : 0) << '\n';
  {
    char opacity[48]{};
    std::snprintf(opacity, sizeof(opacity), "preview_opacity=%.3f",
                  static_cast<double>(g_preview_badge_opacity));
    out << opacity << '\n';
  }
  out << "preview_text=" << Narrow(g_ui->preview_strings().text) << '\n';
  if (g_state != nullptr) {
    float cx = 0.0f;
    float cy = 0.0f;
    g_state->renderer.ViewCenter(&cx, &cy);
    const D2D1_RECT_F content = g_state->renderer.ContentRect();
    char center[96]{};
    std::snprintf(center, sizeof(center), "view_center=%.1f,%.1f",
                  static_cast<double>(cx), static_cast<double>(cy));
    out << center << '\n';
    std::snprintf(center, sizeof(center), "content_center=%.1f,%.1f",
                  static_cast<double>((content.left + content.right) * 0.5f),
                  static_cast<double>((content.top + content.bottom) * 0.5f));
    out << center << '\n';
    out << "image_width=" << TargetImageWidth() << '\n';
    out << "image_height=" << TargetImageHeight() << '\n';
  }
  if (g_state != nullptr) {
    out << "rotation=" << g_state->renderer.rotation() << '\n';
    out << "rotation_degrees="
        << nv::render::RotationDegrees(g_state->renderer.rotation()) << '\n';
    out << "display_width=" << g_state->renderer.display_width() << '\n';
    out << "display_height=" << g_state->renderer.display_height() << '\n';
  }
  out << "empty=" << (g_ui->empty().visible ? 1 : 0) << '\n';
  out << "empty_kind=" << Narrow(nv::ui::EmptyKindName(VisibleEmptyKind()))
      << '\n';
  out << "empty_lines=" << g_ui->empty().format_lines << '\n';
  out << "empty_compact=" << (g_ui->empty().compact ? 1 : 0) << '\n';
  out << "empty_focus=" << g_ui->empty_focus() << '\n';
  out << "drag_over=" << (g_drag_over ? 1 : 0) << '\n';
  // v3.0 (規劃/26 §2.10): lets the strip tests assert that a held drag is still
  // running when the pointer reaches the bottom bar (and no reveal happened).
  out << "dragging=" << ((g_state != nullptr && g_state->dragging) ? 1 : 0)
      << '\n';
  // V3.1-06 (§06b): which source started the last drag and whether the pan was
  // recentred first. `drag_press=1` means a candidate is still held.
  out << "drag_source=" << g_last_drag_source << '\n';
  out << "pan_after=" << (g_last_drag_recentered ? "center" : "keep") << '\n';
  out << "drag_press=" << (g_drag_press.active ? 1 : 0) << '\n';
  // V3.1-06 (§06e): the rename overlay's state and its geometry (the EDIT is a
  // top-level window, so the card rectangles are what tests compare).
  out << "rename_open="
      << ((g_ui != nullptr &&
           g_ui->overlay().IsModal(nv::ui::OverlayKind::kRename))
              ? 1
              : 0)
      << '\n';
  if (g_ui != nullptr &&
      g_ui->overlay().IsModal(nv::ui::OverlayKind::kRename) &&
      g_ui->rename_card().visible) {
    const nv::ui::RenameCardLayout& rename = g_ui->rename_card();
    out << rect_line("rename_card", rename.card);
    out << rect_line("rename_input", rename.input);
    out << rect_line("rename_ok", rename.primary);
    out << rect_line("rename_cancel", rename.secondary);
  }
  out << "rename_error=" << Narrow(g_rename_error) << '\n';
  {
    std::wstring text;
    if (g_rename_edit != nullptr) {
      wchar_t buffer[512]{};
      GetWindowTextW(g_rename_edit, buffer, static_cast<int>(std::size(buffer)));
      text = buffer;
    }
    out << "rename_text=" << Narrow(text) << '\n';
  }
  // V3.1-06 (§06d): the newest toast line, so a check can prove that a copy
  // command produced a visible confirmation after the menu closed. `back()` is
  // the newest entry (the drawer walks the deque from the back).
  {
    std::wstring toast;
    if (g_ui->overlay().toast_visible() && !g_ui->overlay().toasts().empty()) {
      toast = g_ui->overlay().toasts().back().text;
    }
    out << "toast_text=" << Narrow(toast) << '\n';
    out << "toast_visible=" << (g_ui->overlay().toast_visible() ? 1 : 0) << '\n';
  }
  out << "loading=" << (g_pending_path.empty() ? 0 : 1) << '\n';
  out << "formats_open="
      << (g_ui->overlay().IsModal(nv::ui::OverlayKind::kFormats) ? 1 : 0)
      << '\n';
  out << "formats_rows=" << g_ui->format_panel().rows.size() << '\n';
  out << "format_families=" << g_ui->format_strings().rows.size() << '\n';
  out << "format_count="
      << (g_format_summary != nullptr ? g_format_summary->extension_count : 0)
      << '\n';
  if (g_format_summary != nullptr) {
    std::wstring highlights;
    for (const std::wstring& name : g_format_summary->highlights) {
      if (!highlights.empty()) highlights += L"/";
      highlights += name;
    }
    out << "format_highlights=" << Narrow(highlights) << '\n';
  }
  // v3.0 (規劃/26 §2.1): the info card is a separate top-level window. The keys
  // keep their v2.0 names so the existing checks (navigation/rotation/…, which
  // only assert "the card is visible" and the row count) stay meaningful; the
  // geometry keys below are now the *info window's* client rectangle.
  const nv::ui::InfoPanelStrings info_lines = BuildInfoStrings();
  const nv::ui::InfoPanelLayout* info_layout =
      InfoWindowVisible() ? &g_info_window->layout() : nullptr;
  out << "info_open=" << (InfoWindowVisible() ? 1 : 0) << '\n';
  out << "info_rows=" << info_lines.lines.size() << '\n';
  out << "info_exif_ok=" << (g_exif_data.ok ? 1 : 0) << '\n';
  out << "info_scroll=" << (info_layout ? info_layout->scroll : 0.0f) << '\n';
  out << "info_scroll_max=" << (info_layout ? info_layout->max_scroll : 0.0f)
      << '\n';
  out << "info_scrollable="
      << ((info_layout && info_layout->scrollable) ? 1 : 0) << '\n';
  out << "info_first_visible=" << (info_layout ? info_layout->first_visible : 0)
      << '\n';
  out << "info_last_visible=" << (info_layout ? info_layout->last_visible : -1)
      << '\n';
  out << "info_window_saved=" << (g_settings.info_window_saved ? 1 : 0) << '\n';
  // Whether the window is actually on screen right now (minimised/fullscreen
  // temporarily hide it without destroying it).
  out << "info_window_shown=" << (InfoWindowVisible() && g_info_shown ? 1 : 0)
      << '\n';
  out << "info_window_visible_setting="
      << (g_settings.info_window_visible ? 1 : 0) << '\n';
  out << "info_window_dpi="
      << (InfoWindowVisible()
              ? g_info_window->layout().dpi_scale
              : nv::platform::DpiScaleForWindow(hwnd))
      << '\n';
  if (InfoWindowVisible()) {
    const RECT rect = g_info_window->WindowRect();
    out << "info_window_rect=" << rect.left << ',' << rect.top << ','
        << rect.right << ',' << rect.bottom << '\n';
    RECT info_client{};
    GetClientRect(g_info_window->hwnd(), &info_client);
    out << "info_window_client=" << info_client.right - info_client.left << 'x'
        << info_client.bottom - info_client.top << '\n';
  } else {
    out << "info_window_rect=0,0,0,0\ninfo_window_client=0x0\n";
  }
  // Every line, in draw order: "h:Group" for a header, "Label|Value" for a row.
  for (size_t i = 0; i < info_lines.lines.size(); ++i) {
    const nv::ui::InfoLine& line = info_lines.lines[i];
    out << "info_line_" << i << '=' << (line.header ? "h:" : "")
        << Narrow(line.header ? line.label : line.label + L"|" + line.value)
        << '\n';
  }
  // Named values the check script asserts without parsing the line list.
  const auto info_value = [&out, &info_lines](const char* name,
                                             nv::i18n::StringId id) {
    for (const nv::ui::InfoLine& line : info_lines.lines) {
      if (!line.header &&
          line.label == nv::i18n::Text(id)) {
        out << "info_" << name << '=' << Narrow(line.value) << '\n';
        return;
      }
    }
    out << "info_" << name << "=\n";
  };
  info_value("name", nv::i18n::StringId::kInfoName);
  info_value("path", nv::i18n::StringId::kInfoPath);
  info_value("size", nv::i18n::StringId::kInfoFileSize);
  info_value("format", nv::i18n::StringId::kInfoFormat);
  info_value("dimension", nv::i18n::StringId::kInfoDimensions);
  info_value("depth", nv::i18n::StringId::kInfoDepth);
  info_value("frames", nv::i18n::StringId::kInfoFrames);
  info_value("camera", nv::i18n::StringId::kInfoCamera);
  info_value("lens", nv::i18n::StringId::kInfoLens);
  info_value("taken", nv::i18n::StringId::kInfoTaken);
  info_value("exposure", nv::i18n::StringId::kInfoExposure);
  info_value("aperture", nv::i18n::StringId::kInfoAperture);
  info_value("iso", nv::i18n::StringId::kInfoIso);
  info_value("focal", nv::i18n::StringId::kInfoFocal);
  info_value("bias", nv::i18n::StringId::kInfoBias);
  info_value("flash", nv::i18n::StringId::kInfoFlash);
  info_value("metering", nv::i18n::StringId::kInfoMetering);
  info_value("whitebalance", nv::i18n::StringId::kInfoWhiteBalance);
  info_value("gps", nv::i18n::StringId::kInfoGps);
  info_value("icc", nv::i18n::StringId::kInfoIcc);
  // Error / confirmation / loading state (規劃/22/11 §10).
  out << "error_open="
      << ((g_ui->overlay().error_open() && g_ui->error_card().visible) ? 1 : 0)
      << '\n';
  out << "error_kind="
      << (g_ui->overlay().error_open()
              ? nv::ErrorKindName(g_error.kind)
              : "none")
      << '\n';
  out << "error_buttons=" << g_ui->error_card().buttons.size() << '\n';
  out << "error_default=" << g_ui_inputs.error.default_action << '\n';
  out << "confirm_open="
      << ((g_ui->overlay().IsModal(nv::ui::OverlayKind::kConfirm) &&
           g_ui->confirm_card().visible)
              ? 1
              : 0)
      << '\n';
  out << "confirm_kind="
      << (g_confirm == ConfirmKind::kDelete ? "delete"
          : g_confirm == ConfirmKind::kWrapNext ? "wrap_next"
          : g_confirm == ConfirmKind::kWrapPrev ? "wrap_prev"
                                                : "none")
      << '\n';
  out << "confirm_remember=" << (g_wrap_remember ? 1 : 0) << '\n';
  out << "confirm_target=" << Narrow(g_wrap_target.filename().wstring())
      << '\n';
  // V3.1-06 (§06c): the old save-options card is gone, so its dump fields
  // (save_options/save_format/save_quality/save_has_quality/save_focus/
  // save_estimate*/save_target and the save_* rects) were removed together with
  // the card. `save_busy` stays: it tracks the async encoder that still exists.
  out << "save_busy=" << (g_save_busy ? 1 : 0) << '\n';
  out << "loading_shown=" << (g_ui->loading().visible ? 1 : 0) << '\n';
  out << "loading_state=" << g_loading_state << '\n';
  out << "toast_count=" << g_ui->overlay().toasts().size() << '\n';
  out << "popup=" << Narrow(nv::ui::OverlayKindName(g_ui->overlay().popup()))
      << '\n';
  // v3.0 (規劃/26 §2.8): the hamburger opens the settings window, so the dump
  // has to expose that window's state — a popup stays "none" in that path.
  out << "settings_open=" << (g_settings_window_open ? 1 : 0) << '\n';
  out << "menu_rows=" << g_ui->menu_items().size() << '\n';
  out << "menu_above=" << (g_ui->menu().above ? 1 : 0) << '\n';
  // v3.0 (規劃/26 §2.9): the row list and the keyboard highlight, so the
  // right-click checks can assert order/labels and Up/Down without OCR.
  std::string menu_labels;
  for (const nv::ui::MenuItem& item : g_ui->menu_items()) {
    if (!menu_labels.empty()) menu_labels += '|';
    menu_labels +=
        item.kind == nv::ui::MenuItemKind::kSeparator ? "-" : Narrow(item.label);
  }
  out << "menu_labels=" << menu_labels << '\n';
  out << "menu_hover=" << g_ui->menu_hover() << '\n';
  out << "folder_files=" << (g_state ? g_state->folder.size() : 0) << '\n';
  out << "folder_index=" << (g_state ? g_state->folder.index : 0) << '\n';
  out << "folder_path="
      << Narrow(g_state ? g_state->folder.folder.wstring() : std::wstring())
      << '\n';
  out << "file_name="
      << Narrow(g_state ? g_state->path.filename().wstring() : std::wstring())
      << '\n';
  out << "folder_wrap=" << Narrow(g_settings.folder_wrap) << '\n';
  out << "watch_interval=" << g_watch_interval_ms << '\n';
  // V3-05: which source drove the last snapshot, and the notify merge window
  // (0 while polling). tests/run_watch_check.ps1 asserts both.
  out << "watch_mode=" << (g_watch_uses_notify ? "notify" : "poll") << '\n';
  out << "watch_merge_ms="
      << (g_watch_uses_notify ? nv::platform::DirNotifier::kMergeMs : 0) << '\n';
  out << "client=" << client.right - client.left << 'x'
      << client.bottom - client.top << '\n';
  out << "window=" << window.right - window.left << 'x'
      << window.bottom - window.top << '\n';
  out << "window_rect=" << window.left << ',' << window.top << ','
      << window.right << ',' << window.bottom << '\n';
  {
    std::string areas;
    for (const nv::platform::IntRect& area : MonitorWorkAreas()) {
      if (!areas.empty()) areas += ';';
      areas += std::to_string(area.left) + "," + std::to_string(area.top) +
               "," + std::to_string(area.right) + "," +
               std::to_string(area.bottom);
    }
    out << "work_areas=" << areas << '\n';
  }
  out << rect_line("title", layout.title);
  out << rect_line("content", layout.content);
  out << rect_line("bottom", layout.bottom_bar);
  // v3.0 (規劃/26 §2.7): present only in fullscreen; zero rectangles otherwise.
  out << rect_line("fs_exit", layout.fs_exit);
  out << rect_line("fs_close", layout.fs_close);
  out << rect_line("strip", layout.strip);
  out << rect_line("strip_tooltip_panel", g_ui->strip_tooltip().panel);
  out << rect_line("info", info_layout ? info_layout->panel : nv::ui::Rect{});
  out << rect_line("info_header",
                   info_layout ? info_layout->header : nv::ui::Rect{});
  out << rect_line("info_close",
                   info_layout ? info_layout->close : nv::ui::Rect{});
  out << rect_line("info_body",
                   info_layout ? info_layout->body : nv::ui::Rect{});
  out << rect_line("info_footer",
                   info_layout ? info_layout->footer : nv::ui::Rect{});
  out << rect_line("info_copy_all",
                   info_layout ? info_layout->copy_all : nv::ui::Rect{});
  out << rect_line("arrows_left", layout.arrows_left);
  out << rect_line("arrows_right", layout.arrows_right);
  out << rect_line("anim_pill", g_ui->anim_pill().pill);
  out << rect_line("anim_prev", g_ui->anim_pill().prev);
  out << rect_line("anim_label_rect", g_ui->anim_pill().label);
  out << rect_line("anim_next", g_ui->anim_pill().next);
  out << rect_line("anim_toggle", g_ui->anim_pill().toggle);
  out << rect_line("preview_badge", g_ui->preview_badge().badge);
  out << rect_line("menu", g_ui->menu().panel);
  out << rect_line("empty_card", g_ui->empty().card);
  out << rect_line("empty_title", g_ui->empty().title);
  out << rect_line("empty_formats", g_ui->empty().formats);
  out << rect_line("empty_primary", g_ui->empty().primary);
  out << rect_line("empty_open_folder", g_ui->empty().open_folder);
  out << rect_line("empty_all_formats", g_ui->empty().all_formats);
  out << rect_line("empty_note", g_ui->empty().note);
  out << rect_line("drag_hint", layout.drag_hint);
  out << rect_line("formats_panel", g_ui->format_panel().panel);
  out << rect_line("formats_close", g_ui->format_panel().close);
  out << rect_line("error_card", g_ui->error_card().card);
  out << rect_line("error_title", g_ui->error_card().title);
  out << rect_line("error_message", g_ui->error_card().message);
  out << rect_line("error_detail", g_ui->error_card().detail);
  out << rect_line("error_close", g_ui->error_card().close);
  for (size_t i = 0; i < g_ui->error_card().buttons.size(); ++i) {
    char name[32]{};
    std::snprintf(name, sizeof(name), "error_button_%zu", i);
    out << rect_line(name, g_ui->error_card().buttons[i]);
  }
  out << rect_line("confirm_card", g_ui->confirm_card().card);
  out << rect_line("confirm_primary", g_ui->confirm_card().primary);
  out << rect_line("confirm_secondary", g_ui->confirm_card().secondary);
  out << rect_line("confirm_remember_row", g_ui->confirm_card().remember);
  out << rect_line("confirm_remember_box", g_ui->confirm_card().remember_box);
  out << rect_line("loading_chip", g_ui->loading().chip);
  out << rect_line("loading_bar", g_ui->loading().bar);
  out << rect_line("loading_label", g_ui->loading().label);
  for (size_t i = 0; i < g_ui->toast_rects().size(); ++i) {
    char name[32]{};
    std::snprintf(name, sizeof(name), "toast_%zu", i);
    out << rect_line(name, g_ui->toast_rects()[i]);
  }
  for (size_t i = 0; i < g_ui->format_panel().rows.size(); ++i) {
    char name[32]{};
    std::snprintf(name, sizeof(name), "formats_row_%zu", i);
    out << rect_line(name, g_ui->format_panel().rows[i]);
  }
  if (!g_ui->menu().rows.empty()) {
    for (size_t i = 0; i < g_ui->menu().rows.size(); ++i) {
      char name[32]{};
      std::snprintf(name, sizeof(name), "menu_row_%zu", i);
      out << rect_line(name, g_ui->menu().rows[i]);
    }
  }
  std::string buttons;
  for (size_t i = 0; i < nv::ui::kTitleButtonCount; ++i) {
    char buffer[64]{};
    std::snprintf(buffer, sizeof(buffer), "%s%.0f,%.0f,%.0f,%.0f", i ? "|" : "",
                  title.buttons[i].left, title.buttons[i].top,
                  title.buttons[i].right, title.buttons[i].bottom);
    buttons += buffer;
  }
  out << "buttons=" << buttons << '\n';
  std::string segments;
  for (size_t i = 0; i < nv::ui::kSegCount; ++i) {
    segments += title.segment_visible[i] ? '1' : '0';
  }
  out << "segments=" << segments << '\n';
  out.close();
  nv::platform::LogInfo("ui: layout_dump=" +
                        Narrow(g_layout_dump_path.wstring()));
  if (destroy) {
    // The dump exits the process without WM_CLOSE; the card still belongs to the
    // app shutdown path, so its "was open" intent must survive.
    CloseInfoWindowForAppClose();
    DestroyWindow(hwnd);
  }
}

void ApplyDpiScale(HWND hwnd) {
  if (!g_state) return;
  const float scale = nv::platform::DpiScaleForWindow(hwnd);
  g_state->strip.SetDpiScale(scale);
  if (g_ui) g_ui->SetDpiScale(scale);
  if (g_info_window) g_info_window->SetDpiScale(scale);
  RefreshUi(hwnd, false);
  wchar_t line[128]{};
  swprintf_s(line, L"ui: dpi_scale=%.2f strip_height=%.0f", scale,
             g_state->strip.height());
  nv::platform::LogInfo(Narrow(line));
}

void ApplyThemeState(HWND hwnd) {
  if (!g_state) return;
  const bool high_contrast = nv::platform::IsHighContrastEnabled();
  g_state->renderer.SetHighContrast(high_contrast);
  if (g_ui) g_ui->SetTheme(g_settings.dark_theme, high_contrast);

  // Create the replacement brush first, then swap the class brush, then
  // release the old one so WM_ERASEBKGND never sees a deleted HBRUSH.
  HBRUSH brush = CreateSolidBrush(g_state->renderer.background_colorref());
  if (brush != nullptr) {
    if (hwnd != nullptr) {
      SetClassLongPtrW(hwnd, GCLP_HBRBACKGROUND,
                       reinterpret_cast<LONG_PTR>(brush));
      if (g_background_brush != nullptr) DeleteObject(g_background_brush);
      g_background_brush = brush;
    } else {
      if (g_background_brush != nullptr) DeleteObject(g_background_brush);
      g_background_brush = brush;
    }
  }

  const COLORREF background = g_state->renderer.background_colorref();
  wchar_t line[128]{};
  swprintf_s(line, L"ui: high_contrast=%d background=%u,%u,%u",
             high_contrast ? 1 : 0, GetRValue(background),
             GetGValue(background), GetBValue(background));
  nv::platform::LogInfo(Narrow(line));
  if (hwnd != nullptr) InvalidateRect(hwnd, nullptr, TRUE);
}

void ApplyLanguage(HWND hwnd, nv::i18n::Language language, bool save) {
  nv::i18n::SetLanguage(language);
  // A session override (--language) must not rewrite the configured language:
  // only an explicit menu/settings choice persists the code.
  if (save) {
    g_settings.language = nv::i18n::LanguageCode(language);
    nv::platform::SaveSettings(g_settings);
  }
  nv::platform::LogInfo(std::string("ui: language=") +
                        Narrow(nv::i18n::LanguageCode(language)));
  if (g_settings_window) g_settings_window->Invalidate();
  UpdateInfoWindowContent(hwnd);
  if (hwnd != nullptr) {
    // Every chrome/empty-state label is rebuilt from the table, so the cached
    // layout must be recomputed as well as repainted.
    RefreshUi(hwnd, false);
    UpdateTitle(hwnd);
    InvalidateRect(hwnd, nullptr, TRUE);
  }
}

// --- v2.0 S2: settings application (規劃/22/07 §3.3) -----------------------

bool SaveSettingsNow() {
  const bool ok = nv::platform::SaveSettings(g_settings);
  if (!ok) {
    nv::platform::LogError("settings: save failed");
    if (g_settings_window && g_settings_window->IsOpen()) {
      g_settings_window->SetFooterStatus(
          nv::i18n::Text(nv::i18n::StringId::kSettingsSaveFailed));
    }
  }
  return ok;
}

nv::i18n::Language LanguageFromSettings() {
  nv::i18n::Language language = nv::i18n::SystemLanguage();
  if (!g_settings.language.empty() &&
      !nv::i18n::ParseLanguage(g_settings.language, &language)) {
    language = nv::i18n::SystemLanguage();
  }
  return language;
}

void ApplyThemeFromSettings(HWND hwnd) {
  g_settings.dark_theme = nv::platform::ResolveDarkTheme(g_settings.theme);
  if (g_state) g_state->renderer.SetTheme(g_settings.dark_theme);
  if (g_ui) {
    g_ui->SetTheme(g_settings.dark_theme,
                   nv::platform::IsHighContrastEnabled());
  }
  ApplyThemeState(hwnd);
  if (g_settings_window) g_settings_window->ApplyTheme();
  if (g_info_window) g_info_window->ApplyTheme(g_settings.dark_theme);
}

void ApplyFullscreenSettings() {
  if (!g_state) return;
  int mode = 0;
  if (g_settings.fullscreen_bg == L"black") {
    mode = 1;
  } else if (g_settings.fullscreen_bg == L"opacity" || g_settings.fullscreen_bg == L"dim") {
    mode = 2;
  }
  g_state->renderer.SetFullscreenBackground(mode);
  g_state->renderer.SetFullscreen(g_window_state.fullscreen());
}

void ApplyAllSettings(HWND hwnd) {
  if (g_state) {
    ApplyThemeFromSettings(hwnd);
    g_state->renderer.SetCheckerboard(g_settings.checkerboard);
    g_state->renderer.SetCheckerOpacity(g_settings.checker_opacity);
    g_state->renderer.SetPixelMode(g_settings.pixel_mode);
    ApplyFullscreenSettings();
    if (!g_strip_override) {
      nv::ui::StripMode saved = nv::ui::StripMode::kAuto;
      if (nv::ui::ParseStripMode(g_settings.thumbstrip_mode, &saved)) {
        g_strip_mode = saved;
      }
    }
    // Bring the fade state in line with the (possibly new) mode: pinned is
    // opaque immediately, auto starts hidden.
    ResetStripVisualState(hwnd);
    ApplyTopmost(hwnd, g_settings.topmost);
    ApplyLanguage(hwnd, LanguageFromSettings(), false);
  }
  SaveSettingsNow();
  if (hwnd != nullptr) {
    RefreshUi(hwnd, true);
    InvalidateRect(hwnd, nullptr, TRUE);
  }
  if (g_settings_window) g_settings_window->Invalidate();
}

// One entry point for a settings-window edit: apply to the live UI, then
// persist. `key` is a schema name, or "*" after "restore defaults".
void ApplySettingsKey(HWND hwnd, const std::wstring& key) {
  if (!g_state) return;
  // Slider drags preview live but only persist when the drag ends (規劃/22/07
  // 坑 6: no per-pixel disk writes).
  const bool live = key == L"checker_opacity_live";
  if (key == L"theme") {
    ApplyThemeFromSettings(hwnd);
  } else if (key == L"checker") {
    g_state->renderer.SetCheckerboard(g_settings.checkerboard);
  } else if (key == L"checker_opacity" || live) {
    g_state->renderer.SetCheckerOpacity(g_settings.checker_opacity);
  } else if (key == L"pixel_mode") {
    g_state->renderer.SetPixelMode(g_settings.pixel_mode);
  } else if (key == L"strip_mode") {
    nv::ui::StripMode saved = nv::ui::StripMode::kAuto;
    if (!g_strip_override &&
        nv::ui::ParseStripMode(g_settings.thumbstrip_mode, &saved)) {
      g_strip_mode = saved;
    }
    ResetStripVisualState(hwnd);
    nv::platform::LogInfo("ui: strip_mode=" +
                          Narrow(nv::ui::StripModeName(g_strip_mode)));
  } else if (key == L"topmost") {
    ApplyTopmost(hwnd, g_settings.topmost);
  } else if (key == L"language") {
    ApplyLanguage(hwnd, LanguageFromSettings(), false);
  } else if (key == L"fullscreen_bg") {
    ApplyFullscreenSettings();
  } else if (key == L"*") {
    ApplyAllSettings(hwnd);
    return;
  } else if (key == L"birdseye") {
    // v3.0 (規劃/26 §2.4): nothing extra here - the shared tail below rebuilds
    // the layout, which is what places or removes the bird's eye panel; the
    // renderer switches its derived photo on in BuildUiInputs.
  } else if (key == L"remember_window" || key == L"single_instance" ||
             key == L"wheel" || key == L"folder_wrap" ||
             key == L"keep_rotation" || key == L"zoom_step") {
    // Applied on the next relevant input/launch; nothing to repaint.
  }

  SaveSettingsNow();
  if (!live) {
    nv::platform::LogInfo("settings: applied=" + Narrow(key));
    UpdateTitle(hwnd);
    RefreshUi(hwnd, true);
    InvalidateRect(hwnd, nullptr, TRUE);
    if (g_settings_window) g_settings_window->Invalidate();
  }
}

// --- Window placement persistence (規劃/22/15 §3) ---------------------------
// Only the normal (restored) rectangle is stored; GetWindowPlacement keeps it
// while maximized so toggling that does not lose the position. Fullscreen and
// minimized windows are never written: their rectangle is the monitor or the
// -32000 iconic coordinate (規劃/22/15 坑 1/2/3).
void SaveWindowPlacement(HWND hwnd) {
  if (hwnd == nullptr) return;
  if (!nv::platform::ShouldSavePlacement(g_settings.remember_window,
                                         g_window_state.fullscreen(),
                                         IsIconic(hwnd) != FALSE)) {
    return;
  }
  WINDOWPLACEMENT placement{};
  placement.length = sizeof(placement);
  if (!GetWindowPlacement(hwnd, &placement)) return;
  if (placement.showCmd == SW_SHOWMINIMIZED) return;
  const RECT& rect = placement.rcNormalPosition;
  const int width = rect.right - rect.left;
  const int height = rect.bottom - rect.top;
  if (width < 320 || height < 240) return;
  g_settings.window_x = rect.left;
  g_settings.window_y = rect.top;
  g_settings.window_w = width;
  g_settings.window_h = height;
  g_settings.window_maximized = placement.showCmd == SW_SHOWMAXIMIZED;
  g_settings.window_saved = true;
  SaveSettingsNow();
  nv::platform::LogInfo("settings: window=" + std::to_string(rect.left) + "," +
                        std::to_string(rect.top) + " " +
                        std::to_string(width) + "x" + std::to_string(height) +
                        (g_settings.window_maximized ? " maximized" : ""));
}

// Restores the saved rectangle only when at least 100x100 physical pixels of it
// still fall on a monitor work area; the rectangle is then clamped fully into
// that work area (unplugged screen, DPI change, taskbar moved: 規劃/22/15
// §3.2/坑 4/16). Returns false so the caller centres the default size.
bool RestoreWindowPlacement(HWND hwnd) {
  if (hwnd == nullptr || !g_settings.remember_window ||
      !g_settings.window_saved) {
    return false;
  }
  const nv::platform::IntRect candidate{
      g_settings.window_x, g_settings.window_y,
      g_settings.window_x + g_settings.window_w,
      g_settings.window_y + g_settings.window_h};
  const std::vector<nv::platform::IntRect> areas = MonitorWorkAreas();
  if (!nv::platform::PlacementVisible(candidate, areas)) {
    nv::platform::LogInfo("settings: window placement invalid; centering");
    return false;
  }
  const nv::platform::IntRect restored = nv::platform::ClampPlacement(
      candidate, areas,
      nv::platform::DpiPixels(nv::ui::metrics::kMinWindowWidth,
                              nv::platform::DpiScaleForWindow(hwnd)),
      nv::platform::DpiPixels(nv::ui::metrics::kMinWindowHeight,
                              nv::platform::DpiScaleForWindow(hwnd)));
  SetWindowPos(hwnd, nullptr, restored.left, restored.top,
               restored.right - restored.left, restored.bottom - restored.top,
               SWP_NOZORDER | SWP_NOACTIVATE);
  g_settings.window_x = restored.left;
  g_settings.window_y = restored.top;
  g_settings.window_w = restored.right - restored.left;
  g_settings.window_h = restored.bottom - restored.top;
  nv::platform::LogInfo(
      "settings: window restored " + std::to_string(restored.left) + "," +
      std::to_string(restored.top) + " " +
      std::to_string(restored.right - restored.left) + "x" +
      std::to_string(restored.bottom - restored.top));
  return true;
}

void OpenSettingsWindow(HWND hwnd) {  if (!g_settings_window) {
    g_settings_window = std::make_unique<nv::ui::SettingsWindow>();
  }
  if (g_settings_window->IsOpen()) {
    SetForegroundWindow(g_settings_window->hwnd());
    return;
  }
  if (!g_settings_window->Open(
          hwnd, &g_settings,
          [hwnd](const std::wstring& key) { ApplySettingsKey(hwnd, key); },
          [] { g_settings_window_open = false; })) {
    ShowErrorToast(hwnd, nv::i18n::Text(nv::i18n::StringId::kErrorOperationFailed));
    return;
  }
  g_settings_window_open = true;
  nv::platform::LogInfo("settings: opened");
  if (g_test_settings_page != nv::ui::SettingsPage::kGeneral) {
    g_settings_window->SetPage(g_test_settings_page);
  }
}

void InvalidateStrip(HWND hwnd) {
  if (!g_state) return;
  const nv::ui::Rect strip = StripRect();
  if (strip.empty()) return;
  RECT client{};
  GetClientRect(hwnd, &client);
  RECT area{static_cast<LONG>(std::lround(strip.left)),
            static_cast<LONG>(std::lround(strip.top)),
            static_cast<LONG>(std::lround(strip.right)),
            static_cast<LONG>(std::lround(strip.bottom))};
  InvalidateRect(hwnd, &area, FALSE);
}

size_t FindInFolder(const std::filesystem::path& path) {
  if (!g_state) return nv::render::ThumbnailStrip::kNone;
  const auto& files = g_state->folder.files;
  for (size_t i = 0; i < files.size(); ++i) {
    if (files[i] == path) return i;
  }
  return nv::render::ThumbnailStrip::kNone;
}

void RequestVisibleThumbnails();

void UpdateFolderUi(HWND hwnd, const std::filesystem::path& requested) {
  if (!g_state) return;
  auto& s = *g_state;
  s.strip.SetViewWidth(static_cast<float>(s.renderer.client_width()));

  size_t current = nv::render::ThumbnailStrip::kNone;
  if (!s.folder.empty() && s.folder.index < s.folder.files.size() &&
      s.folder.files[s.folder.index] == requested) {
    current = s.folder.index;
  }
  s.strip.SetCurrent(current);
  RefreshUi(hwnd, false);
  if (current != nv::render::ThumbnailStrip::kNone) {
    s.strip.EnsureVisible(current);
  }
  RequestVisibleThumbnails();
  (void)hwnd;
}

// V3.1-07 S4: `allow_raw` is only true from the rest timer (400 ms after the
// last load). A RAW neighbour costs 0.3-0.5 s under LibRaw's process-global lock
// and would block the embedded preview of the *next* key during a page-through,
// so it is planned only once the view has settled (see PrefetchAdjacent).
void PrefetchAdjacent(bool allow_raw = false);

// V3.1-01: burst bookkeeping (defined next to LoadPath, declared here because
// the WM_TIMER settle handler needs it).
void NoteNavigationBurst(HWND hwnd);
void OnBurstSettle(HWND hwnd);

// --- V3-04 display tier (規劃/25 §7.1–§7.5) --------------------------------
// `needed` is the long edge the next frame will occupy at fit, i.e. the tier
// the view actually needs; `scale` is the fit scale of the *new* image (a load
// always resets the view, so fit is the scale of the first frame).
struct TierRequest {
  uint32_t needed = 0;
  float scale = 0.0f;
  uint32_t source_long = 0;
};

TierRequest ComputeTierRequest(const nv::ProbeResult& probe);
uint32_t RequestedDisplayEdge(const std::filesystem::path& path,
                              const nv::ProbeResult& probe);
bool ShowDisplayTier(HWND hwnd, const std::filesystem::path& requested,
                     std::shared_ptr<nv::Image> image, uint32_t edge,
                     const char* source, bool transition = false);
// V3.1-02: shows a smaller frame that is already available while `requested`
// decodes. Returns true when a transition frame is on screen.
bool TryShowTransitionFrame(HWND hwnd, const std::filesystem::path& requested,
                            uint32_t requested_edge);
void SaveDecodedToDiskCache(const std::filesystem::path& path,
                            const std::shared_ptr<nv::Image>& image,
                            uint32_t edge);
void TraceCache(const std::filesystem::path& path, uint32_t edge,
                const char* source);

// Shows `path`. `refresh_list` rescans the folder (external open, F5); sibling
// navigation and thumbnail clicks reuse the existing list, which is faster and
// avoids invalidating references while navigating.
// --- v2.0 S2 / 22/12: animation playback and the frame pill ----------------

void StopAnimation(HWND hwnd);

// The pill shows for any playable sequence, plus the degraded "first frame
// only" state of a file that advertises animation but could not be decoded
// (規劃/22/12 §3.4). A static file never gets one (坑 15).
bool AnimPillVisible() {
  if (!g_state || !g_state->current) return false;
  const nv::Image& image = *g_state->current;
  return image.frames.size() > 1 || image.info.animated;
}

bool AnimHasControls() {
  if (g_force_anim == ForceAnimState::kFirstFrame) return false;
  return g_state && g_state->current && g_state->current->frames.size() > 1;
}

// Starts/stops the idle-fade timer so the pill is repainted only while it can
// actually change. Full opacity on (re)appearance, never a stale dim value.
void UpdateAnimPillTimer(HWND hwnd) {
  const bool visible = AnimPillVisible();
  if (visible && !g_anim_pill_timer_running) {
    // A pill that was dimmed keeps its idle clock: only a fresh appearance
    // (full opacity) resets the two-second countdown.
    if (g_anim_pill_opacity >= 0.999f) g_anim_pill_last_move_ms = NowMs();
    g_anim_pill_tick_ms = NowMs();
    SetTimer(hwnd, kAnimPillTimer, 40, nullptr);
    g_anim_pill_timer_running = true;
  } else if (!visible && g_anim_pill_timer_running) {
    KillTimer(hwnd, kAnimPillTimer);
    g_anim_pill_timer_running = false;
    g_anim_pill_opacity = 1.0f;
  }
}

void TickAnimPillFade(HWND hwnd) {
  if (!g_anim_pill_timer_running) return;
  const double now = NowMs();
  const double dt = std::max(0.0, now - g_anim_pill_tick_ms);
  g_anim_pill_tick_ms = now;
  const bool idle = now - g_anim_pill_last_move_ms >=
                    static_cast<double>(nv::ui::metrics::kAnimPillIdleMs);
  // High contrast keeps the pill steady and fully opaque (規劃/22/12 坑 16).
  const float target = (idle && !nv::platform::IsHighContrastEnabled())
                           ? nv::ui::metrics::kAnimPillRestOpacity
                           : 1.0f;
  const float step = static_cast<float>(dt / nv::ui::metrics::kAnimPillFadeMs);
  float next = g_anim_pill_opacity;
  if (next < target) {
    next = std::min(target, next + step);
  } else if (next > target) {
    next = std::max(target, next - step);
  }
  if (std::fabs(next - g_anim_pill_opacity) > 0.002f) {
    g_anim_pill_opacity = next;
    InvalidateRect(hwnd, nullptr, FALSE);
  }
  // Once fully dimmed there is nothing left to animate: stop the 40 ms tick so
  // a paused animation costs no periodic wakeups. The next mouse move starts
  // it again (規劃/22/12 §6 CPU budget).
  if (target < 1.0f && std::fabs(g_anim_pill_opacity - target) <= 0.002f) {
    KillTimer(hwnd, kAnimPillTimer);
    g_anim_pill_timer_running = false;
  }
}

void ShowAnimFrame(HWND hwnd, size_t index) {
  if (!g_state || !g_state->current) return;
  nv::Image& image = *g_state->current;
  if (index >= image.frames.size()) return;
  // reset_view=false keeps fit/zoom/pan and the view rotation: a frame update
  // must never move the picture (規劃/22/12 坑 2/18).
  if (!g_state->renderer.SetImage(image.info, image.frames[index].pixels, false)) {
    StopAnimation(hwnd);
    return;
  }
  g_state->anim.SetFrame(index);
  InvalidateRect(hwnd, nullptr, TRUE);
}

void StopAnimation(HWND hwnd) {
  if (!g_state) return;
  KillTimer(hwnd, kAnimationTimer);
  const bool was_playing = g_state->anim.playing();
  g_state->anim.Reset();
  // A new image starts with a fully visible, freshly timed pill.
  g_anim_pill_opacity = 1.0f;
  g_anim_pill_last_move_ms = NowMs();
  if (was_playing) {
    nv::platform::LogInfo("animation: stopped");
  }
  UpdateAnimPillTimer(hwnd);
}

void StartAnimation(HWND hwnd) {
  StopAnimation(hwnd);
  if (!g_state || !g_state->current) {
    UpdateAnimPillTimer(hwnd);
    return;
  }
  nv::Image& image = *g_state->current;
  if (image.frames.size() < 2) {
    // Animated source that could only produce one frame: keep the pill as a
    // "first frame only" label instead of pretending there is nothing to say.
    UpdateAnimPillTimer(hwnd);
    UpdateTitle(hwnd);
    RefreshUi(hwnd, false);
    return;
  }
  g_state->anim.Start(image.frames.size(), image.loop_count);
  SetTimer(hwnd, kAnimationTimer,
           std::max<UINT>(16, image.frames[0].delay_ms), nullptr);
  // --force-anim parks playback in a deterministic state (test hook).
  if (g_force_anim == ForceAnimState::kPaused ||
      g_force_anim == ForceAnimState::kFirstFrame ||
      g_force_anim == ForceAnimState::kTruncated) {
    g_state->anim.Pause();
    KillTimer(hwnd, kAnimationTimer);
  } else if (g_force_anim == ForceAnimState::kEnded) {
    g_state->anim.FinishAt(image.frames.size() - 1);
    KillTimer(hwnd, kAnimationTimer);
    g_state->renderer.SetImage(image.info,
                               image.frames[image.frames.size() - 1].pixels,
                               false);
    InvalidateRect(hwnd, nullptr, TRUE);
  }
  nv::platform::LogInfo("animation: started frames=" +
                        std::to_string(image.frames.size()) +
                        " loop=" + std::to_string(image.loop_count) +
                        " source=" + image.info.source);
  UpdateAnimPillTimer(hwnd);
  UpdateTitle(hwnd);
  RefreshUi(hwnd, false);
  InvalidateRect(hwnd, nullptr, FALSE);
}

void AdvanceAnimation(HWND hwnd) {
  if (!g_state || !g_state->current) {
    StopAnimation(hwnd);
    return;
  }
  nv::Image& image = *g_state->current;
  if (image.frames.size() < 2) {
    StopAnimation(hwnd);
    return;
  }

  const nv::AnimStep step =
      g_state->anim.Advance(image.frames.size(), image.loop_count);
  if (step.ended) {
    // Finite loop finished: stay on the last frame and turn the button into
    // Play (規劃/22/12 §3.2; 坑 5).
    KillTimer(hwnd, kAnimationTimer);
    nv::platform::LogInfo("animation: ended frame=" +
                          std::to_string(g_state->anim.index() + 1) + "/" +
                          std::to_string(image.frames.size()));
    UpdateTitle(hwnd);
    RefreshUi(hwnd, false);
    InvalidateRect(hwnd, nullptr, FALSE);
    return;
  }
  if (!step.show) {
    StopAnimation(hwnd);
    return;
  }
  if (!g_state->renderer.SetImage(image.info, image.frames[step.index].pixels,
                                  false)) {
    StopAnimation(hwnd);
    return;
  }
  // The pill counter must follow the frame; the layout is otherwise cached, so
  // without this the control would keep showing "1 / 4" while the picture
  // advances (規劃/22/12 坑 13).
  RefreshUi(hwnd, false);
  InvalidateRect(hwnd, nullptr, FALSE);
  const nv::ImageFrame& frame = image.frames[step.index];
  SetTimer(hwnd, kAnimationTimer, std::max<UINT>(16, frame.delay_ms), nullptr);
}

void ToggleAnimation(HWND hwnd) {
  if (!g_state || !g_state->current) return;
  nv::Image& image = *g_state->current;
  if (image.frames.size() < 2) return;
  const bool was_ended = g_state->anim.ended();
  const bool play = g_state->anim.Toggle(image.frames.size());
  if (play) {
    if (was_ended) {
      // Replay always starts at frame 0, not where the loop stopped (坑 6).
      g_state->renderer.SetImage(image.info, image.frames[0].pixels, false);
      InvalidateRect(hwnd, nullptr, TRUE);
    }
    const nv::ImageFrame& frame =
        image.frames[std::min(g_state->anim.index(), image.frames.size() - 1)];
    SetTimer(hwnd, kAnimationTimer, std::max<UINT>(16, frame.delay_ms),
             nullptr);
    nv::platform::LogInfo("animation: resumed frame=" +
                          std::to_string(g_state->anim.index() + 1));
  } else {
    KillTimer(hwnd, kAnimationTimer);
    nv::platform::LogInfo("animation: paused");
  }
  UpdateTitle(hwnd);
  RefreshUi(hwnd, false);
  InvalidateRect(hwnd, nullptr, FALSE);
}

// Frame stepping (`,` / `.`). Stepping always pauses first, so the visible
// frame cannot be advanced again by the timer while the user inspects it
// (規劃/22/12 §3.3).
void StepAnimationFrame(HWND hwnd, int delta) {
  if (!g_state || !g_state->current) return;
  nv::Image& image = *g_state->current;
  if (image.frames.size() < 2) return;
  const bool was_playing = g_state->anim.playing();
  const nv::AnimStep step = g_state->anim.Step(image.frames.size(), delta);
  if (was_playing) {
    KillTimer(hwnd, kAnimationTimer);
    nv::platform::LogInfo("animation: paused");
  }
  if (!step.show) return;
  if (!g_state->renderer.SetImage(image.info, image.frames[step.index].pixels,
                                  false)) {
    return;
  }
  nv::platform::LogInfo("animation: frame=" +
                        std::to_string(g_state->anim.index() + 1) + "/" +
                        std::to_string(image.frames.size()));
  UpdateTitle(hwnd);
  RefreshUi(hwnd, false);
  InvalidateRect(hwnd, nullptr, TRUE);
}

// --- v2.0 S3 / 22/13: two-stage loading -----------------------------------
// A slow format (RAW, PSD/PSB, >32 MB) shows a preview while the full frame
// decodes. The preview is display-only: it never enters the memory LRU, the
// disk cache or the prefetch path (規劃/22/13 §3.2 and 坑 6/12).
bool PreviewActive() { return g_state != nullptr && g_state->preview_active; }

// The title and the info panel report the size of the file, not of the preview
// (規劃/22/13 §3.2 and 坑 5).
uint32_t TargetImageWidth() {
  if (g_state == nullptr) return 0;
  if (g_state->preview_active && g_state->preview_target_width != 0) {
    return g_state->preview_target_width;
  }
  // V3-04: a display tier is smaller than the file; the title and the info
  // panel must keep reporting the real size (規劃/25 §7.4b).
  if (g_state->current_is_tier && g_state->current &&
      g_state->current->info.full_width != 0) {
    return g_state->current->info.full_width;
  }
  return g_state->renderer.image_width();
}

uint32_t TargetImageHeight() {
  if (g_state == nullptr) return 0;
  if (g_state->preview_active && g_state->preview_target_height != 0) {
    return g_state->preview_target_height;
  }
  if (g_state->current_is_tier && g_state->current &&
      g_state->current->info.full_height != 0) {
    return g_state->current->info.full_height;
  }
  return g_state->renderer.image_height();
}

void ResetPreviewState(HWND hwnd) {
  g_pending_preview.reset();
  if (hwnd != nullptr) KillTimer(hwnd, kPreviewBadgeTimer);
  g_preview_badge_opacity = 0.0f;
  g_preview_badge_tick_ms = 0.0;
  g_preview_shown_ms = 0.0;
  if (g_state == nullptr) return;
  auto& s = *g_state;
  s.preview_active = false;
  s.preview_failed = false;
  s.preview_path.clear();
  s.preview_target_width = 0;
  s.preview_target_height = 0;
  s.preview_source.clear();
}

// Uploads a preview and makes it the displayed frame. The target size comes
// from the decoder (full_*), so the title can show 6000x4000 while the preview
// is 1620x1080 (規劃/22/13 坑 5).
bool ApplyPreviewFrame(HWND hwnd, const PendingPreview& pending) {
  if (g_state == nullptr || pending.image == nullptr ||
      pending.image->Empty()) {
    return false;
  }
  auto& s = *g_state;
  // A fresh file starts in fit mode; the preview is the first frame of this
  // view, so reset_view is correct here. When a 128 px cached thumbnail is
  // upgraded to the camera-embedded JPEG (RAW), the current view is kept: the
  // user may already have zoomed into the first preview (規劃/22/13 §3.2).
  const bool upgrade = s.preview_active && !s.preview_failed &&
                       s.preview_source == "cache";
  const bool uploaded =
      upgrade ? s.renderer.SetImage(*pending.image, false, true)
              : s.renderer.SetImage(*pending.image);
  if (!uploaded) return false;

  s.current = pending.image;
  s.path = pending.path;
  // V3.1-07 S3b: the preview (up to 1920 px for RAW) is sharper than the 160 px
  // transition frame it replaces, so the placeholder state must not outlive it.
  ClearTransitionFrame();
  s.preview_active = true;
  s.preview_failed = false;
  s.preview_path = pending.path;
  s.preview_target_width =
      pending.target_width != 0 ? pending.target_width : pending.image->info.width;
  s.preview_target_height =
      pending.target_height != 0 ? pending.target_height
                                 : pending.image->info.height;
  s.preview_source = pending.source;
  g_preview_shown_ms = pending.shown_ms;
  g_preview_badge_opacity = 1.0f;
  g_preview_badge_tick_ms = 0.0;
  if (hwnd != nullptr) KillTimer(hwnd, kPreviewBadgeTimer);

  // Deterministic hook: zoom the preview so a test can assert that the full
  // frame keeps the same manual view.
  if (g_test_preview_zoom_percent > 0) {
    s.renderer.SetZoom(static_cast<float>(g_test_preview_zoom_percent) / 100.0f);
    char zoom_line[64]{};
    std::snprintf(zoom_line, sizeof(zoom_line), "preview: test zoom=%d%%",
                  g_test_preview_zoom_percent);
    nv::platform::LogInfo(zoom_line);
  }

  DismissErrorOnNewImage();
  // A preview is a readable frame: the delayed loading chip (armed before the
  // renderer existed) must not cover it (規劃/22/13 §3.2).
  CancelLoading(hwnd);
  UpdateTitle(hwnd);
  RefreshUi(hwnd, false);
  if (hwnd != nullptr) InvalidateRect(hwnd, nullptr, TRUE);
  // The open file changed: don't let the 2 s watcher report it as modified.
  SyncWatchBaseline();

  char line[256]{};
  std::snprintf(line, sizeof(line),
                "preview: shown source=%s size=%ux%u target=%ux%u ms=%.0f",
                pending.source.empty() ? "unknown" : pending.source.c_str(),
                s.renderer.image_width(), s.renderer.image_height(),
                s.preview_target_width, s.preview_target_height,
                g_preview_shown_ms);
  nv::platform::LogInfo(line);
  return true;
}

// V3-03: a thumbnail/embedded preview must fill the target aspect, not expose
// bars when the two aspect ratios differ. Crop the centre of the preview; the
// target size (title/info) still comes from full_width/full_height.
std::shared_ptr<nv::Image> CropPreviewToTargetAspect(
    std::shared_ptr<nv::Image> image, uint32_t target_width,
    uint32_t target_height) {
  if (!image || image->Empty() || target_width == 0 || target_height == 0) {
    return image;
  }
  const nv::ImageInfo& info = image->info;
  if (info.format != nv::PixelFormat::kBgra8 || info.stride != info.width * 4u) {
    return image;
  }
  const double target_aspect = static_cast<double>(target_width) / target_height;
  const double source_aspect = static_cast<double>(info.width) / info.height;
  if (std::abs(target_aspect - source_aspect) <= 0.01) return image;

  uint32_t crop_width = info.width;
  uint32_t crop_height = info.height;
  if (source_aspect > target_aspect) {
    crop_width = std::max(1u, static_cast<uint32_t>(
                                  crop_height * target_aspect + 0.5));
  } else {
    crop_height = std::max(1u, static_cast<uint32_t>(
                                  crop_width / target_aspect + 0.5));
  }
  if (crop_width == info.width && crop_height == info.height) return image;

  auto cropped = std::make_shared<nv::Image>();
  cropped->info = info;
  cropped->info.width = crop_width;
  cropped->info.height = crop_height;
  cropped->info.stride = crop_width * 4u;
  cropped->pixels.resize(static_cast<size_t>(crop_width) * crop_height * 4u);
  const uint32_t x = (info.width - crop_width) / 2;
  const uint32_t y = (info.height - crop_height) / 2;
  for (uint32_t row = 0; row < crop_height; ++row) {
    std::memcpy(cropped->pixels.data() + static_cast<size_t>(row) * cropped->info.stride,
                image->pixels.data() +
                    static_cast<size_t>(y + row) * info.stride + x * 4u,
                cropped->info.stride);
  }
  return cropped;
}

int TryShowPreview(HWND hwnd, const std::filesystem::path& path,
                   std::shared_ptr<nv::Image> image, const char* source,
                   uint32_t target_width, uint32_t target_height,
                   double shown_ms) {
  if (g_state == nullptr || image == nullptr || image->Empty()) return 0;
  image = CropPreviewToTargetAspect(std::move(image), target_width, target_height);
  PendingPreview pending;
  pending.image = std::move(image);
  pending.path = path;
  pending.source = source != nullptr ? source : "";
  pending.target_width = target_width;
  pending.target_height = target_height;
  pending.shown_ms = shown_ms;

  // The very first command-line image is opened before the device exists on
  // purpose (M1-2 startup overlap). Keep the preview and apply it as soon as
  // the renderer can upload it instead of dropping it.
  if (!g_state->renderer.ready()) {
    g_pending_preview = std::move(pending);
    return 1;
  }
  return ApplyPreviewFrame(hwnd, pending) ? 2 : 0;
}

// Called once the renderer is initialized, before the first frame is shown.
void ApplyPendingPreview(HWND hwnd) {
  if (g_state == nullptr || !g_pending_preview) return;
  PendingPreview pending = std::move(*g_pending_preview);
  g_pending_preview.reset();
  // The full decode may already have replaced the request; then the preview is
  // stale and must not overwrite the sharp frame (坑 3).
  if (g_state->path == pending.path ||
      (!g_pending_path.empty() && pending.path != g_pending_path)) {
    nv::platform::LogInfo("preview: dropped stale queued preview");
    return;
  }
  ApplyPreviewFrame(hwnd, pending);
  // V3.1-04a: a RAW load deferred its full decode even before the renderer
  // existed (the first image of a session). Now that there is a content
  // rectangle, make the same decision a second open would: preview is final, or
  // a half-size request when it is too small.
  if (!g_raw_deferred_path.empty() && g_raw_deferred_path == pending.path) {
    g_raw_deferred_path.clear();
    HandleRawPreviewDecision(hwnd, pending.path, pending.image);
  }
}

void StartPreviewBadgeFade(HWND hwnd) {
  if (g_preview_badge_opacity <= 0.01f) {
    g_preview_badge_opacity = 0.0f;
    return;
  }
  g_preview_badge_tick_ms = NowMs();
  if (hwnd != nullptr) {
    SetTimer(hwnd, kPreviewBadgeTimer,
             static_cast<UINT>(nv::ui::metrics::kPreviewBadgeTickMs), nullptr);
  }
}

// Blocks an export while only the low-resolution preview exists, so "save as"
// / "copy image" / wallpaper never write a 128 px frame (規劃/22/13 §3.2).
bool BlockExportWhilePreview(HWND hwnd) {
  if (!PreviewActive()) return false;
  nv::platform::LogInfo("preview: export blocked while loading");
  ShowErrorToast(hwnd, nv::i18n::Text(nv::i18n::StringId::kPreviewBusy));
  return true;
}

// V3.1-01 B: count navigations inside a 400 ms window. Two or more make a
// burst; the settle timer (re-armed on every load) turns it off 400 ms after
// the last one, so slow paging (S1/S2, 1.2 s) never enters it.
void NoteNavigationBurst(HWND hwnd) {
  const double now = NowMs();
  if (g_last_nav_ms > 0.0 && now - g_last_nav_ms <= kBurstWindowMs) {
    ++g_nav_streak;
  } else {
    g_nav_streak = 1;
  }
  g_last_nav_ms = now;
  const bool burst = g_nav_streak >= 2;
  if (burst != g_nav_burst) {
    g_nav_burst = burst;
    char line[96]{};
    std::snprintf(line, sizeof(line), "burst: %s streak=%d",
                  burst ? "on" : "off", g_nav_streak);
    nv::platform::LogInfo(line);
    if (g_state && g_state->decode) {
      g_state->decode->SetBurstMode(burst && g_prefetch_parallel);
    }
    // V3.1-07 S1: a burst outranks the thumbnail pre-read. Drop whatever is
    // still queued so the one thumbnail worker stops competing with the tier
    // decodes; the rail re-asks for its visible slots after the burst, and the
    // pre-read timer re-arms 400 ms after the last key.
    if (burst && g_state && g_state->thumbs) g_state->thumbs->CancelPending();
  }
  if (hwnd != nullptr) {
    SetTimer(hwnd, kBurstSettleTimer, static_cast<UINT>(kBurstWindowMs),
             nullptr);
  }
}

void OnBurstSettle(HWND hwnd) {
  if (hwnd != nullptr) KillTimer(hwnd, kBurstSettleTimer);
  g_nav_streak = 0;
  g_last_nav_ms = 0.0;
  // V3.1-02 invariant: a placeholder may not outlive the burst. A pending
  // request clears it when it lands; if nothing is pending (the request it
  // stood in for was cancelled and no newer one arrived), drop the flag so the
  // upgrade/export paths stop treating the small frame as a deliberate tier.
  if (g_state != nullptr && g_state->transition_active &&
      g_pending_path.empty()) {
    nv::platform::LogInfo("transition: cleared on settle");
    ClearTransitionFrame();
  }
  if (!g_nav_burst) return;
  g_nav_burst = false;
  nv::platform::LogInfo("burst: off (settled)");
  if (g_state && g_state->decode) g_state->decode->SetBurstMode(false);
}

// V3.1-01 A: the neighbour chain is re-armed on *every* load, not only on a
// cache hit. The V3-04 code called PrefetchAdjacent() from the hit paths only,
// so a miss-heavy burst decoded each file as the current request, cancelled it
// on the next key, and never queued a neighbour again (24mp25 S3: 5 prefetches,
// 18 cancels, 1–2 frames on screen). The 50 ms debounce keeps a double load of
// the same path from queueing the same two neighbours twice.
void RearmPrefetchChain(const std::filesystem::path& path) {
  if (!g_prefetch_enabled || g_state == nullptr) return;
  // Before the renderer exists there is no content rectangle, so "the tier the
  // next load asks for" is unknown; queueing full frames here would be the
  // 96 MB path the first open deliberately avoids (規劃/25 §7.5).
  if (!g_state->renderer.ready()) return;
  const double now = NowMs();
  if (g_prefetch_armed_path == path && now - g_prefetch_armed_ms < 50.0) {
    return;
  }
  g_prefetch_armed_path = path;
  g_prefetch_armed_ms = now;
  PrefetchAdjacent();
}

// V3.1-07 S1: the single entry point for the thumbnails the *transition frame*
// needs (the file being opened, and the ±6 neighbours while the view rests). The
// completion handler logs these as `thumbs: prewarm` instead of the rail's
// `thumbnail ready:`, so `run_strip_check`'s "a hidden rail requests no previews"
// keeps measuring the rail's own queue (SyncStripThumbnails) rather than this
// one. Request() already skips what is cached or queued.
//
// RAW is excluded on purpose: a LibRaw thumbnail is a half-size demosaic
// (0.3-0.5 s, `libraw_decoder.cpp:295`) and takes the process-global LibRaw
// lock, which would delay the embedded preview the RAW load actually shows
// (~80-150 ms). RAW's placeholder is that preview, not a 160 px frame.
bool IsRawFormatPath(const std::filesystem::path& path) {
  std::wstring extension = path.extension().wstring();
  std::ranges::transform(
      extension, extension.begin(),
      [](wchar_t ch) { return static_cast<wchar_t>(std::towlower(ch)); });
  return nv::FormatRouter::FormatForExtension(extension) == nv::ImageFormat::kRaw;
}

bool RequestThumbForPlaceholder(size_t index,
                                const std::filesystem::path& path) {
  if (g_state == nullptr || !g_state->thumbs) return false;
  if (IsRawFormatPath(path)) return false;
  // Inside a burst a 160 px frame cannot arrive before the next key anyway (its
  // decode costs about as much as the tier), so the request would only compete
  // with the display decode. The pre-read covers the resting case, which is
  // exactly what a burst starting from rest needs.
  if (g_nav_burst) return false;
  // Bounded bookkeeping: the set exists only to tell `thumbs: prewarm` from the
  // rail's `thumbnail ready:` in OnThumbnailDone (see the function above).
  if (g_prewarm_paths.size() > 96) g_prewarm_paths.clear();
  // Request() dedups by memory/queue, so re-asking is cheap. Only a request this
  // call actually queued claims the log line: a frame the rail asked for stays
  // the rail's, which is what `run_strip_check` measures.
  if (!g_state->thumbs->Request(index, path)) return false;
  g_prewarm_paths.insert(nv::MakeCacheKey(path));
  return true;
}

// V3.1-07 S2: a RAW revisit now ends at the tier put-from-preview, and the
// generic tier hit never logged a `load: done` (a cache hit is silent apart from
// `display cache hit` — for JPEG too). The S2 acceptance reads a latency, so the
// RAW-only hit reports one; adding it to every tier hit would change the
// `load: done` counts the switch-speed scripts measure. `raw_mode=tier` keeps
// the state machine honest: this frame came off the ladder, so a later 1:1
// upgrade still turns raw_mode into "full" when the real pixels land.
void LogRawTierRevisit(const std::filesystem::path& path, uint32_t edge,
                       double load_started) {
  if (g_state != nullptr) g_state->raw_mode = "tier";
  char line[512]{};
  std::snprintf(line, sizeof(line), "load: done ms=%.0f path=%s",
                NowMs() - load_started, Narrow(path.wstring()).c_str());
  nv::platform::LogInfo(line);
  char hit[320]{};
  std::snprintf(hit, sizeof(hit),
                "raw: revisited from display tier edge=%u path=%s", edge,
                Narrow(path.wstring()).c_str());
  nv::platform::LogInfo(hit);
}

void LoadPath(HWND hwnd, const std::filesystem::path& path, bool refresh_list) {
  if (!g_state || !g_state->decode) return;
  // V3.1-01: must run before the request so DecodeService knows whether this
  // load belongs to a burst (the gate reads it when a worker looks for work).
  NoteNavigationBurst(hwnd);
  // Every explicit load starts a new two-stage attempt: drop the previous
  // preview state and badge (規劃/22/13 坑 17).
  ResetPreviewState(hwnd);
  // §7.6: a view target belongs to one file; a new load must never apply the
  // previous upgrade's scale to a frame it did not ask for.
  g_upgrade_target_scale = 0.0f;
  if (g_state != nullptr) g_state->upgrade_in_flight.clear();
  const double load_started = NowMs();
  // V3.1-07 S1/S4: the settle timer is armed for *every* load, before the cache
  // lookups below can return early. Re-arming on every load means a burst never
  // triggers it (SetTimer with the same id just resets the delay) and it runs
  // ~400 ms after the user stops — exactly the window before the next burst
  // starts. V3.1-07 S4 also hangs the RAW neighbour prefetch off this timer
  // ("only at rest"), so a cache hit must arm it too: it used to skip this line
  // and left the next file unprefetched (measured: 4 ms hit, then 80 ms miss).
  if (hwnd != nullptr && g_state->thumbs) {
    SetTimer(hwnd, kThumbPrewarmTimer, kThumbPrewarmDelayMs, nullptr);
  }
  // Any explicit load after the forced test error is a user action (navigation,
  // thumbnail, retry): let the next successful image dismiss it.
  g_force_error_sticky = false;
  StopAnimation(hwnd);
  // keep_rotation (規劃/22/05 §3.5): on by default, because the next file in a
  // folder is usually shot in the same orientation. Off resets once per load.
  if (!g_settings.keep_rotation && g_state->renderer.rotation() != 0) {
    g_state->renderer.ResetRotation();
    nv::platform::LogInfo("cmd: view=rotate reset keep_rotation=0");
  }

  // OpenSibling and thumbnail clicks pass references into folder.files; copy
  // before any rebuild of the list.
  const std::filesystem::path requested = path;
  auto& s = *g_state;

  // V3.1-04a: a new load supersedes a pending RAW preview decision; its
  // completion must not promote or half-decode the previous file.
  g_raw_deferred_path.clear();
  s.raw_mode = "none";
  s.preview_edge = 0;
  s.format = nv::ImageFormat::kUnknown;
  s.file_bytes = 0;

  // EXIF belongs to one file: drop the previous tags before the new image is
  // even decoded, and start the read in parallel (規劃/22/06 §4.2).
  if (requested != s.path) {
    g_exif_data = nv::ExifData{};
    g_exif_key.clear();
    // A new file starts the card at the top; the window keeps its position.
    if (InfoWindowVisible()) g_info_window->ResetScroll();
  }
  if (InfoWindowVisible()) {
    RequestExifForPath(requested);
  }

  // A new load supersedes everything queued. This matters even when the new
  // image comes from the memory/disk cache: without cancelling, the previous
  // request would still be "current" and its completion would replace the
  // cached image with a stale one.
  if (!g_pending_path.empty()) {
    nv::platform::LogInfo("load: cancelled path=" +
                          Narrow(g_pending_path.wstring()));
  }
  if (s.decode) s.decode->CancelAll();
  g_pending_path.clear();
  CancelLoading(hwnd);

  bool rebuilt = false;
  if (refresh_list || s.folder.folder != requested.parent_path()) {
    const double scan_start = NowMs();
    s.folder = nv::BuildFolderList(requested);
    char scan_line[128]{};
    std::snprintf(scan_line, sizeof(scan_line), "folder: %zu files in %.1fms",
                  s.folder.files.size(), NowMs() - scan_start);
    nv::platform::LogInfo(scan_line);
    rebuilt = true;
    if (s.thumbs) s.thumbs->CancelPending();
    // The pre-read bookkeeping is per folder: its paths no longer resolve, and
    // the plan's indices now address different files.
    g_prewarm_paths.clear();
    ClearPrewarmPlan(hwnd);
  } else if (!s.folder.empty()) {
    const size_t found = FindInFolder(requested);
    if (found != nv::render::ThumbnailStrip::kNone) s.folder.index = found;
  }

  if (rebuilt) s.strip.SetItems(s.folder.files);
  UpdateFolderUi(hwnd, requested);

  const nv::ImageFormat requested_format = nv::FormatRouter::Detect(requested);
  // V3.1-08: remember what the early-upgrade policy needs, once per load.
  s.format = requested_format;
  {
    std::error_code size_error;
    const uint64_t bytes = std::filesystem::file_size(requested, size_error);
    s.file_bytes = size_error ? 0 : bytes;
  }
  const bool animation_source = requested_format == nv::ImageFormat::kGif ||
                                requested_format == nv::ImageFormat::kApng;
  // V3-04b: RAW stays out of the tier *decode* chain. Its two-stage path already
  // shows a camera-embedded preview (1920 px, ~250 ms) before the full decode,
  // and a 1280 tier is *smaller* than that preview: replacing it would shrink the
  // picture at a manual zoom and look softer, while the tier decode itself costs
  // ~1.2 s (LibRaw open dominates). V3.1-07 S2 lowers the *revisit* cost without
  // touching that rule: the promoted preview is stored as the fit tier (see
  // `cache_edge` below and PromoteRawPreviewToFinalFrame), so a hit is served
  // from the cache and a miss still never decodes a RAW tier.
  const bool tier_source =
      !animation_source && requested_format != nv::ImageFormat::kRaw;

  // V3-04: which display tier does this view need? Probed from the header only
  // (no decode); the tier decision itself is DecideTierEdge() below.
  //
  // §7.13-3: Detect() is the one header pass that decides the decoder, and the
  // UI needs that answer for the animation check as well. Every later probe
  // therefore passes the format in instead of re-reading the header with a
  // second Detect(). Before this change an open could Detect 2–3 times (tier
  // probe + preview-size probe) on top of the decoder's own header read.
  nv::ProbeResult tier_probe;
  uint32_t requested_edge = 0;
  if (g_display_cache_mode != DisplayCacheMode::kOff && tier_source &&
      s.renderer.ready()) {
    tier_probe = nv::FormatRouter::Probe(requested, requested_format);
    requested_edge = RequestedDisplayEdge(requested, tier_probe);
    if (requested_edge != 0) {
      nv::platform::LogInfo("display tier request: edge=" +
                            std::to_string(requested_edge) + " path=" +
                            Narrow(requested.wstring()));
    }
  }

  // V3.1-07 S2: RAW has no decode-side tier chain (`tier_source` above keeps
  // `requested_edge` at 0), but PromoteRawPreviewToFinalFrame stores the
  // embedded preview's pixels as the *fit* tier (put-from-preview), so the
  // caches must be asked for exactly that entry — otherwise a revisit pays the
  // ~80 ms LibRaw preview decode again (規劃/v3.1/07 §3 S2: 回访 80 ms → 0).
  //
  // Lookups only: `requested_edge` stays 0, so a miss still takes the 04a
  // deferred-preview path and a RAW tier is never *decoded*. §7.3 rule 3 is
  // honoured with the same `full_frames` test RequestedDisplayEdge applies: as
  // soon as this session produced the full frame of the file, a revisit must go
  // there, not back to the tier.
  uint32_t cache_edge = requested_edge;
  const bool raw_tier_lookup =
      cache_edge == 0 && g_display_cache_mode != DisplayCacheMode::kOff &&
      requested_format == nv::ImageFormat::kRaw && s.renderer.ready() &&
      !s.full_frames.contains(requested.wstring());
  if (raw_tier_lookup) cache_edge = FitDisplayTierEdge();

  if (s.renderer.ready()) {
    if (!animation_source) {
      // §7.3 rule 2: a full frame in memory always wins over any tier.
      if (auto cached = s.cache.Get(nv::MakeMemoryCacheKey(requested))) {
        if (s.renderer.SetImage(*cached)) {
          s.current = cached;
          s.path = requested;
          s.current_is_tier = false;
          s.display_edge = 0;
          s.cache_source = "mem";
          ClearTransitionFrame();
          s.renderer.SetSourceIsTier(false);
          nv::platform::LogInfo("memory cache hit: " + Narrow(requested.wstring()));
          TraceCache(requested, 0, "mem");
          DismissErrorOnNewImage();
          UpdateTitle(hwnd);
          StartAnimation(hwnd);
          RefreshUi(hwnd, false);
          InvalidateRect(hwnd, nullptr, TRUE);
          SyncWatchBaseline();
          PrefetchAdjacent();
          return;
        }
      }
      // §7.3 rule 1: the needed tier first, then every larger tier — a bigger
      // tier is sharper and still far smaller than the full frame.
      for (uint32_t edge = cache_edge; edge != 0; edge = nv::NextTier(edge)) {
        auto cached = s.cache.Get(nv::MakeMemoryCacheKey(requested, edge));
        if (!cached) continue;
        if (ShowDisplayTier(hwnd, requested, std::move(cached), edge, "mem")) {
          if (raw_tier_lookup) LogRawTierRevisit(requested, edge, load_started);
          return;
        }
      }
    }

    if (s.disk_cache && !animation_source) {
      if (auto cached =
              s.disk_cache->Load(requested, kDiskCacheMaxFrameBytes)) {
        LogMemoryUsage("disk_loaded");
        if (s.renderer.SetImage(*cached)) {
          LogMemoryUsage("disk_image_ready");
          s.current = cached;
          s.cache.Put(nv::MakeMemoryCacheKey(requested), cached);
          s.path = requested;
          s.current_is_tier = false;
          s.display_edge = 0;
          s.cache_source = "disk";
          ClearTransitionFrame();
          s.renderer.SetSourceIsTier(false);
          nv::platform::LogInfo("disk cache hit: " + Narrow(requested.wstring()));
          TraceCache(requested, 0, "disk");
          DismissErrorOnNewImage();
          UpdateTitle(hwnd);
          StartAnimation(hwnd);
          RefreshUi(hwnd, false);
          InvalidateRect(hwnd, nullptr, TRUE);
          SyncWatchBaseline();
          PrefetchAdjacent();
          return;
        }
      }
      // Tier entries, same upward walk as the memory cache.
      for (uint32_t edge = cache_edge; edge != 0; edge = nv::NextTier(edge)) {
        auto loaded = s.disk_cache->LoadTier(
            requested, edge, static_cast<uint64_t>(edge) * edge * 4ull);
        if (!loaded) continue;
        if (ShowDisplayTier(hwnd, requested, std::move(loaded), edge, "disk")) {
          if (raw_tier_lookup) LogRawTierRevisit(requested, edge, load_started);
          return;
        }
      }
    }
  }

  // --- Two-stage loading (規劃/22/13) --------------------------------------
  // The full-size caches above had no entry, so this is a real decode. Decide
  // whether the format is slow enough to deserve a preview, and try the shared
  // thumbnail cache first (sync, ~1 ms). RAW additionally queues the embedded
  // JPEG preview when the cache misses.
  //
  // V3-03: a 24 MP JPEG can be only ~12 MB, so the old 32 MB size test missed
  // it. Probe the header for the pixel count (only >=4 MB) and require an
  // existing thumbnail; the first open must not flash a blurry frame or queue
  // a redundant thumbnail job.
  // V3-04: reuse the format the tier/animation check already resolved instead
  // of opening the file for a third magic probe (規劃/25 §7.13-3).
  const nv::ImageFormat preview_format = requested_format;
  std::error_code size_ec;
  const uint64_t file_bytes = std::filesystem::file_size(requested, size_ec);
  uint64_t preview_pixels = 0;
  const bool probe_preview_pixels =
      !size_ec && file_bytes >= nv::kPreviewProbeMinFileBytes &&
      preview_format != nv::ImageFormat::kRaw &&
      preview_format != nv::ImageFormat::kPsd &&
      preview_format != nv::ImageFormat::kGif &&
      preview_format != nv::ImageFormat::kApng &&
      preview_format != nv::ImageFormat::kWebp;
  if (probe_preview_pixels) {
    // Reuse the header probe the tier decision already made (one file open),
    // and pass the format that Detect() already produced (§7.13-3).
    if (tier_probe.width != 0) {
      preview_pixels = static_cast<uint64_t>(tier_probe.width) *
                       static_cast<uint64_t>(tier_probe.height);
      // Evidence for "one Detect per open": the preview decision did not have
      // to open the file again.
      nv::platform::LogInfo("probe: preview reused tier probe");
    } else {
      const nv::ProbeResult size_probe =
          nv::FormatRouter::Probe(requested, requested_format);
      if (size_probe.ok()) {
        preview_pixels = static_cast<uint64_t>(size_probe.width) *
                         static_cast<uint64_t>(size_probe.height);
      }
      nv::platform::LogInfo("probe: preview header read (no tier probe)");
    }
  }
  const nv::PreviewPlan preview_candidate = nv::PlanPreview(
      preview_format, size_ec ? 0 : file_bytes, preview_pixels, true);
  std::shared_ptr<nv::Image> cached_preview;
  if (preview_candidate.cache && s.thumbs) {
    cached_preview = s.thumbs->TryLoadPreview(requested);
  }
  const nv::PreviewPlan preview_plan = nv::PlanPreview(
      preview_format, size_ec ? 0 : file_bytes, preview_pixels,
      cached_preview != nullptr);
  int preview_state = 0;  // 0 none, 1 queued, 2 shown
  if (cached_preview) {
    const uint32_t target_width =
        cached_preview->info.full_width != 0
            ? cached_preview->info.full_width
            : cached_preview->info.width;
    const uint32_t target_height =
        cached_preview->info.full_height != 0
            ? cached_preview->info.full_height
            : cached_preview->info.height;
    preview_state = TryShowPreview(hwnd, requested, std::move(cached_preview),
                                   "cache", target_width, target_height,
                                   NowMs() - load_started);
  }
  // Keep the load clock for the "load: done ms=" line even when no loading
  // indicator is shown because a preview already owns the canvas.
  if (preview_state != 0) g_load_start_ms = load_started;

  // --- V3.1-02 / V3.1-07 S3: transition frame ------------------------------
  // Nothing from the caches and no preview produced a frame: a smaller tier or
  // the strip thumbnail can change the screen now instead of after the decode
  // (規劃/v3.1/01 §V3.1-02).
  // V3.1-07 S3b: RAW has no tier (`requested_edge == 0`) and no neighbour
  // prefetch, so a 5-9/s page-through of RAW files used to change nothing at
  // all. Its 160 px thumbnail is a valid placeholder too, so the gate is "tier
  // or RAW" instead of "tier".
  const bool transition_candidate =
      requested_edge != 0 || requested_format == nv::ImageFormat::kRaw;
  if (preview_state == 0 && transition_candidate) {
    g_load_start_ms = load_started;
    if (TryShowTransitionFrame(hwnd, requested, requested_edge)) {
      preview_state = 2;  // a readable frame owns the canvas
    }
  }

  // V3.1-07 S3b: make sure the 160 px frame of *this* file gets produced.
  // `SyncStripThumbnails` only asks for thumbnails of visible slots, and a
  // hidden rail (`--strip=auto`, the default) asks for nothing — which left a
  // cold folder with no placeholder at all and made 5-9 key presses look dead.
  // The request is cheap (one 0.07 MB entry, its own below-normal worker) and
  // OnThumbnailDone puts the frame on screen while the tier still decodes; this
  // requests with this edge first; the caller promotes it to a full frame.
  g_pending_edge = requested_edge;
  if (!cached_preview && preview_state != 2 && s.thumbs && !s.folder.empty()) {
    RequestThumbForPlaceholder(s.folder.index, requested);
  }

  nv::platform::LogInfo("open request: " + Narrow(requested.wstring()));
  nv::platform::LogInfo("load: start path=" + Narrow(requested.wstring()));
  // A shown preview means the window already has a readable frame: the
  // "Loading…" title/chip would only hide it (規劃/22/13 §3.2).
  if (preview_state != 2) {
    wchar_t title[512]{};
    swprintf_s(title,
               nv::i18n::Text(nv::i18n::StringId::kLoadingTitleFormat),
               nv::i18n::Text(nv::i18n::StringId::kAppTitle), requested.filename().wstring().c_str());
    SetWindowTextW(hwnd, title);
    NotifyWinEvent(EVENT_OBJECT_NAMECHANGE, hwnd, OBJID_WINDOW, CHILDID_SELF);
  }
  // Hide the empty card while the decode is in flight: a load is not "no
  // image" (規劃/22/03 坑 5).
  g_pending_path = requested;
  RefreshUi(hwnd, false);
  if (preview_state == 2) {
    CancelLoading(hwnd);
  } else {
    BeginLoading(hwnd, false);
  }
  // --- V3.1-04a: RAW defers the main decode -------------------------------
  // The embedded JPEG is the candidate final frame for a fit view, so no
  // 5999x3999 decode is started yet; OnPreviewDone decides (preview is final, or
  // a half-size request when the preview is too small). The first image of a
  // session still full-decodes: LoadPath runs before the renderer exists
  // (規劃/25 §7.15 已知限制①), and there is no content rectangle to compare to.
  const bool raw_preview_candidate =
      g_raw_preview_mode == RawPreviewMode::kPreview &&
      requested_format == nv::ImageFormat::kRaw && !g_settings.pixel_mode &&
      preview_plan.embedded;
  if (raw_preview_candidate && g_display_cache_mode != DisplayCacheMode::kOff) {
    g_raw_deferred_path = requested;
    s.raw_mode = "deferred";
    nv::platform::LogInfo("raw: deferred full decode (preview candidate) path=" +
                          Narrow(requested.wstring()));
  } else {
    if (requested_format == nv::ImageFormat::kRaw) s.raw_mode = "full";
    s.decode->Request(requested, requested_edge);
  }
  // V3.1-01 A: unconditionally re-arm the neighbour prefetch for this load
  // (hit, miss and cancel all end up here). Only the queue is refilled; whether
  // a worker may start it is DecodeService's gate.
  RearmPrefetchChain(requested);
  // The dedicated preview worker gives the embedded JPEG priority over the two
  // full-size workers and the prefetch queue (規劃/22/13 §3.2). A queued
  // preview (renderer not ready yet) also skips this second request, but a
  // cached 128 px thumbnail is upgraded to the embedded JPEG when RAW has one.
  const bool cache_preview =
      preview_state != 0 && (s.preview_source == "cache" || preview_state == 1);
  if (preview_plan.embedded &&
      (preview_state == 0 || cache_preview || raw_preview_candidate)) {
    g_preview_request_start_ms = load_started;
    s.decode->RequestPreview(requested);
    nv::platform::LogInfo("preview: requested source=embedded");
  }
}

void OpenFile(HWND hwnd, const std::filesystem::path& path) {
  // A real open leaves the empty state, including a transient unsupported note
  // and the all-formats modal it may have been opened from. A stale error card
  // must not survive the next image either (規劃/22/11 坑 1).
  g_empty_base = nv::ui::EmptyKind::kEmpty;
  g_empty_note = false;
  g_empty_note_text.clear();
  KillTimer(hwnd, kEmptyNoteTimer);
  if (g_ui != nullptr) {
    g_ui->overlay().CloseModal();
    g_ui->overlay().SetErrorOpen(false);
  }
  g_error = nv::UiError{};
  // V3.1-07 S1-3 / S4: a real open (command line, drag-drop, "open with", F5)
  // carries no paging direction; the pre-read and the neighbour prefetch then
  // start forward, as they always did.
  NoteNavigationDirection(0);
  LoadPath(hwnd, path, true);
}

void OpenIndex(HWND hwnd, size_t index) {
  if (!g_state || g_state->folder.empty()) return;
  auto& folder = g_state->folder;
  index %= folder.size();
  // V3.1-07 S1-3: a jump (thumbnail click, Home/End, test hook) still has a
  // direction — the shorter way round the list. A same-index open keeps the last
  // known direction, which is what a re-open of the current file should do.
  if (folder.size() > 1 && index != folder.index) {
    const long long size = static_cast<long long>(folder.size());
    const long long forward =
        (static_cast<long long>(index) - static_cast<long long>(folder.index) +
         size) % size;
    NoteNavigationDirection(forward * 2 <= size ? 1 : -1);
  }
  folder.index = index;
  const std::filesystem::path path = folder.files[index];
  LoadPath(hwnd, path, false);
}

// --- v2.0 S2 / 22/10: folder wrap -----------------------------------------
// The immediate sibling folders are cached for 2 s; checking whether a folder
// holds a supported image is deferred until it is the next candidate.
const std::vector<std::filesystem::path>& CachedSiblingFolders(
    const std::filesystem::path& parent) {
  const double now = NowMs();
  if (g_sibling_cache.valid && g_sibling_cache.parent == parent &&
      now < g_sibling_cache.expires_ms) {
    return g_sibling_cache.folders;
  }
  g_sibling_cache.parent = parent;
  g_sibling_cache.folders = nv::ListSiblingFolders(parent);
  g_sibling_cache.expires_ms = now + 2000.0;
  g_sibling_cache.valid = true;
  return g_sibling_cache.folders;
}

void InvalidateSiblingCache() { g_sibling_cache.valid = false; }

// Finds the next/previous folder with at least one supported image. Returns an
// empty path at the end of the drive (or at a drive/UNC root, 22/10 坑 13).
std::filesystem::path FindAdjacentFolder(const std::filesystem::path& folder,
                                         int direction) {
  const std::filesystem::path parent = folder.parent_path();
  if (parent.empty()) return {};
  const auto& siblings = CachedSiblingFolders(parent);
  size_t probed = 0;
  const auto target = nv::FindAdjacentImageFolder(siblings, folder, direction,
                                                  200, &probed);
  char line[256]{};
  std::snprintf(line, sizeof(line), "nav: peek dir=%d probed=%zu found=%d",
                direction, probed, target ? 1 : 0);
  nv::platform::LogInfo(line);
  return target ? *target : std::filesystem::path();
}

void EnterAdjacentFolder(HWND hwnd, const std::filesystem::path& folder,
                         bool last) {
  nv::platform::LogInfo("nav: folder=" + Narrow(folder.wstring()) +
                        (last ? " first=0" : " first=1"));
  EnterFolderWithLast(hwnd, folder, last);
}

void OpenSibling(HWND hwnd, int delta) {
  if (!g_state || g_state->folder.empty()) return;
  // V3.1-07 S1-3 / S4: remember which way the user is paging even when the step
  // turns out to be a no-op; the pre-read fills that side first and the neighbour
  // prefetch walks it (depth from the same-direction streak).
  if (delta != 0) NoteNavigationDirection(delta > 0 ? 1 : -1);
  const std::filesystem::path keep = g_state->folder.folder;
  nv::WrapStrategy wrap = nv::WrapStrategy::kLoop;
  (void)nv::ParseWrapStrategy(g_settings.folder_wrap, &wrap);
  const bool at_edge =
      (delta > 0 && g_state->folder.index + 1 >= g_state->folder.size()) ||
      (delta < 0 && g_state->folder.index == 0);

  // Peeking is only needed at an edge and only for the policies that can leave
  // the folder; "loop" never touches the file system.
  std::filesystem::path adjacent;
  if (at_edge && wrap != nv::WrapStrategy::kLoop) {
    adjacent = FindAdjacentFolder(keep, delta);
  }
  const nv::StepAction action = nv::PlanFolderStep(
      g_state->folder.index, g_state->folder.size(), delta, wrap,
      !adjacent.empty());

  switch (action) {
    case nv::StepAction::kMove:
      if (!nv::StepFolder(g_state->folder, delta)) return;
      LoadPath(hwnd, g_state->folder.current(), false);
      return;
    case nv::StepAction::kLoop:
      if (g_state->folder.size() <= 1) {
        // A genuine no-op: do not reload the same file just to repaint.
        nv::platform::LogInfo("nav: edge=loop-single");
        return;
      }
      if (!nv::StepFolder(g_state->folder, delta)) return;
      LoadPath(hwnd, g_state->folder.current(), false);
      return;
    case nv::StepAction::kAdjacent:
      EnterAdjacentFolder(hwnd, adjacent, delta < 0);
      return;
    case nv::StepAction::kAskAdjacent:
      ShowWrapPrompt(hwnd, delta > 0, adjacent);
      return;
    case nv::StepAction::kAskLoop:
      ShowWrapPrompt(hwnd, delta > 0, {});
      return;
  }
}

// --- Command layer (規劃/22/09 §3) -----------------------------------------
// Keyboard, bottom bar, title bar, image arrows and test hooks all end up in
// DispatchCommand(); QueryCommand() is the single availability decision.

nv::ui::CommandContext BuildCommandContext() {
  nv::ui::CommandContext context;
  if (g_state == nullptr) return context;
  context.has_image = g_ui_inputs.has_image;
  context.file_count = g_state->folder.size();
  context.has_multiple_frames =
      g_state->current != nullptr && g_state->current->frames.size() > 1;
  context.animation_playing = g_state->anim.playing();
  context.fullscreen = g_window_state.fullscreen();
  context.info_open = InfoWindowVisible();
  // Ctrl+T / the more-panel toggle is "on" only when the rail is pinned; auto
  // is the faded-in-by-hover state and off is settings-only (規劃/22/08 §3.2).
  context.strip_visible = g_strip_mode == nv::ui::StripMode::kPinned;
  context.fit = g_state->renderer.fit();
  nv::WrapStrategy wrap = nv::WrapStrategy::kLoop;
  (void)nv::ParseWrapStrategy(g_settings.folder_wrap, &wrap);
  context.wrap_crosses_folders = wrap != nv::WrapStrategy::kLoop;
  return context;
}

// One log line per rotation command: the script asserts the state from the
// dump, and this line makes it visible without the dump.
std::string ViewRotationLogLine(const char* direction) {
  const nv::render::Renderer& renderer = g_state->renderer;
  char line[192]{};
  std::snprintf(line, sizeof(line),
                "cmd: view=rotate %s rotation=%d angle=%d display=%ux%u",
                direction, renderer.rotation(),
                nv::render::RotationDegrees(renderer.rotation()),
                renderer.display_width(), renderer.display_height());
  return line;
}

void DispatchCommand(HWND hwnd, nv::ui::CommandId id) {  if (g_state == nullptr || id == nv::ui::CommandId::kNone) return;
  nv::ui::CommandContext context = BuildCommandContext();

  // Space on a still image is "next" (規劃/22/09 §4): only an animated source
  // can toggle playback.
  if (id == nv::ui::CommandId::kToggleAnimation &&
      !context.has_multiple_frames) {
    id = nv::ui::CommandId::kNext;
  }

  const nv::ui::CommandState state = nv::ui::QueryCommand(id, context);
  if (!state.enabled) {
    nv::platform::LogInfo(std::string("cmd: blocked=") +
                          nv::ui::CommandName(id));
    return;
  }

  switch (id) {
    case nv::ui::CommandId::kPrev:
      nv::platform::LogInfo("cmd: nav=prev");
      OpenSibling(hwnd, -1);
      break;
    case nv::ui::CommandId::kNext:
      nv::platform::LogInfo("cmd: nav=next");
      OpenSibling(hwnd, +1);
      break;
    case nv::ui::CommandId::kFirst:
      nv::platform::LogInfo("cmd: nav=first");
      OpenIndex(hwnd, 0);
      break;
    case nv::ui::CommandId::kLast:
      nv::platform::LogInfo("cmd: nav=last");
      OpenIndex(hwnd, context.file_count - 1);
      break;
    case nv::ui::CommandId::kZoomIn:
      g_state->renderer.ZoomBy(static_cast<float>(g_settings.zoom_step));
      nv::platform::LogInfo("cmd: zoom=in");
      break;
    case nv::ui::CommandId::kZoomOut:
      g_state->renderer.ZoomBy(1.0f / static_cast<float>(g_settings.zoom_step));
      nv::platform::LogInfo("cmd: zoom=out");
      break;
    case nv::ui::CommandId::kFit:
      g_state->renderer.FitToWindow();
      nv::platform::LogInfo("cmd: zoom=fit");
      break;
    case nv::ui::CommandId::kActual:
      ApplyActualSize();
      nv::platform::LogInfo("cmd: zoom=actual");
      break;
    case nv::ui::CommandId::kRotateLeft:
      g_state->renderer.RotateLeft();
      nv::platform::LogInfo(ViewRotationLogLine("left"));
      break;
    case nv::ui::CommandId::kRotateRight:
      g_state->renderer.RotateRight();
      nv::platform::LogInfo(ViewRotationLogLine("right"));
      break;
    case nv::ui::CommandId::kToggleFullscreen:
      nv::platform::LogInfo("cmd: view=fullscreen");
      ToggleFullscreen(hwnd);
      return;
    case nv::ui::CommandId::kToggleInfo:
      ToggleInfoPanel(hwnd);
      nv::platform::LogInfo(std::string("cmd: view=info ") +
                            (InfoWindowVisible() ? "1" : "0"));
      break;
    case nv::ui::CommandId::kToggleStrip:
      // One implementation: the more panel's thumbnails switch (22/02).
      nv::platform::LogInfo("cmd: view=strip");
      ExecuteMenuCommand(hwnd, kCmdToggleThumbnails);
      return;
    case nv::ui::CommandId::kToggleAnimation:
      ToggleAnimation(hwnd);
      nv::platform::LogInfo("cmd: animation=toggle");
      break;
    case nv::ui::CommandId::kFramePrev:
      StepAnimationFrame(hwnd, -1);
      nv::platform::LogInfo("cmd: animation=prev");
      break;
    case nv::ui::CommandId::kFrameNext:
      StepAnimationFrame(hwnd, +1);
      nv::platform::LogInfo("cmd: animation=next");
      break;
    default:
      return;
  }

  // Zoom commands format the new percentage in the title; navigation has
  // already refreshed and repainted.
  UpdateTitle(hwnd);
  RefreshUi(hwnd, false);
  InvalidateRect(hwnd, nullptr, TRUE);
}

// --- File operations (M1-6) ------------------------------------------------

std::wstring TrimText(std::wstring text) {
  size_t begin = 0;
  while (begin < text.size() && std::iswspace(text[begin]) != 0) ++begin;
  size_t end = text.size();
  while (end > begin && std::iswspace(text[end - 1]) != 0) --end;
  return text.substr(begin, end - begin);
}

std::wstring LowerExtension(const std::filesystem::path& path) {
  std::wstring extension = path.extension().wstring();
  std::ranges::transform(extension, extension.begin(), [](wchar_t ch) {
    return static_cast<wchar_t>(std::towlower(ch));
  });
  return extension;
}

bool IsValidFileName(const std::wstring& name) {
  if (name.empty() || name == L"." || name == L"..") return false;
  for (wchar_t ch : name) {
    if (ch < 0x20 || std::wcschr(L"<>:\"/\\|?*", ch) != nullptr) return false;
  }
  return name.back() != L' ' && name.back() != L'.';
}

// --- Dropped / opened folders and the transient unsupported note ----------

// Shows `folder` as the current folder: opens its first supported image, or
// enters the empty-folder state without ever queueing a decode (規劃/22/03
// §4.3, 坑 5). The folder itself stays the navigator's folder in both cases.
void EnterFolderWithLast(HWND hwnd, const std::filesystem::path& folder,
                         bool open_last) {
  if (!g_state) return;
  StopAnimation(hwnd);
  auto& s = *g_state;
  if (s.decode) s.decode->CancelAll();
  g_pending_path.clear();
  CancelLoading(hwnd);
  if (g_ui != nullptr) g_ui->overlay().SetErrorOpen(false);
  g_error = nv::UiError{};
  ResetPreviewState(hwnd);
  s.current.reset();
  s.path.clear();
  g_pending_path.clear();
  s.folder = nv::BuildFolderListFromFolder(folder);
  s.strip.SetItems(s.folder.files);
  if (s.thumbs) s.thumbs->CancelPending();
  InvalidateSiblingCache();
  nv::platform::LogInfo("folder: open " + Narrow(folder.wstring()) +
                        " files=" + std::to_string(s.folder.size()));

  if (s.folder.empty()) {
    s.strip.Clear();
    s.renderer.ClearImage();
    g_empty_base = nv::ui::EmptyKind::kFolder;
    nv::platform::LogInfo("empty: state=folder");
    RefreshUi(hwnd, false);
    UpdateTitle(hwnd);
    InvalidateRect(hwnd, nullptr, TRUE);
    SyncWatchBaseline();
    return;
  }

  g_empty_base = nv::ui::EmptyKind::kEmpty;
  const size_t index = open_last ? s.folder.size() - 1 : 0;
  // V3.1-07 S1-3: entering a folder at its last image means the user is paging
  // backwards, at its first one forwards.
  NoteNavigationDirection(open_last ? -1 : 1);
  UpdateFolderUi(hwnd, s.folder.files[index]);
  OpenIndex(hwnd, index);
  SyncWatchBaseline();
}

void EnterFolder(HWND hwnd, const std::filesystem::path& folder) {
  EnterFolderWithLast(hwnd, folder, false);
}

// --- v2.0 S2 / 22/10: folder watching -------------------------------------
// The watcher only compares snapshots; these helpers own the UI reaction.

void SyncWatchBaseline() {
  if (!g_state || g_state->folder.folder.empty()) return;
  const bool include_count = !nv::IsRemotePath(g_state->folder.folder);
  g_watcher.Reset(nv::TakeFolderSnapshot(g_state->folder.folder, g_state->path,
                                         include_count),
                  NowMs());
  // V3-05: the baseline may belong to a folder that just changed (navigation, a
  // newly opened folder, ...). Selecting the source here means every caller of
  // SyncWatchBaseline keeps the watcher in sync without a second call.
  UpdateWatchSource();
}

// Case-insensitive path identity, like the folder keys in FolderWatcher. The
// paths compared here always come from the same code, so normalising is enough;
// the ordinal compare only removes drive/folder case differences.
bool SameWatchFolder(const std::filesystem::path& left,
                     const std::filesystem::path& right) {
  const std::wstring a = left.lexically_normal().wstring();
  const std::wstring b = right.lexically_normal().wstring();
  if (a.empty() || b.empty()) return a.empty() && b.empty();
  return CompareStringOrdinal(a.c_str(), static_cast<int>(a.size()), b.c_str(),
                              static_cast<int>(b.size()), TRUE) == CSTR_EQUAL;
}

// V3-05 (規劃/25 §8): local fixed disk -> ReadDirectoryChangesW; remote path,
// --watch-poll or a failed Start() -> the legacy 2 s poll (code unchanged from
// v2.0). Called on every baseline and on navigation, so it is a no-op while the
// folder and the chosen source are still valid.
void UpdateWatchSource() {
  if (g_main_window == nullptr) return;
  const std::filesystem::path folder =
      g_state != nullptr ? g_state->folder.folder : std::filesystem::path();
  if (!folder.empty() && SameWatchFolder(folder, g_watch_source_folder) &&
      (!g_watch_uses_notify || g_dir_notify.running())) {
    return;
  }
  g_watch_source_folder = folder;

  const bool local =
      !folder.empty() && !g_watch_force_poll && !nv::IsRemotePath(folder);
  if (local && g_dir_notify.Start(g_main_window, folder)) {
    g_watch_uses_notify = true;
    g_watch_notify_deferred = false;
    // The notifier owns the cadence now; the 2 s timer would only add work.
    KillTimer(g_main_window, kWatchTimer);
    nv::platform::LogInfo(
        "watch: mode=notify folder=" + Narrow(folder.wstring()) +
        " merge=" + std::to_string(nv::platform::DirNotifier::kMergeMs));
    return;
  }

  // Fallback: remote path, forced poll, or Start() failed (logged with the
  // Win32 error by the notifier). This is exactly the v2.0 code path.
  g_dir_notify.Stop();
  g_watch_uses_notify = false;
  g_watch_notify_deferred = false;
  SetTimer(g_main_window, kWatchTimer, g_watch_interval_ms, nullptr);
  std::string line = "watch: mode=poll interval=" +
                     std::to_string(g_watch_interval_ms);
  if (folder.empty()) {
    line += " reason=no-folder";
  } else if (nv::IsRemotePath(folder)) {
    line += " reason=remote";
  } else if (g_watch_force_poll) {
    line += " reason=forced";
  } else {
    // Start() failed (logged with the Win32 error it kept).
    line += " reason=start-failed error=" + Narrow(g_dir_notify.error_text());
  }
  nv::platform::LogInfo(line);
}

// The single tick entry point for both sources. Poll mode keeps the old
// behaviour; notify mode only uses the timer while a change arrived that the
// last snapshot could not apply (paused UI, or the 2 s minimum-rescan window),
// retrying until it is consumed so a single notification is never lost.
void OnWatchTick(HWND hwnd) {
  if (!g_watch_uses_notify) {
    PollFolderWatch(hwnd);
    return;
  }
  if (!g_watch_notify_deferred) return;
  if (!PollFolderWatch(hwnd)) {
    // Still minimised / modal: keep the change pending and look again.
    SetTimer(hwnd, kWatchTimer, kWatchDeferredRetryMs, nullptr);
    return;
  }
  if (!g_watcher.rescan_deferred()) {
    g_watch_notify_deferred = false;
    return;
  }
  // The change landed inside the 2 s minimum-rescan window. The snapshot was
  // deliberately not adopted (that is what makes the poll retry it), and a
  // notification has no second chance: keep retrying until the gate opens.
  SetTimer(hwnd, kWatchTimer, kWatchDeferredRetryMs, nullptr);
}

// V3-05: the notifier coalesced a burst into one message. It runs the same
// snapshot -> FolderWatcher::Update -> rescan path as the poll (suppression,
// pause and own-write guards included), only without the 2 s wait.
void OnDirNotify(HWND hwnd) {
  if (!g_watch_uses_notify) return;
  if (!g_dir_notify.running()) {
    // The watcher thread died (share violation, handle revoked, ...). Fall back
    // to polling instead of silently stopping: same code path as a Start()
    // failure, just later in the session.
    nv::platform::LogInfo("watch: notify stopped, mode=poll interval=" +
                          std::to_string(g_watch_interval_ms));
    g_dir_notify.Stop();
    g_watch_uses_notify = false;
    g_watch_notify_deferred = false;
    SetTimer(hwnd, kWatchTimer, g_watch_interval_ms, nullptr);
    return;
  }
  nv::platform::LogInfo("watch: notify");
  g_watch_notify_deferred = true;
  OnWatchTick(hwnd);
}

// The list changed but the open file still exists: keep the picture, zoom and
// rotation, only refresh the list/index/rail (22/10 §3.1).
void ApplyWatchedFolderChange(HWND hwnd) {
  if (!g_state) return;
  auto& s = *g_state;
  const std::filesystem::path keep = s.path;
  const size_t previous_index = s.folder.index;
  if (!nv::RescanFolder(s.folder, keep)) {
    if (!s.folder.empty()) {
      if (keep.empty()) {
        // Empty state -> a supported image appeared: open the first one without
        // pretending that a current file was removed.
        nv::platform::LogInfo("watch: folder=appeared files=" +
                              std::to_string(s.folder.size()));
        g_empty_base = nv::ui::EmptyKind::kEmpty;
        OpenIndex(hwnd, 0);
        SyncWatchBaseline();
        return;
      }
      // The open file vanished in the same change: the removal branch owns the
      // "show the next image" decision.
      const size_t index = std::min(previous_index, s.folder.size() - 1);
      nv::platform::LogInfo("watch: current=removed-in-rescan");
      ShowToast(hwnd, nv::i18n::Text(nv::i18n::StringId::kToastFileRemoved));
      OpenIndex(hwnd, index);
      SyncWatchBaseline();
      return;
    }
    // No supported image left: the empty-folder state owns the screen.
    nv::platform::LogInfo("watch: folder=empty");
    s.strip.Clear();
    s.renderer.ClearImage();
    ResetPreviewState(hwnd);
    s.path.clear();
    s.current.reset();
    g_empty_base = nv::ui::EmptyKind::kFolder;
    UpdateTitle(hwnd);
    RefreshUi(hwnd, false);
    InvalidateRect(hwnd, nullptr, TRUE);
    SyncWatchBaseline();
    return;
  }

  if (s.thumbs) s.thumbs->CancelPending();
  s.strip.SetItems(s.folder.files);
  s.strip.SetViewWidth(static_cast<float>(s.renderer.client_width()));
  s.strip.SetCurrent(s.folder.index);
  s.strip.EnsureVisible(s.folder.index);
  char line[192]{};
  std::snprintf(line, sizeof(line), "watch: rescan files=%zu index=%zu",
                s.folder.size(), s.folder.index);
  nv::platform::LogInfo(line);
  UpdateTitle(hwnd);
  RefreshUi(hwnd, false);
  RequestVisibleThumbnails();
  InvalidateRect(hwnd, nullptr, TRUE);
  SyncWatchBaseline();
}

// The open file disappeared: move to the image that now occupies its slot (or
// the last one), never guess by size/timestamp (22/10 §3.1).
void HandleWatchedCurrentRemoved(HWND hwnd) {
  if (!g_state) return;
  auto& s = *g_state;
  const std::filesystem::path folder = s.folder.folder;
  const size_t previous_index = s.folder.index;
  nv::platform::LogInfo("watch: current=removed path=" + Narrow(s.path.wstring()));
  ShowToast(hwnd, nv::i18n::Text(nv::i18n::StringId::kToastFileRemoved));

  if (s.decode) s.decode->CancelAll();
  ResetPreviewState(hwnd);
  s.current.reset();
  s.path.clear();
  s.folder = nv::BuildFolderListFromFolder(folder);
  if (s.thumbs) s.thumbs->CancelPending();
  s.strip.SetItems(s.folder.files);

  if (s.folder.empty()) {
    s.strip.Clear();
    s.renderer.ClearImage();
    g_empty_base = nv::ui::EmptyKind::kFolder;
    g_pending_path.clear();
    nv::platform::LogInfo("empty: state=folder");
    UpdateTitle(hwnd);
    RefreshUi(hwnd, false);
    InvalidateRect(hwnd, nullptr, TRUE);
    SyncWatchBaseline();
    return;
  }

  const size_t next = std::min(previous_index, s.folder.size() - 1);
  char line[128]{};
  std::snprintf(line, sizeof(line), "watch: advance index=%zu files=%zu", next,
                s.folder.size());
  nv::platform::LogInfo(line);
  g_empty_base = nv::ui::EmptyKind::kEmpty;
  OpenIndex(hwnd, next);
  SyncWatchBaseline();
}

bool PollFolderWatch(HWND hwnd) {
  if (!g_state || g_state->folder.folder.empty()) return true;
  // Paused while the window is not on screen or a modal owns the UI (22/10
  // §3.3). Fullscreen keeps polling: the user may be watching a live folder.
  const bool paused =
      IsIconic(hwnd) != FALSE || !IsWindowVisible(hwnd) ||
      g_settings_window_open || g_confirm != ConfirmKind::kNone;
  if (paused) {
    if (!g_watch_pause_logged) {
      g_watch_pause_logged = true;
      nv::platform::LogInfo("watch: paused");
    }
    return false;
  }
  if (g_watch_pause_logged) {
    g_watch_pause_logged = false;
    nv::platform::LogInfo("watch: resumed");
  }

  const std::filesystem::path folder = g_state->folder.folder;
  const bool remote = nv::IsRemotePath(folder);
  // Remote drives trust the directory timestamp only; local drives sample the
  // entry count once every five polls as a fallback for file systems whose
  // directory timestamp is coarse (22/10 坑 5). A notification is already a
  // real change, so the count is always sampled on that path.
  const bool include_count =
      !remote && (g_watch_uses_notify || (g_watch_poll_count++ % 5 == 0));
  const nv::FolderSnapshot snapshot =
      nv::TakeFolderSnapshot(folder, g_state->path, include_count);
  const nv::WatchEvent event = g_watcher.Update(snapshot, NowMs());

  switch (event.type) {
    case nv::WatchEventType::kNone:
      return true;
    case nv::WatchEventType::kFolderChanged:
      ApplyWatchedFolderChange(hwnd);
      return true;
    case nv::WatchEventType::kCurrentRemoved:
      HandleWatchedCurrentRemoved(hwnd);
      return true;
    case nv::WatchEventType::kCurrentModified:
      nv::platform::LogInfo("watch: current=modified path=" +
                            Narrow(g_state->path.wstring()));
      ShowToast(hwnd, nv::i18n::Text(nv::i18n::StringId::kToastFileModified));
      return true;
  }
  return true;
}

// Transient note under the empty-state title. It never changes the base state,
// so after kEmptyNoteLifetimeMs the card returns to "empty" or "folder".
void ShowEmptyNote(HWND hwnd, const std::wstring& text) {
  if (text.empty()) return;
  g_empty_note = true;
  g_empty_note_text = text;
  nv::platform::LogInfo("empty: state=unsupported note=" + Narrow(text));
  if (hwnd != nullptr) {
    SetTimer(hwnd, kEmptyNoteTimer, kEmptyNoteLifetimeMs, nullptr);
    RefreshUi(hwnd, true);
  }
}

void ExpireEmptyNote(HWND hwnd) {
  if (!g_empty_note) return;
  g_empty_note = false;
  g_empty_note_text.clear();
  nv::platform::LogInfo("empty: note=cleared");
  RefreshUi(hwnd, true);
}

// One entry point for OS drops and the --simulate-drop hook, so the tested path
// is the shipped path (規劃/22/03 §9).
void OnDroppedPaths(HWND hwnd,
                    const std::vector<std::filesystem::path>& paths) {
  if (paths.empty()) return;
  // Multi-select opens the first item only; a folder first wins because that
  // is what Explorer's "open" does (規劃/22/03 §4.3).
  const std::filesystem::path path = paths.front();
  // Dropping our own drag back onto the window must not reopen the file that is
  // already showing (規劃/22/19 坑 12). The flag is only set while DoDragDrop
  // runs, so a normal drop of the same file from Explorer still works.
  if (!g_self_drag_path.empty() &&
      _wcsicmp(path.c_str(), g_self_drag_path.c_str()) == 0) {
    nv::platform::LogInfo("drag: self-drop ignored");
    return;
  }
  std::error_code ec;
  const bool directory = std::filesystem::is_directory(path, ec);
  const bool regular = std::filesystem::is_regular_file(path, ec);
  nv::platform::LogInfo("drop: path=" + Narrow(path.wstring()) +
                        " dir=" + (directory ? "1" : "0"));
  if (directory) {
    EnterFolder(hwnd, path);
    return;
  }

  const std::wstring extension = LowerExtension(path);
  if (regular && nv::FormatRouter::IsSupportedExtension(extension)) {
    OpenFile(hwnd, path);
    return;
  }

  wchar_t message[512]{};
  // A path that no longer exists (or never was a file) is reported by name;
  // an existing file with a bad extension by its extension.
  const std::wstring label =
      (regular && !extension.empty()) ? extension : path.filename().wstring();
  swprintf_s(message,
             nv::i18n::Text(nv::i18n::StringId::kEmptyUnsupported),
             label.c_str());
  // An unsupported drop never clears the picture that is already open (坑 6).
  if (g_state != nullptr && !g_state->path.empty() &&
      g_state->renderer.image_width() > 0) {
    ShowToast(hwnd, message);
    return;
  }
  ShowEmptyNote(hwnd, message);
}

// --simulate-ole-drop: builds a real CF_HDROP data object and drives the
// registered IDropTarget through DragEnter/DragOver/Drop. Unlike
// --simulate-drop this exercises the OLE layer itself (ReadPaths, effect
// negotiation and the hover transitions), which is what a user's drag does.
void PerformOleDrop(const std::filesystem::path& path) {
  if (!g_drop_target) {
    nv::platform::LogError("test: ole drop skipped=no_target");
    return;
  }
  PIDLIST_ABSOLUTE folder_pidl = nullptr;
  PIDLIST_ABSOLUTE item_pidl = nullptr;
  SFGAOF attributes = 0;
  const std::filesystem::path parent = path.parent_path();
  const HRESULT folder_parsed = parent.empty()
                                    ? E_INVALIDARG
                                    : SHParseDisplayName(parent.c_str(), nullptr,
                                                         &folder_pidl, 0,
                                                         &attributes);
  const HRESULT item_parsed =
      SHParseDisplayName(path.c_str(), nullptr, &item_pidl, 0, &attributes);
  if (FAILED(folder_parsed) || folder_pidl == nullptr ||
      FAILED(item_parsed) || item_pidl == nullptr) {
    nv::platform::LogError("test: ole drop path parse failed");
    if (folder_pidl != nullptr) CoTaskMemFree(folder_pidl);
    if (item_pidl != nullptr) CoTaskMemFree(item_pidl);
    return;
  }
  Microsoft::WRL::ComPtr<IDataObject> data;
  // SHCreateDataObject wants a parent folder and child PIDLs; the child here is
  // the last (and only) item of the file's own PIDL.
  PCUITEMID_CHILD child = ILFindLastID(item_pidl);
  const HRESULT created =
      SHCreateDataObject(folder_pidl, 1, &child, nullptr,
                         IID_PPV_ARGS(data.GetAddressOf()));
  CoTaskMemFree(folder_pidl);
  CoTaskMemFree(item_pidl);
  if (FAILED(created) || !data) {
    nv::platform::LogError("test: ole drop data object creation failed");
    return;
  }

  POINTL point{64, 64};
  DWORD effect = DROPEFFECT_NONE;
  const HRESULT entered =
      g_drop_target->DragEnter(data.Get(), MK_LBUTTON, point, &effect);
  char line[128]{};
  std::snprintf(line, sizeof(line), "test: ole DragEnter hr=0x%08lX effect=%lu",
                static_cast<unsigned long>(entered), effect);
  nv::platform::LogInfo(line);

  DWORD over = DROPEFFECT_NONE;
  g_drop_target->DragOver(MK_LBUTTON, point, &over);
  std::snprintf(line, sizeof(line), "test: ole DragOver effect=%lu", over);
  nv::platform::LogInfo(line);

  DWORD dropped = DROPEFFECT_NONE;
  const HRESULT drop =
      g_drop_target->Drop(data.Get(), MK_LBUTTON, point, &dropped);
  std::snprintf(line, sizeof(line), "test: ole Drop hr=0x%08lX effect=%lu",
                static_cast<unsigned long>(drop), dropped);
  nv::platform::LogInfo(line);
}

// --- v2.0 S3 / 22/19: drag-out ---------------------------------------------
// Copies the current file into an OLE drag (CF_HDROP + CF_UNICODETEXT) with a
// copy-only drop source. The dry-run variant is the automated hook: it runs the
// same selection/logging/cleanup path without entering DoDragDrop's nested
// message loop.
void StartFileDrag(HWND hwnd, const std::filesystem::path& path,
                   const char* source, size_t strip_index,
                   bool recenter = false) {
  g_drag_press = DragOutPress{};
  if (g_state != nullptr) g_state->dragging = false;
  // V3.1-06: a plain left press was panning the picture; leaving the window ends
  // that pan before the file drag starts, and the frame goes back to the middle
  // so returning to the window does not show a half-shoved picture.
  bool recentered = false;
  if (recenter && g_state != nullptr && g_state->renderer.ready()) {
    g_state->renderer.CenterView();
    recentered = true;
  }
  g_last_drag_source = source;
  g_last_drag_recentered = recentered;
  // DoDragDrop installs its own mouse capture; leaving ours behind is what
  // makes the window keep "holding" the pointer (規劃/22/19 坑 2).
  if (GetCapture() == hwnd) ReleaseCapture();
  if (g_ui != nullptr) {
    g_ui->ClearPointerState();
    g_ui->SetHover(nv::ui::HitId::kNone);
  }
  g_strip_hover_index = -1;
  HideStripTooltip(hwnd);
  if (path.empty()) {
    nv::platform::LogWarn("drag: out skipped=no_path");
    return;
  }

  DWORD effect = DROPEFFECT_NONE;
  if (g_test_drag_dry_run) {
    effect = DROPEFFECT_COPY;
  } else {
    g_self_drag_path = path;
    effect = nv::platform::BeginFileDrag(hwnd, path);
    g_self_drag_path.clear();
  }

  std::string line = "drag: out path=" + Narrow(path.wstring()) +
                     " source=" + source;
  if (strip_index != nv::render::ThumbnailStrip::kNone) {
    line += " index=" + std::to_string(strip_index);
  }
  line += " effect=" + std::to_string(static_cast<unsigned long>(effect));
  if (recentered) line += " pan=center";
  nv::platform::LogInfo(line);

  RefreshUi(hwnd, false);
  InvalidateRect(hwnd, nullptr, TRUE);
}

// Returns true when the press turned into a file drag (the caller must then stop
// processing the move: there is no pan and no hover update).
bool MaybeStartFileDrag(HWND hwnd, float x, float y) {
  if (!g_drag_press.active) return false;
  const float scale = g_ui != nullptr ? g_ui->dpi_scale() : 1.0f;
  nv::ui::DragOutRequest request;
  request.image_area = !g_drag_press.on_strip;
  request.strip_item = g_drag_press.on_strip;
  request.ctrl = g_drag_press.ctrl;
  request.dx = x - static_cast<float>(g_drag_press.anchor.x);
  request.dy = y - static_cast<float>(g_drag_press.anchor.y);
  request.threshold = nv::ui::kDragOutThresholdDip * scale;
  // V3.1-06 (§06b): plain left-drag pans; the file drag starts only once the
  // pointer has left the client area by 16 DIP, which is the moment the user is
  // clearly dragging *out* rather than looking at the picture.
  RECT client{};
  GetClientRect(hwnd, &client);
  const float outside_px = std::max(
      std::max(static_cast<float>(client.left) - x,
               x - static_cast<float>(client.right)),
      std::max(static_cast<float>(client.top) - y,
               y - static_cast<float>(client.bottom)));
  request.outside_dip = outside_px > 0.0f ? outside_px / scale : 0.0f;
  if (!nv::ui::ShouldStartDragOut(request)) return false;

  const std::filesystem::path path = g_drag_press.path;
  const bool on_strip = g_drag_press.on_strip;
  const size_t index = g_drag_press.strip_index;
  // A Ctrl press never panned, so only the leave-window path has a pan to end.
  StartFileDrag(hwnd, path, on_strip ? "strip" : "image", index,
                /*recenter=*/!g_drag_press.ctrl);
  return true;
}

void ShowDragHintOnce(HWND hwnd) {
  if (g_drag_hint_shown || g_test_drag_dry_run) return;
  g_drag_hint_shown = true;
  ShowToast(hwnd, nv::i18n::Text(nv::i18n::StringId::kToastDragHint));
}

nv::Image* CurrentImage() {
  if (!g_state || !g_state->current || g_state->current->Empty()) return nullptr;
  return g_state->current.get();
}

// --- View rotation for exports (規劃/22/05 §3.7) ---------------------------
// Save-as/copy/wallpaper must match what the user sees. The decoded pixels and
// the caches stay upright; only a temporary copy is rotated for the export, so
// rotation never leaks into ImageInfo or the caches.
const nv::Image* ViewImage(const nv::Image& source, nv::Image* scratch) {
  if (g_state == nullptr || g_state->renderer.rotation() == 0) return &source;
  *scratch = source;
  // Exports are single frames; do not carry a whole animation in the copy.
  scratch->frames.clear();
  scratch->loop_count = 0;
  nv::RotateImageForView(scratch, g_state->renderer.rotation());
  return scratch;
}

bool SaveImageWithView(const nv::Image& source,
                       const std::filesystem::path& target,
                       nv::platform::ImageSaveFormat format, int quality,
                       std::wstring* error) {
  nv::Image rotated;
  const nv::Image* output = ViewImage(source, &rotated);
  return nv::platform::SaveImage(target, *output, format, quality, error);
}

// Builds an operation error from the platform layer's message. The technical
// text stays in the detail line; the card shows the localized message.
nv::UiError OperationError(nv::ErrorKind fallback,
                           const std::filesystem::path& path,
                           const std::wstring& text,
                           nv::i18n::StringId message_id =
                               nv::i18n::StringId::kCount) {
  const nv::Error error = nv::Error::Make(0, Narrow(text));
  return nv::ClassifyOperationError(fallback, error, path, message_id);
}

void OpenFileDialog(HWND hwnd) {
  std::vector<wchar_t> file(32768, L'\0');
  std::wstring initial_dir;
  if (g_state && !g_state->path.empty()) {
    initial_dir = g_state->path.parent_path().wstring();
  }

  OPENFILENAMEW dialog{};
  dialog.lStructSize = sizeof(dialog);
  dialog.hwndOwner = hwnd;
  const std::wstring filter = BuildOpenFilter();
  dialog.lpstrFilter = filter.c_str();
  dialog.lpstrFile = file.data();
  dialog.nMaxFile = static_cast<DWORD>(file.size());
  dialog.lpstrInitialDir = initial_dir.empty() ? nullptr : initial_dir.c_str();
  dialog.lpstrTitle = nv::i18n::Text(nv::i18n::StringId::kDialogOpenImage);
  dialog.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_EXPLORER;
  if (GetOpenFileNameW(&dialog) != FALSE) {
    OpenFile(hwnd, std::filesystem::path(file.data()));
  }
}

// Folder picker for the empty card. IFileOpenDialog + FOS_PICKFOLDERS; COM is
// initialized (OLE) on this thread for the whole GUI lifetime.
std::optional<std::filesystem::path> ChooseFolder(HWND hwnd) {
  Microsoft::WRL::ComPtr<IFileOpenDialog> dialog;
  if (FAILED(CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER,
                              IID_PPV_ARGS(dialog.GetAddressOf())))) {
    nv::platform::LogError("open folder: IFileOpenDialog creation failed");
    return std::nullopt;
  }
  FILEOPENDIALOGOPTIONS options{};
  if (SUCCEEDED(dialog->GetOptions(&options))) {
    dialog->SetOptions(options | FOS_PICKFOLDERS | FOS_FORCEFILESYSTEM |
                       FOS_PATHMUSTEXIST);
  }
  dialog->SetTitle(nv::i18n::Text(nv::i18n::StringId::kEmptyOpenFolder));
  if (FAILED(dialog->Show(hwnd))) return std::nullopt;

  Microsoft::WRL::ComPtr<IShellItem> item;
  if (FAILED(dialog->GetResult(item.GetAddressOf())) || !item) {
    return std::nullopt;
  }
  PWSTR raw = nullptr;
  if (FAILED(item->GetDisplayName(SIGDN_FILESYSPATH, &raw)) || raw == nullptr) {
    return std::nullopt;
  }
  std::filesystem::path folder(raw);
  CoTaskMemFree(raw);
  return folder;
}

// Runs one empty-card button. The keyboard (Tab/Enter) and the mouse both land
// here, so the command exists once (S1-02 decision).
void ActivateEmptyAction(HWND hwnd, int action) {
  const char* name = action == 0 ? "primary"
                     : action == 1 ? "folder"
                                   : "formats";
  nv::platform::LogInfo(std::string("empty: activate=") + name);
  switch (action) {
    case 0:
      OpenFileDialog(hwnd);
      break;
    case 1:
      if (const auto folder = ChooseFolder(hwnd)) EnterFolder(hwnd, *folder);
      break;
    case 2:
      if (g_ui != nullptr) {
        g_ui->overlay().OpenModal(nv::ui::OverlayKind::kFormats);
        RefreshUi(hwnd, true);
      }
      break;
    default:
      break;
  }
}

// Shared by the Save-as dialog and the --save-as/--save-to test hooks. The
// extension picks the encoder; the saved pixels follow the current view
// rotation. The product path uses StartSave() instead so the encode runs off
// the UI thread (規劃/22/14 §4); this synchronous version is what the hooks and
// the "no quality" formats use.
bool SaveCurrentImageTo(HWND hwnd, const std::filesystem::path& target,
                        bool notify, int quality = 90) {
  // Only the preview exists: exporting it would write a 128 px image
  // (規劃/22/13 §3.2). The toast explains why nothing was saved.
  if (BlockExportWhilePreview(hwnd)) return false;
  // §7.6: a save writes file pixels, so a display tier is replaced by a full
  // decode first (logged as "export: forced full decode").
  if (!EnsureFullForExport(hwnd, ExportPixels::kFile)) return false;
  nv::Image* image = CurrentImage();
  if (image == nullptr) {
    MessageBeep(MB_ICONWARNING);
    return false;
  }
  nv::platform::ImageSaveFormat format = nv::platform::ImageSaveFormat::kPng;
  if (!nv::platform::TryImageFormatFromPath(target, &format) ||
      !nv::platform::EncoderAvailable(format)) {
    // A wrong extension is a usage mistake, not a failed save: a danger toast
    // keeps the user in place without a modal card (規劃/22/11 §4.1).
    ShowErrorToast(hwnd,
                   nv::i18n::Text(nv::i18n::StringId::kErrorUnsupportedSaveExtension));
    return false;
  }

  std::wstring error;
  if (!SaveImageWithView(*image, target, format, quality, &error)) {
    ShowError(hwnd, OperationError(nv::ErrorKind::kSaveFailed, target, error,
                                   nv::i18n::StringId::kErrorSaveImage));
    return false;
  }
  char line[512]{};
  std::error_code size_ec;
  const uintmax_t bytes = std::filesystem::file_size(target, size_ec);
  std::snprintf(line, sizeof(line),
                "saved image: %s rotation=%d format=%s quality=%d bytes=%llu",
                Narrow(target.wstring()).c_str(),
                g_state ? g_state->renderer.rotation() : 0,
                nv::platform::SaveFormatToken(format), quality,
                static_cast<unsigned long long>(size_ec ? 0 : bytes));
  nv::platform::LogInfo(line);
  // Our own write must not be reported as "the file was modified", and a save
  // to the same folder should show up in the list immediately instead of two
  // seconds later (22/10 §3.1 suppression window).
  if (g_state != nullptr && !g_state->folder.folder.empty() &&
      target.parent_path().lexically_normal() ==
          g_state->folder.folder.lexically_normal()) {
    g_watcher.Suppress(NowMs());
    if (!g_state->path.empty()) {
      ApplyWatchedFolderChange(hwnd);
    }
  }
  if (notify) {
    const bool animated = image->frames.size() > 1;
    ShowToast(hwnd, nv::i18n::Text(animated
                                       ? nv::i18n::StringId::kToastImageSavedFrame
                                       : nv::i18n::StringId::kToastImageSaved));
  }
  return true;
}

// --- v2.0 S3 / 22/14: save-options card and background save -----------------

nv::i18n::StringId SaveFilterId(nv::platform::ImageSaveFormat format) {
  switch (format) {
    case nv::platform::ImageSaveFormat::kJpeg:
      return nv::i18n::StringId::kFilterJpeg;
    case nv::platform::ImageSaveFormat::kBmp:
      return nv::i18n::StringId::kFilterBmp;
    case nv::platform::ImageSaveFormat::kTiff:
      return nv::i18n::StringId::kFilterTiff;
    case nv::platform::ImageSaveFormat::kWebp:
      return nv::i18n::StringId::kFilterWebp;
    case nv::platform::ImageSaveFormat::kGif:
      return nv::i18n::StringId::kFilterGif;
    case nv::platform::ImageSaveFormat::kPng:
    default:
      return nv::i18n::StringId::kFilterPng;
  }
}

// Filter rows for the system save dialog: only formats the startup probe could
// actually encode, so an unavailable option never reaches the user (坑 1).
std::wstring BuildSaveFilter(
    std::vector<nv::platform::ImageSaveFormat>* formats) {
  static constexpr nv::platform::ImageSaveFormat kOrder[] = {
      nv::platform::ImageSaveFormat::kPng,
      nv::platform::ImageSaveFormat::kJpeg,
      nv::platform::ImageSaveFormat::kBmp,
      nv::platform::ImageSaveFormat::kTiff,
      nv::platform::ImageSaveFormat::kWebp,
      nv::platform::ImageSaveFormat::kGif,
  };
  std::wstring filter;
  if (formats != nullptr) formats->clear();
  for (const nv::platform::ImageSaveFormat format : kOrder) {
    if (!nv::platform::EncoderAvailable(format)) continue;
    if (formats != nullptr) formats->push_back(format);
    filter += nv::i18n::Text(SaveFilterId(format));
    filter.push_back(L'\0');
    filter += nv::platform::SaveFormatPatterns(format);
    filter.push_back(L'\0');
  }
  filter.push_back(L'\0');
  return filter;
}

void StartSave(HWND hwnd, const std::filesystem::path& target,
               nv::platform::ImageSaveFormat format, int quality,
               bool notify);

// Starts the worker encode. `notify` is unused by the async path (the toast is
// shown when the outcome arrives) but keeps the call sites symmetrical.
void StartSave(HWND hwnd, const std::filesystem::path& target,
               nv::platform::ImageSaveFormat format, int quality,
               bool notify) {
  (void)notify;
  if (BlockExportWhilePreview(hwnd)) return;
  if (!EnsureFullForExport(hwnd, ExportPixels::kFile)) return;
  if (!g_state || !g_state->current || g_state->current->Empty()) {
    MessageBeep(MB_ICONWARNING);
    return;
  }
  if (!nv::platform::EncoderAvailable(format)) {
    ShowErrorToast(hwnd,
                   nv::i18n::Text(nv::i18n::StringId::kErrorUnsupportedSaveExtension));
    return;
  }
  if (g_save_service == nullptr || g_save_busy) return;
  if (!g_save_service->Save(g_state->current, g_state->renderer.rotation(),
                            target, format, quality)) {
    return;
  }
  g_save_busy = true;
  // The loading chip doubles as the save progress: the same 400 ms delay keeps
  // a small save from flashing it (規劃/22/14 §5).
  BeginLoading(hwnd, false);
  char line[256]{};
  std::snprintf(line, sizeof(line), "save: start format=%s quality=%d path=%s",
                nv::platform::SaveFormatToken(format), quality,
                Narrow(target.wstring()).c_str());
  nv::platform::LogInfo(line);
  RefreshUi(hwnd, false);
}

void SaveAsDialog(HWND hwnd, const std::filesystem::path& suggested_path) {
  if (BlockExportWhilePreview(hwnd)) return;
  nv::Image* image = CurrentImage();
  if (image == nullptr || !g_state) {
    MessageBeep(MB_ICONWARNING);
    return;
  }

  auto& state = *g_state;
  std::vector<nv::platform::ImageSaveFormat> formats;
  const std::wstring filter = BuildSaveFilter(&formats);
  if (formats.empty()) {
    ShowErrorToast(hwnd,
                   nv::i18n::Text(nv::i18n::StringId::kErrorSaveImage));
    return;
  }

  // The source format is only offered as "use original image format" when the
  // encoder exists; otherwise the checkbox is hidden by the dialog.
  nv::platform::ImageSaveFormat source_format =
      nv::platform::ImageSaveFormat::kPng;
  const bool source_writable =
      nv::platform::TryImageFormatFromPath(state.path, &source_format) &&
      nv::platform::EncoderAvailable(source_format);

  nv::platform::ImageSaveFormat default_format =
      source_writable ? source_format : nv::platform::ImageSaveFormat::kPng;
  size_t default_index = 0;
  bool found = false;
  for (size_t i = 0; i < formats.size(); ++i) {
    if (formats[i] == default_format) {
      default_index = i;
      found = true;
      break;
    }
  }
  if (!found) {
    default_format = formats.front();
    default_index = 0;
  }

  std::filesystem::path target = suggested_path;
  if (target.empty()) {
    std::wstring stem = state.path.stem().wstring();
    if (stem.empty()) stem = L"image";
    target = state.path.parent_path() /
             (stem + nv::platform::SaveFormatExtension(default_format));
  }

  std::error_code size_ec;
  uint64_t source_bytes = 0;
  if (!state.path.empty()) {
    source_bytes = std::filesystem::file_size(state.path, size_ec);
    if (size_ec) source_bytes = 0;
  }

  nv::platform::SaveAsRequest request;
  request.image = state.current;
  request.suggested_path = target;
  request.source_path = state.path;
  request.source_format = source_format;
  request.initial_quality = 90;
  request.rotation = state.renderer.rotation();
  request.source_unmodified = (request.rotation % 4) == 0;
  request.source_bytes = source_bytes;
  request.owner = hwnd;
  request.filter = filter;
  request.formats = formats;
  request.default_format_index = default_index;
  request.title = nv::i18n::Text(nv::i18n::StringId::kDialogSaveAs);

  const nv::platform::SaveAsOutcome outcome =
      nv::platform::ShowSaveAsDialog(request);
  if (!outcome.accepted) return;

  if (outcome.byte_copy) {
    // "Use original image format" on an untouched source: copy the file bytes
    // instead of re-encoding (no generation loss, estimate == source size).
    std::error_code copy_ec;
    std::filesystem::copy_file(state.path, outcome.path,
                               std::filesystem::copy_options::overwrite_existing,
                               copy_ec);
    if (copy_ec) {
      ShowError(hwnd, OperationError(nv::ErrorKind::kSaveFailed, outcome.path,
                                     Widen(copy_ec.message()),
                                     nv::i18n::StringId::kErrorSaveImage));
      return;
    }
    std::error_code bytes_ec;
    const uintmax_t bytes = std::filesystem::file_size(outcome.path, bytes_ec);
    char line[512]{};
    std::snprintf(line, sizeof(line),
                  "save: done format=%s bytes=%llu ms=0 animated=0 path=%s",
                  nv::platform::SaveFormatToken(outcome.format),
                  static_cast<unsigned long long>(bytes_ec ? 0 : bytes),
                  Narrow(outcome.path.wstring()).c_str());
    nv::platform::LogInfo(line);
    ShowToast(hwnd, nv::i18n::Text(nv::i18n::StringId::kToastImageSaved));
    if (!state.folder.folder.empty() &&
        outcome.path.parent_path().lexically_normal() ==
            state.folder.folder.lexically_normal()) {
      g_watcher.Suppress(NowMs());
      if (!state.path.empty()) ApplyWatchedFolderChange(hwnd);
    }
  } else {
    StartSave(hwnd, outcome.path, outcome.format, outcome.quality, true);
  }
  RefreshUi(hwnd, false);
}

void CopyCurrentImage(HWND hwnd) {
  if (BlockExportWhilePreview(hwnd)) return;
  // §7.6: the clipboard must carry file pixels, not a tier.
  if (!EnsureFullForExport(hwnd, ExportPixels::kFile)) return;
  nv::Image* image = CurrentImage();
  if (image == nullptr) {
    MessageBeep(MB_ICONWARNING);
    return;
  }
  nv::Image rotated;
  const nv::Image* output = ViewImage(*image, &rotated);
  std::wstring error;
  if (!nv::platform::CopyImageToClipboard(hwnd, *output, &error)) {
    ShowErrorToast(hwnd,
                   nv::i18n::Text(nv::i18n::StringId::kErrorCopyImage), error);
    return;
  }
  // V3.1-06 (§06d): the success used to be silent — only the failure spoke.
  ShowToastAfterMenu(hwnd, nv::i18n::StringId::kToastImageCopied);
  nv::platform::LogInfo("copied image: rotation=" +
                        std::to_string(g_state->renderer.rotation()));
}

void CopyCurrentPath(HWND hwnd) {
  if (!g_state || g_state->path.empty()) {
    MessageBeep(MB_ICONWARNING);
    return;
  }
  std::wstring error;
  if (!nv::platform::CopyTextToClipboard(hwnd, g_state->path.wstring(), &error)) {
    ShowErrorToast(hwnd,
                   nv::i18n::Text(nv::i18n::StringId::kErrorCopyPath), error);
    return;
  }
  ShowToastAfterMenu(hwnd, nv::i18n::StringId::kToastPathCopied);
}

// V3.1-06 (§06d): queue a toast for the next message-loop turn. The context menu
// command runs while the popup is still closing, so the confirmation must not be
// painted in the same turn.
void ShowToastAfterMenu(HWND hwnd, nv::i18n::StringId id) {
  if (hwnd == nullptr) return;
  if (!PostMessageW(hwnd, kDeferredToast, static_cast<WPARAM>(id), 0)) {
    // Posting can only fail when the window is gone; show it now is pointless.
    return;
  }
}

// --- Title bar segment clicks (規劃/22/02 §3.1) ----------------------------
// The title bar stays HTCAPTION so dragging, double-click maximize, the system
// menu and Alt+Space remain native. A *click* (press and release without the
// window moving) is detected through the enter/exit size-move pair the system
// sends around its caption modal loop, which keeps every native gesture intact.

struct TitlePress {
  bool valid = false;
  // True once the button was released and the double-click window is running.
  bool confirmed = false;
  POINT client{};
  RECT window{};
};
TitlePress g_title_press;

// Formatted text of one title segment, or an empty string when the segment has
// nothing copyable.
std::wstring TitleSegmentText(nv::ui::TitleSegment segment) {
  if (g_state == nullptr) return {};
  const nv::render::Renderer& renderer = g_state->renderer;
  switch (segment) {
    case nv::ui::kSegFileName:
      return g_state->path.wstring();
    case nv::ui::kSegDimension: {
      if (renderer.image_width() == 0) return {};
      wchar_t buffer[64]{};
      swprintf_s(buffer, L"%u \u00D7 %u", renderer.image_width(),
                 renderer.image_height());
      return buffer;
    }
    case nv::ui::kSegFileSize:
      return FormatFileSize(CurrentFileSize());
    default:
      // v3.0 removed the index and the percentage from the caption; the bottom
      // bar owns both (規劃/26 §2.2).
      return {};
  }
}

const char* TitleSegmentName(nv::ui::TitleSegment segment) {
  switch (segment) {
    case nv::ui::kSegFileName:
      return "file_name";
    case nv::ui::kSegDimension:
      return "dimension";
    case nv::ui::kSegFileSize:
      return "file_size";
    default:
      return "none";
  }
}

// Runs the documented click action for the segment under `x`. Returns true when
// the click was consumed by the title bar.
bool HandleTitleSegmentClick(HWND hwnd, float x, float y) {
  if (g_ui == nullptr) return false;
  const nv::ui::TitleLayout& title = g_ui->title();
  for (size_t i = 0; i < nv::ui::kSegCount; ++i) {
    if (!title.segment_visible[i] || title.segment[i].empty()) continue;
    if (!title.segment[i].Contains(x, y)) continue;

    const auto segment = static_cast<nv::ui::TitleSegment>(i);
    nv::platform::LogInfo(std::string("ui: title_click=") +
                          TitleSegmentName(segment));

    const std::wstring text = TitleSegmentText(segment);
    if (text.empty()) return true;
    std::wstring error;
    if (!nv::platform::CopyTextToClipboard(hwnd, text, &error)) {
      ShowErrorToast(hwnd,
                     nv::i18n::Text(nv::i18n::StringId::kErrorCopyPath), error);
      return true;
    }
    const nv::i18n::StringId toast = segment == nv::ui::kSegFileName
                                         ? nv::i18n::StringId::kToastPathCopied
                                     : segment == nv::ui::kSegDimension
                                         ? nv::i18n::StringId::kToastDimensionCopied
                                         : nv::i18n::StringId::kToastFileSizeCopied;
    ShowToast(hwnd, nv::i18n::Text(toast));
    return true;
  }
  return false;
}

bool DeleteCurrentFile(HWND hwnd) {
  if (!g_state || g_state->path.empty()) return false;
  auto& state = *g_state;

  const size_t index = state.folder.index;
  const std::filesystem::path directory = state.folder.folder;
  // Our own write must never look like an external change (22/10 坑 1).
  g_watcher.Suppress(NowMs());
  std::wstring error;
  if (!nv::platform::DeleteToRecycleBin(state.path, &error)) {
    ShowError(hwnd, OperationError(nv::ErrorKind::kUnknown, state.path, error,
                                   nv::i18n::StringId::kErrorDelete));
    return false;
  }
  nv::platform::LogInfo("deleted image: " + Narrow(state.path.wstring()));
  ShowToast(hwnd, nv::i18n::Text(nv::i18n::StringId::kToastFileDeleted));

  if (state.decode) state.decode->CancelAll();
  ResetPreviewState(hwnd);
  state.current.reset();
  state.path.clear();
  state.folder = nv::BuildFolderList(directory / L"__nv_deleted__.jpg");
  if (state.thumbs) state.thumbs->CancelPending();
  state.strip.SetItems(state.folder.files);

  if (state.folder.empty()) {
    state.strip.Clear();
    state.renderer.ClearImage();
    // The last image is gone: the folder now has no supported images, which is
    // the folder-empty state, not "nothing was ever opened" (規劃/22/03 §4.4).
    g_empty_base = nv::ui::EmptyKind::kFolder;
    g_empty_note = false;
    g_pending_path.clear();
    nv::platform::LogInfo("empty: state=folder");
    RefreshUi(hwnd, false);
    UpdateTitle(hwnd);
    InvalidateRect(hwnd, nullptr, TRUE);
    SyncWatchBaseline();
    return true;
  }

  const size_t next = std::min(index, state.folder.size() - 1);
  OpenIndex(hwnd, next);
  SyncWatchBaseline();
  return true;
}

void DeleteCurrent(HWND hwnd) {
  if (!g_state || g_state->path.empty()) return;
  if (g_ui == nullptr) return;
  // Dangerous actions go through the in-window confirmation card instead of a
  // modal MessageBox (規劃/22/11 §4.3 / §5).
  if (!g_ui->overlay().OpenModal(nv::ui::OverlayKind::kConfirm)) return;
  g_confirm = ConfirmKind::kDelete;
  nv::platform::LogInfo("confirm: open=delete path=" +
                        Narrow(g_state->path.wstring()));
  RefreshUi(hwnd, true);
  InvalidateRect(hwnd, nullptr, TRUE);
}

void ShowWrapPrompt(HWND hwnd, bool forward,
                    const std::filesystem::path& target) {
  if (g_ui == nullptr) return;
  if (!g_ui->overlay().OpenModal(nv::ui::OverlayKind::kConfirm)) return;
  g_confirm = forward ? ConfirmKind::kWrapNext : ConfirmKind::kWrapPrev;
  g_wrap_forward = forward;
  g_wrap_remember = false;
  g_wrap_target = target;
  nv::platform::LogInfo(std::string("confirm: open=wrap ") +
                        (forward ? "next" : "prev") +
                        " target=" + Narrow(target.filename().wstring()));
  RefreshUi(hwnd, true);
  InvalidateRect(hwnd, nullptr, TRUE);
}

void ShowWrapConfirm(HWND hwnd, bool forward) {
  ShowWrapPrompt(hwnd, forward, {});
}

void CloseConfirm(HWND hwnd, bool run_primary) {
  if (g_confirm == ConfirmKind::kNone) return;
  const ConfirmKind kind = g_confirm;
  const bool wrap =
      kind == ConfirmKind::kWrapNext || kind == ConfirmKind::kWrapPrev;
  const bool remember = wrap && g_wrap_remember;
  const bool forward = kind == ConfirmKind::kWrapNext;
  const std::filesystem::path target = g_wrap_target;
  g_confirm = ConfirmKind::kNone;
  CloseWrapState();
  if (g_ui != nullptr) g_ui->overlay().CloseModal();
  nv::platform::LogInfo(std::string("confirm: action=") +
                        (run_primary ? "primary" : "cancel") +
                        " remember=" + (remember ? "1" : "0"));
  RefreshUi(hwnd, true);
  InvalidateRect(hwnd, nullptr, TRUE);
  if (remember) {
    // Enter/Open remembers "cross folders"; staying remembers "loop". The new
    // policy is written immediately and also applies to the rest of this
    // session (22/10 §4.3).
    g_settings.folder_wrap = run_primary ? L"next" : L"loop";
    if (!SaveSettingsNow()) {
      ShowErrorToast(hwnd,
                     nv::i18n::Text(nv::i18n::StringId::kSettingsSaveFailed));
    }
    nv::platform::LogInfo("nav: wrap_remember=" +
                          Narrow(g_settings.folder_wrap));
  }
  if (!run_primary) return;
  if (kind == ConfirmKind::kDelete) {
    DeleteCurrentFile(hwnd);
  } else if (wrap && g_state != nullptr && !g_state->folder.empty()) {
    if (!target.empty()) {
      nv::platform::LogInfo(std::string("nav: wrap=") +
                            (forward ? "next-folder" : "prev-folder"));
      EnterAdjacentFolder(hwnd, target, !forward);
    } else {
      nv::platform::LogInfo(std::string("nav: wrap=") +
                            (forward ? "first" : "last"));
      OpenIndex(hwnd, forward ? 0 : g_state->folder.size() - 1);
    }
  }
}

// Title bar segment clicks end up here too; a failure is a toast, not a card,
// because the user's action (copy) can simply be repeated.

bool RenameCurrentTo(HWND hwnd, const std::wstring& input_name) {
  if (!g_state || g_state->path.empty()) return false;
  auto& state = *g_state;
  const std::filesystem::path old_path = state.path;

  const std::wstring name = TrimText(input_name);
  if (name.empty()) return false;

  std::filesystem::path target = old_path.parent_path() / name;
  if (!target.has_extension()) {
    target = old_path.parent_path() / (name + old_path.extension().wstring());
  }
  if (!IsValidFileName(target.filename().wstring())) {
    ShowErrorToast(
        hwnd, nv::i18n::Text(nv::i18n::StringId::kErrorInvalidFileName));
    return false;
  }
  if (!nv::FormatRouter::IsSupportedExtension(LowerExtension(target))) {
    ShowErrorToast(
        hwnd,
        nv::i18n::Text(nv::i18n::StringId::kErrorUnsupportedRenameExtension));
    return false;
  }

  if (old_path == target) return false;
  const bool case_only = _wcsicmp(old_path.c_str(), target.c_str()) == 0;
  std::error_code ec;
  if (!case_only && std::filesystem::exists(target, ec)) {
    ShowErrorToast(hwnd,
                   nv::i18n::Text(nv::i18n::StringId::kErrorFileExists));
    return false;
  }

  std::wstring error;
  g_watcher.Suppress(NowMs());
  if (!nv::platform::RenamePath(old_path, target, &error)) {
    ShowError(hwnd, OperationError(nv::ErrorKind::kUnknown, old_path, error,
                                   nv::i18n::StringId::kErrorRename));
    return false;
  }
  nv::platform::LogInfo("renamed image: " + Narrow(old_path.wstring()) + " -> " +
                        Narrow(target.wstring()));
  OpenFile(hwnd, target);
  SyncWatchBaseline();
  return true;
}

// --- V3.1-06 §06e: rename overlay ------------------------------------------
//
// The v2.0 rename used nv::platform::PromptForText: a separate Win32 window with
// native EDIT/BUTTON, so it looked nothing like the rest of the UI. The overlay
// below reuses the confirmation card's shell. The D3D11 swap chain is bound to the
// window (`CreateSwapChainForHwnd`), and a WS_CHILD control cannot be drawn above
// it, so the text box is a borderless top-level EDIT owned by the main window and
// placed exactly over `rename_card().input`: the card draws the box and its accent
// border, the EDIT draws only the text/caret/IME composition, and typing,
// selection, the clipboard and IME all stay native.
namespace {
constexpr UINT kRenameKey = WM_APP + 10;
constexpr int kRenameEditId = 4201;

// The EDIT is a top-level popup, so Enter/Escape arrive here and not at the main
// window. Forward them and swallow the single-line beep. (A WNDPROC swap, not
// SetWindowSubclass: comctl32 is not on this target's link line.)
WNDPROC g_rename_edit_prev_proc = nullptr;
LRESULT CALLBACK RenameEditProc(HWND edit, UINT msg, WPARAM wp, LPARAM lp) {
  if (msg == WM_KEYDOWN && (wp == VK_RETURN || wp == VK_ESCAPE)) {
    PostMessageW(GetParent(edit), kRenameKey, wp, 0);
    return 0;
  }
  if (msg == WM_CHAR && (wp == VK_RETURN || wp == VK_ESCAPE)) return 0;
  return CallWindowProcW(g_rename_edit_prev_proc, edit, msg, wp, lp);
}

void EnsureRenameEdit(HWND hwnd) {
  if (g_rename_font == nullptr) {
    HDC dc = GetDC(hwnd);
    const int height =
        -MulDiv(11, GetDeviceCaps(dc, LOGPIXELSY), 72);  // ~15 px at 100 %
    ReleaseDC(hwnd, dc);
    // V3.1-13: "Segoe UI" carries no CJK glyphs, so a Chinese file name was
    // rendered as blank spaces and only the ".jpg" tail stayed visible - the
    // user read that as "the text is hidden on the left". Use the shell's message
    // font (Microsoft YaHei UI on a zh-CN system), which covers CJK and matches
    // the rest of the UI.
    NONCLIENTMETRICSW metrics{};
    metrics.cbSize = sizeof(metrics);
    if (SystemParametersInfoW(SPI_GETNONCLIENTMETRICS, sizeof(metrics), &metrics,
                              0)) {
      LOGFONTW font = metrics.lfMessageFont;
      font.lfHeight = height;
      font.lfWeight = FW_NORMAL;
      font.lfQuality = CLEARTYPE_QUALITY;
      font.lfCharSet = DEFAULT_CHARSET;
      g_rename_font = CreateFontIndirectW(&font);
    }
    if (g_rename_font == nullptr) {
      g_rename_font = CreateFontW(
          height, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
          OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
          DEFAULT_PITCH | FF_DONTCARE, L"Microsoft YaHei UI");
    }
  }
  const uint32_t fill = g_ui != nullptr ? g_ui->colors().bg_panel_inactive
                                        : 0xFF16181B;
  if (g_rename_brush == nullptr) {
    g_rename_brush =
        CreateSolidBrush(RGB((fill >> 16) & 0xFF, (fill >> 8) & 0xFF,
                             fill & 0xFF));
  }
  if (g_rename_edit == nullptr) {
    // hMenu must be null for a top-level window: a non-null value is treated as
    // an HMENU there (the control-id form only exists for WS_CHILD), which made
    // CreateWindowEx fail with ERROR_INVALID_MENU_HANDLE.
    g_rename_edit = CreateWindowExW(
        WS_EX_TOOLWINDOW, L"EDIT", L"",
        WS_POPUP | WS_VISIBLE | ES_AUTOHSCROLL | ES_LEFT, 0, 0, 10, 10, hwnd,
        nullptr, nullptr, nullptr);
    if (g_rename_edit == nullptr) {
      nv::platform::LogError("ui: rename edit create failed err=" +
                             std::to_string(GetLastError()));
      return;
    }
    SendMessageW(g_rename_edit, WM_SETFONT,
                 reinterpret_cast<WPARAM>(g_rename_font), TRUE);
    g_rename_edit_prev_proc = reinterpret_cast<WNDPROC>(SetWindowLongPtrW(
        g_rename_edit, GWLP_WNDPROC,
        reinterpret_cast<LONG_PTR>(RenameEditProc)));
    nv::platform::LogInfo("ui: rename edit created");
  }
}

void DestroyRenameEdit() {
  if (g_rename_edit != nullptr) {
    DestroyWindow(g_rename_edit);
    g_rename_edit = nullptr;
  }
}
}  // namespace

bool RenameOverlayOpen() {
  return g_ui != nullptr && g_ui->overlay().IsModal(nv::ui::OverlayKind::kRename);
}

void OpenRenameOverlay(HWND hwnd) {
  if (!g_state || g_state->path.empty() || g_ui == nullptr) return;
  if (!g_ui->overlay().OpenModal(nv::ui::OverlayKind::kRename)) return;
  g_rename_error.clear();
  EnsureRenameEdit(hwnd);
  // V3.1-13: size/margins first, *then* the text. `EM_SETMARGINS` re-lays the
  // control out and re-scrolls it to the caret, so doing it after the selection
  // put a long name back to its tail ("text hidden on the left").
  PositionRenameEdit(hwnd);
  const std::wstring name = g_state->path.filename().wstring();
  const std::wstring initial =
      g_rename_has_test_input ? g_test_rename_input : name;
  g_rename_has_test_input = false;
  if (g_rename_edit != nullptr) {
    SetWindowTextW(g_rename_edit, initial.c_str());
    // Select the base name, not the extension: typing replaces "picture" but
    // keeps ".jpg" (the native dialog did the same).
    const size_t dot = initial.rfind(L'.');
    const int end = static_cast<int>(dot == std::wstring::npos ? initial.size()
                                                              : dot);
    // SetWindowTextW leaves the caret at the end and the single-line EDIT keeps
    // that horizontal scroll. Park the caret at 0, scroll it into view, and only
    // then select the base name (EM_SETSEL itself does not scroll).
    SendMessageW(g_rename_edit, EM_SETSEL, 0, 0);
    SendMessageW(g_rename_edit, EM_SCROLLCARET, 0, 0);
    SendMessageW(g_rename_edit, EM_SETSEL, 0, end);
    SetFocus(g_rename_edit);
  }
  nv::platform::LogInfo("ui: rename overlay open name=" + Narrow(name));
  RefreshUi(hwnd, true);
  InvalidateRect(hwnd, nullptr, TRUE);
}

void CancelRenameOverlay(HWND hwnd) {
  if (!RenameOverlayOpen()) return;
  g_ui->overlay().CloseModal();
  DestroyRenameEdit();
  g_rename_error.clear();
  nv::platform::LogInfo("ui: rename overlay cancel");
  if (hwnd != nullptr) SetFocus(hwnd);
  RefreshUi(hwnd, true);
  InvalidateRect(hwnd, nullptr, TRUE);
}

void CommitRenameOverlay(HWND hwnd) {
  if (!RenameOverlayOpen() || g_rename_edit == nullptr || g_state == nullptr) {
    return;
  }
  wchar_t buffer[512]{};
  GetWindowTextW(g_rename_edit, buffer, static_cast<int>(std::size(buffer)));
  std::wstring name = TrimText(buffer);
  // Validate here so the message can stay inside the card; RenameCurrentTo does
  // the same checks again as the authority.
  std::wstring error;
  if (name.empty() || !IsValidFileName(name)) {
    // Check the typed name itself, not only the leaf: "bad/name" would split
    // into a subdirectory and fail deeper in the rename with a raw OS error
    // (V3.1-06 §06e keeps the user in the card instead).
    error = nv::i18n::Text(nv::i18n::StringId::kErrorInvalidFileName);
  } else {
    std::filesystem::path target = g_state->path.parent_path() / name;
    if (!target.has_extension()) {
      target = target.parent_path() / (name + g_state->path.extension().wstring());
    }
    std::error_code ec;
    if (!IsValidFileName(target.filename().wstring())) {
      error = nv::i18n::Text(nv::i18n::StringId::kErrorInvalidFileName);
    } else if (!nv::FormatRouter::IsSupportedExtension(LowerExtension(target))) {
      error =
          nv::i18n::Text(nv::i18n::StringId::kErrorUnsupportedRenameExtension);
    } else if (_wcsicmp(g_state->path.c_str(), target.c_str()) != 0 &&
               std::filesystem::exists(target, ec)) {
      error = nv::i18n::Text(nv::i18n::StringId::kErrorFileExists);
    }
  }
  if (!error.empty()) {
    g_rename_error = error;
    nv::platform::LogInfo("ui: rename overlay rejected");
    RefreshUi(hwnd, true);
    InvalidateRect(hwnd, nullptr, TRUE);
    return;
  }
  const std::wstring final_name = name;
  g_ui->overlay().CloseModal();
  DestroyRenameEdit();
  g_rename_error.clear();
  if (hwnd != nullptr) SetFocus(hwnd);
  nv::platform::LogInfo("ui: rename overlay commit");
  RenameCurrentTo(hwnd, final_name);
  RefreshUi(hwnd, true);
  InvalidateRect(hwnd, nullptr, TRUE);
}

void RenameCurrent(HWND hwnd) {
  if (!g_state || g_state->path.empty()) return;
  OpenRenameOverlay(hwnd);
}

void SetCurrentWallpaper(HWND hwnd) {
  if (BlockExportWhilePreview(hwnd)) return;
  // §7.6: the wallpaper file must be written from the full frame.
  if (!EnsureFullForExport(hwnd, ExportPixels::kFile)) return;
  nv::Image* image = CurrentImage();
  if (image == nullptr || !g_state) {
    MessageBeep(MB_ICONWARNING);
    return;
  }

  const std::filesystem::path directory =
      nv::platform::DataDir() / L"wallpaper";
  std::error_code ec;
  std::filesystem::create_directories(directory, ec);
  if (ec) {
    ShowError(hwnd, OperationError(nv::ErrorKind::kUnknown, directory,
                                   nv::i18n::Text(
                                       nv::i18n::StringId::kErrorWallpaperDirectory),
                                   nv::i18n::StringId::kErrorWallpaper));
    return;
  }
  const std::filesystem::path file = directory / L"wallpaper.bmp";
  std::wstring error;
  if (!SaveImageWithView(*image, file, nv::platform::ImageSaveFormat::kBmp, 95,
                         &error)) {
    ShowError(hwnd, OperationError(nv::ErrorKind::kSaveFailed, file, error,
                                   nv::i18n::StringId::kErrorWallpaper));
    return;
  }
  if (!nv::platform::SetDesktopWallpaper(file, &error)) {
    ShowError(hwnd, OperationError(nv::ErrorKind::kUnknown, file, error,
                                   nv::i18n::StringId::kErrorWallpaper));
    return;
  }
  nv::platform::LogInfo("desktop wallpaper set: " + Narrow(file.wstring()));
  ShowToast(hwnd, nv::i18n::Text(nv::i18n::StringId::kToastWallpaperSet));
}

void RevealCurrent(HWND hwnd) {
  if (!g_state || g_state->path.empty()) return;
  std::wstring error;
  if (!nv::platform::RevealInExplorer(g_state->path, &error)) {
    ShowErrorToast(hwnd,
                   nv::i18n::Text(nv::i18n::StringId::kErrorReveal), error);
  }
}

// Runs one error-card button. The card's buttons are rebuilt from the same
// ErrorActionsFor() the drawing uses, so an index always names the visible
// action (規劃/22/11 §8 step 5).
void ActivateErrorAction(HWND hwnd, int index) {
  const std::vector<nv::ErrorAction> actions = ErrorActionsFor(g_error);
  if (index < 0 || static_cast<size_t>(index) >= actions.size()) {
    CloseError(hwnd);
    return;
  }
  const nv::ErrorAction action = actions[static_cast<size_t>(index)];
  const nv::ErrorKind kind = g_error.kind;
  const std::filesystem::path path = g_error.path;
  nv::platform::LogInfo(std::string("cmd: error=") +
                        nv::ErrorActionName(action));
  switch (action) {
    case nv::ErrorAction::kRetry: {
      CloseError(hwnd);
      if (kind == nv::ErrorKind::kSaveFailed) {
        SaveAsDialog(hwnd);
      } else if (!path.empty()) {
        nv::platform::LogInfo("cmd: retry=" + Narrow(path.wstring()));
        LoadPath(hwnd, path, true);
      }
      break;
    }
    case nv::ErrorAction::kCopyPath: {
      std::wstring error;
      if (!nv::platform::CopyTextToClipboard(hwnd, path.wstring(), &error)) {
        ShowErrorToast(hwnd,
                       nv::i18n::Text(nv::i18n::StringId::kErrorCopyPath),
                       error);
      } else {
        ShowToast(hwnd,
                  nv::i18n::Text(nv::i18n::StringId::kToastPathCopied));
      }
      break;
    }
    case nv::ErrorAction::kOpenWith: {
      const INT_PTR result =
          reinterpret_cast<INT_PTR>(ShellExecuteW(hwnd, L"openas", path.c_str(),
                                                  nullptr, nullptr,
                                                  SW_SHOWNORMAL));
      if (result <= 32) {
        ShowErrorToast(
            hwnd, nv::i18n::Text(nv::i18n::StringId::kErrorOpenImage));
      } else {
        nv::platform::LogInfo("cmd: open_with=" + Narrow(path.wstring()));
      }
      break;
    }
    case nv::ErrorAction::kReveal: {
      std::wstring error;
      if (!nv::platform::RevealInExplorer(path, &error)) {
        ShowErrorToast(hwnd,
                       nv::i18n::Text(nv::i18n::StringId::kErrorReveal), error);
      }
      break;
    }
    case nv::ErrorAction::kDetails:
    case nv::ErrorAction::kNone:
      break;
  }
}

void ActivateErrorDefault(HWND hwnd) {
  if (ErrorActionsFor(g_error).empty()) {
    CloseError(hwnd);
    return;
  }
  ActivateErrorAction(hwnd, 0);
}

void ShowContextMenu(HWND hwnd, POINT client) {
  if (g_ui == nullptr || g_state == nullptr) return;
  RECT client_rect{};
  GetClientRect(hwnd, &client_rect);
  client.x = std::clamp<LONG>(client.x, client_rect.left,
                              std::max<LONG>(client_rect.left,
                                             client_rect.right - 1));
  client.y = std::clamp<LONG>(client.y, client_rect.top,
                              std::max<LONG>(client_rect.top,
                                             client_rect.bottom - 1));
  // v3.0 (規劃/26 §2.9): the right-click menu is one of the in-window popups, so
  // Escape, click-outside and the keyboard all use the existing stack. Any
  // other popup (zoom / more) is replaced.
  g_context_menu_anchor = client;
  g_ui->overlay().OpenPopup(nv::ui::OverlayKind::kContext);
  g_ui->ClearPointerState();
  nv::platform::LogInfo("ui: context_menu x=" + std::to_string(client.x) +
                        " y=" + std::to_string(client.y));
  RefreshUi(hwnd, true);
  InvalidateRect(hwnd, nullptr, TRUE);
}

void RequestVisibleThumbnails() {
  if (!g_state || !g_state->thumbs || !g_state->strip.visible() ||
      g_state->folder.empty()) {
    return;
  }
  // A hidden rail must not spend decode work on previews nobody sees: off and a
  // faded-out auto rail request nothing (規劃/22/08 §3.6).
  //
  // V3.1-07 S1: the queue is *not* cleared any more. It is shared with the
  // transition-frame pre-read (`RequestThumbForPlaceholder`), whose whole point
  // is to run with the rail hidden — the rail fading out used to cancel the
  // frames the next key press was going to show. The rail still adds no work of
  // its own; the pending items are the ones the pre-read asked for.
  if (g_strip_mode == nv::ui::StripMode::kOff) return;
  if (g_strip_mode == nv::ui::StripMode::kAuto && EffectiveStripOpacity() < 0.05f) {
    return;
  }

  auto& s = *g_state;
  s.strip.SetViewWidth(static_cast<float>(s.renderer.client_width()));

  std::vector<size_t> indices;
  s.strip.VisibleIndices(kThumbMargin, &indices);
  size_t requested = 0;
  for (size_t index : indices) {
    if (index >= s.folder.size()) continue;
    if (s.strip.HasThumbnail(index) || s.strip.HasFailed(index)) continue;

    const std::filesystem::path& path = s.folder.files[index];
    if (auto cached = s.thumbs->GetCached(path)) {
      s.strip.SetThumbnail(index, std::move(cached));
    } else {
      s.thumbs->Request(index, path);
      ++requested;
    }
  }

  if (requested > 0) {
    char line[160]{};
    std::snprintf(line, sizeof(line),
                  "thumbs: visible=%zu requested=%zu scroll=%.0f",
                  indices.size(), requested, s.strip.scroll());
    nv::platform::LogInfo(line);
  }
}

// --- V3-04 display tier (規劃/25 §7.1–§7.5) --------------------------------
//
// The chain is: ask for the tier the view needs (ChooseTier), read the memory
// and disk entries for that tier and every larger tier, and only then decode —
// at the tier, not at full resolution (§7.4b). A tier frame is stored
// re-encoded (JPEG q92 / PNG) so 1 GB holds ~1500 entries instead of ~60 raw
// BGRA frames (§7.5).
TierRequest ComputeTierRequest(const nv::ProbeResult& probe) {
  TierRequest request;
  if (g_state == nullptr || !g_state->renderer.ready()) return request;
  if (probe.width == 0 || probe.height == 0) return request;
  const D2D1_RECT_F content = g_state->renderer.ContentRect();
  const float content_w = content.right - content.left;
  const float content_h = content.bottom - content.top;
  if (content_w < 2.0f || content_h < 2.0f) return request;

  // View rotation only swaps the axes; the plan asks for max(w, h) so the tier
  // is rotation-independent (規劃/25 §7.2).
  const bool swap = g_state->renderer.rotated_90();
  const float display_w = static_cast<float>(swap ? probe.height : probe.width);
  const float display_h = static_cast<float>(swap ? probe.width : probe.height);
  const float fit = std::min(content_w / display_w, content_h / display_h);
  if (!(fit > 0.0f)) return request;
  request.scale = fit;
  request.source_long =
      probe.width > probe.height ? probe.width : probe.height;

  const float needed = std::max(display_w, display_h) * fit;
  if (!(needed >= 1.0f)) return request;
  request.needed = static_cast<uint32_t>(std::ceil(std::min(needed, 1.0e6f)));
  return request;
}

uint32_t RequestedDisplayEdge(const std::filesystem::path& path,
                              const nv::ProbeResult& probe) {
  if (g_display_cache_mode == DisplayCacheMode::kOff || g_state == nullptr) {
    return 0;
  }
  const TierRequest request = ComputeTierRequest(probe);
  if (request.needed == 0) return 0;
  // The policy itself is pure and unit-tested (規劃/25 §7.9-5): the "no falling
  // back to a tier once the full frame existed" rule is the one that is
  // invisible until the user navigates back and sees a blur.
  nv::TierLoadInputs inputs;
  inputs.needed_long_edge = request.needed;
  inputs.scale = request.scale;
  inputs.source_long_edge = request.source_long;
  inputs.full_frame_available = g_state->full_frames.contains(path.wstring());
  inputs.force = g_display_cache_mode == DisplayCacheMode::kForce;
  return nv::DecideTierEdge(inputs);
}

void TraceCache(const std::filesystem::path& path, uint32_t edge,
                const char* source) {
  if (!g_cache_trace) return;
  char line[600]{};
  std::snprintf(line, sizeof(line), "cache=%s src=%s edge=%u path=%s", source,
                edge != 0 ? "tier" : "full", edge,
                Narrow(path.wstring()).c_str());
  nv::platform::LogInfo(line);
}

// The tier a *load* would ask for right now, derived from the content
// rectangle alone (a new load always resets the view to fit). Prefetch uses it
// so a neighbour fills exactly the entry the next navigation will look for,
// including the very first image of a session — that one is decoded full
// because the renderer does not exist yet, and prefetching its neighbours at
// full resolution would compete with the next decode (規劃/25 §7.5).
uint32_t FitDisplayTierEdge() {
  if (g_state == nullptr || !g_state->renderer.ready()) return 0;
  const D2D1_RECT_F content = g_state->renderer.ContentRect();
  const float long_edge =
      std::max(content.right - content.left, content.bottom - content.top);
  if (!(long_edge >= 1.0f)) return 0;
  return nv::ChooseTier(static_cast<uint32_t>(std::ceil(long_edge)));
}

// --- V3-04b: §7.6 "upgrade to the full frame" ------------------------------
//
// A tier is a *display* object: it is at most `display_edge` pixels, so it can
// only ever satisfy a view that needs at most that many pixels. Two paths ask
// for more:
//
//  - the view itself, as soon as the tier would be upscaled beyond
//    `kTierUpscaleLimit` (RAW: only at 100 %), which requests the full frame
//    once and keeps the tier on screen until it arrives — the same "show what we
//    have, replace it when the sharp frame lands" contract as the two-stage
//    preview (規劃/22/13). V3-04b upgraded at 100 % only, which left every
//    mid-zoom view soft; V3.1-08 uses the same threshold;
//  - an export, which must always produce file pixels (§7.6). It decodes
//    synchronously because the caller needs the pixels *now* to write them, and
//    it keeps the stricter 100 % rule so a mid-zoom capture is not forced to
//    decode.

// The scale the user should see once the full frame replaces the tier; 0 means
// "keep the current view" (fit mode does that by itself).
float g_upgrade_target_scale = 0.0f;
// True while OnDecodeDone is applying a completion. The frame is in flux there
// (a tier may still be the current frame while the swap is half done) and the
// handler itself calls RefreshUi through CancelLoading, so a §7.6 upgrade fired
// from inside it would ask for the same full decode a second time.
bool g_applying_completion = false;

struct CompletionGuard {
  CompletionGuard() { g_applying_completion = true; }
  ~CompletionGuard() { g_applying_completion = false; }
  CompletionGuard(const CompletionGuard&) = delete;
  CompletionGuard& operator=(const CompletionGuard&) = delete;
};

// Client pixels per *file* pixel. V3.1-08: the renderer is file-relative for
// every frame now (logical size + stretched bitmap), so this is just its zoom.
// Keeping the old tier correction here would divide by the tier ratio twice.
float ViewFileScale() {
  if (g_state == nullptr) return 1.0f;
  return g_state->renderer.zoom();
}

// True when the tier cannot supply the pixels this view shows *for export*: at
// >= 100 % of the file every screen pixel would be an interpolation of a tier
// pixel, and pixel mode explicitly promises hard source blocks. Screenshots use
// this (not the early-upgrade rule below) so a fit or mid-zoom capture keeps the
// tier the user actually sees -- the V3-04 sharpness A/B depends on it.
bool TierCannotSupplyView() {
  if (g_state == nullptr || !g_state->current_is_tier) return false;
  return ViewFileScale() >= 1.0f || g_settings.pixel_mode;
}

// V3.1-08: the *view* upgrades earlier than the export path (kTierUpscaleLimit),
// except for RAW, which keeps the 100 % rule (see display_tier.h). `/` is the
// reason this is a separate predicate: a screenshot at 30 % must not force a
// synchronous full decode.
bool TierNeedsFullFrame() {
  if (g_state == nullptr || !g_state->current || !g_state->current_is_tier) {
    return false;
  }
  const nv::ImageInfo& info = g_state->current->info;
  nv::TierUpgradeInputs inputs;
  inputs.have_long_edge = std::max(info.width, info.height);
  inputs.file_long_edge = std::max(info.full_width, info.full_height);
  inputs.file_scale = ViewFileScale();
  inputs.pixel_mode = g_settings.pixel_mode;
  inputs.raw = g_state->format == nv::ImageFormat::kRaw;
  inputs.file_bytes = g_state->file_bytes;
  return nv::ShouldUpgradeToFullFrame(inputs);
}

void MaybeUpgradeToFullFrame(HWND hwnd) {
  // `hwnd` is unused on purpose: an upgrade never shows the loading chip (see
  // the g_load_start_ms comment below), and it never needs a repaint of its own.
  (void)hwnd;
  if (g_state == nullptr || !g_state->current_is_tier) return;
  if (g_applying_completion) return;
  auto& s = *g_state;
  if (!TierNeedsFullFrame()) return;
  if (s.path.empty() || !s.decode) return;
  // Rule §7.3-3: the full frame is requested once per file; full_frames also
  // short-circuits a second request while the first is still decoding.
  if (s.full_frames.contains(s.path.wstring())) return;
  // One upgrade per file at a time. RefreshUi runs on every paint, so without
  // this a 100 % view would queue the same full decode again on the next frame.
  if (s.upgrade_in_flight == s.path) return;
  // Another load/upgrade is already in flight; its completion (or the next
  // RefreshUi after it) gets another chance.
  if (!g_pending_path.empty()) return;

  g_upgrade_target_scale = ViewFileScale();
  g_pending_path = s.path;
  s.upgrade_in_flight = s.path;
  // V3.1-08: no "正在加载" chip for an upgrade. It only sharpens the picture the
  // user is already looking at (and is usually still zooming), so a card
  // flashing mid-zoom is pure noise; the 400 ms delay would fire for every RAW
  // upgrade. The start timestamp is still set so the completion's
  // `load: done ms=` reports the real decode time.
  if (g_loading_state == 0) g_load_start_ms = NowMs();
  s.decode->Request(s.path, 0);
  {
    // V3.1-08 evidence: which bitmap was on screen, what the view needed, and
    // the scale the upgrade has to preserve. `reason` separates the three
    // branches of ShouldUpgradeToFullFrame so a test can tell them apart.
    const nv::ImageInfo& info = s.current->info;
    const uint32_t have = std::max(info.width, info.height);
    const uint32_t file = std::max(info.full_width, info.full_height);
    const char* reason = g_settings.pixel_mode            ? "pixel"
                         : g_upgrade_target_scale >= 1.0f ? "100%"
                         : s.format == nv::ImageFormat::kRaw ? "raw"
                                                             : "upscale";
    char line[640]{};
    std::snprintf(line, sizeof(line),
                  "upgrade: requested full frame scale=%.3f have=%u file=%u "
                  "reason=%s path=%s",
                  static_cast<double>(g_upgrade_target_scale), have, file,
                  reason, Narrow(s.path.wstring()).c_str());
    nv::platform::LogInfo(line);
  }
  TraceCache(s.path, 0, "upgrade");
}

// §7.6: export pixels must be file pixels. Returns false only when the export
// must not run (the reason has already been shown to the user).
bool EnsureFullForExport(HWND hwnd, ExportPixels mode) {
  if (g_state == nullptr || !g_state->current_is_tier) return true;
  if (mode == ExportPixels::kScreen && !TierCannotSupplyView()) {
    // A fit-scale capture of the tier *is* what the user sees; forcing a full
    // decode here would also silently invalidate the V3-04 sharpness A/B,
    // which compares the tier rendering against the full-frame rendering.
    return true;
  }
  auto& s = *g_state;
  if (s.path.empty() || !s.current) return false;

  const double started = NowMs();
  const nv::DecodeResult result = nv::FormatRouter::Decode(s.path, {});
  if (!result.ok()) {
    char line[512]{};
    std::snprintf(line, sizeof(line),
                  "export: forced full decode failed ms=%.1f path=%s",
                  NowMs() - started, Narrow(s.path.wstring()).c_str());
    nv::platform::LogInfo(line);
    ShowErrorToast(hwnd,
                   nv::i18n::Text(nv::i18n::StringId::kErrorOpenImage));
    return false;
  }
  // Keep the view: the export happens from `current`, and the screen should not
  // jump because the user pressed Ctrl+S.
  if (!s.renderer.SetImage(*result.image, false, true)) return false;
  s.renderer.SetSourceIsTier(false);
  s.current = result.image;
  s.current_is_tier = false;
  s.display_edge = 0;
  s.cache_source = "none";
  ClearTransitionFrame();
  s.full_frames.insert(s.path.wstring());
  s.cache.Put(nv::MakeMemoryCacheKey(s.path), s.current);
  SaveDecodedToDiskCache(s.path, s.current, 0);
  char line[512]{};
  std::snprintf(line, sizeof(line), "export: forced full decode ms=%.1f path=%s",
                NowMs() - started, Narrow(s.path.wstring()).c_str());
  nv::platform::LogInfo(line);
  UpdateTitle(hwnd);
  RefreshUi(hwnd, false);
  if (hwnd != nullptr) InvalidateRect(hwnd, nullptr, TRUE);
  return true;
}

// Uploads a tier frame exactly like a cache hit and returns true when the frame
// is on screen. Every tail step mirrors the full-frame cache hit path
// (DismissErrorOnNewImage / RefreshUi / SyncWatchBaseline / prefetch).
//
// `transition` (V3.1-02) marks a *placeholder* frame: it must not enter any
// cache (a 512 tier under the 1280 key would be served as the real thing on the
// next visit), must not persist to disk and must not re-arm the neighbour
// prefetch, because the request it is standing in for is still in flight.
bool ShowDisplayTier(HWND hwnd, const std::filesystem::path& requested,
                     std::shared_ptr<nv::Image> image, uint32_t edge,
                     const char* source, bool transition) {
  if (g_state == nullptr || !image || image->Empty() || edge == 0) return false;
  auto& s = *g_state;
  if (!s.renderer.SetImage(*image)) return false;

  s.current = std::move(image);
  s.path = requested;
  s.current_is_tier = true;
  s.display_edge = edge;
  s.cache_source = source;
  // §7.7: the proxy policy and the upgrade trigger both read this, so it must
  // be set before the first paint of the frame.
  s.renderer.SetSourceIsTier(true);
  if (transition) {
    s.transition_active = true;
    s.transition_edge = edge;
    s.transition_source = source;
    ++g_transition_count;
    char line[512]{};
    std::snprintf(line, sizeof(line),
                  "transition: shown source=%s edge=%u ms=%.0f path=%s", source,
                  edge, s.transition_ms, Narrow(requested.wstring()).c_str());
    nv::platform::LogInfo(line);
  } else {
    s.transition_active = false;
    s.transition_edge = 0;
    s.transition_source.clear();
    s.cache.Put(nv::MakeMemoryCacheKey(requested, edge), s.current);
    nv::platform::LogInfo("display cache hit: " + Narrow(requested.wstring()) +
                          " edge=" + std::to_string(edge) + " source=" + source);
    TraceCache(requested, edge, source);
  }
  DismissErrorOnNewImage();
  UpdateTitle(hwnd);
  StartAnimation(hwnd);
  RefreshUi(hwnd, false);
  InvalidateRect(hwnd, nullptr, TRUE);
  SyncWatchBaseline();
  if (!transition) PrefetchAdjacent();
  return true;
}

// V3.1-02 / V3.1-07 S3: the target tier is not ready, but a smaller frame of the
// same file may already exist. Showing it is what makes "every key press changes
// the screen" true while the 22 MB decode runs; the tier replaces it when it
// lands, and nothing about this frame outlives that replacement.
//
// Sources, best first (規劃/v3.1/01 §V3.1-02):
//  1. a smaller tier in the memory cache (a file visited zoomed out earlier);
//  2. the strip's thumbnail (already generated for a visible slot).
// The EXIF IFD1 thumbnail (source 3 in the plan) is not implemented: none of
// the switch-speed samples or the test corpus carries one, so it could not be
// verified, and 1/2 already cover the burst cases. Recorded as a follow-up in
// 規劃/v3.1/01 and in 04.
//
// S3 changed *when* this runs, not what it shows: `auto` no longer waits for a
// burst and no longer yields to an in-flight target tier, so every switch that
// missed the cache gets the 160 px frame first (the replica's cadence). The
// "in flight" rule of V3.1-02 survives verbatim in `sharp`. Whatever the mode,
// the frame stays a placeholder: ShowDisplayTier(transition=true) never caches,
// never writes the disk cache and never re-arms the neighbour prefetch.
//
// S3b: `requested_edge` may be 0 (no display tier, i.e. RAW). There is no
// smaller tier to walk then, but the thumbnail is still a valid placeholder and
// RAW is the format where a fast page-through used to show *nothing* (no tier,
// no neighbour prefetch, and every embedded preview cancelled by the next key).
bool TryShowTransitionFrame(HWND hwnd, const std::filesystem::path& requested,
                            uint32_t requested_edge) {
  if (g_state == nullptr || !g_state->renderer.ready()) return false;
  if (g_transition_mode == TransitionMode::kOff) return false;
  // `sharp` is the V3.1-02 behaviour kept as a rollback: a placeholder is only
  // for a burst, and none at all while the requested tier is already decoding
  // (its sharp frame is tens of milliseconds away, so a blur would be pure
  // cost). `auto` deliberately drops both conditions (規劃/v3.1/07 §3 S3-1/2);
  // `force` is `auto` without the bookkeeping for the 400 ms test window.
  if (g_transition_mode == TransitionMode::kSharp) {
    if (!g_nav_burst) return false;
    // V3.1-07 S3b: `sharp` must reproduce V3.1-02 exactly, and 02 never showed
    // a placeholder for a format without a display tier (RAW) — keep that here
    // even though `auto` now allows it.
    if (requested_edge == 0) return false;
    if (g_state->decode &&
        g_state->decode->IsPrefetchInFlight(requested, requested_edge)) {
      nv::platform::LogInfo("transition: skipped (target tier in flight)");
      return false;
    }
  }
  // Forbidden where the pixels must be real: pixel mode promises hard source
  // blocks, and a 1:1 view would stretch the placeholder even more than fit
  // (規劃/v3.1/01 §V3.1-02: "1:1 / 像素模式 100% 不出现过渡帧").
  if (g_settings.pixel_mode) return false;
  if (ViewFileScale() >= 1.0f) return false;
  auto& s = *g_state;
  if (s.transition_active && s.path == requested) return false;

  std::shared_ptr<nv::Image> frame;
  std::string source;
  uint32_t edge = 0;
  // 1. larger-to-smaller walk *below* the requested tier: the largest smaller
  // frame is the sharpest placeholder available.
  for (uint32_t candidate = nv::PrevTier(requested_edge); candidate != 0;
       candidate = nv::PrevTier(candidate)) {
    if (auto cached = s.cache.Get(nv::MakeMemoryCacheKey(requested, candidate))) {
      frame = std::move(cached);
      edge = candidate;
      source = "small-tier";
      break;
    }
  }
  // 2. the strip thumbnail. TryLoadPreview is a memory/disk lookup (~1 ms), not
  // a decode, and it is only ever populated for a slot the strip has shown.
  if (!frame && s.thumbs) {
    if (auto thumb = s.thumbs->TryLoadPreview(requested)) {
      edge = nv::ImageLongEdge(*thumb);
      if (edge != 0) {
        frame = std::move(thumb);
        source = "thumb";
      }
    }
  }
  if (!frame || edge == 0) {
    // Evidence, and the only way to tell "nothing available" from "skipped by a
    // guard": a cold directory with a hidden rail has no thumbnail yet, and S1
    // (thumbnail pre-read) is what fills that gap.
    nv::platform::LogInfo("transition: none (no cached frame) mode=" +
                          std::string(TransitionModeName(g_transition_mode)));
    return false;
  }

  s.transition_ms = g_load_start_ms > 0.0 ? NowMs() - g_load_start_ms : 0.0;
  return ShowDisplayTier(hwnd, requested, std::move(frame), edge,
                         source.c_str(), /*transition=*/true);
}

void PrefetchAdjacent(bool allow_raw) {
  if (!g_prefetch_enabled) return;
  if (!g_state || !g_state->decode || g_state->folder.size() < 2) return;

  // Tier prefetch: a neighbour costs ~4 MB instead of 96 MB, and the entry it
  // fills is the one the next load looks for (規劃/25 §7.5). This also covers
  // the first image of a session: it is decoded full (no renderer yet), but its
  // neighbours are prefetched at the tier the *next* load will ask for.
  const uint32_t prefetch_edge =
      g_display_cache_mode != DisplayCacheMode::kOff ? FitDisplayTierEdge() : 0;
  const bool tier_prefetch = prefetch_edge != 0;

  if (!tier_prefetch) {
    const uint64_t current_bytes =
        static_cast<uint64_t>(g_state->renderer.image_width()) *
        static_cast<uint64_t>(g_state->renderer.image_height()) * 4ull;
    if (current_bytes > kPrefetchMaxCurrentBytes) {
      nv::platform::LogInfo("prefetch skipped: current frame is " +
                            std::to_string(current_bytes / (1024ull * 1024ull)) +
                            " MB");
      return;
    }
  }

  const size_t count = g_state->folder.size();
  const size_t current = g_state->folder.index;

  // --- V3.1-07 S4: direction + depth --------------------------------------
  // Only the side the user is paging towards. Depth grows with the
  // same-direction streak (1 -> 1, 2 -> 2, 3+ -> 3), which is the replica's
  // "行进方向 + 深度 1-3" (規劃/v3.1/07 §1.1/§3 S4). With no direction yet
  // (first open, F5) forward comes first, as it always did.
  const int direction = g_nav_direction < 0 ? -1 : 1;
  const int depth_limit = std::clamp(g_prefetch_max_depth, 1, kPrefetchMaxDepth);
  const int depth = std::clamp(g_nav_dir_streak, 1, depth_limit);
  const bool current_is_raw =
      nv::FormatRouter::Detect(g_state->folder.files[current]) ==
      nv::ImageFormat::kRaw;

  std::vector<std::filesystem::path> paths;
  int raw_queued = 0;
  int cached = 0;
  int raw_skipped = 0;
  for (int step = 1; step <= depth; ++step) {
    const long long candidate =
        static_cast<long long>(current) + static_cast<long long>(direction) * step;
    if (candidate < 0 || candidate >= static_cast<long long>(count)) {
      break;  // folder edge: further steps are out of range too
    }
    const std::filesystem::path& path =
        g_state->folder.files[static_cast<size_t>(candidate)];
    const bool raw =
        nv::FormatRouter::Detect(path) == nv::ImageFormat::kRaw;
    // V3.1-07 S4: RAW neighbours are now prefetched too, but at the *fit tier*
    // (LibRaw half_size, 0.3-0.5 s each under the process-global LibRaw lock).
    // Three limits keep that from delaying the embedded preview of the file the
    // user actually opens next (坑 335 / 07 §6 / §3 S4 risk note):
    //  * only at rest (`allow_raw`, i.e. the 400 ms settle timer) — during a
    //    page-through a 0.4 s decode started for the *next* file would hold the
    //    lock exactly when that file's preview needs it (measured: preview
    //    78 ms -> 422/859 ms in a 110 ms burst);
    //  * while the file on screen is a RAW, at most one RAW neighbour is queued;
    //  * `--prefetch-raw=off` restores the pre-S4 rule (skip them entirely).
    if (raw) {
      if (!g_prefetch_raw_neighbours || !allow_raw) {
        ++raw_skipped;
        continue;
      }
      if (current_is_raw && raw_queued >= 1) {
        ++raw_skipped;
        continue;
      }
    } else if (allow_raw) {
      // The settle-time pass exists only to add RAW neighbours; every load
      // already planned the non-RAW ones, so re-queueing them here would only
      // restart decodes that the per-load plan already finished or dropped.
      ++cached;
      continue;
    }
    // A neighbour already in the memory cache needs no prefetch. This is not
    // just an optimisation: when the "tier" *is* the full frame (a 1280x854
    // HEIC in a 1280-wide window), DecodeService re-keys the prefetch result as
    // edge 0, so the next prefetch's tier lookup can never hit and the neighbour
    // was re-decoded on every arrow press — each HEIC decode also retains HEIF
    // codec handles (measured: 6 HEIC files, 20 arrows, 42 prefetches, +90
    // handles). Checking both keys stops that loop for every format.
    if (g_state->cache.Get(nv::MakeMemoryCacheKey(path)) != nullptr ||
        (prefetch_edge != 0 &&
         g_state->cache.Get(nv::MakeMemoryCacheKey(path, prefetch_edge)) !=
             nullptr)) {
      ++cached;
      continue;
    }
    paths.push_back(path);
    if (raw) ++raw_queued;
  }
  if (cached != 0) {
    nv::platform::LogInfo("prefetch skipped: cached neighbour");
  }
  if (raw_skipped != 0) {
    nv::platform::LogInfo("prefetch skipped: raw neighbour");
  }
  // Evidence for S4: which way the walk went and how deep, so a burst of
  // same-direction keys can be read straight off the log (`cur=` names the file
  // it was planned for; `rest=1` is the settle-time plan, the only one that may
  // include a RAW neighbour; a miss plans twice, before and after its decode).
  char line[256]{};
  std::snprintf(line, sizeof(line),
                "prefetch: plan dir=%d depth=%d rest=%d queued=%zu raw=%d cached=%d cur=%s",
                direction, depth, allow_raw ? 1 : 0, paths.size(), raw_queued,
                cached,
                Narrow(g_state->folder.files[current].filename().wstring())
                    .c_str());
  nv::platform::LogInfo(line);
  g_state->decode->Prefetch(paths, prefetch_edge);
}

// V3.1-07 S1 (minimal): fill the thumbnail cache around the current file while
// the view rests, so the next burst has frames to show from its first key.
// Forwards first (the direction a burst takes), then backwards; the rail's queue
// is separate and the thumbnail service dedups by key, so a repeated call after
// a folder change is cheap.
// Drops the pre-read plan (folder rebuild, burst, shutdown). Cheap and safe to
// call when there is no plan.
void ClearPrewarmPlan(HWND hwnd) {
  g_prewarm_plan.clear();
  g_prewarm_cursor = 0;
  if (hwnd != nullptr) KillTimer(hwnd, kThumbPrewarmStepTimer);
}

// V3.1-07 S1-1 / S1-3: one plan entry per tick, so pre-read requests start at
// least kThumbPrewarmStepMs apart. Entries that are already cached/queued (or
// RAW, which the pre-read never touches) are skipped in the same tick: the
// throttle spaces out *work*, not bookkeeping.
//
// The plan is rebuilt by PrewarmThumbnails on every rest; this only drains it.
void PumpPrewarmStep(HWND hwnd) {
  if (g_state == nullptr || g_prewarm_plan.empty()) {
    ClearPrewarmPlan(hwnd);
    return;
  }
  // A burst outranks the pre-read: its first key already called
  // CancelPending(), and a 160 px frame cannot land before the next key anyway.
  // Drop the rest instead of feeding the queue the burst just cleared.
  if (g_nav_burst) {
    if (g_prewarm_queued != 0) {
      nv::platform::LogInfo("thumbs: prewarm dropped queued=" +
                            std::to_string(g_prewarm_queued) + " (burst)");
    }
    ClearPrewarmPlan(hwnd);
    return;
  }
  const auto& folder = g_state->folder;
  while (g_prewarm_cursor < g_prewarm_plan.size()) {
    const size_t index = g_prewarm_plan[g_prewarm_cursor++];
    if (index >= folder.files.size()) continue;
    if (RequestThumbForPlaceholder(index, folder.files[index])) {
      ++g_prewarm_queued;
      break;  // at most one new request per tick
    }
  }
  if (g_prewarm_cursor >= g_prewarm_plan.size()) {
    // Evidence for the throttle: `queued` requests cannot have started closer
    // together than (queued - 1) * step_ms, and span_ms is what it took.
    char line[192]{};
    std::snprintf(line, sizeof(line),
                  "thumbs: prewarm done queued=%d span_ms=%.0f",
                  g_prewarm_queued, NowMs() - g_prewarm_started_ms);
    nv::platform::LogInfo(line);
    ClearPrewarmPlan(hwnd);
  }
}

// V3.1-07 S1 (完整版): fill the thumbnail cache around the current file while
// the view rests, so the next burst has frames to show from its first key.
//
// Order (規劃/v3.1/07 §3 S1 "当前张优先 → 行进方向 ±6 → 反向"): the current file
// (a burst may have cancelled its per-load request), then ±1..±6 in the paging
// direction, then the other side. `g_nav_direction` is +1/-1 from the last
// navigation and 0 when unknown, in which case forward comes first (the old
// behaviour).
//
// Requests are paced by PumpPrewarmStep, not issued here; `--thumb-prewarm=off`
// turns the whole thing off, and the 400 ms settle timer keeps a burst from ever
// reaching this function.
void PrewarmThumbnails(HWND hwnd) {
  if (!g_thumb_prewarm) return;
  if (g_state == nullptr || !g_state->thumbs || g_state->folder.empty()) return;
  // The load that cancelled the pre-read re-arms this timer, but the burst
  // settle timer (same 400 ms) can fire first. Re-arm instead of building a plan
  // that RequestThumbForPlaceholder would refuse item by item.
  if (g_nav_burst) {
    if (hwnd != nullptr) {
      SetTimer(hwnd, kThumbPrewarmTimer, kThumbPrewarmDelayMs, nullptr);
    }
    return;
  }
  auto& s = *g_state;
  const size_t count = s.folder.size();
  if (count < 2) return;
  const size_t current = s.folder.index;
  const int direction = g_nav_direction < 0 ? -1 : 1;

  ClearPrewarmPlan(hwnd);
  g_prewarm_plan.push_back(current);
  for (const int side : {direction, -direction}) {
    for (int step = 1; step <= kThumbPrewarmRadius; ++step) {
      const long long candidate =
          static_cast<long long>(current) + static_cast<long long>(side) * step;
      if (candidate < 0 || candidate >= static_cast<long long>(count)) continue;
      g_prewarm_plan.push_back(static_cast<size_t>(candidate));
    }
  }
  g_prewarm_cursor = 0;
  g_prewarm_queued = 0;
  g_prewarm_started_ms = NowMs();
  // Evidence for S1-3: the direction and the exact order being walked.
  std::string plan;
  for (const size_t index : g_prewarm_plan) {
    const long long relative =
        static_cast<long long>(index) - static_cast<long long>(current);
    if (!plan.empty()) plan += ',';
    if (relative >= 0) plan += '+';
    plan += std::to_string(relative);
  }
  nv::platform::LogInfo("thumbs: prewarm start dir=" + std::to_string(direction) +
                        " radius=" + std::to_string(kThumbPrewarmRadius) +
                        " step_ms=" + std::to_string(kThumbPrewarmStepMs) +
                        " plan=" + plan);
  // The first request is immediate (the view has already been at rest for
  // 400 ms); every later one waits for a tick.
  PumpPrewarmStep(hwnd);
  if (!g_prewarm_plan.empty() && hwnd != nullptr) {
    SetTimer(hwnd, kThumbPrewarmStepTimer, kThumbPrewarmStepMs, nullptr);
  }
}

void SaveDecodedToDiskCache(const std::filesystem::path& path,
                            const std::shared_ptr<nv::Image>& image,
                            uint32_t edge) {
  if (!image || image->Empty()) return;
  // The 64 MB rule stays for full frames only (§9 M1-4: a 126 MB 8K frame is
  // faster to decode than to read back). A tier is bounded by its own budget
  // and is always small enough to store (規劃/25 §7.5 / §7.13-2).
  if (edge == 0 && image->pixels.size() > kDiskCacheMaxFrameBytes) {
    nv::platform::LogInfo("disk cache write skipped: frame is " +
                          std::to_string(image->pixels.size() /
                                         (1024ull * 1024ull)) +
                          " MB");
    return;
  }
  // Large frames (8K is 126 MB) are written by a background thread; doing this
  // inline blocked the UI for >100 ms right after the image appeared.
  if (g_state && g_state->cache_writer) {
    g_state->cache_writer->Enqueue(path, image, true, edge);
  }
}

void OnDecodeDone(HWND hwnd, LPARAM lparam) {
  auto* completion = reinterpret_cast<nv::DecodeCompletion*>(lparam);
  if (completion == nullptr) return;
  if (!g_state) {
    delete completion;
    return;
  }
  const CompletionGuard applying;
  // §7.6: whatever happens to this completion (applied, superseded or failed),
  // the upgrade it belonged to is over and may be requested again later.
  const bool upgrade_done = !completion->prefetch &&
                            completion->max_edge == 0 &&
                            g_state->upgrade_in_flight == completion->path;
  if (upgrade_done) g_state->upgrade_in_flight.clear();

  if (completion->prefetch) {
    if (completion->result.ok()) {
      if (!completion->result.image->info.animated) {
        g_state->cache.Put(
            nv::MakeMemoryCacheKey(completion->path, completion->max_edge),
            completion->result.image);
        // A tier that came from the disk cache is already stored: writing it
        // again would just re-encode and re-trim. A freshly decoded tier is
        // persisted so the next open of that neighbour is a disk hit.
        const bool from_disk =
            completion->result.image->info.source == "disk-cache";
        if (!from_disk) {
          SaveDecodedToDiskCache(completion->path, completion->result.image,
                                 completion->max_edge);
        }
        if (completion->max_edge == 0) {
          g_state->full_frames.insert(completion->path.wstring());
        }
      }
      nv::platform::LogInfo("prefetched: " + Narrow(completion->path.wstring()));
    }
    delete completion;
    return;
  }

  const bool current = g_state && g_state->decode &&
                       completion->request_id == g_state->decode->CurrentId();
  if (current) {
    // The load is over: success shows the image, failure falls back to the
    // empty/error state instead of hiding the card forever.
    g_pending_path.clear();
    CancelLoading(hwnd);
    const double load_ms =
        g_load_start_ms > 0.0 ? NowMs() - g_load_start_ms : 0.0;
    // Keep the start time while a --force-loading indicator is still pending:
    // its 400 ms log must report the real delay, not 0.
    if (g_loading_state == 0) g_load_start_ms = 0.0;
    // --force-preview-fail: model a full decode that fails after a preview was
    // shown, so "keep the preview + error card" is testable (規劃/22/13 §6).
    if (g_force_preview_fail && g_state->preview_active) {
      g_force_preview_fail = false;
      completion->result.image.reset();
      completion->result.error =
          nv::Error::Make(2200, "forced preview failure (test hook)");
    }
    if (completion->result.ok()) {
      // The sharp frame replaces the preview and must keep the current view:
      // a zoom the user set on the preview survives the swap (規劃/22/13 §3.4).
      if (g_state->renderer.SetImage(*completion->result.image, false, true)) {
        LogMemoryUsage("decoded_image_ready");
        g_state->current = completion->result.image;
        // V3-04: remember which tier this frame is. The decode request carried
        // it, so a tier can never be mistaken for the full frame (規劃/25 §7.4).
        g_state->current_is_tier = completion->max_edge != 0;
        g_state->display_edge = completion->max_edge;
        g_state->cache_source = "none";
        ClearTransitionFrame();
        // V3.1-04a: a full decode of a RAW replaces the preview/half-size frame;
        // the dump must report the new mode (it is only ever set for RAW loads).
        if (completion->max_edge == 0 && g_state->raw_mode != "none") {
          g_state->raw_mode = "full";
        }
        // V3.1-11: a half-size LibRaw frame is a *real* frame now (it replaces
        // the camera preview placeholder), so the dump must stop saying
        // "preview" the moment it lands.
        if (completion->max_edge != 0 && g_state->raw_mode == "preview") {
          g_state->raw_mode = "half";
        }
        g_state->renderer.SetSourceIsTier(g_state->current_is_tier);
        TraceCache(completion->path, completion->max_edge, "none");
        // §7.6: this completion may be the full frame the upgrade asked for.
        // The source pixels just changed from the tier to the file, so the
        // stored zoom (relative to tier pixels) must be converted, otherwise a
        // 100 % request would end up at 470 % of the real file.
        if (upgrade_done) {
          const float target = g_upgrade_target_scale;
          g_state->upgrade_in_flight.clear();
          g_upgrade_target_scale = 0.0f;
          // V3.1-08: do NOT re-apply the scale that was current when the
          // upgrade was *requested*. A RAW full decode takes 1.1-1.5 s and the
          // user keeps zooming during it, so restoring that value yanked the
          // view backwards (the "it suddenly zooms in/out by itself" report).
          // SetImage(preserve_view) already kept the live zoom and the pan,
          // because the logical (file) size did not change; nothing to restore.
          char upgraded[256]{};
          std::snprintf(upgraded, sizeof(upgraded),
                        "upgrade: full frame applied scale=%.3f requested=%.3f",
                        static_cast<double>(ViewFileScale()),
                        static_cast<double>(target));
          nv::platform::LogInfo(upgraded);
        }
        if (!completion->result.image->info.animated) {
          g_state->cache.Put(
              nv::MakeMemoryCacheKey(completion->path, completion->max_edge),
              completion->result.image);
          SaveDecodedToDiskCache(completion->path, completion->result.image,
                                 completion->max_edge);
          if (completion->max_edge == 0) {
            g_state->full_frames.insert(completion->path.wstring());
          }
        }
        g_state->path = completion->path;
        DismissErrorOnNewImage();
        if (g_state->preview_active) {
          g_state->preview_active = false;
          g_state->preview_failed = false;
          g_state->preview_path.clear();
          g_state->preview_target_width = 0;
          g_state->preview_target_height = 0;
          g_state->preview_source.clear();
          // The badge fades out over 160 ms instead of disappearing in one
          // frame (規劃/22/13 §3.5).
          StartPreviewBadgeFade(hwnd);
          nv::platform::LogInfo("preview: replaced with full image");
        }
        nv::platform::LogInfo("decoded: " + Narrow(completion->path.wstring()));
        char done[256]{};
        std::snprintf(done, sizeof(done), "load: done ms=%.0f path=%s", load_ms,
                      Narrow(completion->path.wstring()).c_str());
        nv::platform::LogInfo(done);
        UpdateTitle(hwnd);
        // The title/bottom segments show dimensions, size, index and zoom, so
        // the cached layout must be rebuilt before the frame is drawn.
        RefreshUi(hwnd, false);
        // The card follows the new file (dimensions/frames/format change) and a
        // pending restore from the previous session happens now that an image
        // exists (規劃/26 §2.1).
        UpdateInfoWindowContent(hwnd);
        if (g_info_restore_pending && !InfoWindowVisible()) {
          OpenInfoWindow(hwnd);
        }
        StartAnimation(hwnd);
        InvalidateRect(hwnd, nullptr, TRUE);
        SyncWatchBaseline();
        PrefetchAdjacent();
      } else {
        nv::platform::LogError("D2D bitmap creation failed");
        ShowError(hwnd, nv::MakeUiError(
                            nv::ErrorKind::kUnknown, completion->path,
                            L"D2D bitmap creation failed",
                            nv::ErrorAction::kRetry |
                                nv::ErrorAction::kOpenWith |
                                nv::ErrorAction::kCopyPath,
                            nv::i18n::StringId::kErrorRenderFailed,
                            nv::i18n::StringId::kErrorRenderFailedHint));
      }
    } else {
      nv::platform::LogError("decode failed: " + completion->result.error.message);
      char failed[256]{};
      std::snprintf(failed, sizeof(failed), "load: failed ms=%.0f path=%s",
                    load_ms, Narrow(completion->path.wstring()).c_str());
      nv::platform::LogInfo(failed);
      ShowError(hwnd, nv::ClassifyDecodeError(
                          completion->result.error, completion->path,
                          nv::FormatRouter::Detect(completion->path)));
      // V3.1-02: the placeholder must not survive a failed load as if it were
      // the requested frame.
      ClearTransitionFrame();
      if (g_state->preview_active) {
        // The preview stays on screen and the badge says the load failed
        // (規劃/22/13 §3.2 step 4, 坑 9).
        g_state->preview_failed = true;
        g_preview_badge_opacity = 1.0f;
        if (hwnd != nullptr) KillTimer(hwnd, kPreviewBadgeTimer);
        nv::platform::LogInfo("preview: failed; keeping preview");
      }
      RefreshUi(hwnd, false);
      UpdateTitle(hwnd);
      InvalidateRect(hwnd, nullptr, TRUE);
    }
  }

  delete completion;
}

// V3.1-04a: the camera-embedded JPEG becomes the *final* frame. `current_is_tier`
// keeps the rest honest: the §7.6 upgrade (1:1 / pixel mode) and
// EnsureFullForExport still decode the full RAW synchronously.
// V3.1-11 (option C, "同源"): the RAW display chain never ends on a
// camera-rendered frame. The embedded preview is shown only as a *placeholder*
// (the same display-only role a 160 px transition frame has), and the first real
// frame is a LibRaw half-size decode - the same rendering pipeline as the full
// frame, so fit/half/full cannot drift in colour. The reference implementation
// does the same thing structurally: every displayed frame has one source.
//
// Why not keep the preview: measured on three CR3 files, even after matching
// LibRaw's exposure (`no_auto_bright = 0`, `bright = 1.10`) the full frame is
// ~0.89 of the camera JPEG with a 4 % mean error, because the in-camera tone
// curve cannot be reproduced. The only way to be truly consistent is to never
// show two different renderings of the same picture.
void RequestRawHalfFrame(HWND hwnd, const std::filesystem::path& path) {
  // `hwnd` is unused on purpose: the follow-up decode never raises the loading
  // chip (see below) and never needs a repaint of its own.
  (void)hwnd;
  if (g_state == nullptr || !g_state->decode) return;
  auto& s = *g_state;
  // Same edge the "preview too small" branch has always used: the fit tier of
  // the content rectangle, so the half frame is exactly what a revisit would
  // hit (and what gets cached as the fit tier).
  const D2D1_RECT_F content = s.renderer.ContentRect();
  const float content_long =
      std::max(content.right - content.left, content.bottom - content.top);
  const uint32_t edge = nv::ChooseTier(
      static_cast<uint32_t>(std::ceil(std::max(content_long, 1.0f))));
  s.raw_mode = "half";
  g_pending_path = path;
  // The placeholder is already on screen and this decode only sharpens it, so
  // the 400 ms "正在加载" chip must stay off (V3.1-11; the chip used to appear
  // over the camera preview and the two-stage test caught it). The start stamp
  // is still set so the completion's `load: done ms=` stays truthful.
  if (g_loading_state == 0) g_load_start_ms = NowMs();
  s.decode->Request(path, edge);
  char line[320]{};
  std::snprintf(line, sizeof(line),
                "raw: half-size request edge=%u content=%.0f path=%s", edge,
                static_cast<double>(content_long),
                Narrow(path.filename().wstring()).c_str());
  nv::platform::LogInfo(line);
}

void PromoteRawPreviewToFinalFrame(HWND hwnd,
                                   const std::filesystem::path& path,
                                   const std::shared_ptr<nv::Image>& image) {
  if (g_state == nullptr || !image || image->Empty()) return;
  auto& s = *g_state;
  const uint32_t edge = nv::ImageLongEdge(*image);
  const double load_ms = g_load_start_ms > 0.0 ? NowMs() - g_load_start_ms : 0.0;
  g_pending_path.clear();
  CancelLoading(hwnd);
  // The preview is a placeholder, not the final frame (V3.1-11): mark it like a
  // transition frame so the export/upgrade paths keep asking for real pixels and
  // the burst-settle rule can drop it.
  s.current = image;
  s.path = path;
  s.current_is_tier = true;
  s.display_edge = edge;
  s.preview_edge = edge;
  s.raw_mode = "preview";
  s.cache_source = "raw-preview";
  s.renderer.SetSourceIsTier(true);
  s.transition_active = true;
  s.transition_edge = edge;
  s.transition_source = "raw-preview";
  ++g_transition_count;
  // The preview is no longer a *pending* preview: no badge, no loading chip.
  s.preview_active = false;
  s.preview_failed = false;
  s.preview_path.clear();
  s.preview_target_width = 0;
  s.preview_target_height = 0;
  s.preview_source.clear();
  StartPreviewBadgeFade(hwnd);
  char line[512]{};
  std::snprintf(line, sizeof(line),
                "raw: preview as placeholder edge=%u size=%ux%u", edge,
                s.renderer.image_width(), s.renderer.image_height());
  nv::platform::LogInfo(line);
  std::snprintf(line, sizeof(line), "load: done ms=%.0f path=%s", load_ms,
                Narrow(path.wstring()).c_str());
  nv::platform::LogInfo(line);
  // The camera JPEG is never the fit tier any more: caching it would bring the
  // colour drift back on the next open. The half frame below is cached instead
  // (same 4.4 MB at edge 1280, same revisit hit), so S2's benefit survives while
  // the pixels stay LibRaw-rendered. `--preview-cache` therefore no longer has
  // anything to skip; it is kept as a parsed flag for the old command lines.
  UpdateTitle(hwnd);
  RefreshUi(hwnd, false);
  UpdateInfoWindowContent(hwnd);
  if (hwnd != nullptr) InvalidateRect(hwnd, nullptr, TRUE);
  SyncWatchBaseline();
  RequestRawHalfFrame(hwnd, path);
  // The request above sets `raw_mode = "half"` for the decode that is now in
  // flight; what is on screen *right now* is still the camera preview.
  s.raw_mode = "preview";
}

// V3.1-04a: no usable preview (H.265/BITMAP thumb, or a preview smaller than
// the content rectangle) — ask for a half-size decode, never the full frame at
// fit. `image` may be null (preview unavailable).
void HandleRawPreviewDecision(HWND hwnd, const std::filesystem::path& path,
                              const std::shared_ptr<nv::Image>& image) {
  if (g_state == nullptr) return;
  auto& s = *g_state;
  if (!s.renderer.ready()) {
    // No content rectangle to judge against (a first open whose preview beat the
    // renderer): fall back to the v3.0 full decode rather than guess a tier.
    s.raw_mode = "full";
    s.preview_active = false;
    s.preview_path.clear();
    g_pending_path = path;
    g_load_start_ms = NowMs();
    BeginLoading(hwnd, false);
    s.decode->Request(path, 0);
    return;
  }
  const uint32_t preview_long = image ? nv::ImageLongEdge(*image) : 0;
  const D2D1_RECT_F content = s.renderer.ContentRect();
  const float content_long =
      std::max(content.right - content.left, content.bottom - content.top);
  const bool big_enough =
      preview_long != 0 &&
      static_cast<float>(preview_long) >= 0.8f * std::max(content_long, 1.0f);
  if (big_enough && !g_settings.pixel_mode) {
    PromoteRawPreviewToFinalFrame(hwnd, path, image);
    return;
  }
  const uint32_t edge = nv::ChooseTier(
      static_cast<uint32_t>(std::ceil(std::max(content_long, 1.0f))));
  s.raw_mode = "half";
  char line[320]{};
  std::snprintf(line, sizeof(line),
                "raw: preview too small edge=%u content=%.0f -> half-size "
                "request edge=%u",
                preview_long, static_cast<double>(content_long), edge);
  nv::platform::LogInfo(line);
  s.preview_active = false;
  s.preview_failed = false;
  s.preview_path.clear();
  g_pending_path = path;
  g_load_start_ms = NowMs();
  BeginLoading(hwnd, false);
  s.decode->Request(path, edge);
}

void OnPreviewDone(HWND hwnd, LPARAM lparam) {
  auto* completion = reinterpret_cast<nv::DecodeCompletion*>(lparam);
  if (completion == nullptr) return;
  if (!g_state || !completion->preview) {
    delete completion;
    return;
  }
  auto& s = *g_state;
  // Late preview: a newer request (navigation/F5) already owns the preview
  // epoch, or the full frame is already on screen (規劃/22/13 坑 3). A cached
  // 128 px thumbnail may be upgraded by the embedded JPEG, so that case is
  // accepted too.
  const bool upgradable = s.preview_active && !s.preview_failed &&
                          s.preview_source == "cache";
  const bool current = s.decode != nullptr &&
                       completion->request_id == s.decode->CurrentPreviewId() &&
                       completion->path == g_pending_path &&
                       (!s.preview_active || upgradable);
  if (!current) {
    nv::platform::LogInfo("preview: dropped stale completion");
    delete completion;
    return;
  }
  if (!completion->result.ok()) {
    // No usable embedded preview: stay on the single-stage path; the cache
    // miss is not an error the user needs to see (規劃/22/13 坑 7).
    nv::platform::LogInfo("preview: unavailable (" +
                          completion->result.error.message + ")");
    // V3.1-04a: a RAW load deferred its main decode, so "preview unavailable"
    // must still produce a frame — the half-size request (H.265 previews).
    if (!g_raw_deferred_path.empty() &&
        g_raw_deferred_path == completion->path) {
      g_raw_deferred_path.clear();
      HandleRawPreviewDecision(hwnd, completion->path, nullptr);
    }
    delete completion;
    return;
  }

  const uint32_t target_width =
      completion->result.image->info.full_width != 0
          ? completion->result.image->info.full_width
          : completion->result.image->info.width;
  const uint32_t target_height =
      completion->result.image->info.full_height != 0
          ? completion->result.image->info.full_height
          : completion->result.image->info.height;
  const double shown_ms = g_preview_request_start_ms > 0.0
                              ? NowMs() - g_preview_request_start_ms
                              : 0.0;
  TryShowPreview(hwnd, completion->path, completion->result.image, "embedded",
                 target_width, target_height, shown_ms);
  // The preview is the displayed frame now; its "Loading…" indicator (if the
  // 400 ms delay already fired) must go away.
  if (g_state->preview_active) CancelLoading(hwnd);
  // V3.1-04a: the deferred RAW decision. Big enough for the content rectangle ->
  // this preview *is* the final frame (no 800 ms full decode); too small or no
  // preview at all -> a half-size request.
  if (!g_raw_deferred_path.empty() && g_raw_deferred_path == completion->path) {
    g_raw_deferred_path.clear();
    HandleRawPreviewDecision(hwnd, completion->path, completion->result.image);
  }
  delete completion;
}

void OnThumbnailDone(HWND hwnd, LPARAM lparam) {
  auto* completion = reinterpret_cast<nv::ThumbnailCompletion*>(lparam);
  if (completion == nullptr) return;
  if (!g_state) {
    delete completion;
    return;
  }

  // V3.1-07 S3b: the file that is *still loading* just got its 160 px frame.
  // Put it on screen now — this is the frame the load would have shown had the
  // thumbnail been cached, and it is what keeps a 5-9/s page-through from
  // leaving the previous picture up (the replica does the same synchronously in
  // ApplyCachedThumbIfAny). Never downgrade: if anything of this file is already
  // on screen (`g_state->path == path`, e.g. its tier landed first) the blur is
  // skipped, and the same-path/transition guards inside cover a second arrival.
  if (completion->image && !completion->image->Empty() &&
      !g_pending_path.empty() && completion->path == g_pending_path &&
      g_state->path != completion->path) {
    if (TryShowTransitionFrame(hwnd, completion->path, g_pending_edge)) {
      // A readable frame owns the canvas now: the "Loading…" chip would only
      // cover it (same rule as the preview path in LoadPath).
      CancelLoading(hwnd);
    }
  }

  // V3.1-07 S1: was this frame requested by the pre-read rather than by the
  // rail? Its completion is logged separately, so "a hidden rail requests no
  // previews" keeps measuring the rail's own queue.
  const bool prewarm =
      g_prewarm_paths.erase(nv::MakeCacheKey(completion->path)) != 0;

  size_t index = nv::render::ThumbnailStrip::kNone;
  if (!g_state->folder.empty()) {
    if (completion->index < g_state->folder.size() &&
        g_state->folder.files[completion->index] == completion->path) {
      index = completion->index;
    } else {
      index = FindInFolder(completion->path);
    }
  }

  if (index != nv::render::ThumbnailStrip::kNone) {
    if (completion->image) {
      g_state->strip.SetThumbnail(index, completion->image);
      nv::platform::LogInfo(std::string(prewarm ? "thumbs: prewarm "
                                                : "thumbnail ready: ") +
                            Narrow(completion->path.filename().wstring()));
    } else {
      g_state->strip.SetFailed(index);
      // A failed pre-read may be retried later; a failed rail slot is marked so
      // the rail does not ask again (HasFailed). The set only tracks attribution.
      g_prewarm_paths.erase(nv::MakeCacheKey(completion->path));
      nv::platform::LogError("thumbnail failed: " +
                             completion->error.message);
    }
    RefreshUi(hwnd, false);
    InvalidateStrip(hwnd);
  }
  delete completion;
}

// Background save finished (規劃/22/14 §4): toast on success, error card on
// failure. The atomic write already happened inside SaveImage, so a failure
// never left a partial target.
void OnSaveDone(HWND hwnd, LPARAM lparam) {
  auto* outcome = reinterpret_cast<nv::platform::SaveOutcome*>(lparam);
  if (outcome == nullptr) return;
  g_save_busy = false;
  CancelLoading(hwnd);
  if (outcome->ok) {
    const bool animated =
        g_state && g_state->current && g_state->current->frames.size() > 1;
    char line[512]{};
    std::snprintf(line, sizeof(line),
                  "save: done format=%s bytes=%llu ms=%.0f animated=%d path=%s",
                  nv::platform::SaveFormatToken(
                      nv::platform::ImageFormatFromPath(
                          outcome->target, nv::platform::ImageSaveFormat::kPng)),
                  static_cast<unsigned long long>(outcome->bytes), outcome->ms,
                  animated ? 1 : 0,
                  Narrow(outcome->target.wstring()).c_str());
    nv::platform::LogInfo(line);
    ShowToast(hwnd, nv::i18n::Text(animated
                                       ? nv::i18n::StringId::kToastImageSavedFrame
                                       : nv::i18n::StringId::kToastImageSaved));
    // Our own write must not look like an external modification, and a save to
    // the current folder should appear immediately (22/10 suppression window).
    if (g_state != nullptr && !g_state->folder.folder.empty() &&
        outcome->target.parent_path().lexically_normal() ==
            g_state->folder.folder.lexically_normal()) {
      g_watcher.Suppress(NowMs());
      if (!g_state->path.empty()) ApplyWatchedFolderChange(hwnd);
    }
  } else {
    nv::platform::LogError("save: failed detail=" + Narrow(outcome->error));
    ShowError(hwnd, OperationError(nv::ErrorKind::kSaveFailed, outcome->target,
                                   outcome->error,
                                   nv::i18n::StringId::kErrorSaveImage));
  }
  delete outcome;
  RefreshUi(hwnd, false);
  InvalidateRect(hwnd, nullptr, FALSE);
}

void Paint(HWND hwnd) {
  PAINTSTRUCT ps{};
  HDC dc = BeginPaint(hwnd, &ps);

  // Fast first paint: before the GPU device exists the window is filled with
  // the theme background using GDI, so the window becomes visible without
  // waiting ~200 ms for the driver. The color matches DrawFrame's Clear.
  if (!g_state || !g_state->renderer.ready()) {
    if (g_background_brush != nullptr) {
      FillRect(dc, &ps.rcPaint, g_background_brush);
    }
    // Paint the title/bottom bands so the very first frame already has the
    // v2.0 chrome silhouette instead of a flat rectangle.
    if (g_ui != nullptr) g_ui->PaintGdiBackground(dc);
    // A fatal startup failure (no D3D device) is drawn with GDI: the custom
    // error card needs the renderer that just failed (規劃/22/11 §5).
    if (!g_fatal_error_text.empty()) {
      RECT client{};
      GetClientRect(hwnd, &client);
      HFONT font = CreateFontW(
          -nv::platform::DpiPixels(14.0f,
                                   nv::platform::DpiScaleForWindow(hwnd)),
          0, 0, 0, FW_SEMIBOLD, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
          OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
          DEFAULT_PITCH | FF_DONTCARE, L"Segoe UI");
      HGDIOBJ previous = font != nullptr ? SelectObject(dc, font) : nullptr;
      SetBkMode(dc, TRANSPARENT);
      SetTextColor(dc, RGB(232, 234, 237));
      RECT area = client;
      area.left += 32;
      area.right -= 32;
      DrawTextW(dc, g_fatal_error_text.c_str(),
                static_cast<int>(g_fatal_error_text.size()), &area,
                DT_CENTER | DT_VCENTER | DT_WORDBREAK);
      if (previous != nullptr) SelectObject(dc, previous);
      if (font != nullptr) DeleteObject(font);
    }
    EndPaint(hwnd, &ps);
    if (!g_gdi_paint_reported) {
      g_gdi_paint_reported = true;
      ReportStartupPhase("gdi_paint");
    }
    return;
  }

  // Borderless resize safety net: the first show can grow the client rectangle
  // after the last WM_SIZE (DWM applies the new non-client calculation), so the
  // swap chain and the cached layout are re-synced here before drawing.
  if (g_ui != nullptr) {
    RECT client{};
    if (GetClientRect(hwnd, &client) && client.right > 0 && client.bottom > 0) {
      const uint32_t width = static_cast<uint32_t>(client.right);
      const uint32_t height = static_cast<uint32_t>(client.bottom);
      if (g_state->renderer.client_width() != width ||
          g_state->renderer.client_height() != height) {
        g_state->renderer.Resize(width, height);
        ApplyDpiScale(hwnd);
        if (!g_state->strip.empty() &&
            g_state->strip.current() != nv::render::ThumbnailStrip::kNone) {
          g_state->strip.EnsureVisible(g_state->strip.current());
        }
        RequestVisibleThumbnails();
      } else {
        const nv::ui::Rect& cached = g_ui->layout().client;
        if (std::fabs(cached.width() - static_cast<float>(width)) > 0.5f ||
            std::fabs(cached.height() - static_cast<float>(height)) > 0.5f) {
          RefreshUi(hwnd, false);
        }
      }
    }
  }

  // Animation frames keep the cheap interpolation path; a paused animation is a
  // static picture and upgrades to cubic on the next repaint (規劃/22/18 §3.4).
  g_state->renderer.SetAnimating(g_state->anim.playing());
  g_state->renderer.Render();
  EndPaint(hwnd, &ps);
  if (!g_first_image_reported && g_state->renderer.image_width() != 0) {
    g_first_image_reported = true;
    ReportStartupPhase("first_image");
    LogMemoryUsage("first_image");
  }
}

// Test hook: renders one frame, writes it as PNG, then closes the window.
void TakeScreenshot(HWND hwnd) {
  if (!g_state || g_screenshot_path.empty()) return;

  // §7.6: a 100 % capture must contain file pixels, otherwise the same view
  // would hash differently with --display-cache=on and the 22/18 quality
  // baselines would silently drift. A fit-scale capture is left alone on
  // purpose: it is the tier rendering the user is looking at, and the V3-04
  // sharpness A/B compares exactly that.
  if (!EnsureFullForExport(hwnd, ExportPixels::kScreen)) return;

  std::vector<uint8_t> pixels;
  uint32_t width = 0;
  uint32_t height = 0;
  if (!g_state->renderer.RenderAndCapture(&pixels, &width, &height)) {
    nv::platform::LogError("screenshot: frame capture failed");
  } else if (!nv::platform::SavePng(g_screenshot_path, pixels.data(), width,
                                    height, width * 4)) {
    nv::platform::LogError("screenshot: PNG encode failed: " +
                           Narrow(g_screenshot_path.wstring()));
  } else {
    nv::platform::LogInfo("screenshot saved: " +
                          Narrow(g_screenshot_path.wstring()) + " " +
                          std::to_string(width) + "x" + std::to_string(height));
  }
  CloseInfoWindowForAppClose();
  DestroyWindow(hwnd);
}

// --- Single-instance activation (規劃/22/15 §5.2) --------------------------
void ActivateMainWindow(HWND hwnd) {
  if (hwnd == nullptr) return;
  if (IsIconic(hwnd)) {
    ShowWindow(hwnd, SW_RESTORE);
  } else {
    ShowWindow(hwnd, SW_SHOW);
  }
  SetForegroundWindow(hwnd);
  FLASHWINFO flash{};
  flash.cbSize = sizeof(flash);
  flash.hwnd = hwnd;
  flash.dwFlags = FLASHW_ALL | FLASHW_TIMERNOFG;
  flash.uCount = 3;
  FlashWindowEx(&flash);
}

// A forwarded path from another launch: open it in the existing window and
// bring that window forward. An empty path is a bare activation (double-click
// without a file argument).
void HandleInstanceRequest(const std::wstring& path) {
  HWND hwnd = g_main_window;
  if (hwnd == nullptr) return;
  if (!path.empty()) {
    nv::platform::LogInfo("single_instance: open=" + Narrow(path));
    OpenFile(hwnd, path);
  } else {
    nv::platform::LogInfo("single_instance: activate");
  }
  ActivateMainWindow(hwnd);
  nv::platform::LogInfo("single_instance: activate existing");
}

LRESULT CALLBACK WindowProc(HWND hwnd, UINT message, WPARAM wparam, LPARAM lparam) {
  switch (message) {
    case kDeferredToast:
      // V3.1-06 (§06d): the turn after a context-menu command; paint the toast
      // with the menu already gone.
      ShowToast(hwnd, nv::i18n::Text(static_cast<nv::i18n::StringId>(wparam)));
      return 0;
    case kRenameKey:
      // V3.1-06 (§06e): Enter commits, Escape cancels — the same keys the native
      // dialog had, now owned by the overlay.
      if (wparam == VK_RETURN) {
        CommitRenameOverlay(hwnd);
      } else {
        CancelRenameOverlay(hwnd);
      }
      return 0;
    case WM_CTLCOLOREDIT: {
      // The rename EDIT paints only text: the box, background and border belong
      // to the overlay card, so the two never look like different widgets.
      if (g_rename_edit != nullptr &&
          reinterpret_cast<HWND>(lparam) == g_rename_edit &&
          g_ui != nullptr) {
        HDC dc = reinterpret_cast<HDC>(wparam);
        SetBkMode(dc, TRANSPARENT);
        SetTextColor(dc, RGB(230, 232, 236));
        return reinterpret_cast<LRESULT>(g_rename_brush);
      }
      break;
    }
    // --- Borderless window (規劃/22/01 §4.1) --------------------------------
    // The window keeps WS_OVERLAPPEDWINDOW so snap, resize, Alt+Space and
    // Alt+F4 stay native; only the non-client area is removed here.
    case WM_NCCALCSIZE:
      if (!g_borderless || wparam != TRUE) break;
      if (!g_window_state.fullscreen() && IsZoomed(hwnd)) {
        // A maximized window is inflated by the invisible resize frame. Clamp
        // the client rectangle to the monitor work area so nothing is pushed
        // off-screen and the taskbar stays visible (規劃/22/01 坑 1).
        auto* params = reinterpret_cast<NCCALCSIZE_PARAMS*>(lparam);
        MONITORINFO monitor{};
        monitor.cbSize = sizeof(monitor);
        if (GetMonitorInfoW(MonitorFromWindow(hwnd, MONITOR_DEFAULTTONEAREST),
                            &monitor)) {
          params->rgrc[0] = monitor.rcWork;
        }
      }
      // Otherwise the client covers the whole window: there is no non-client
      // area left in the borderless frame.
      return 0;

    case WM_NCHITTEST: {
      if (!g_borderless) break;
      const POINT screen{GET_X_LPARAM(lparam), GET_Y_LPARAM(lparam)};
      POINT client = screen;
      ScreenToClient(hwnd, &client);

      if (!g_window_state.fullscreen()) {
        RECT window{};
        GetWindowRect(hwnd, &window);
        const float scale = nv::platform::DpiScaleForWindow(hwnd);
        const LONG border =
            std::max<LONG>(4, nv::platform::DpiPixels(
                                  nv::ui::metrics::kResizeBorder, scale));
        const bool zoomed = IsZoomed(hwnd) != FALSE;
        if (!zoomed) {
          const bool left = screen.x < window.left + border;
          const bool right = screen.x >= window.right - border;
          const bool top = screen.y < window.top + border;
          const bool bottom = screen.y >= window.bottom - border;
          if (top && left) return HTTOPLEFT;
          if (top && right) return HTTOPRIGHT;
          if (bottom && left) return HTBOTTOMLEFT;
          if (bottom && right) return HTBOTTOMRIGHT;
          if (left) return HTLEFT;
          if (right) return HTRIGHT;
          if (top) return HTTOP;
          if (bottom) return HTBOTTOM;
        }

        if (g_ui != nullptr) {
          // v3.0 (規劃/26 §2.9): while a menu popup is open it owns the pointer.
          // Caption hits must go to the client so the click closes the popup
          // (the standard outside-click behaviour) instead of maximizing or
          // dragging the window behind the menu.
          if (g_ui->overlay().popup() != nv::ui::OverlayKind::kNone) {
            return HTCLIENT;
          }
          const nv::ui::HitId hit = g_ui->HitTestTitleButtons(
              static_cast<float>(client.x), static_cast<float>(client.y));
          if (hit == nv::ui::HitId::kTitleMaximize) {
            // HTMAXBUTTON keeps the Windows 11 snap-layout flyout and lets the
            // system handle the click (規劃/22/02 坑 1/2).
            return HTMAXBUTTON;
          }
          if (nv::ui::IsTitleButton(hit)) return HTCLIENT;
          if (!g_ui->layout().title.empty() &&
              client.y < static_cast<LONG>(g_ui->layout().title.bottom)) {
            return HTCAPTION;
          }
        }
      }
      return HTCLIENT;
    }

    case WM_GETMINMAXINFO: {
      auto* info = reinterpret_cast<MINMAXINFO*>(lparam);
      const float scale = nv::platform::DpiScaleForWindow(hwnd);
      info->ptMinTrackSize.x =
          nv::platform::DpiPixels(nv::ui::metrics::kMinWindowWidth, scale);
      info->ptMinTrackSize.y =
          nv::platform::DpiPixels(nv::ui::metrics::kMinWindowHeight, scale);
      if (g_borderless) {
        MONITORINFO monitor{};
        monitor.cbSize = sizeof(monitor);
        if (GetMonitorInfoW(MonitorFromWindow(hwnd, MONITOR_DEFAULTTONEAREST),
                            &monitor)) {
          info->ptMaxPosition.x = monitor.rcWork.left - monitor.rcMonitor.left;
          info->ptMaxPosition.y = monitor.rcWork.top - monitor.rcMonitor.top;
          info->ptMaxSize.x = monitor.rcWork.right - monitor.rcWork.left;
          info->ptMaxSize.y = monitor.rcWork.bottom - monitor.rcWork.top;
        }
      }
      return 0;
    }

    case WM_NCACTIVATE:
      if (!g_borderless) break;
      g_window_active = wparam != FALSE;
      if (g_ui != nullptr) g_ui_inputs.window_active = g_window_active;
      InvalidateRect(hwnd, nullptr, FALSE);
      // Returning TRUE suppresses the default non-client painting: there is no
      // non-client area left to draw.
      return TRUE;

    case WM_NCPAINT:
      if (!g_borderless) break;
      return 0;

    case WM_ACTIVATE:
      g_window_active = LOWORD(wparam) != WA_INACTIVE;
      InvalidateRect(hwnd, nullptr, FALSE);
      return 0;

    case WM_NCMOUSEMOVE:
      if (g_borderless && wparam == HTMAXBUTTON && g_ui != nullptr) {
        if (g_ui->hover() != nv::ui::HitId::kTitleMaximize) {
          g_ui->SetHover(nv::ui::HitId::kTitleMaximize);
          InvalidateRect(hwnd, nullptr, FALSE);
        }
      }
      break;

    case WM_NCMOUSELEAVE:
      if (g_ui != nullptr && nv::ui::IsTitleButton(g_ui->hover())) {
        g_ui->SetHover(nv::ui::HitId::kNone);
        InvalidateRect(hwnd, nullptr, FALSE);
      }
      return 0;

    // --- Title bar clicks (規劃/22/02 §3.1) ---------------------------------
    // The caption stays HTCAPTION, so the system owns the press and runs its own
    // move loop; for a click without movement it sends neither WM_NCLBUTTONUP
    // nor WM_EXITSIZEMOVE (verified on this host). The release is therefore
    // polled with a timer, and the click is confirmed by two facts: the window
    // rectangle did not change and the cursor stayed near the press point.
    // A following double-click (maximize/restore) arrives after the first
    // release, so acting is deferred by the system double-click time and
    // cancelled by WM_NCLBUTTONDBLCLK.
    case WM_NCLBUTTONDOWN:
      // v3.0 (規劃/26 §2.6): the maximize button is HTMAXBUTTON so the Win11
      // snap-layout flyout keeps working, but this host turns that press into
      // neither SC_MAXIMIZE nor WM_NCLBUTTONUP — it delivers the press and then
      // swallows the release (verified with a DefWindowProc probe). Run the same
      // press/release pair the other title buttons use: capture here, and the
      // client WM_LBUTTONUP dispatches kTitleMaximize (SC_MAXIMIZE/SC_RESTORE).
      if (g_borderless && !g_window_state.fullscreen() && g_ui != nullptr &&
          wparam == HTMAXBUTTON) {
        g_ui->SetPressed(nv::ui::HitId::kTitleMaximize);
        SetCapture(hwnd);
        return 0;
      }
      if (g_borderless && !g_window_state.fullscreen() && g_ui != nullptr) {
        const POINT screen{GET_X_LPARAM(lparam), GET_Y_LPARAM(lparam)};
        POINT client = screen;
        ScreenToClient(hwnd, &client);
        if (g_ui->HitTestTitleButtons(static_cast<float>(client.x),
                                     static_cast<float>(client.y)) ==
                nv::ui::HitId::kNone &&
            !g_ui->layout().title.empty() &&
            static_cast<float>(client.y) < g_ui->layout().title.bottom) {
          g_title_press.valid = true;
          g_title_press.confirmed = false;
          g_title_press.client = client;
          GetWindowRect(hwnd, &g_title_press.window);
          SetTimer(hwnd, kTitleClickTimer, 30, nullptr);
        }
      }
      break;

    case WM_NCLBUTTONDBLCLK:
      // The second click of a double-click maximizes/restores; it must not also
      // run a segment action.
      if (g_title_press.valid) {
        g_title_press.valid = false;
        KillTimer(hwnd, kTitleClickTimer);
      }
      break;

    case WM_NCLBUTTONUP:
      // Keep the press alive: the confirmation happens in the timer handler,
      // which also waits out the double-click time.
      break;

    case WM_SYSCOMMAND:
      // Alt+Space must keep opening the system menu after the frame is gone.
      if ((wparam & 0xFFF0) == SC_KEYMENU) {
        return DefWindowProcW(hwnd, message, wparam, lparam);
      }
      break;

    case WM_ERASEBKGND:
      // While the GDI-only first frame is on screen, let the class background
      // brush erase so a compositor snapshot before WM_PAINT still matches the
      // theme. Once D3D owns the window, erasing would flicker.
      if (g_state && g_state->renderer.ready()) return 1;
      return DefWindowProcW(hwnd, message, wparam, lparam);

    case WM_PAINT:
      Paint(hwnd);
      return 0;

    case WM_TIMER:
      if (wparam == nv::render::kQualityUpgradeTimerId) {
        // The renderer owns the 150 ms interaction window. A queued WM_TIMER can
        // arrive just after a new interaction re-armed the timer; killing it
        // there would leave the frame in linear forever, so re-arm while the
        // window is still open (規劃/22/18 §3.2).
        const int remaining =
            g_state != nullptr ? g_state->renderer.interactive_remaining_ms()
                               : 0;
        if (remaining > 0) {
          SetTimer(hwnd, nv::render::kQualityUpgradeTimerId,
                   static_cast<UINT>(remaining + 16), nullptr);
          return 0;
        }
        KillTimer(hwnd, nv::render::kQualityUpgradeTimerId);
        InvalidateRect(hwnd, nullptr, FALSE);
        return 0;
      }
      if (wparam == kLayoutDumpTimer) {
        KillTimer(hwnd, kLayoutDumpTimer);
        WriteLayoutDump(hwnd, true);
        return 0;
      }
      // --dump-at / --quit-at / --minimize-at: deterministic hooks for
      // tests/run_watch_check.ps1, which mutates files while the app runs.
      if (wparam == kDumpAtTimer) {
        KillTimer(hwnd, kDumpAtTimer);
        WriteLayoutDump(hwnd, false);
        return 0;
      }
      if (wparam == kQuitTimer) {
        KillTimer(hwnd, kQuitTimer);
        PostMessageW(hwnd, WM_CLOSE, 0, 0);
        return 0;
      }
      if (wparam == kMinimizeTimer) {
        KillTimer(hwnd, kMinimizeTimer);
        ShowWindow(hwnd, SW_MINIMIZE);
        nv::platform::LogInfo("test: minimized");
        return 0;
      }
      if (wparam == kRestoreTimer) {
        KillTimer(hwnd, kRestoreTimer);
        ShowWindow(hwnd, SW_RESTORE);
        nv::platform::LogInfo("test: restored");
        return 0;
      }
      if (wparam == kPreviewBadgeTimer) {
        // Fade the "Preview" badge after the sharp frame arrived (規劃/22/13
        // §3.5). Only the opacity changes; layout and the image are untouched.
        const double now = NowMs();
        const double dt = g_preview_badge_tick_ms > 0.0
                              ? now - g_preview_badge_tick_ms
                              : 0.0;
        g_preview_badge_tick_ms = now;
        g_preview_badge_opacity -=
            static_cast<float>(dt / nv::ui::metrics::kPreviewBadgeFadeMs);
        if (g_preview_badge_opacity <= 0.0f) {
          g_preview_badge_opacity = 0.0f;
          KillTimer(hwnd, kPreviewBadgeTimer);
        }
        RefreshUi(hwnd, false);
        InvalidateRect(hwnd, nullptr, FALSE);
        return 0;
      }
      if (wparam == kWatchTimer) {
        // Poll mode keeps the 2 s cadence; notify mode only uses this timer to
        // retry a change that arrived while the UI was paused.
        OnWatchTick(hwnd);
        return 0;
      }
      if (wparam == kBurstSettleTimer) {
        // V3.1-01: 400 ms after the last navigation the burst is over and the
        // prefetch gate returns to its V3-04 shape.
        OnBurstSettle(hwnd);
        return 0;
      }
      if (wparam == kThumbPrewarmTimer) {
        // V3.1-07 S1: the view has been at rest for 400 ms — fill the 160 px
        // frames the next burst will need.
        KillTimer(hwnd, kThumbPrewarmTimer);
        PrewarmThumbnails(hwnd);
        // V3.1-07 S4: the same settle is the one moment a RAW neighbour may be
        // prefetched without racing the next file's embedded preview (the
        // per-load plan never includes RAW; see PrefetchAdjacent).
        if (!g_nav_burst) PrefetchAdjacent(/*allow_raw=*/true);
        return 0;
      }
      if (wparam == kThumbPrewarmStepTimer) {
        // V3.1-07 S1-1: the 40 ms gate between two pre-read requests.
        PumpPrewarmStep(hwnd);
        return 0;
      }
      if (wparam == kScreenshotTimer) {
        KillTimer(hwnd, kScreenshotTimer);
        TakeScreenshot(hwnd);
        return 0;
      }
      if (wparam == kAnimationTimer) {
        AdvanceAnimation(hwnd);
        return 0;
      }
      if (wparam == kAnimPillTimer) {
        TickAnimPillFade(hwnd);
        return 0;
      }
      if (wparam == kTitleClickTimer) {
        if (!g_title_press.valid) {
          KillTimer(hwnd, kTitleClickTimer);
          return 0;
        }
        // Still pressed: the user may be dragging. Keep waiting; the rectangle
        // check below discards a press that turned into a move.
        if ((GetAsyncKeyState(VK_LBUTTON) & 0x8000) != 0) return 0;

        if (!g_title_press.confirmed) {
          // Released. Wait out the system double-click time so the second click
          // of a double-click (maximize/restore) cannot also copy something.
          g_title_press.confirmed = true;
          SetTimer(hwnd, kTitleClickTimer, GetDoubleClickTime(), nullptr);
          return 0;
        }

        RECT now{};
        GetWindowRect(hwnd, &now);
        const bool moved = now.left != g_title_press.window.left ||
                           now.top != g_title_press.window.top ||
                           now.right != g_title_press.window.right ||
                           now.bottom != g_title_press.window.bottom;
        POINT cursor{};
        GetCursorPos(&cursor);
        ScreenToClient(hwnd, &cursor);
        const POINT pressed = g_title_press.client;
        // A drag that returned to its start position is not a click either.
        const int slack = GetSystemMetrics(SM_CXDRAG);
        const bool stayed =
            std::abs(cursor.x - pressed.x) <= slack &&
            std::abs(cursor.y - pressed.y) <= slack;
        g_title_press.valid = false;
        KillTimer(hwnd, kTitleClickTimer);
        if (!moved && stayed) {
          HandleTitleSegmentClick(hwnd, static_cast<float>(pressed.x),
                                  static_cast<float>(pressed.y));
          InvalidateRect(hwnd, nullptr, FALSE);
        }
        return 0;
      }
      if (wparam == kToastTimer) {
        ExpireToast(hwnd);
        return 0;
      }
      if (wparam == kLoadingDelayTimer) {
        // 400 ms passed while the decode is still running: show the indicator
        // (規劃/22/11 §4.4).
        KillTimer(hwnd, kLoadingDelayTimer);
        if (g_loading_state == 1) ShowLoadingNow(hwnd);
        return 0;
      }
      if (wparam == kLoadingTickTimer) {
        if (g_loading_state != 2) {
          KillTimer(hwnd, kLoadingTickTimer);
          return 0;
        }
        g_loading_phase += 0.05f;
        if (g_loading_phase > 1.0f) g_loading_phase -= 1.0f;
        RefreshUi(hwnd, false);
        InvalidateRect(hwnd, nullptr, FALSE);
        return 0;
      }
      if (wparam == kConfirmTimer) {
        KillTimer(hwnd, kConfirmTimer);
        if (g_test_force_confirm) {
          g_test_force_confirm = false;
          DeleteCurrent(hwnd);
        }
        return 0;
      }
      if (wparam == kLoadingEndTimer) {
        // --force-loading <ms>: end the simulated load and let the empty state
        // return.
        KillTimer(hwnd, kLoadingEndTimer);
        nv::platform::LogInfo("load: forced_end");
        g_force_loading = false;
        g_pending_path.clear();
        CancelLoading(hwnd);
        RefreshUi(hwnd, true);
        return 0;
      }
      if (wparam == kSaveAsTimer) {
        KillTimer(hwnd, kSaveAsTimer);
        if (!g_test_save_as.empty()) {
          const std::filesystem::path target = g_test_save_as;
          g_test_save_as.clear();
          SaveCurrentImageTo(hwnd, target, false);
        }
        if (!g_test_save_to.empty()) {
          const std::filesystem::path target = g_test_save_to;
          const int quality =
              g_test_save_quality > 0 ? g_test_save_quality : 90;
          g_test_save_to.clear();
          // --save-to also accepts .webp/.gif, which the async product path
          // would show a quality card for; the hook saves synchronously.
          SaveCurrentImageTo(hwnd, target, true, quality);
        }
        return 0;
      }
      if (wparam == kSaveOptionsTimer) {
        KillTimer(hwnd, kSaveOptionsTimer);
        if (!g_test_save_options.empty()) {
          const std::filesystem::path target = g_test_save_options;
          g_test_save_options.clear();
          SaveAsDialog(hwnd, target);
        }
        return 0;
      }
      if (wparam == kInfoScrollTimer) {
        KillTimer(hwnd, kInfoScrollTimer);
        if (!ApplyTestInfoScroll(hwnd) && g_test_info_scroll_attempts < 20) {
          ++g_test_info_scroll_attempts;
          SetTimer(hwnd, kInfoScrollTimer, 200, nullptr);
        }
        return 0;
      }
      if (wparam == kInfoWindowTimer) {
        // --info-window / the restored "was open" flag: wait for the first image
        // before creating the card (a window without a file would be empty).
        KillTimer(hwnd, kInfoWindowTimer);
        if (InfoWindowVisible()) return 0;
        if (g_state == nullptr || g_state->path.empty()) {
          if (++g_info_open_attempts < 25) {
            SetTimer(hwnd, kInfoWindowTimer, 200, nullptr);
          }
          return 0;
        }
        if (g_test_open_info_window || g_settings.info_window_visible) {
          g_info_restore_pending = false;
          OpenInfoWindow(hwnd);
          // The hook can run while fullscreen/minimised is already active (e.g.
          // --fullscreen starts earlier): the card must not appear there.
          SyncInfoWindowVisibility(hwnd);
        }
        return 0;
      }
      if (wparam == kInfoKeyTimer) {
        KillTimer(hwnd, kInfoKeyTimer);
        if (g_test_info_key == 0) return 0;
        if (!InfoWindowVisible()) {
          // A slow (ASan) start can beat the open timer; retry instead of
          // recording a false "the key did nothing".
          if (++g_info_key_attempts < 25) {
            SetTimer(hwnd, kInfoKeyTimer, 200, nullptr);
          }
          return 0;
        }
        const unsigned key = g_test_info_key;
        const bool sent = g_info_window->SendKey(key);
        char line[64]{};
        std::snprintf(line, sizeof(line), "test: info_key %s %s",
                      key == VK_ESCAPE ? "escape" : "i",
                      sent ? "sent" : "skipped");
        nv::platform::LogInfo(line);
        g_test_info_key = 0;
        return 0;
      }
      if (wparam == kInfoScreenshotTimer) {
        KillTimer(hwnd, kInfoScreenshotTimer);
        if (g_test_info_screenshot.empty()) return 0;
        if (!InfoWindowVisible()) {
          if (++g_info_shot_attempts < 25) {
            SetTimer(hwnd, kInfoScreenshotTimer, 200, nullptr);
            return 0;
          }
          nv::platform::LogError("info_screenshot: window not open");
          g_test_info_screenshot.clear();
          return 0;
        }
        const std::filesystem::path path = g_test_info_screenshot;
        g_test_info_screenshot.clear();
        if (g_info_window->CaptureToPng(path)) {
          nv::platform::LogInfo("info_screenshot saved: " +
                                Narrow(path.wstring()));
        } else {
          nv::platform::LogError("info_screenshot: capture failed");
        }
        return 0;
      }
      if (wparam == kSettingsTimer) {
        // Test hook driver for the settings window: open, run the queued
        // activations one per tick, then dump/capture and end the process.
        if (g_settings_window == nullptr || !g_settings_window->IsOpen()) {
          const bool wanted = g_test_open_settings ||
                              !g_test_settings_actions.empty() ||
                              !g_test_settings_dump.empty() ||
                              !g_test_settings_screenshot.empty();
          if (!wanted) {
            KillTimer(hwnd, kSettingsTimer);
            return 0;
          }
          OpenSettingsWindow(hwnd);
          if (g_settings_window == nullptr || !g_settings_window->IsOpen()) {
            nv::platform::LogError("settings: test open failed");
            KillTimer(hwnd, kSettingsTimer);
            return 0;
          }
          SetTimer(hwnd, kSettingsTimer, 300, nullptr);
          return 0;
        }
        if (static_cast<size_t>(g_test_settings_action_index) <
            g_test_settings_actions.size()) {
          const std::wstring name =
              g_test_settings_actions[static_cast<size_t>(
                  g_test_settings_action_index++)];
          nv::platform::LogInfo("test: settings_click=" + Narrow(name));
          g_settings_window->ActivateByName(name);
          SetTimer(hwnd, kSettingsTimer, 200, nullptr);
          return 0;
        }
        KillTimer(hwnd, kSettingsTimer);
        if (!g_test_settings_dump.empty()) {
          WriteUtf8(g_test_settings_dump,
                    ToUtf8(g_settings_window->DumpState()));
          nv::platform::LogInfo("test: settings_dump=" +
                                Narrow(g_test_settings_dump.wstring()));
        }
        if (!g_test_settings_screenshot.empty()) {
          const bool captured = g_settings_window->CaptureToPng(
              g_test_settings_screenshot.wstring());
          nv::platform::LogInfo(std::string("test: settings_shot=") +
                                (captured ? "ok" : "failed"));
        }
        const bool end = !g_test_settings_dump.empty() ||
                         !g_test_settings_screenshot.empty();
        g_test_settings_dump.clear();
        g_test_settings_screenshot.clear();
        if (end) {
          CloseInfoWindowForAppClose();
          DestroyWindow(hwnd);
        }
        return 0;
      }
      if (wparam == kStripFadeTimer) {
        TickStripFade(hwnd);
        return 0;
      }
      if (wparam == kArrowFadeTimer) {
        TickArrowFade(hwnd);
        return 0;
      }
      if (wparam == kArrowIdleTimer) {
        KillTimer(hwnd, kArrowIdleTimer);
        const double idle = NowMs() - g_arrow_last_activity_ms;
        if (idle >= static_cast<double>(kArrowIdleMs) - 1.0) {
          HideArrows(hwnd, "idle");
        } else {
          // Activity landed between the deadline and this message: re-arm for
          // what is left instead of hiding the pair the user just asked for.
          SetTimer(hwnd, kArrowIdleTimer,
                   static_cast<UINT>(std::max(16.0,
                                              static_cast<double>(kArrowIdleMs) -
                                                  idle)),
                   nullptr);
        }
        return 0;
      }
      if (wparam == kFsRevealTimer) {
        TickFullscreenReveal(hwnd);
        return 0;
      }
      if (wparam == kFullscreenTestTimer) {
        KillTimer(hwnd, kFullscreenTestTimer);
        if (!g_window_state.fullscreen()) ToggleFullscreen(hwnd);
        return 0;
      }
      if (wparam == kStripHideTimer) {
        // Grace period over: start the 160 ms fade out. Re-entering the rail
        // cancels this timer in ShowStrip().
        KillTimer(hwnd, kStripHideTimer);
        g_strip_hide_at_ms = 0.0;
        if (StripRevealEnabled()) {
          g_strip_target = 0.0f;
          // V3.1-09: remember when the fade started so the end can be measured.
          g_strip_hide_start_ms = NowMs();
          nv::platform::LogInfo("ui: strip=hide");
          if (!StripAnimationsEnabled()) {
            // Reduced motion: one-frame hide; the state matches the fade end.
            g_strip_opacity = 0.0f;
            if (g_state && g_state->thumbs) g_state->thumbs->CancelPending();
            nv::platform::LogInfo("ui: strip=hidden");
            RefreshUi(hwnd, true);
            InvalidateStrip(hwnd);
            return 0;
          }
          g_strip_fade_tick_ms = NowMs();
          SetTimer(hwnd, kStripFadeTimer, 16, nullptr);
        }
        return 0;
      }
      if (wparam == kStripTooltipTimer) {
        KillTimer(hwnd, kStripTooltipTimer);
        ShowStripTooltip(hwnd);
        return 0;
      }
      if (wparam == kHoverTestTimer) {
        // --hover-thumb <index>: wait for the first image, make the auto rail
        // visible, then post a real WM_MOUSEMOVE at the slot centre so the
        // hover/tooltip path runs unchanged (規劃/22/08 §7).
        if ((g_test_hover_thumb < 0 && !g_test_hover_hotzone &&
             !g_test_hover_cycle && !g_test_hover_bottom &&
             !g_test_drag_to_bottom && !g_test_hover_fs) ||
            !g_state) {
          KillTimer(hwnd, kHoverTestTimer);
          return 0;
        }
        if (!g_ui_inputs.has_image) {
          if (++g_test_hover_attempts < 40) {
            SetTimer(hwnd, kHoverTestTimer, 250, nullptr);
          } else {
            KillTimer(hwnd, kHoverTestTimer);
            nv::platform::LogInfo("test: hover_thumb=skipped no_image");
          }
          return 0;
        }
        if (g_test_hover_cycle) {
          // Phase 0 reveals through the hot zone, phase 1 moves back into the
          // image; the app then owns the 400 ms grace and the fade out.
          const nv::ui::Rect strip = StripRect();
          if (strip.empty()) {
            KillTimer(hwnd, kHoverTestTimer);
            nv::platform::LogInfo("test: hover_cycle=skipped no_rect");
            return 0;
          }
          const int x = static_cast<int>(std::lround(strip.width() * 0.5f));
          if (g_test_hover_phase == 0) {
            const float hot = nv::ui::metrics::kStripHotZone *
                              (g_ui != nullptr ? g_ui->dpi_scale() : 1.0f);
            const int y =
                static_cast<int>(std::lround(strip.bottom - hot * 0.5f));
            nv::platform::LogInfo("test: hover_cycle=enter");
            MoveCursorToClient(hwnd, x, y);
            PostMessageW(hwnd, WM_MOUSEMOVE, 0, MAKELPARAM(x, y));
            g_test_hover_phase = 1;
            SetTimer(hwnd, kHoverTestTimer, 700, nullptr);
            return 0;
          }
          if (g_test_hover_phase == 1) {
            const int y = static_cast<int>(
                std::lround(strip.top - 120.0f *
                                          (g_ui != nullptr ? g_ui->dpi_scale()
                                                           : 1.0f)));
            nv::platform::LogInfo("test: hover_cycle=leave");
            MoveCursorToClient(hwnd, x, y);
            PostMessageW(hwnd, WM_MOUSEMOVE, 0, MAKELPARAM(x, y));
            g_test_hover_phase = 2;
            // 400 ms grace + 160 ms fade, plus slack for the timer.
            SetTimer(hwnd, kHoverTestTimer, 1200, nullptr);
            return 0;
          }
          KillTimer(hwnd, kHoverTestTimer);
          nv::platform::LogInfo("test: hover_cycle=done");
          if (g_hover_owns_dump) {
            SetTimer(hwnd, kLayoutDumpTimer, 200, nullptr);
          }
          return 0;
        }
        if (g_test_hover_bottom) {
          // v3.0 (規劃/26 §2.10): a plain move onto the bottom bar must reveal
          // the auto rail; the same WM_MOUSEMOVE handler runs, so this covers
          // the real "reach for the bottom edge" gesture.
          KillTimer(hwnd, kHoverTestTimer);
          const nv::ui::Rect bottom = g_ui->layout().bottom_bar;
          if (bottom.empty()) {
            nv::platform::LogInfo("test: hover_bottom=skipped no_rect");
            return 0;
          }
          const int x = static_cast<int>(
              std::lround((bottom.left + bottom.right) * 0.5f));
          const int y = static_cast<int>(
              std::lround((bottom.top + bottom.bottom) * 0.5f));
          nv::platform::LogInfo("test: hover_bottom at=" + std::to_string(x) +
                                "," + std::to_string(y));
          MoveCursorToClient(hwnd, x, y);
          PostMessageW(hwnd, WM_MOUSEMOVE, 0, MAKELPARAM(x, y));
          if (g_hover_owns_dump) {
            SetTimer(hwnd, kLayoutDumpTimer, 600, nullptr);
          }
          return 0;
        }
        if (g_test_drag_to_bottom) {
          // v3.0 (規劃/26 §2.10): press inside the image, pan a little, then
          // drag onto the bottom bar. The button stays down so the dump records
          // a live drag: `dragging=1` and `strip_shown=0`.
          const nv::ui::Rect content = g_ui->layout().content;
          const nv::ui::Rect bottom = g_ui->layout().bottom_bar;
          if (content.empty() || bottom.empty()) {
            KillTimer(hwnd, kHoverTestTimer);
            nv::platform::LogInfo("test: drag_to_bottom=skipped no_rect");
            return 0;
          }
          const int startX = static_cast<int>(
              std::lround((content.left + content.right) * 0.5f));
          const int startY = static_cast<int>(
              std::lround((content.top + content.bottom) * 0.5f));
          const float s = g_ui->dpi_scale();
          if (g_test_hover_phase == 0) {
            MoveCursorToClient(hwnd, startX, startY);
            PostMessageW(hwnd, WM_MOUSEMOVE, 0, MAKELPARAM(startX, startY));
            PostMessageW(hwnd, WM_LBUTTONDOWN, MK_LBUTTON,
                         MAKELPARAM(startX, startY));
            nv::platform::LogInfo("test: drag_to_bottom=press");
            g_test_hover_phase = 1;
            SetTimer(hwnd, kHoverTestTimer, 200, nullptr);
            return 0;
          }
          if (g_test_hover_phase == 1) {
            const int x = startX + static_cast<int>(std::lround(40.0f * s));
            MoveCursorToClient(hwnd, x, startY);
            PostMessageW(hwnd, WM_MOUSEMOVE, MK_LBUTTON,
                         MAKELPARAM(x, startY));
            nv::platform::LogInfo("test: drag_to_bottom=pan at=" +
                                  std::to_string(x) + "," +
                                  std::to_string(startY));
            g_test_hover_phase = 2;
            SetTimer(hwnd, kHoverTestTimer, 200, nullptr);
            return 0;
          }
          const int x = static_cast<int>(
              std::lround((bottom.left + bottom.right) * 0.5f));
          const int y = static_cast<int>(
              std::lround((bottom.top + bottom.bottom) * 0.5f));
          MoveCursorToClient(hwnd, x, y);
          PostMessageW(hwnd, WM_MOUSEMOVE, MK_LBUTTON, MAKELPARAM(x, y));
          nv::platform::LogInfo("test: drag_to_bottom=bar at=" +
                                std::to_string(x) + "," + std::to_string(y));
          g_test_hover_phase = 3;
          KillTimer(hwnd, kHoverTestTimer);
          if (g_hover_owns_dump) {
            SetTimer(hwnd, kLayoutDumpTimer, 400, nullptr);
          }
          return 0;
        }
        if (g_test_hover_fs) {
          // v3.0 (規劃/26 §2.7): fullscreen first, then a real move into the
          // top-right reveal zone; the app owns the 160 ms fade and 3 s idle.
          if (!g_window_state.fullscreen()) {
            ToggleFullscreen(hwnd);
            if (!g_window_state.fullscreen()) {
              if (++g_test_hover_attempts < 20) {
                SetTimer(hwnd, kHoverTestTimer, 250, nullptr);
              } else {
                KillTimer(hwnd, kHoverTestTimer);
                nv::platform::LogInfo("test: hover_fs=skipped no_fullscreen");
              }
              return 0;
            }
          }
          KillTimer(hwnd, kHoverTestTimer);
          const nv::ui::Rect hot = FullscreenRevealHotRect();
          if (hot.empty()) {
            nv::platform::LogInfo("test: hover_fs=skipped no_rect");
            return 0;
          }
          const int x = static_cast<int>(
              std::lround((hot.left + hot.right) * 0.5f));
          const int y = static_cast<int>(
              std::lround((hot.top + hot.bottom) * 0.5f));
          nv::platform::LogInfo("test: hover_fs at=" + std::to_string(x) + "," +
                                std::to_string(y));
          MoveCursorToClient(hwnd, x, y);
          PostMessageW(hwnd, WM_MOUSEMOVE, 0, MAKELPARAM(x, y));
          if (g_hover_owns_dump) {
            SetTimer(hwnd, kLayoutDumpTimer, 900, nullptr);
          }
          return 0;
        }
        if (g_test_hover_hotzone) {
          // Post a real move into the 8 DIP reveal band; the WM_MOUSEMOVE
          // handler drives ShowStrip() and the fade, exactly like a pointer.
          KillTimer(hwnd, kHoverTestTimer);
          const nv::ui::Rect strip = StripRect();
          if (strip.empty()) {
            nv::platform::LogInfo("test: hover_hotzone=skipped no_rect");
            return 0;
          }
          const float hot = nv::ui::metrics::kStripHotZone *
                            (g_ui != nullptr ? g_ui->dpi_scale() : 1.0f);
          const int x = static_cast<int>(std::lround(strip.width() * 0.5f));
          const int y =
              static_cast<int>(std::lround(strip.bottom - hot * 0.5f));
          nv::platform::LogInfo("test: hover_hotzone at=" + std::to_string(x) +
                                "," + std::to_string(y));
          MoveCursorToClient(hwnd, x, y);
          PostMessageW(hwnd, WM_MOUSEMOVE, 0, MAKELPARAM(x, y));
          if (g_hover_owns_dump) {
            SetTimer(hwnd, kLayoutDumpTimer, 600, nullptr);
          }
          return 0;
        }
        if (g_strip_mode == nv::ui::StripMode::kAuto && !StripShown()) {
          ShowStrip(hwnd);
          if (!StripShown()) {
            // Let the first fade frames run before the synthetic move: the rail
            // only owns hits once it is visible.
            if (++g_test_hover_attempts < 40) {
              SetTimer(hwnd, kHoverTestTimer, 60, nullptr);
            } else {
              KillTimer(hwnd, kHoverTestTimer);
              nv::platform::LogInfo("test: hover_thumb=skipped hidden");
            }
            return 0;
          }
        }
        KillTimer(hwnd, kHoverTestTimer);
        const nv::ui::Rect strip = StripRect();
        const size_t index = static_cast<size_t>(g_test_hover_thumb);
        if (strip.empty() || index >= g_state->folder.files.size()) {
          nv::platform::LogInfo("test: hover_thumb=skipped out_of_range");
          return 0;
        }
        const D2D1_RECT_F cell = g_state->strip.CellRect(index, strip.top);
        const int x = static_cast<int>(std::lround((cell.left + cell.right) * 0.5f));
        const int y = static_cast<int>(std::lround((cell.top + cell.bottom) * 0.5f));
        nv::platform::LogInfo("test: hover_thumb=" + std::to_string(index) +
                              " at=" + std::to_string(x) + "," +
                              std::to_string(y));
        MoveCursorToClient(hwnd, x, y);
        PostMessageW(hwnd, WM_MOUSEMOVE, 0, MAKELPARAM(x, y));
        if (g_hover_owns_dump) {
          // 500 ms tooltip + fade/repaint margin.
          SetTimer(hwnd, kLayoutDumpTimer, 900, nullptr);
        }
        return 0;
      }
      if (wparam == kEmptyNoteTimer) {
        KillTimer(hwnd, kEmptyNoteTimer);
        ExpireEmptyNote(hwnd);
        return 0;
      }
      if (wparam == kTestDropTimer) {
        // One queued --simulate-drop per tick, so each drop's folder scan and
        // decode finish before the next one is applied.
        if (g_test_drops.empty()) {
          KillTimer(hwnd, kTestDropTimer);
          return 0;
        }
        const std::filesystem::path path = g_test_drops.front();
        g_test_drops.erase(g_test_drops.begin());
        nv::platform::LogInfo("test: drop=" + Narrow(path.wstring()));
        OnDroppedPaths(hwnd, {path});
        if (g_test_drops.empty()) {
          KillTimer(hwnd, kTestDropTimer);
        } else {
          SetTimer(hwnd, kTestDropTimer, 500, nullptr);
        }
        return 0;
      }
      if (wparam == kClickHitTimer) {
        // One queued --click-hit per tick so each click repaints before the
        // next one is dispatched (the menu it opens must exist when it lands).
        if (g_test_click_hits.empty()) {
          KillTimer(hwnd, kClickHitTimer);
          return 0;
        }
        const std::wstring name = g_test_click_hits.front();
        g_test_click_hits.erase(g_test_click_hits.begin());
        PerformTestClick(hwnd, name);
        if (g_test_click_hits.empty()) {
          KillTimer(hwnd, kClickHitTimer);
        } else {
          // Tight loop for the remaining clicks: the first one waited for the
          // window, the others only need one repaint in between.
          SetTimer(hwnd, kClickHitTimer, 160, nullptr);
        }
        return 0;
      }
      if (wparam == kSendKeysTimer) {
        // The first image decodes asynchronously; navigation must not run
        // before it exists (Query would reject it and the check would only
        // look at "blocked"). Poll for up to 10 s.
        if (g_keys_wait_image && !g_ui_inputs.has_image &&
            g_keys_wait_attempts < 40) {
          ++g_keys_wait_attempts;
          SetTimer(hwnd, kSendKeysTimer, 250, nullptr);
          return 0;
        }
        g_keys_wait_image = false;
        // One chord per tick; the next tick clears the simulated modifiers so
        // a posted key can never leak them into later real input.
        if (g_test_keys.empty()) {
          g_test_modifiers = TestModifiers{};
          KillTimer(hwnd, kSendKeysTimer);
          if (g_keys_owns_dump) {
            g_keys_owns_dump = false;
            SetTimer(hwnd, kLayoutDumpTimer, 800, nullptr);
          }
          return 0;
        }
        const std::wstring token = g_test_keys.front();
        g_test_keys.erase(g_test_keys.begin());
        UINT virtual_key = 0;
        TestModifiers modifiers;
        if (ParseKeyToken(token, &virtual_key, &modifiers)) {
          g_test_modifiers = modifiers;
          g_test_sending_key = true;
          nv::platform::LogInfo("test: key=" + Narrow(token));
          PostMessageW(hwnd, WM_KEYDOWN, virtual_key, 0);
          PostMessageW(hwnd, WM_KEYUP, virtual_key, 0);
        } else {
          nv::platform::LogError("test: key=" + Narrow(token) + " ignored");
        }
        SetTimer(hwnd, kSendKeysTimer,
                 g_test_keys.empty()
                     ? 250
                     : static_cast<UINT>(g_test_keys_interval_ms),
                 nullptr);
        return 0;
      }
      if (wparam == kTestActionTimer) {
        KillTimer(hwnd, kTestActionTimer);
        if (g_test_context_menu) {
          // v3.0 (規劃/26 §2.9): post the same message a real right-button release
          // produces, so the opener and everything behind it run unchanged.
          g_test_context_menu = false;
          POINT screen = g_test_context_menu_client;
          ClientToScreen(hwnd, &screen);
          nv::platform::LogInfo(
              "test: context_menu x=" +
              std::to_string(g_test_context_menu_client.x) + " y=" +
              std::to_string(g_test_context_menu_client.y));
          PostMessageW(hwnd, WM_CONTEXTMENU, reinterpret_cast<WPARAM>(hwnd),
                       MAKELPARAM(screen.x, screen.y));
        }
        if (g_test_click_index >= 0) {
          OpenIndex(hwnd, static_cast<size_t>(g_test_click_index));
        }
        if (g_test_scroll_notches != 0 && g_state) {
          const float step = 3.0f * 68.0f * g_state->strip.dpi_scale();
          g_state->strip.ScrollBy(static_cast<float>(g_test_scroll_notches) *
                                  step);
          RequestVisibleThumbnails();
          InvalidateStrip(hwnd);
        }
        if (!g_test_rename_to.empty()) {
          RenameCurrentTo(hwnd, g_test_rename_to);
          g_test_rename_to.clear();
        }
        if (g_test_open_rename_dialog) {
          // Test hook: opens the modal input box so UIA/localization checks can
          // inspect the dialog; it is closed by the test with Cancel.
          g_test_open_rename_dialog = false;
          RenameCurrent(hwnd);
        }
        if (g_test_delete_current) {
          DeleteCurrentFile(hwnd);
          g_test_delete_current = false;
        }
        if (!g_test_ole_drop.empty()) {
          const std::filesystem::path path = g_test_ole_drop;
          g_test_ole_drop.clear();
          PerformOleDrop(path);
        }
        if (g_test_drag_current) {
          g_test_drag_current = false;
          // V3.1-06: the same press -> move -> decision chain the window uses.
          // A move inside the client area must stay a pan; only leaving it by
          // the configured amount turns into a file drag.
          const float scale = g_ui != nullptr ? g_ui->dpi_scale() : 1.0f;
          const D2D1_RECT_F content = g_state->renderer.ContentRect();
          const float cx = (content.left + content.right) * 0.5f;
          const float cy = (content.top + content.bottom) * 0.5f;
          // Default: 20 DIP beyond the client's right edge (outside the window);
          // an explicit delta is measured from the content centre.
          float target_x = cx;
          float target_y = cy;
          if (g_test_drag_use_edge) {
            RECT client{};
            GetClientRect(hwnd, &client);
            target_x = static_cast<float>(client.right) + 20.0f * scale;
          } else {
            target_x = cx + g_test_drag_dx * scale;
            target_y = cy + g_test_drag_dy * scale;
          }
          g_drag_press = DragOutPress{};
          g_drag_press.active = true;
          g_drag_press.ctrl = g_test_drag_ctrl;
          g_drag_press.anchor =
              POINT{static_cast<LONG>(cx), static_cast<LONG>(cy)};
          g_drag_press.path = g_state->path;
          g_state->dragging = !g_test_drag_ctrl;
          g_state->last_mouse = g_drag_press.anchor;
          if (!MaybeStartFileDrag(hwnd, target_x, target_y)) {
            g_drag_press = DragOutPress{};
            g_state->dragging = false;
            g_last_drag_source.clear();
            g_last_drag_recentered = false;
            nv::platform::LogInfo("drag: dry_run inside_window (no file drag)");
          }
        }
        if (g_test_drag_strip_index >= 0 && g_state &&
            static_cast<size_t>(g_test_drag_strip_index) <
                g_state->folder.files.size()) {
          // V3.1-06: the rail is no longer a drag source. The hook now proves
          // it: a 100 DIP drag from a slot starts no file drag, and the release
          // opens the slot instead.
          const size_t index = static_cast<size_t>(g_test_drag_strip_index);
          g_test_drag_strip_index = -1;
          const nv::ui::Rect strip = StripRect();
          float sx = 0.0f;
          float sy = 0.0f;
          if (!strip.empty()) {
            sx = strip.left + (strip.right - strip.left) * 0.5f;
            sy = strip.top + (strip.bottom - strip.top) * 0.5f;
          }
          g_drag_press = DragOutPress{};
          g_drag_press.active = true;
          g_drag_press.on_strip = true;
          g_drag_press.anchor = POINT{static_cast<LONG>(sx),
                                      static_cast<LONG>(sy)};
          g_drag_press.path = g_state->folder.files[index];
          g_drag_press.strip_index = index;
          const float scale = g_ui != nullptr ? g_ui->dpi_scale() : 1.0f;
          const bool started = MaybeStartFileDrag(hwnd, sx + 100.0f * scale, sy);
          g_drag_press = DragOutPress{};
          if (started) {
            nv::platform::LogError("strip: drag_out started (policy bug)");
          } else {
            nv::platform::LogInfo("strip: drag_out blocked (release opens item)");
            OpenIndex(hwnd, index);
          }
        }
        return 0;
      }
      return DefWindowProcW(hwnd, message, wparam, lparam);

    case WM_SIZE:
      if (g_state) {
        // Minimising pauses an animated image; restoring continues it only if
        // it was playing before (規劃/22/12 坑 8/9).
        if (wparam == SIZE_MINIMIZED) {
          // Owned windows are hidden by the system when the owner is minimised;
          // keep our own state in step so a restore shows the card again
          // (規劃/26 §2.1).
          SyncInfoWindowVisibility(hwnd, true);
          if (g_state->anim.OnMinimized()) {
            KillTimer(hwnd, kAnimationTimer);
            nv::platform::LogInfo("animation: paused minimized");
            UpdateTitle(hwnd);
          }
          return 0;
        }
        if (wparam == SIZE_RESTORED && g_state->anim.paused_for_minimize()) {
          const size_t frames =
              g_state->current ? g_state->current->frames.size() : 0;
          if (g_state->anim.OnRestored(frames)) {
            nv::Image& image = *g_state->current;
            const nv::ImageFrame& frame =
                image.frames[std::min(g_state->anim.index(),
                                      image.frames.size() - 1)];
            SetTimer(hwnd, kAnimationTimer,
                     std::max<UINT>(16, frame.delay_ms), nullptr);
            nv::platform::LogInfo("animation: resumed restored");
          }
          UpdateTitle(hwnd);
        }
        const uint32_t width = LOWORD(lparam);
        const uint32_t height = HIWORD(lparam);
        if (width == 0 || height == 0) return 0;
        // Snap, maximize and restore change the state behind our back; keep the
        // state machine (and therefore the maximize/restore icon) in sync.
        if (!g_window_state.fullscreen()) {
          const bool zoomed = IsZoomed(hwnd) != FALSE;
          if (zoomed != g_window_state.maximized()) {
            g_window_state.SetMaximized(zoomed);
            ApplyWindowFrame(hwnd);
            nv::platform::LogInfo(std::string("window: state=") +
                                  nv::ui::WindowModeName(g_window_state.mode()));
          }
        }
        g_state->renderer.Resize(width, height);
        ApplyDpiScale(hwnd);
        if (!g_state->strip.empty() &&
            g_state->strip.current() != nv::render::ThumbnailStrip::kNone) {
          g_state->strip.EnsureVisible(g_state->strip.current());
        }
        RequestVisibleThumbnails();
        UpdateTitle(hwnd);
        SyncInfoWindowVisibility(hwnd);
        InvalidateRect(hwnd, nullptr, TRUE);
      }
      return 0;

    case WM_DPICHANGED: {
      const auto* suggested = reinterpret_cast<const RECT*>(lparam);
      if (suggested != nullptr) {
        SetWindowPos(hwnd, nullptr, suggested->left, suggested->top,
                     suggested->right - suggested->left,
                     suggested->bottom - suggested->top,
                     SWP_NOZORDER | SWP_NOACTIVATE);
      }
      ApplyDpiScale(hwnd);
      InvalidateRect(hwnd, nullptr, TRUE);
      return 0;
    }

    case WM_SETTINGCHANGE:
      if (wparam == SPI_SETHIGHCONTRAST) {
        ApplyThemeState(hwnd);
      } else if (lparam != 0 && g_settings.theme == nv::platform::ThemeMode::kSystem &&
                 wcscmp(reinterpret_cast<const wchar_t*>(lparam),
                        L"ImmersiveColorSet") == 0) {
        // theme=system follows the Windows app theme (規劃/22/07 §3.2).
        ApplyThemeFromSettings(hwnd);
      }
      return 0;

    case WM_SYSCOLORCHANGE:
    case WM_THEMECHANGED:
      ApplyThemeState(hwnd);
      return 0;

    case kDecodeDone:
      OnDecodeDone(hwnd, lparam);
      return 0;

    case kThumbnailDone:
      OnThumbnailDone(hwnd, lparam);
      return 0;

    case kExifDone:
      OnExifDone(hwnd, lparam);
      return 0;

    case kPreviewDone:
      OnPreviewDone(hwnd, lparam);
      return 0;

    case kSaveDone:
      OnSaveDone(hwnd, lparam);
      return 0;

    // V3-05 (規劃/25 §8): one coalesced local-disk notification.
    case nv::platform::DirNotifier::kMessage:
      OnDirNotify(hwnd);
      return 0;

    case WM_MOUSEWHEEL: {
      if (!g_state) return 0;
      ShowArrows(hwnd, "wheel");
      POINT pt{GET_X_LPARAM(lparam), GET_Y_LPARAM(lparam)};
      ScreenToClient(hwnd, &pt);
      const int delta = GET_WHEEL_DELTA_WPARAM(wparam);
      // The all-formats card may have more families than fit; the wheel scrolls
      // it by whole rows while it is the modal (規劃/22/16 §3.5).
      if (g_ui != nullptr &&
          g_ui->overlay().IsModal(nv::ui::OverlayKind::kFormats)) {
        g_ui->ScrollFormats(delta > 0 ? -3 : 3);
        RefreshUi(hwnd, false);
        InvalidateRect(hwnd, nullptr, FALSE);
        return 0;
      }
      const nv::ui::Rect strip = StripRect();
      if (!strip.empty() && StripShown() &&
          static_cast<float>(pt.y) >= strip.top &&
          static_cast<float>(pt.y) < strip.bottom) {
        g_state->strip.SetViewWidth(strip.width());
        const float step = 3.0f * 68.0f * g_state->strip.dpi_scale();
        g_state->strip.ScrollBy(-static_cast<float>(delta) / 120.0f * step);
        // Scrolling invalidates the tooltip anchor; restart the countdown if
        // the pointer still rests on a slot (規劃/22/08 坑 7).
        g_strip_hover_index = -1;
        HideStripTooltip(hwnd);
        g_state->strip.SetHover(static_cast<float>(pt.x),
                                static_cast<float>(pt.y), strip.width(),
                                strip.bottom);
        SyncStripHover(hwnd, static_cast<float>(pt.x),
                       static_cast<float>(pt.y));
        RequestVisibleThumbnails();
        InvalidateStrip(hwnd);
        return 0;
      }
      // The info card is its own window: its wheel never reaches this one
      // (規劃/26 §2.1).
      // Zoom belongs to the image area; the chrome and any modal/popup consume
      // the wheel instead of changing the view.
      const nv::ui::HitId hit =
          g_ui ? g_ui->HitTest(static_cast<float>(pt.x),
                               static_cast<float>(pt.y))
               : nv::ui::HitId::kNone;
      if (hit != nv::ui::HitId::kImage) return 0;
      // Default wheel mode is zoom: Ctrl+wheel reverses the direction
      // (規劃/22/09 §6). In page mode the wheel navigates and Ctrl+wheel still
      // zooms, so the other behavior is always one modifier away (22/07 §3.3).
      const bool ctrl = (GetKeyState(VK_CONTROL) & 0x8000) != 0;
      nv::platform::WheelMode wheel = nv::platform::WheelMode::kZoom;
      (void)nv::platform::ParseWheelMode(g_settings.wheel, &wheel);
      if (wheel == nv::platform::WheelMode::kPage && !ctrl) {
        DispatchCommand(hwnd, delta > 0 ? nv::ui::CommandId::kPrev
                                        : nv::ui::CommandId::kNext);
        return 0;
      }
      g_state->renderer.ZoomAt(static_cast<float>(pt.x),
                               static_cast<float>(pt.y),
                               ctrl ? -delta : delta);
      UpdateTitle(hwnd);
      RefreshUi(hwnd, false);
      InvalidateRect(hwnd, nullptr, TRUE);
      return 0;
    }

    case WM_LBUTTONDOWN: {
      if (!g_state) return 0;
      ShowArrows(hwnd, "click");
      const float x = static_cast<float>(GET_X_LPARAM(lparam));
      const float y = static_cast<float>(GET_Y_LPARAM(lparam));
      // Ctrl+drag exports the file; without Ctrl the image area keeps panning
      // (規劃/22/19 §3.1). The state is sampled once at button-down so
      // releasing Ctrl mid-drag still finishes the drag the user started.
      const bool ctrl_down = (GetKeyState(VK_CONTROL) & 0x8000) != 0;

      // The confirmation card is a modal: only its controls act, every other
      // click is swallowed by the window (規劃/22/11 §4.3).
      if (g_ui && g_ui->overlay().IsModal(nv::ui::OverlayKind::kConfirm)) {
        const nv::ui::HitId hit = g_ui->HitTest(x, y);
        if (hit == nv::ui::HitId::kConfirmPrimary ||
            hit == nv::ui::HitId::kConfirmSecondary ||
            hit == nv::ui::HitId::kConfirmRemember) {
          g_ui->SetPressed(hit);
          SetCapture(hwnd);
        }
        return 0;
      }

      // An open popup owns the pointer: a row runs its command, anything else
      // (including the owning button) closes the popup without acting
      // (規劃/22/20 §3.2).
      if (g_ui && g_ui->overlay().popup() != nv::ui::OverlayKind::kNone) {
        const int row = g_ui->MenuItemAt(x, y);
        int command = 0;
        if (row >= 0) {
          const auto& items = g_ui->menu_items();
          const size_t index = static_cast<size_t>(row);
          if (index < items.size()) command = items[index].command;
        }
        g_ui->overlay().ClosePopup();
        g_ui->ClearPointerState();
        RefreshUi(hwnd, false);
        if (command != 0) ExecuteMenuCommand(hwnd, command);
        InvalidateRect(hwnd, nullptr, TRUE);
        return 0;
      }

      // The all-formats card is a modal: only its close button acts, and a
      // click outside never dismisses it (規劃/22/20 §3.5).
      if (g_ui &&
          g_ui->overlay().IsModal(nv::ui::OverlayKind::kFormats)) {
        const nv::ui::HitId hit = g_ui->HitTest(x, y);
        if (hit == nv::ui::HitId::kFormatsClose) {
          g_ui->overlay().CloseModal();
          g_ui->ClearPointerState();
          nv::platform::LogInfo("cmd: formats=closed");
          RefreshUi(hwnd, true);
          InvalidateRect(hwnd, nullptr, FALSE);
        }
        return 0;
      }

      const nv::ui::HitId hit =
          g_ui ? g_ui->HitTest(x, y) : nv::ui::HitId::kNone;

      if (hit != nv::ui::HitId::kNone &&
          (nv::ui::IsTitleButton(hit) || nv::ui::IsBottomPart(hit) ||
           nv::ui::IsEmptyAction(hit) || nv::ui::IsArrow(hit) ||
           nv::ui::IsAnimPillButton(hit) || nv::ui::IsErrorAction(hit) ||
           nv::ui::IsInfoAction(hit) || nv::ui::IsFullscreenAction(hit) ||
           nv::ui::IsRenameAction(hit))) {
        if (nv::ui::IsEmptyAction(hit)) {
          // Clicking a card button also gives it the keyboard focus, so a
          // following Enter repeats the same action.
          g_ui->SetEmptyFocus(hit == nv::ui::HitId::kEmptyPrimary
                                  ? 0
                                  : (hit == nv::ui::HitId::kEmptyOpenFolder ? 1
                                                                            : 2));
        }
        g_ui->SetPressed(hit);
        SetCapture(hwnd);
        return 0;
      }

      // v3.0 (規劃/26 §2.4): a press on the bird's eye panel never falls through
      // to the picture or the rail behind it.
      if (hit == nv::ui::HitId::kMinimap) {
        BeginMinimapDrag(hwnd, x, y);
        return 0;
      }

      if (hit == nv::ui::HitId::kStrip) {
        // A click dismisses the hover tooltip; the rail stays visible while the
        // pointer is still on it. The jump itself is deferred to button-up: a
        // drag from the same press must export that slot, not open it
        // (規劃/22/19 §3.1).
        HideStripTooltip(hwnd);
        g_strip_hover_index = -1;
        const nv::ui::Rect strip = StripRect();
        size_t index = nv::render::ThumbnailStrip::kNone;
        if (g_state->strip.HitTest(x, y, strip.width(), strip.bottom, &index) &&
            index < g_state->folder.files.size()) {
          g_drag_press = DragOutPress{};
          g_drag_press.active = true;
          g_drag_press.on_strip = true;
          g_drag_press.ctrl = ctrl_down;
          g_drag_press.anchor = POINT{static_cast<LONG>(x),
                                      static_cast<LONG>(y)};
          g_drag_press.path = g_state->folder.files[index];
          g_drag_press.strip_index = index;
          SetCapture(hwnd);
        }
        return 0;
      }

      if (hit == nv::ui::HitId::kImage) {
        // No picture, no pan: the empty card's buttons must never start a
        // drag of an invisible image (規劃/22/03 坑 11).
        if (!g_ui_inputs.has_image) return 0;
        // The reveal hot zone belongs to the rail, not the image: a click there
        // while the auto rail is still fading in must not start a pan.
        if (g_strip_mode == nv::ui::StripMode::kAuto && !StripShown()) {
          const nv::ui::Rect strip = StripRect();
          const float hot = nv::ui::metrics::kStripHotZone *
                            (g_ui != nullptr ? g_ui->dpi_scale() : 1.0f);
          if (!strip.empty() && y >= strip.bottom - hot && y <= strip.bottom) {
            ShowStrip(hwnd);
            return 0;
          }
        }
        if (ctrl_down) {
          // Ctrl+press is a drag-out candidate, never a pan. A release without
          // movement is a no-op click plus the one-time hint.
          g_drag_press = DragOutPress{};
          g_drag_press.active = true;
          g_drag_press.ctrl = true;
          g_drag_press.anchor = POINT{static_cast<LONG>(x),
                                      static_cast<LONG>(y)};
          g_drag_press.path = g_state->path;
          SetCapture(hwnd);
          return 0;
        }
        // V3.1-06 (§06b): a plain press is still a pan, but it is also the
        // drag-out candidate — dragging the pointer 16 DIP outside the window
        // turns it into a file drag (MaybeStartFileDrag). The pan runs until
        // then, so the picture follows the pointer exactly like before.
        g_drag_press = DragOutPress{};
        g_drag_press.active = true;
        g_drag_press.ctrl = false;
        g_drag_press.anchor = POINT{static_cast<LONG>(x), static_cast<LONG>(y)};
        g_drag_press.path = g_state->path;
        g_state->dragging = true;
        g_state->last_mouse = POINT{GET_X_LPARAM(lparam),
                                    GET_Y_LPARAM(lparam)};
        SetCapture(hwnd);
      }
      return 0;
    }

    case WM_MOUSEMOVE: {
      if (!g_state) return 0;
      // Any movement brings the circles back and restarts the idle countdown.
      ShowArrows(hwnd, "move");
      TRACKMOUSEEVENT track{};
      track.cbSize = sizeof(track);
      track.dwFlags = TME_LEAVE;
      track.hwndTrack = hwnd;
      TrackMouseEvent(&track);

      const float x = static_cast<float>(GET_X_LPARAM(lparam));
      const float y = static_cast<float>(GET_Y_LPARAM(lparam));

      // A pending press that moved past 4 DIP turns into a file drag. This runs
      // before hover/pan handling: once the drag starts the pointer belongs to
      // OLE, not to the image or the rail (規劃/22/19 §3.1).
      if (MaybeStartFileDrag(hwnd, x, y)) return 0;

      // v3.0 (規劃/26 §2.4): dragging the bird's eye frame pans the picture and
      // nothing else. MinimapDragPan() only converts the delta; there is no
      // SetZoom/Fit call in this path, so the picture can never grow.
      if (g_state->minimap_dragging) {
        const float dx = x - static_cast<float>(g_state->minimap_last_mouse.x);
        const float dy = y - static_cast<float>(g_state->minimap_last_mouse.y);
        if (dx != 0.0f || dy != 0.0f) {
          float pan_x = 0.0f;
          float pan_y = 0.0f;
          nv::ui::MinimapDragPan(g_ui->layout().minimap,
                                 g_state->renderer.zoom(), dx, dy, &pan_x,
                                 &pan_y);
          g_state->renderer.PanBy(pan_x, pan_y);
          g_state->minimap_last_mouse =
              POINT{GET_X_LPARAM(lparam), GET_Y_LPARAM(lparam)};
          // Pan does not rebuild the layout, but the frame has to follow it, so
          // refresh the frame while the panel is open.
          RefreshUi(hwnd, false);
          InvalidateRect(hwnd, nullptr, TRUE);
        }
        return 0;
      }

      // Any mouse movement wakes the animation pill: it dims after two idle
      // seconds and returns to full opacity here (規劃/22/12 §3.1).
      g_anim_pill_last_move_ms = NowMs();
      if (g_anim_pill_opacity < 1.0f) {
        g_anim_pill_opacity = 1.0f;
        InvalidateRect(hwnd, nullptr, FALSE);
      }
      // The fade tick stops once fully dimmed; wake it again on movement.
      if (!g_anim_pill_timer_running) UpdateAnimPillTimer(hwnd);

      bool repaint_chrome = false;
      if (g_ui != nullptr) {
        const nv::ui::HitId hit = g_ui->HitTest(x, y);
        if (g_ui->hover() != hit) {
          g_ui->SetHover(hit);
          repaint_chrome = true;
        }
        // The popup menu highlights rows, not HitIds (規劃/22/02 §3.3).
        if (g_ui->overlay().popup() != nv::ui::OverlayKind::kNone &&
            g_ui->UpdateMenuHover(x, y)) {
          repaint_chrome = true;
        }
      }

      bool strip_hover_changed = false;
      const nv::ui::Rect strip = StripRect();
      if (!strip.empty()) {
        strip_hover_changed =
            g_state->strip.SetHover(x, y, strip.width(), strip.bottom);
      }

      // Auto mode (or any fullscreen mode): the reveal area is the rail slot
      // plus the bottom bar (規劃/26 §2.10) — reaching for the bottom edge
      // anywhere reveals the rail. While a button is held (image pan,
      // Ctrl+drag-out candidate or any captured press) a move into that area
      // must NOT reveal it: a drag that crosses the bottom bar has to keep
      // doing what it started.
      if (g_ui_inputs.has_image && StripRevealEnabled() &&
          !strip.empty() && g_ui != nullptr &&
          !g_ui->overlay().BlocksImageCommands()) {
        const nv::ui::Rect bar = g_ui->layout().bottom_bar;
        float left = strip.left;
        float top = strip.top;
        float right = strip.right;
        float bottom = strip.bottom;
        if (!bar.empty()) {
          left = std::min(left, bar.left);
          top = std::min(top, bar.top);
          right = std::max(right, bar.right);
          bottom = std::max(bottom, bar.bottom);
        }
        const bool in_hot =
            x >= left && x <= right && y >= top && y <= bottom;
        const bool in_rail = StripShown() && strip.Contains(x, y);
        const bool held = g_state->dragging || g_drag_press.active ||
                          GetCapture() == hwnd;
        if (!held && (in_hot || in_rail)) {
          ShowStrip(hwnd);
        } else if (!held) {
          ScheduleStripHide(hwnd);
        }
      }
      if (g_ui_inputs.has_image && !g_state->dragging) {
        SyncStripHover(hwnd, x, y);
      }

      // v3.0 (規劃/26 §2.7): reaching for the top-right corner in fullscreen
      // reveals the exit/close pair; the 3 s deadline owns hiding, so leaving
      // the corner needs no action here.
      if (g_window_state.fullscreen()) {
        const nv::ui::Rect hot = FullscreenRevealHotRect();
        if (!hot.empty() && hot.Contains(x, y)) ShowFullscreenReveal(hwnd);
      }

      if (g_state->dragging) {
        const POINT pt{GET_X_LPARAM(lparam), GET_Y_LPARAM(lparam)};
        g_state->renderer.PanBy(
            static_cast<float>(pt.x - g_state->last_mouse.x),
            static_cast<float>(pt.y - g_state->last_mouse.y));
        g_state->last_mouse = pt;
        // The bird's eye frame follows a picture pan too (規劃/26 §2.4). Pan is
        // the one view change that does not go through RefreshUi, so refresh the
        // frame here while the panel is open.
        if (g_ui_inputs.minimap.visible) RefreshUi(hwnd, false);
        InvalidateRect(hwnd, nullptr, TRUE);
      } else if (repaint_chrome) {
        InvalidateRect(hwnd, nullptr, FALSE);
      } else if (strip_hover_changed) {
        InvalidateStrip(hwnd);
      }
      return 0;
    }

    case WM_CAPTURECHANGED:
      // Alt+Tab / another window stealing the capture must not leave a stale
      // drag candidate behind (規劃/22/19 坑 2/17).
      if (g_drag_press.active) {
        g_drag_press = DragOutPress{};
        nv::platform::LogInfo("drag: press cancelled by capture change");
      }
      // Same rule for the bird's eye frame (規劃/26 §2.4): a capture change ends
      // the gesture instead of leaving the panel stuck to the cursor.
      if (g_state != nullptr && g_state->minimap_dragging) {
        g_state->minimap_dragging = false;
        nv::platform::LogInfo("ui: minimap drag=cancelled");
      }
      return 0;

    case WM_MOUSELEAVE:
      if (g_ui != nullptr) {
        g_ui->SetHover(nv::ui::HitId::kNone);
        InvalidateRect(hwnd, nullptr, FALSE);
      }
      // Leaving the window hides the tooltip at once and starts the rail's hide
      // grace period (規劃/22/08 §3.5).
      g_strip_hover_index = -1;
      HideStripTooltip(hwnd);
      ScheduleStripHide(hwnd);
      // The pointer is no longer over the window: the circles slide away at once
      // instead of waiting for the idle deadline.
      g_arrow_pointer_inside = false;
      HideArrows(hwnd, "leave");
      if (g_state && g_state->strip.hovered() !=
                             nv::render::ThumbnailStrip::kNone) {
        const nv::ui::Rect strip = StripRect();
        if (!strip.empty()) {
          g_state->strip.SetHover(-1.0f, -1.0f, strip.width(), strip.bottom);
          InvalidateStrip(hwnd);
        }
      }
      return 0;

    case WM_SETCURSOR:
      if (LOWORD(lparam) == HTCLIENT && g_state && g_ui) {
        POINT pt{};
        if (GetCursorPos(&pt)) {
          ScreenToClient(hwnd, &pt);
          const float x = static_cast<float>(pt.x);
          const float y = static_cast<float>(pt.y);
          const nv::ui::HitId hit = g_ui->HitTest(x, y);
          bool hand = nv::ui::IsTitleButton(hit) ||
                      nv::ui::IsBottomPart(hit) ||
                      nv::ui::IsEmptyAction(hit) || nv::ui::IsArrow(hit) ||
                      nv::ui::IsErrorAction(hit) || nv::ui::IsInfoAction(hit) ||
                      nv::ui::IsRenameAction(hit) ||
                      nv::ui::IsFullscreenAction(hit) ||
                      hit == nv::ui::HitId::kFormatsClose ||
                      hit == nv::ui::HitId::kMinimap ||
                      hit == nv::ui::HitId::kConfirmPrimary ||
                      hit == nv::ui::HitId::kConfirmSecondary ||
                      hit == nv::ui::HitId::kConfirmRemember;
          const nv::ui::Rect strip = StripRect();
          if (!hand && hit == nv::ui::HitId::kStrip && !strip.empty()) {
            size_t index = nv::render::ThumbnailStrip::kNone;
            hand = g_state->strip.HitTest(x, y, strip.width(), strip.bottom,
                                          &index);
          }
          if (hand) {
            SetCursor(LoadCursorW(nullptr, IDC_HAND));
            return TRUE;
          }
        }
      }
      return DefWindowProcW(hwnd, message, wparam, lparam);

    case WM_LBUTTONUP: {
      if (g_state != nullptr && g_state->minimap_dragging) {
        g_state->minimap_dragging = false;
        ReleaseCapture();
        nv::platform::LogInfo("ui: minimap drag=end");
        return 0;
      }
      if (g_drag_press.active) {
        // The press never turned into a file drag: it is a click (or the end of
        // a pan). A rail click opens the slot under the press; Ctrl+click on the
        // image teaches the gesture; a plain image press was panning and falls
        // through to the normal release path (V3.1-06 §06b).
        const DragOutPress press = g_drag_press;
        g_drag_press = DragOutPress{};
        if (press.on_strip &&
            press.strip_index != nv::render::ThumbnailStrip::kNone) {
          ReleaseCapture();
          OpenIndex(hwnd, press.strip_index);
          return 0;
        }
        if (press.ctrl) {
          ReleaseCapture();
          ShowDragHintOnce(hwnd);
          return 0;
        }
      }
      const nv::ui::HitId pressed =
          g_ui ? g_ui->pressed() : nv::ui::HitId::kNone;
      if (g_ui != nullptr) g_ui->SetPressed(nv::ui::HitId::kNone);
      if (g_state && g_state->dragging) {
        g_state->dragging = false;
        ReleaseCapture();
      } else if (pressed != nv::ui::HitId::kNone) {
        ReleaseCapture();
      }
      if (pressed == nv::ui::HitId::kNone || g_ui == nullptr || !g_state) {
        return 0;
      }

      const float x = static_cast<float>(GET_X_LPARAM(lparam));
      const float y = static_cast<float>(GET_Y_LPARAM(lparam));
      if (g_ui->HitTest(x, y) != pressed) return 0;

      nv::platform::LogInfo(std::string("cmd: id=") + HitIdName(pressed));
      switch (pressed) {
        case nv::ui::HitId::kTitleMenu:
          // v3.0 (規劃/26 §2.8): the hamburger opens the settings window
          // directly. The native context menu stays on right-click (below) and
          // the 更多▾ button keeps the view switches — two entries, two jobs.
          OpenSettingsWindow(hwnd);
          break;
        case nv::ui::HitId::kTitleFullscreen:
          ToggleFullscreen(hwnd);
          break;
        case nv::ui::HitId::kTitleMinimize:
          ShowWindow(hwnd, SW_MINIMIZE);
          break;
        case nv::ui::HitId::kTitleMaximize:
          SendMessageW(hwnd, WM_SYSCOMMAND,
                       g_window_state.maximized() ? SC_RESTORE : SC_MAXIMIZE, 0);
          break;
        case nv::ui::HitId::kTitleClose:
          PostMessageW(hwnd, WM_CLOSE, 0, 0);
          break;
        // v3.0 (規劃/26 §2.7): the fullscreen reveal pair.
        case nv::ui::HitId::kFullscreenExit:
          ExitFullscreen(hwnd);
          break;
        case nv::ui::HitId::kFullscreenClose:
          PostMessageW(hwnd, WM_CLOSE, 0, 0);
          break;
        case nv::ui::HitId::kBottomFit:
          g_state->renderer.FitToWindow();
          UpdateTitle(hwnd);
          RefreshUi(hwnd, true);
          break;
        case nv::ui::HitId::kBottomActual:
          ApplyActualSize();
          UpdateTitle(hwnd);
          RefreshUi(hwnd, true);
          break;
        case nv::ui::HitId::kBottomRotateLeft:
          DispatchCommand(hwnd, nv::ui::CommandId::kRotateLeft);
          break;
        case nv::ui::HitId::kBottomRotateRight:
          DispatchCommand(hwnd, nv::ui::CommandId::kRotateRight);
          break;
        case nv::ui::HitId::kBottomPercent:
          g_ui->overlay().TogglePopup(nv::ui::OverlayKind::kPercent);
          nv::platform::LogInfo(std::string("ui: percent_menu=") +
                                (g_ui->overlay().IsPopup(
                                     nv::ui::OverlayKind::kPercent)
                                     ? "open"
                                     : "closed"));
          RefreshUi(hwnd, true);
          break;
        case nv::ui::HitId::kBottomInfo:
          ToggleInfoPanel(hwnd);
          break;
        case nv::ui::HitId::kBottomMore:
          g_ui->overlay().TogglePopup(nv::ui::OverlayKind::kMore);
          nv::platform::LogInfo(std::string("ui: more_panel=") +
                                (g_ui->overlay().IsPopup(
                                     nv::ui::OverlayKind::kMore)
                                     ? "open"
                                     : "closed"));
          RefreshUi(hwnd, true);
          break;
        case nv::ui::HitId::kEmptyPrimary:
          ActivateEmptyAction(hwnd, 0);
          break;
        case nv::ui::HitId::kEmptyOpenFolder:
          ActivateEmptyAction(hwnd, 1);
          break;
        case nv::ui::HitId::kEmptyAllFormats:
          ActivateEmptyAction(hwnd, 2);
          break;
        case nv::ui::HitId::kArrowLeft:
          DispatchCommand(hwnd, nv::ui::CommandId::kPrev);
          break;
        case nv::ui::HitId::kArrowRight:
          DispatchCommand(hwnd, nv::ui::CommandId::kNext);
          break;
        case nv::ui::HitId::kAnimPrev:
          StepAnimationFrame(hwnd, -1);
          nv::platform::LogInfo("cmd: id=anim.prev");
          break;
        case nv::ui::HitId::kAnimNext:
          StepAnimationFrame(hwnd, +1);
          nv::platform::LogInfo("cmd: id=anim.next");
          break;
        case nv::ui::HitId::kAnimToggle:
          ToggleAnimation(hwnd);
          nv::platform::LogInfo("cmd: id=anim.toggle");
          break;
        case nv::ui::HitId::kErrorAction0:
        case nv::ui::HitId::kErrorAction1:
        case nv::ui::HitId::kErrorAction2:
          ActivateErrorAction(hwnd, nv::ui::ErrorActionIndex(pressed));
          break;
        case nv::ui::HitId::kErrorClose:
          CloseError(hwnd);
          break;
        case nv::ui::HitId::kConfirmPrimary:
          CloseConfirm(hwnd, true);
          break;
        case nv::ui::HitId::kRenameOk:
          CommitRenameOverlay(hwnd);
          break;
        case nv::ui::HitId::kRenameCancel:
          CancelRenameOverlay(hwnd);
          break;
        case nv::ui::HitId::kConfirmSecondary:
          CloseConfirm(hwnd, false);
          break;
        case nv::ui::HitId::kConfirmRemember:
          if (g_confirm != ConfirmKind::kDelete) {
            g_wrap_remember = !g_wrap_remember;
            nv::platform::LogInfo(std::string("confirm: remember=") +
                                  (g_wrap_remember ? "1" : "0"));
            RefreshUi(hwnd, false);
            InvalidateRect(hwnd, nullptr, FALSE);
          }
          break;
        default:
          break;
      }
      return 0;
    }

    case WM_KEYUP:
      // Ends one simulated --send-keys chord; from here on the real keyboard
      // modifiers are honoured again and the simulated ones are cleared so a
      // queued Ctrl chord cannot leak into the next real key.
      g_test_sending_key = false;
      g_test_modifiers = TestModifiers{};
      return DefWindowProcW(hwnd, message, wparam, lparam);

    case WM_KEYDOWN:
      // Any key brings the circles back and restarts the idle countdown.
      ShowArrows(hwnd, "key");
      if (g_state) {
        const bool ctrl =
            g_test_modifiers.ctrl ||
            (!g_test_sending_key && (GetKeyState(VK_CONTROL) & 0x8000) != 0);
        const bool shift =
            g_test_modifiers.shift ||
            (!g_test_sending_key && (GetKeyState(VK_SHIFT) & 0x8000) != 0);
        const bool alt =
            g_test_modifiers.alt ||
            (!g_test_sending_key && (GetKeyState(VK_MENU) & 0x8000) != 0);

        // Escape is owned by the overlay stack: one press closes exactly one
        // layer (settings -> modal -> popup -> info -> fullscreen).
        if (wparam == VK_ESCAPE && g_ui != nullptr) {
          // V3.1-06 (§06e): the rename overlay owns Escape while it is open —
          // cancel, not "close the modal and leave the EDIT behind".
          if (RenameOverlayOpen()) {
            CancelRenameOverlay(hwnd);
            return 0;
          }
          const nv::ui::EscapeAction action = g_ui->HandleEscape();
          nv::platform::LogInfo(std::string("cmd: escape=") +
                                nv::ui::EscapeActionName(action));
          if (action == nv::ui::EscapeAction::kClosedError) {
            g_error = nv::UiError{};
          } else if (action == nv::ui::EscapeAction::kClosedModal &&
                     g_confirm != ConfirmKind::kNone) {
            // Route the cancel through the same path as the secondary button so
            // a checked "remember" is honoured for the stay choice too.
            CloseConfirm(hwnd, false);
            return 0;
          }
          // The info window owns its own Escape; a press in the main window
          // therefore never closes it (規劃/27 坑 219, single owner).
          if (action == nv::ui::EscapeAction::kExitedFullscreen) {
            ExitFullscreen(hwnd);
          } else if (action != nv::ui::EscapeAction::kNone) {
            RefreshUi(hwnd, true);
          }
          return 0;
        }
        // F11 is window-level and works even while a popup/modal is open.
        if (wparam == VK_F11) {
          ToggleFullscreen(hwnd);
          return 0;
        }

        // V3.1-06 (§06e): Enter/Escape for the rename overlay. The EDIT forwards
        // them through kRenameKey; these two also make the simulated-key hook
        // (which posts to the main window) and a lost focus behave the same.
        if (RenameOverlayOpen()) {
          if (wparam == VK_RETURN) {
            CommitRenameOverlay(hwnd);
            return 0;
          }
        }

        // v3.0 (規劃/26 §2.9): the right-click menu owns Up/Down/Enter while it
        // is open. Esc is handled above by the overlay stack; every other key
        // still falls through to the "popup blocks navigation" guard below.
        if (g_ui != nullptr &&
            g_ui->overlay().popup() == nv::ui::OverlayKind::kContext) {
          if (wparam == VK_DOWN || wparam == VK_UP) {
            if (g_ui->StepMenuHover(wparam == VK_DOWN ? 1 : -1)) {
              nv::platform::LogInfo("ui: menu_hover=" +
                                    std::to_string(g_ui->menu_hover()));
              InvalidateRect(hwnd, nullptr, FALSE);
            }
            return 0;
          }
          if (wparam == VK_RETURN) {
            const int row = g_ui->menu_hover();
            const auto& items = g_ui->menu_items();
            if (row >= 0 && static_cast<size_t>(row) < items.size()) {
              const int command = items[static_cast<size_t>(row)].command;
              g_ui->overlay().ClosePopup();
              g_ui->ClearPointerState();
              RefreshUi(hwnd, true);
              ExecuteMenuCommand(hwnd, command);
              InvalidateRect(hwnd, nullptr, TRUE);
            }
            return 0;
          }
        }

        // Enter belongs to the topmost card: the delete confirmation treats it
        // as Cancel (the safe action), the folder-edge prompt runs its primary
        // action, and the error card runs its default (規劃/22/11 坑 7).
        if (g_ui != nullptr &&
            g_ui->overlay().IsModal(nv::ui::OverlayKind::kConfirm)) {
          if (wparam == VK_RETURN) {
            const bool wrap = g_confirm == ConfirmKind::kWrapNext ||
                              g_confirm == ConfirmKind::kWrapPrev;
            CloseConfirm(hwnd, wrap);
          } else if (wparam == VK_SPACE && g_confirm != ConfirmKind::kDelete) {
            // Space toggles "remember my choice" in the folder-edge prompt
            // (22/10 §4.3); the delete card has no such row.
            g_wrap_remember = !g_wrap_remember;
            nv::platform::LogInfo(std::string("confirm: remember=") +
                                  (g_wrap_remember ? "1" : "0"));
            RefreshUi(hwnd, false);
            InvalidateRect(hwnd, nullptr, FALSE);
          }
          return 0;
        }
        if (g_ui != nullptr && g_ui->overlay().error_open()) {
          if (wparam == VK_RETURN) {
            ActivateErrorDefault(hwnd);
            return 0;
          }
        }

        // While a modal or a popup owns the keyboard, image commands must not
        // run (規劃/22/20 §3.6).
        if (g_ui != nullptr && g_ui->overlay().BlocksNavigation()) return 0;

        // Empty card: Tab cycles its three buttons, Enter runs the focused one
        // (規劃/22/03 §5). Ctrl+O keeps working because it is handled after
        // this block, independent of has_image (坑 16).
        if (g_ui != nullptr && g_ui_inputs.empty.visible) {
          if (wparam == VK_TAB) {
            g_ui->CycleEmptyFocus((GetKeyState(VK_SHIFT) & 0x8000) != 0);
            nv::platform::LogInfo("empty: focus=" +
                                  std::to_string(g_ui->empty_focus()));
            RefreshUi(hwnd, false);
            InvalidateRect(hwnd, nullptr, FALSE);
            return 0;
          }
          if (wparam == VK_RETURN) {
            ActivateEmptyAction(hwnd, g_ui->empty_focus());
            return 0;
          }
        }

        if (ctrl && wparam == 'O') {
          OpenFileDialog(hwnd);
          return 0;
        }
        if (ctrl && wparam == 'S') {
          SaveAsDialog(hwnd, g_state->path);
          return 0;
        }
        if (ctrl && wparam == 'C') {
          if (shift) {
            CopyCurrentPath(hwnd);
          } else {
            CopyCurrentImage(hwnd);
          }
          return 0;
        }
        if (ctrl && shift && wparam == 'W') {
          SetCurrentWallpaper(hwnd);
          return 0;
        }
        // Settings window (規劃/22/07 §1): Ctrl+, is the standard shortcut and
        // the title-bar ☰ menu has the same entry.
        if (ctrl && wparam == VK_OEM_COMMA) {
          OpenSettingsWindow(hwnd);
          return 0;
        }
        if (wparam == VK_DELETE) {
          DeleteCurrent(hwnd);
          return 0;
        }
        if (wparam == VK_F2) {
          RenameCurrent(hwnd);
          return 0;
        }

        if (wparam == VK_F5 && !g_state->path.empty()) {
          OpenFile(hwnd, g_state->path);
        }
        // Plain T/C are the theme/checkerboard toggles; Ctrl+C is copy image
        // above, so the chord must be checked (規劃/22/09 坑 1).
        if (!ctrl && !alt && !shift && wparam == 'T') {
          // T is a direct dark/light switch; choosing "follow system" is only
          // available in the settings page (規劃/22/07 §3.3).
          g_settings.theme = g_settings.dark_theme
                                 ? nv::platform::ThemeMode::kLight
                                 : nv::platform::ThemeMode::kDark;
          ApplyThemeFromSettings(hwnd);
          SaveSettingsNow();
        }
        if (!ctrl && !alt && !shift && wparam == 'C') {
          g_settings.checkerboard = !g_settings.checkerboard;
          g_state->renderer.SetCheckerboard(g_settings.checkerboard);
          SaveSettingsNow();
          if (g_settings_window) g_settings_window->Invalidate();
        }
        // v3.0 (規劃/26 §2.4): M toggles the bird's eye view. No modifier, same
        // as T/C; the 更多▾ switch stays the discoverable entry.
        if (!ctrl && !alt && !shift && wparam == 'M') {
          ExecuteMenuCommand(hwnd, kCmdToggleBirdseye);
          return 0;
        }

        // Navigation / zoom / view keys (規劃/22/09 §4). The table decides
        // what a chord means; DispatchCommand decides whether it can run.
        const nv::ui::CommandId command =
            nv::ui::CommandForKey(static_cast<unsigned>(wparam), ctrl, shift,
                                  alt);
        if (command != nv::ui::CommandId::kNone) {
          // Holding Ctrl+L/R must not spin the view through four turns: only the
          // first key-down acts (規劃/22/05 §3.5).
          const bool repeat = (lparam & (1ll << 30)) != 0;
          if (repeat && (command == nv::ui::CommandId::kRotateLeft ||
                         command == nv::ui::CommandId::kRotateRight)) {
            return 0;
          }
          DispatchCommand(hwnd, command);
          return 0;
        }
        RefreshUi(hwnd, false);
        UpdateTitle(hwnd);
        InvalidateRect(hwnd, nullptr, TRUE);
      }
      return 0;

    // Alt combinations arrive as WM_SYSKEYDOWN. Only Alt+Enter is ours; every
    // other system chord (Alt+F4, Alt+Space) must reach DefWindowProc.
    case WM_SYSKEYDOWN:
      if (wparam == VK_RETURN &&
          (GetKeyState(VK_MENU) & 0x8000) != 0) {
        // Same guard as WM_KEYDOWN: an open modal/popup owns the keyboard.
        if (g_ui != nullptr && g_ui->overlay().BlocksNavigation()) return 0;
        DispatchCommand(hwnd, nv::ui::CommandId::kToggleInfo);
        return 0;
      }
      break;

    case WM_CONTEXTMENU:
      if (g_state) {
        // A modal owns all input and must not open a second context menu.
        if (g_ui != nullptr && g_ui->overlay().BlocksImageCommands()) {
          return 0;
        }
        POINT point{GET_X_LPARAM(lparam), GET_Y_LPARAM(lparam)};
        if (point.x == -1 && point.y == -1) {
          GetCursorPos(&point);
        }
        // v3.0 (規劃/26 §2.9): the self-drawn menu lives in client pixels. The
        // keyboard menu key can fire with the cursor outside the window, so the
        // opener clamps the point to the client.
        ScreenToClient(hwnd, &point);
        // "Copy this value" on an info row is handled by the info window itself
        // (規劃/26 §2.1); this window keeps its normal context menu.
        ShowContextMenu(hwnd, point);
        return 0;
      }
      return DefWindowProcW(hwnd, message, wparam, lparam);

    case WM_COPYDATA: {
      // The receiver window normally handles the hand-off; accept the same
      // payload on the main window too so a launch racing the receiver's
      // creation is still delivered.
      const auto* data = reinterpret_cast<const COPYDATASTRUCT*>(lparam);
      std::wstring path;
      if (data != nullptr && data->dwData == nv::platform::kInstancePayloadMagic &&
          nv::platform::DecodeInstancePayload(data->lpData, data->cbData,
                                              &path)) {
        HandleInstanceRequest(path);
        return TRUE;
      }
      return FALSE;
    }

    case WM_EXITSIZEMOVE:
      // The user finished dragging/resizing: persist once, not per frame
      // (規劃/22/15 §3.1). Fullscreen and minimized windows opt out inside.
      SaveWindowPlacement(hwnd);
      return 0;

    case WM_CLOSE:
      SaveWindowPlacement(hwnd);
      // The info card is an owned window: close it before the owner so its
      // WM_DESTROY callback never touches a half-destroyed main window. Its
      // "visible" intent stays as it was, so the next launch restores it
      // (規劃/26 §2.1).
      if (InfoWindowVisible()) CloseInfoWindowForAppClose();
      DestroyWindow(hwnd);
      return 0;

    case WM_DESTROY:
      // V3.1-06 (§06e): the rename EDIT is a separate top-level window owned by
      // this one; it must not outlive it.
      DestroyRenameEdit();
      KillTimer(hwnd, kInfoWindowTimer);
      KillTimer(hwnd, kInfoScreenshotTimer);
      KillTimer(hwnd, kInfoKeyTimer);
      KillTimer(hwnd, kAnimationTimer);
      KillTimer(hwnd, kAnimPillTimer);
      KillTimer(hwnd, kLayoutDumpTimer);
      KillTimer(hwnd, kScreenshotTimer);
      KillTimer(hwnd, kTestActionTimer);
      KillTimer(hwnd, kEmptyNoteTimer);
      KillTimer(hwnd, kTestDropTimer);
      KillTimer(hwnd, kClickHitTimer);
      KillTimer(hwnd, kSendKeysTimer);
      KillTimer(hwnd, kTitleClickTimer);
      KillTimer(hwnd, kToastTimer);
      KillTimer(hwnd, kLoadingDelayTimer);
      KillTimer(hwnd, kLoadingTickTimer);
      KillTimer(hwnd, kConfirmTimer);
      KillTimer(hwnd, kLoadingEndTimer);
      KillTimer(hwnd, kSaveAsTimer);
      KillTimer(hwnd, kSaveOptionsTimer);
      KillTimer(hwnd, kInfoScrollTimer);
      KillTimer(hwnd, kStripFadeTimer);
      KillTimer(hwnd, kStripHideTimer);
      KillTimer(hwnd, kStripTooltipTimer);
      KillTimer(hwnd, kHoverTestTimer);
      KillTimer(hwnd, kWatchTimer);
      // V3-05: close the directory handle and join the watcher thread before the
      // window dies; its pending read is cancelled inside Stop().
      g_dir_notify.Stop();
      g_watch_uses_notify = false;
      g_watch_notify_deferred = false;
      KillTimer(hwnd, kDumpAtTimer);
      KillTimer(hwnd, kQuitTimer);
      KillTimer(hwnd, kMinimizeTimer);
      KillTimer(hwnd, kRestoreTimer);
      KillTimer(hwnd, kPreviewBadgeTimer);
      // OLE: revoke while the window still exists, then drop the app reference.
      if (g_drop_target) {
        g_drop_target->Revoke();
        g_drop_target.reset();
      }
      if (g_state) {
        nv::platform::LogInfo("shutdown: exif");
        if (g_exif) g_exif->Shutdown();
        nv::platform::LogInfo("shutdown: thumbs");
        if (g_state->thumbs) g_state->thumbs->Shutdown();
        nv::platform::LogInfo("shutdown: decode");
        if (g_state->decode) g_state->decode->Shutdown();
        nv::platform::LogInfo("shutdown: cache writer");
        if (g_state->cache_writer) g_state->cache_writer->Shutdown();
        nv::platform::LogInfo("shutdown: services done");
      }
      // Save worker is joined before the window dies: a queued completion must
      // not be posted to a destroyed HWND (規劃/22/14 坑 16).
      nv::platform::LogInfo("shutdown: save");
      if (g_save_service) {
        g_save_service->Shutdown();
        g_save_service.reset();
      }
      g_main_window = nullptr;
      PostQuitMessage(0);
      return 0;

    default:
      break;
  }
  return DefWindowProcW(hwnd, message, wparam, lparam);
}

// Fatal startup failure without a renderer: show a GDI text frame and run the
// normal message loop (no MessageBox, 規劃/22/11 §5). Returns when the user
// closes the window.
int RunFatalErrorWindow(HWND hwnd, const std::wstring& text) {
  g_fatal_error_text = text;
  SetWindowTextW(hwnd, nv::i18n::Text(nv::i18n::StringId::kAppTitle));
  InvalidateRect(hwnd, nullptr, TRUE);
  UpdateWindow(hwnd);
  MSG message{};
  while (GetMessageW(&message, nullptr, 0, 0) > 0) {
    TranslateMessage(&message);
    DispatchMessageW(&message);
  }
  return static_cast<int>(message.wParam);
}

}  // namespace

// --- Headless shell commands ------------------------------------------------
// Explorer's context-menu verbs start the application without a window:
//   zoreview.exe --set-wallpaper "<file>"
//   zoreview.exe --copy-image "<file>"
//   zoreview.exe --register-shell | --unregister-shell
//   zoreview.exe --register-assoc | --unregister-assoc | --set-default-assoc
//   zoreview.exe --install [--machine] [--install-dir <dir>] [--no-shortcut]
//   zoreview.exe --uninstall [--machine]
//   zoreview.exe --print-paths [<file>]
// These paths must not create a D3D device, window or thumbnail service.

enum class HeadlessAction {
  kNone,
  kSetWallpaper,
  kCopyImage,
  kRegisterShell,
  kUnregisterShell,
  kRegisterAssociations,
  kUnregisterAssociations,
  kSetDefaultAssociation,
  kInstall,
  kUninstall,
  kPrintPaths,
  kSetLanguage,
  kPrintSettings,
};

struct HeadlessRequest {
  HeadlessAction action = HeadlessAction::kNone;
  std::filesystem::path path;
  std::filesystem::path install_dir;
  std::filesystem::path output_path;
  nv::platform::RegistryScope scope = nv::platform::RegistryScope::kUser;
  bool no_shortcut = false;
  bool associate_default = false;
};

std::filesystem::path ExecutablePath() {
  std::wstring buffer(MAX_PATH * 2, L'\0');
  const DWORD length = GetModuleFileNameW(
      nullptr, buffer.data(), static_cast<DWORD>(buffer.size()));
  if (length == 0 || length >= buffer.size()) return std::filesystem::path();
  buffer.resize(length);
  return std::filesystem::path(buffer);
}
// The Explorer thumbnail provider ships as a sibling DLL; registration is a no-op
// when the build has no shell module (portable/dev layouts).
std::filesystem::path ThumbnailDllPath() {
  const std::filesystem::path exe = ExecutablePath();
  if (exe.empty()) return {};
  std::error_code ec;
  const std::filesystem::path dll = exe.parent_path() / L"ZoreViewThumb.dll";
  if (!std::filesystem::is_regular_file(dll, ec) || ec) return {};
  return dll;
}

// Registers the thumbnail provider that lives next to the executable. Returns
// false only when a DLL exists and registration failed.
bool RegisterThumbnailProviderNextToExe(nv::platform::RegistryScope scope,
                                        std::wstring* error) {
  const std::filesystem::path dll = ThumbnailDllPath();
  if (dll.empty()) return true;
  // Register through regsvr32 (the DLL's own DllRegisterServer) instead of
  // duplicating its registry writes in the exe: that is the documented entry
  // point and it needs no elevation for a per-user install.
  (void)scope;
  std::wstring command = L"regsvr32.exe /s \"" + dll.wstring() + L"\"";
  STARTUPINFOW startup{};
  startup.cb = sizeof(startup);
  PROCESS_INFORMATION process{};
  if (!CreateProcessW(nullptr, command.data(), nullptr, nullptr, FALSE,
                      CREATE_NO_WINDOW, nullptr, nullptr, &startup, &process)) {
    if (error != nullptr) {
      wchar_t line[160]{};
      std::swprintf(line, 160, L"cannot start regsvr32 (error %lu)",
                    static_cast<unsigned long>(GetLastError()));
      *error = line;
    }
    return false;
  }
  WaitForSingleObject(process.hProcess, 30000);
  DWORD code = 1;
  GetExitCodeProcess(process.hProcess, &code);
  CloseHandle(process.hThread);
  CloseHandle(process.hProcess);
  if (code != 0) {
    if (error != nullptr) {
      wchar_t line[160]{};
      std::swprintf(line, 160, L"regsvr32 failed (exit %lu)",
                    static_cast<unsigned long>(code));
      *error = line;
    }
    return false;
  }
  nv::platform::LogInfo("registered thumbnail provider");
  return true;
}
bool SaveWallpaperBmp(const nv::Image& image, std::wstring* error) {
  const std::filesystem::path directory =
      nv::platform::DataDir() / L"wallpaper";
  std::error_code ec;
  std::filesystem::create_directories(directory, ec);
  if (ec) {
    if (error != nullptr) *error = L"Could not create the wallpaper directory.";
    return false;
  }
  return nv::platform::SaveImage(directory / L"wallpaper.bmp", image,
                                 nv::platform::ImageSaveFormat::kBmp, 95,
                                 error);
}

std::filesystem::path AbsolutePath(const std::filesystem::path& path) {
  std::error_code ec;
  const std::filesystem::path absolute = std::filesystem::absolute(path, ec);
  return ec ? path : absolute;
}

bool SetWallpaperFromFile(const std::filesystem::path& source,
                          std::wstring* error) {
  nv::DecodeOptions options;
  options.animation = false;
  const nv::DecodeResult decoded = nv::FormatRouter::Decode(source, options);
  if (!decoded.ok()) {
    if (error != nullptr) {
      *error = L"Could not decode the image: ";
      *error += std::wstring(decoded.error.message.begin(),
                             decoded.error.message.end());
    }
    return false;
  }
  if (!SaveWallpaperBmp(*decoded.image, error)) return false;
  return nv::platform::SetDesktopWallpaper(
      nv::platform::DataDir() / L"wallpaper" / L"wallpaper.bmp", error);
}

std::string ToUtf8(const std::wstring& text) {
  if (text.empty()) return std::string();
  const int size =
      WideCharToMultiByte(CP_UTF8, 0, text.data(),
                          static_cast<int>(text.size()), nullptr, 0, nullptr,
                          nullptr);
  if (size <= 0) return std::string();
  std::string result(static_cast<size_t>(size), '\0');
  WideCharToMultiByte(CP_UTF8, 0, text.data(),
                      static_cast<int>(text.size()), result.data(), size,
                      nullptr, nullptr);
  return result;
}

void WriteUtf8(const std::filesystem::path& output_path,
               const std::string& text) {
  if (output_path.empty()) {
    HANDLE handle = GetStdHandle(STD_OUTPUT_HANDLE);
    if (handle == nullptr || handle == INVALID_HANDLE_VALUE) return;
    DWORD written = 0;
    WriteFile(handle, text.data(), static_cast<DWORD>(text.size()), &written,
              nullptr);
    return;
  }
  std::ofstream file(output_path, std::ios::binary | std::ios::trunc);
  if (file.is_open()) {
    file.write(text.data(), static_cast<std::streamsize>(text.size()));
  }
}

int PrintPaths(const std::filesystem::path& output_path) {
  std::wstring text;
  text += L"exe=";
  text += ExecutablePath().wstring();
  text += L"\ndir=";
  text += nv::platform::ExecutableDirectory().wstring();
  text += L"\ndata=";
  text += nv::platform::DataDir().wstring();
  text += L"\nportable=";
  text += nv::platform::IsPortableMode() ? L"1" : L"0";
  text += L"\n";
  WriteUtf8(output_path, ToUtf8(text));
  return 0;
}

int RunHeadlessAction(const HeadlessRequest& request) {
  std::wstring error;
  switch (request.action) {
    case HeadlessAction::kPrintSettings: {
      // Test hook (規劃/22/02 §7): the more panel writes straight into the
      // settings file, so the checks assert persistence by reading it back
      // from a fresh process.
      const nv::platform::Settings settings = nv::platform::LoadSettings();
      std::wstring text;
      text += L"dark_theme=";
      text += settings.dark_theme ? L"1" : L"0";
      text += L"\ncheckerboard=";
      text += settings.checkerboard ? L"1" : L"0";
      text += L"\nlanguage=";
      text += settings.language;
      text += L"\nthumbstrip_mode=";
      text += settings.thumbstrip_mode;
      text += L"\nbirdseye=";
      text += settings.birdseye ? L"1" : L"0";
      text += L"\nsort_mode=";
      text += settings.sort_mode;
      text += L"\ntopmost=";
      text += settings.topmost ? L"1" : L"0";
      text += L"\nkeep_rotation=";
      text += settings.keep_rotation ? L"1" : L"0";
      text += L"\nzoom_step=";
      wchar_t step[16]{};
      swprintf_s(step, L"%.2f", settings.zoom_step);
      text += step;
      text += L"\ntheme=";
      text += nv::platform::ThemeModeName(settings.theme);
      text += L"\nchecker_opacity=";
      text += std::to_wstring(settings.checker_opacity);
      text += L"\nremember_window=";
      text += settings.remember_window ? L"1" : L"0";
      text += L"\nsingle_instance=";
      text += settings.single_instance ? L"1" : L"0";
      text += L"\nwheel=";
      text += settings.wheel;
      text += L"\npixel_mode=";
      text += settings.pixel_mode ? L"1" : L"0";
      text += L"\nfullscreen_bg=";
      text += settings.fullscreen_bg;
      text += L"\nfolder_wrap=";
      text += settings.folder_wrap;
      text += L"\nwindow=";
      text += std::to_wstring(settings.window_x);
      text += L",";
      text += std::to_wstring(settings.window_y);
      text += L",";
      text += std::to_wstring(settings.window_w);
      text += L"x";
      text += std::to_wstring(settings.window_h);
      text += L"\nwindow_maximized=";
      text += settings.window_maximized ? L"1" : L"0";
      text += L"\nwindow_saved=";
      text += settings.window_saved ? L"1" : L"0";
      text += L"\ninfo_window=";
      text += std::to_wstring(settings.info_window_x);
      text += L",";
      text += std::to_wstring(settings.info_window_y);
      text += L"\ninfo_window_visible=";
      text += settings.info_window_visible ? L"1" : L"0";
      text += L"\ninfo_window_saved=";
      text += settings.info_window_saved ? L"1" : L"0";
      text += L"\nsettings_version=";
      text += std::to_wstring(nv::platform::kSettingsVersion);
      text += L"\n";
      const std::string utf8 = ToUtf8(text);
      // Empty path writes to stdout, which a console-hosted run inherits; the
      // automated checks pass a file so they never depend on that.
      WriteUtf8(request.output_path, utf8);
      return 0;
    }
    case HeadlessAction::kSetWallpaper: {
      if (request.path.empty() ||
          !SetWallpaperFromFile(AbsolutePath(request.path), &error)) {
        nv::platform::LogError("set wallpaper failed: " + Narrow(error));
        return 1;
      }
      nv::platform::LogInfo("desktop wallpaper set from shell verb: " +
                            Narrow(request.path.wstring()));
      return 0;
    }
    case HeadlessAction::kCopyImage: {
      if (request.path.empty()) return 1;
      const nv::DecodeOptions options{0, false};
      const nv::DecodeResult decoded =
          nv::FormatRouter::Decode(AbsolutePath(request.path), options);
      if (!decoded.ok()) {
        nv::platform::LogError("copy image: decode failed");
        return 1;
      }
      if (!nv::platform::CopyImageToClipboard(nullptr, *decoded.image,
                                              &error)) {
        nv::platform::LogError("copy image failed: " + Narrow(error));
        return 1;
      }
      nv::platform::LogInfo("image copied to clipboard from shell verb: " +
                            Narrow(request.path.wstring()));
      return 0;
    }
    case HeadlessAction::kRegisterShell: {
      const std::filesystem::path executable = ExecutablePath();
      if (executable.empty() ||
          !nv::platform::RegisterShellVerbs(executable, &error,
                                            request.scope)) {
        nv::platform::LogError("register shell verbs failed: " + Narrow(error));
        return 1;
      }
      nv::platform::LogInfo("registered shell verbs for " +
                            std::to_string(
                                nv::platform::ShellVerbExtensionCount()) +
                            " extensions");
      if (!RegisterThumbnailProviderNextToExe(request.scope, &error)) {
        nv::platform::LogError(Narrow(error));
        return 1;
      }
      return 0;
    }
    case HeadlessAction::kUnregisterShell: {
      if (!nv::platform::UnregisterShellVerbs(&error, request.scope)) {
        nv::platform::LogError("unregister shell verbs failed: " +
                               Narrow(error));
        return 1;
      }
      nv::platform::LogInfo("unregistered shell verbs");
      return 0;
    }
    case HeadlessAction::kRegisterAssociations: {
      const std::filesystem::path executable = ExecutablePath();
      std::wstring warning;
      if (executable.empty() ||
          !nv::platform::RegisterFileAssociations(
              executable, request.associate_default, &error, &warning,
              request.scope)) {
        nv::platform::LogError("register file associations failed: " +
                               Narrow(error));
        return 1;
      }
      if (!warning.empty()) {
        nv::platform::LogWarn("file associations: " + Narrow(warning));
      }
      nv::platform::LogInfo(
          "registered file associations for " +
          std::to_string(nv::platform::AssociationExtensionCount()) +
          " extensions");
      return 0;
    }
    case HeadlessAction::kUnregisterAssociations: {
      if (!nv::platform::UnregisterFileAssociations(&error, request.scope)) {
        nv::platform::LogError("unregister file associations failed: " +
                               Narrow(error));
        return 1;
      }
      nv::platform::LogInfo("unregistered file associations");
      return 0;
    }
    case HeadlessAction::kSetDefaultAssociation: {
      const std::filesystem::path executable = ExecutablePath();
      std::wstring warning;
      if (executable.empty() ||
          !nv::platform::RegisterFileAssociations(executable, true, &error,
                                                  &warning,
                                                  request.scope)) {
        nv::platform::LogError("set default associations failed: " +
                               Narrow(error));
        return 1;
      }
      if (!warning.empty()) {
        nv::platform::LogWarn("default associations: " + Narrow(warning));
      }
      nv::platform::LogInfo("set ZoreView as the default for supported images");
      return 0;
    }
    case HeadlessAction::kInstall: {
      const std::filesystem::path executable = ExecutablePath();
      std::wstring warning;
      if (executable.empty() ||
          !nv::platform::InstallApplication(
              executable, request.scope, request.install_dir,
              !request.no_shortcut, request.associate_default, &error,
              &warning)) {
        nv::platform::LogError("install failed: " + Narrow(error));
        return 1;
      }
      if (!warning.empty()) {
        nv::platform::LogWarn("install: " + Narrow(warning));
      }
      nv::platform::InstallLayout layout =
          nv::platform::DefaultInstallLayout(request.scope);
      if (!request.install_dir.empty()) layout.install_dir = request.install_dir;
      nv::platform::LogInfo("installed ZoreView to " +
                            Narrow(layout.install_dir.wstring()));
      return 0;
    }
    case HeadlessAction::kUninstall: {
      if (!nv::platform::UninstallApplication(request.scope,
                                              request.install_dir, &error)) {
        nv::platform::LogError("uninstall failed: " + Narrow(error));
        return 1;
      }
      nv::platform::LogInfo("uninstalled ZoreView");
      return 0;
    }
    case HeadlessAction::kPrintPaths:
      return PrintPaths(request.output_path);
    case HeadlessAction::kSetLanguage: {
      nv::i18n::Language language = nv::i18n::Language::kChinese;
      std::wstring code = request.path.wstring();
      if (!nv::i18n::ParseLanguage(code, &language)) {
        nv::platform::LogError("set language failed: unknown language code");
        return 1;
      }
      nv::i18n::SetLanguage(language);
      nv::platform::Settings settings = nv::platform::LoadSettings();
      settings.language = nv::i18n::LanguageCode(language);
      nv::platform::SaveSettings(settings);
      nv::platform::LogInfo("ui: language=" + Narrow(settings.language));
      return 0;
    }
    default:
      return 2;
  }
}

int APIENTRY wWinMain([[maybe_unused]] HINSTANCE instance,
                      [[maybe_unused]] HINSTANCE previous_instance,
                      [[maybe_unused]] PWSTR command_line,
                      int show_command) {
  ReportStartupPhase("begin");

  // Parse the command line before starting the device: Explorer's shell verbs
  // (--set-wallpaper/--copy-image) are single-shot and must not create a D3D
  // device, a window or a thumbnail service. For the normal GUI path the test
  // hooks still need to be active before the first frame is painted.
  int argc = 0;
  LPWSTR* argv = CommandLineToArgvW(GetCommandLineW(), &argc);
  std::filesystem::path open_path;
  HeadlessRequest headless;
  bool portable_requested = false;
  if (argv != nullptr) {
    for (int i = 1; i < argc; ++i) {
      const std::wstring arg = argv[i];
      if (arg == L"--screenshot" && i + 1 < argc) {
        g_screenshot_path = argv[++i];
      } else if (arg == L"--shot-delay" && i + 1 < argc) {
        g_screenshot_delay_ms = std::max(0, _wtoi(argv[++i]));
      } else if (arg == L"--click-index" && i + 1 < argc) {
        g_test_click_index = std::max(0, _wtoi(argv[++i]));
      } else if (arg == L"--scroll-notches" && i + 1 < argc) {
        g_test_scroll_notches = _wtoi(argv[++i]);
      } else if (arg == L"--rename-to" && i + 1 < argc) {
        g_test_rename_to = argv[++i];
      } else if (arg == L"--rename-input" && i + 1 < argc) {
        // V3.1-06 (§06e): the text the overlay opens with, so a check can drive
        // the overlay (open, edit, Enter) without synthesising keystrokes.
        g_test_rename_input = argv[++i];
        g_rename_has_test_input = true;
      } else if (arg == L"--delete-current") {
        g_test_delete_current = true;
      } else if (arg == L"--open-rename-dialog") {
        g_test_open_rename_dialog = true;
      } else if (arg == L"--force-error" && i + 1 < argc) {
        // --force-error missing|access|unsupported|corrupt|codec|oom|save:
        // deterministic presentation for tests/run_error_ui_check.ps1.
        g_force_error = nv::ParseErrorKind(argv[++i], &g_force_error_kind);
      } else if (arg == L"--force-preview-fail") {
        // Two-stage loading: the full decode fails once after a preview was
        // shown, so "keep the preview + error card" is deterministic
        // (規劃/22/13 §6).
        g_force_preview_fail = true;
      } else if (arg == L"--preview-zoom" && i + 1 < argc) {
        // Zoom the preview as soon as it appears; the final dump then proves
        // that the full frame kept the manual view (規劃/22/13 §6).
        g_test_preview_zoom_percent = std::clamp(_wtoi(argv[++i]), 0, 3200);
      } else if (arg == L"--send-keys-at" && i + 1 < argc) {
        // Start the queued chords earlier than the default first-frame delay;
        // the export-block check must land while only the preview exists.
        g_test_keys_start_ms = std::clamp(_wtoi(argv[++i]), 0, 60000);
      } else if (arg == L"--send-keys-interval" && i + 1 < argc) {
        // V3.1-01: the 200 ms burst cadence the switch-speed scenarios use.
        g_test_keys_interval_ms = std::clamp(_wtoi(argv[++i]), 20, 5000);
      } else if (arg == L"--force-toast" && i + 1 < argc) {
        g_force_toasts.emplace_back(argv[++i]);
      } else if (arg == L"--force-confirm") {
        g_test_force_confirm = true;
      } else if (arg == L"--force-loading") {        g_force_loading = true;
        // The optional duration is numeric only: "--force-loading <image>"
        // must keep the image positional (a path also starts with a letter).
        if (i + 1 < argc && argv[i + 1][0] >= L'0' && argv[i + 1][0] <= L'9') {
          g_force_loading_ms = std::max(0, _wtoi(argv[++i]));
        }
        if (g_force_loading_ms > 0 &&
            g_force_loading_ms < static_cast<int>(kLoadingDelayMs)) {
          g_suppress_loading = true;
        }
      } else if (arg == L"--language" && i + 1 < argc) {
        nv::i18n::Language parsed = nv::i18n::Language::kChinese;
        if (nv::i18n::ParseLanguage(argv[++i], &parsed)) {
          g_language_override = parsed == nv::i18n::Language::kChinese ? 0 : 1;
        }
      } else if (arg == L"--info-scroll" && i + 1 < argc) {
        g_test_info_scroll = _wtoi(argv[++i]);
      } else if (arg == L"--force-dpi-scale" && i + 1 < argc) {
        double scale = _wtof(argv[++i]);
        if (scale > 10.0) scale /= 100.0;  // accept 125 as well as 1.25
        if (scale > 0.0) {
          nv::platform::SetForcedDpiScale(static_cast<float>(scale));
        }
      } else if (arg == L"--force-high-contrast") {
        nv::platform::SetForcedHighContrast(1);
      } else if (arg == L"--force-no-high-contrast") {
        nv::platform::SetForcedHighContrast(0);
      } else if (arg == L"--bench-startup" && i + 1 < argc) {
        g_bench_startup_path = argv[++i];
      } else if (arg == L"--portable") {
        portable_requested = true;
      } else if (arg == L"--borderless") {
        g_borderless = true;  // default; accepted for explicit A/B scripts
      } else if (arg == L"--classic-frame") {
        g_borderless = false;
      } else if (arg == L"--strip" && i + 1 < argc) {
        nv::ui::StripMode mode = nv::ui::StripMode::kPinned;
        if (nv::ui::ParseStripMode(argv[++i], &mode)) {
          g_strip_mode = mode;
          g_strip_override = true;
        }
      } else if (arg.rfind(L"--strip=", 0) == 0) {
        // Registered form: --strip=pinned|auto|off.
        nv::ui::StripMode mode = nv::ui::StripMode::kPinned;
        if (nv::ui::ParseStripMode(arg.substr(std::wstring(L"--strip=").size()),
                                   &mode)) {
          g_strip_mode = mode;
          g_strip_override = true;
        }
      } else if (arg == L"--click-hit" && i + 1 < argc) {
        g_test_click_hits.emplace_back(argv[++i]);
      } else if (arg == L"--context-menu" && i + 1 < argc) {
        // v3.0 (規劃/26 §2.9): open the self-drawn right-click menu at a client
        // point "x,y" after the first image, through the real WM_CONTEXTMENU
        // path (screen coordinates are derived at timer time).
        const std::wstring value = argv[++i];
        const size_t comma = value.find(L',');
        if (comma != std::wstring::npos) {
          g_test_context_menu_client.x = _wtoi(value.substr(0, comma).c_str());
          g_test_context_menu_client.y = _wtoi(value.substr(comma + 1).c_str());
          g_test_context_menu = true;
        } else {
          nv::platform::LogError("--context-menu expects x,y");
        }
      } else if (arg == L"--minimap-drag" && i + 1 < argc) {
        // v3.0 (規劃/26 §2.4): dx,dy drag of the bird's eye frame. Queued with
        // the synthetic clicks so it runs after the first image is on screen and
        // the layout dump waits for it.
        g_test_click_hits.push_back(std::wstring(L"minimap.drag:") +
                                    argv[++i]);
      } else if (arg == L"--info-window") {
        // v3.0 (規劃/26 §2.1): open the standalone info window after the first
        // image, so the window checks need no synthetic click first.
        g_test_open_info_window = true;
      } else if (arg == L"--info-screenshot" && i + 1 < argc) {
        // Captures the info window itself (PrintWindow); --screenshot stays
        // reserved for the main window (規劃/26 §6 decision).
        g_test_info_screenshot = argv[++i];
      } else if (arg == L"--info-geometry" && i + 1 < argc) {
        // x,y of the info window's top-left corner: moves it and sends the
        // WM_EXITSIZEMOVE that a real drag would, so placement persistence can
        // be asserted without a mouse (same trick as --move-window).
        const std::wstring text = argv[++i];
        const size_t comma = text.find(L',');
        if (comma != std::wstring::npos) {
          g_test_info_geometry.x = _wtoi(text.substr(0, comma).c_str());
          g_test_info_geometry.y = _wtoi(text.substr(comma + 1).c_str());
          g_test_info_geometry_valid = true;
        }
      } else if (arg == L"--info-key" && i + 1 < argc) {
        // Delivers one key to the info window ("escape" / "i"), which is how the
        // single-owner keyboard check proves the card closes itself
        // (規劃/27 坑 219).
        const std::wstring key = argv[++i];
        if (_wcsicmp(key.c_str(), L"escape") == 0) {
          g_test_info_key = VK_ESCAPE;
        } else if (_wcsicmp(key.c_str(), L"i") == 0) {
          g_test_info_key = 'I';
        }
      } else if (arg == L"--hover-thumb" && i + 1 < argc) {
        // Test hook: post a real WM_MOUSEMOVE at the centre of one rail slot so
        // the 500 ms tooltip path runs unchanged (規劃/22/08 §7).
        g_test_hover_thumb = _wtoi(argv[++i]);
        if (g_test_hover_thumb < 0) g_test_hover_thumb = -1;
      } else if (arg == L"--hover-hotzone") {
        g_test_hover_hotzone = true;
      } else if (arg == L"--hover-cycle") {
        g_test_hover_cycle = true;
      } else if (arg == L"--hover-bottom") {
        // v3.0 (規劃/26 §2.10): move onto the bottom bar and expect a reveal.
        g_test_hover_bottom = true;
      } else if (arg == L"--drag-to-bottom") {
        // Press inside the image, start the pan, then drag onto the bottom bar:
        // the rail must stay hidden and the pan must keep running.
        g_test_drag_to_bottom = true;
      } else if (arg == L"--hover-fs") {
        // v3.0 (規劃/26 §2.7): move into the fullscreen top-right reveal zone.
        g_test_hover_fs = true;
      } else if (arg == L"--fullscreen") {
        g_test_fullscreen = true;
      } else if (arg == L"--rotation" && i + 1 < argc) {
        // --rotation 0|90|180|270: deterministic initial view rotation, used by
        // tests/run_rotation_check.ps1 for the four-angle screenshots.
        const int degrees = _wtoi(argv[++i]);
        g_initial_rotation_steps = ((degrees / 90) % 4 + 4) % 4;
      } else if (arg == L"--save-as" && i + 1 < argc) {
        // Test hook: save the current view (including its rotation) after the
        // queued keys have been applied.
        g_test_save_as = argv[++i];
      } else if (arg == L"--save-to" && i + 1 < argc) {
        // Test hook (22/14 §6): save the current view as the container named
        // by the extension, with an optional --save-quality.
        g_test_save_to = argv[++i];
      } else if ((arg == L"--save-quality" || arg == L"--quality") &&
                 i + 1 < argc) {
        // The plan spells the hook `--quality`; `--save-quality` is the explicit
        // form used by the scripts. Both are accepted.
        g_test_save_quality = std::clamp(_wtoi(argv[++i]), 1, 100);
      } else if (arg == L"--save-options" && i + 1 < argc) {
        // Test hook: open the real system save dialog for a target 1200 ms after
        // the first image, so scripts can drive it with NV_SAVEAS.
        g_test_save_options = argv[++i];
      } else if (arg == L"--send-keys" && i + 1 < argc) {
        // Comma-separated chord sequence, e.g. "F11,Right,Right,Esc".
        std::wstring sequence = argv[++i];
        size_t start = 0;
        while (start <= sequence.size()) {
          const size_t comma = sequence.find(L',', start);
          const size_t end =
              comma == std::wstring::npos ? sequence.size() : comma;
          if (end > start) {
            g_test_keys.push_back(sequence.substr(start, end - start));
          }
          if (comma == std::wstring::npos) break;
          start = comma + 1;
        }
      } else if (arg == L"--simulate-drop" && i + 1 < argc) {
        g_test_drops.emplace_back(argv[++i]);
      } else if (arg == L"--simulate-drag-over") {
        g_test_drag_over = true;
      } else if (arg == L"--simulate-ole-drop" && i + 1 < argc) {
        g_test_ole_drop = argv[++i];
      } else if (arg == L"--simulate-drag-out") {
        // Dry-run drag-out of the current file: asserts the same path selection,
        // logging and state cleanup without entering DoDragDrop's loop.
        g_test_drag_current = true;
        g_test_drag_dry_run = true;
        // V3.1-06: optional "<dx>,<dy>" in DIP from the content centre; the
        // default leaves the window by 20 DIP, which is the gesture that now
        // drags out. "200,0" stays inside and must remain a pan.
        if (i + 1 < argc) {
          const std::wstring value = argv[i + 1];
          const size_t comma = value.find(L',');
          if (comma != std::wstring::npos) {
            ++i;
            g_test_drag_dx = static_cast<float>(_wtof(value.c_str()));
            g_test_drag_dy =
                static_cast<float>(_wtof(value.c_str() + comma + 1));
            g_test_drag_use_edge = false;
          }
        }
      } else if (arg == L"--simulate-strip-drag" && i + 1 < argc) {
        g_test_drag_strip_index = _wtoi(argv[++i]);
        g_test_drag_dry_run = true;
      } else if (arg == L"--simulate-drag-out-ctrl") {
        // V3.1-06: Ctrl+canvas drag is still an immediate drag-out (10 DIP).
        g_test_drag_current = true;
        g_test_drag_dry_run = true;
        g_test_drag_ctrl = true;
        g_test_drag_dx = 10.0f;
        g_test_drag_dy = 0.0f;
        g_test_drag_use_edge = false;
      } else if (arg == L"--print-settings") {
        headless.action = HeadlessAction::kPrintSettings;
        // Same optional-file convention as --print-paths.
        if (i + 1 < argc && argv[i + 1][0] != L'-') {
          headless.output_path = argv[++i];
        }
      } else if (arg == L"--open-settings") {
        g_test_open_settings = true;
      } else if (arg == L"--settings-page" && i + 1 < argc) {
        const std::wstring page = argv[++i];
        if (page == L"view") {
          g_test_settings_page = nv::ui::SettingsPage::kView;
        } else if (page == L"association") {
          g_test_settings_page = nv::ui::SettingsPage::kAssociation;
        } else {
          g_test_settings_page = nv::ui::SettingsPage::kGeneral;
        }
      } else if (arg == L"--settings-click" && i + 1 < argc) {
        g_test_settings_actions.emplace_back(argv[++i]);
      } else if (arg == L"--settings-dump" && i + 1 < argc) {
        g_test_settings_dump = argv[++i];
      } else if (arg == L"--settings-screenshot" && i + 1 < argc) {
        g_test_settings_screenshot = argv[++i];
      } else if (arg == L"--layout-dump" && i + 1 < argc) {
        g_layout_dump_path = argv[++i];
      } else if (arg == L"--dump-at" && i + 1 < argc) {
        // Write the layout dump at this delay without exiting; used with
        // --quit-at by tests/run_watch_check.ps1 while it mutates files.
        g_test_dump_at_ms = std::max(1, _wtoi(argv[++i]));
      } else if (arg == L"--quit-at" && i + 1 < argc) {
        g_test_quit_at_ms = std::max(1, _wtoi(argv[++i]));
      } else if (arg == L"--move-window" && i + 1 < argc) {
        int x = 0;
        int y = 0;
        int w = 0;
        int h = 0;
        if (swscanf_s(argv[++i], L"%d,%d,%d,%d", &x, &y, &w, &h) == 4) {
          g_test_move_window_valid = true;
          g_test_move_window_rect = {x, y, x + w, y + h};
        }
      } else if (arg == L"--minimize-at" && i + 1 < argc) {
        g_test_minimize_at_ms = std::max(1, _wtoi(argv[++i]));
      } else if (arg == L"--restore-at" && i + 1 < argc) {
        g_test_restore_at_ms = std::max(1, _wtoi(argv[++i]));
      } else if (arg == L"--force-anim" && i + 1 < argc) {
        const std::wstring value = argv[++i];
        if (value == L"first") {
          g_force_anim = ForceAnimState::kFirstFrame;
        } else if (value == L"paused") {
          g_force_anim = ForceAnimState::kPaused;
        } else if (value == L"ended") {
          g_force_anim = ForceAnimState::kEnded;
        } else if (value == L"truncated") {
          g_force_anim = ForceAnimState::kTruncated;
        }
      } else if (arg == L"--force-quality" && i + 1 < argc) {
        // --force-quality fast|good|pixel (規劃/22/18 §3.5). Numeric values are
        // accepted too so the script can stay terse.
        const std::wstring value = argv[++i];
        if (value == L"fast") {
          g_force_quality = nv::render::DrawQualityValue(
              nv::render::DrawQuality::kFast);
        } else if (value == L"good") {
          g_force_quality = nv::render::DrawQualityValue(
              nv::render::DrawQuality::kGood);
        } else if (value == L"pixel") {
          g_force_quality = nv::render::DrawQualityValue(
              nv::render::DrawQuality::kPixel);
        } else if (value == L"auto") {
          g_force_quality = nv::render::kQualityAuto;
        } else {
          const int numeric = _wtoi(value.c_str());
          if (numeric >= 0 && numeric <= 2) g_force_quality = numeric;
        }
      } else if (arg == L"--proxy" && i + 1 < argc) {
        // --proxy on|off (or --proxy=on|off): A/B captures for the 1/4 proxy.
        const std::wstring value = argv[++i];
        if (value == L"off" || value == L"0") {
          g_force_proxy = 0;
        } else if (value == L"on" || value == L"1") {
          g_force_proxy = 1;
        }
      } else if (arg == L"--proxy=off") {
        g_force_proxy = 0;
      } else if (arg == L"--proxy=on") {
        g_force_proxy = 1;
      } else if (arg == L"--display-cache" && i + 1 < argc) {
        // --display-cache off|on|force (規劃/25 §7.8). `force` also skips the
        // "this view needs a full frame" guard, for A/B captures.
        const std::wstring value = argv[++i];
        if (value == L"off" || value == L"0") {
          g_display_cache_mode = DisplayCacheMode::kOff;
        } else if (value == L"force") {
          g_display_cache_mode = DisplayCacheMode::kForce;
        } else {
          g_display_cache_mode = DisplayCacheMode::kOn;
        }
      } else if (arg == L"--display-cache=off") {
        g_display_cache_mode = DisplayCacheMode::kOff;
      } else if (arg == L"--display-cache=on") {
        g_display_cache_mode = DisplayCacheMode::kOn;
      } else if (arg == L"--display-cache=force") {
        g_display_cache_mode = DisplayCacheMode::kForce;
      } else if (arg == L"--cache-trace") {
        g_cache_trace = true;
      } else if (arg == L"--prefetch=off") {
        g_prefetch_enabled = false;
      } else if (arg == L"--prefetch=on") {
        g_prefetch_enabled = true;
      } else if (arg == L"--prefetch-parallel=off") {
        // V3.1-01 rollback: keep the burst gate closed (01 A/C stay).
        g_prefetch_parallel = false;
      } else if (arg == L"--prefetch-parallel=on") {
        g_prefetch_parallel = true;
      } else if (arg == L"--prefetch-depth=1") {
        // V3.1-07 S4 rollback: one neighbour instead of 1-3 (the direction walk
        // itself stays, see g_prefetch_max_depth).
        g_prefetch_max_depth = 1;
      } else if (arg == L"--prefetch-depth=2") {
        g_prefetch_max_depth = 2;
      } else if (arg == L"--prefetch-depth=3") {
        g_prefetch_max_depth = 3;
      } else if (arg == L"--prefetch-raw=off") {
        // V3.1-07 S4 rollback: never prefetch a RAW neighbour (pre-S4 rule).
        g_prefetch_raw_neighbours = false;
      } else if (arg == L"--prefetch-raw=on") {
        g_prefetch_raw_neighbours = true;
      } else if (arg == L"--transition" && i + 1 < argc) {
        // V3.1-02 / V3.1-07 S3: off|auto|sharp|force, as `--transition <mode>`
        // or `--transition=<mode>` (the spelling 07 §3 uses for the rollback).
        g_transition_mode = ParseTransitionMode(argv[++i]);
      } else if (arg.rfind(L"--transition=", 0) == 0) {
        g_transition_mode = ParseTransitionMode(arg.substr(13));
      } else if (arg == L"--raw-preview=off") {
        // V3.1-04a rollback: fit always full-decodes the RAW (v3.0 behaviour).
        g_raw_preview_mode = RawPreviewMode::kOff;
      } else if (arg == L"--preview-cache=off") {
        // V3.1-07 S2 rollback: the embedded preview still becomes the final
        // frame, it is just not stored as the fit display tier (no revisit hit).
        g_preview_cache = false;
      } else if (arg == L"--preview-cache=on") {
        g_preview_cache = true;
      } else if (arg == L"--thumb-prewarm=off") {
        // V3.1-07 S1 rollback: no ±6 thumbnail pre-read (see g_thumb_prewarm).
        g_thumb_prewarm = false;
      } else if (arg == L"--thumb-prewarm=on") {
        g_thumb_prewarm = true;
      } else if (arg == L"--raw-preview=on") {
        g_raw_preview_mode = RawPreviewMode::kPreview;
      } else if (arg == L"--watch-interval" && i + 1 < argc) {
        g_watch_interval_ms =
            static_cast<UINT>(std::clamp(_wtoi(argv[++i]), 100, 10000));
      } else if (arg == L"--watch-poll") {
        // V3-05: force the 2 s polling fallback on a local disk, so the
        // fallback branch is testable without an SMB share (規劃/25 §8).
        g_watch_force_poll = true;
      } else if (arg == L"--maximized") {
        g_start_maximized = true;
      } else if (arg == L"--fullscreen") {
        g_start_fullscreen = true;
      } else if (arg == L"--machine") {
        headless.scope = nv::platform::RegistryScope::kMachine;
      } else if (arg == L"--no-shortcut") {
        headless.no_shortcut = true;
      } else if (arg == L"--associate-default") {
        headless.associate_default = true;
      } else if (arg == L"--install-dir" && i + 1 < argc) {
        headless.install_dir = argv[++i];
      } else if (arg == L"--set-wallpaper") {
        headless.action = HeadlessAction::kSetWallpaper;
        if (i + 1 < argc) headless.path = argv[++i];
      } else if (arg == L"--copy-image") {
        headless.action = HeadlessAction::kCopyImage;
        if (i + 1 < argc) headless.path = argv[++i];
      } else if (arg == L"--register-shell") {
        headless.action = HeadlessAction::kRegisterShell;
      } else if (arg == L"--unregister-shell") {
        headless.action = HeadlessAction::kUnregisterShell;
      } else if (arg == L"--register-assoc") {
        headless.action = HeadlessAction::kRegisterAssociations;
      } else if (arg == L"--unregister-assoc") {
        headless.action = HeadlessAction::kUnregisterAssociations;
      } else if (arg == L"--set-default-assoc") {
        headless.action = HeadlessAction::kSetDefaultAssociation;
      } else if (arg == L"--install") {
        headless.action = HeadlessAction::kInstall;
      } else if (arg == L"--uninstall") {
        headless.action = HeadlessAction::kUninstall;
      } else if (arg == L"--set-language" && i + 1 < argc) {
        headless.action = HeadlessAction::kSetLanguage;
        headless.path = argv[++i];
      } else if (arg == L"--print-paths") {
        headless.action = HeadlessAction::kPrintPaths;
        // The optional file argument is only consumed when it is not another
        // switch, so "--print-paths --portable" keeps working.
        if (i + 1 < argc && argv[i + 1][0] != L'-') {
          headless.output_path = argv[++i];
        }
      } else if (arg == L"--quiet") {
        // Accepted for the Apps & Features quiet-uninstall string.
      } else if (arg.rfind(L"--", 0) == 0) {
        continue;
      } else if (open_path.empty()) {
        open_path = arg;
      }
    }
    LocalFree(argv);
  }

  // Portable mode must be decided before logging/settings touch DataDir().
  if (portable_requested) nv::platform::SetPortableMode(true);

  if (headless.action != HeadlessAction::kNone) {
    nv::platform::LogInit();
    // Shell integration labels (verbs, ProgID, shortcut) follow the saved UI
    // language even when the command runs without a window.
    nv::i18n::Language language = nv::i18n::SystemLanguage();
    const nv::platform::Settings settings = nv::platform::LoadSettings();
    if (!settings.language.empty() &&
        !nv::i18n::ParseLanguage(settings.language, &language)) {
      language = nv::i18n::SystemLanguage();
    }
    if (g_language_override >= 0) {
      language = g_language_override == 0 ? nv::i18n::Language::kChinese
                                          : nv::i18n::Language::kEnglish;
    }
    nv::i18n::SetLanguage(language);
    const HRESULT shell_com = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    const int code = RunHeadlessAction(headless);
    if (SUCCEEDED(shell_com)) CoUninitialize();
    nv::platform::LogShutdown();
    return code;
  }

  // The first D3D11 device costs ~200 ms (driver/UMD initialization) on this
  // machine. Start it on a worker immediately so it overlaps COM, window
  // creation and the first decode; the UI thread only waits for it when the
  // window is already on screen.
  auto device_future = std::async(std::launch::async, [] {
    return nv::render::Renderer::CreateDeviceBundle();
  });

  nv::platform::LogInit();
  nv::platform::InstallCrashHandler();
  nv::platform::LogInfo("ZoreView starting");
  ReportStartupPhase("logging");

  const HRESULT com_hr = OleInitialize(nullptr);
  if (FAILED(com_hr) && com_hr != RPC_E_CHANGED_MODE) {
    return 1;
  }
  // OLE is required, not just COM: RegisterDragDrop and IDropTarget fail with
  // an STA that was initialized with CoInitializeEx only (規劃/22/03 §4.1).
  const bool ole_initialized = SUCCEEDED(com_hr);
  ReportStartupPhase("com");

  // Format capabilities are probed once: build flags + installed WIC codecs.
  // The empty state and "all formats" card both read this one summary.
  g_format_summary = &nv::GetFormatSummary();
  {
    const nv::FormatCapabilities& caps = nv::FormatCapabilitiesForThisBuild();
    char line[192]{};
    std::snprintf(line, sizeof(line),
                  "format: freeimage=%d libraw=%d heif=%d avif=%d "
                  "extensions=%zu routed=%zu",
                  caps.freeimage ? 1 : 0, caps.libraw ? 1 : 0,
                  caps.heif ? 1 : 0, caps.avif ? 1 : 0,
                  g_format_summary->extension_count,
                  g_format_summary->routed_extension_count);
    nv::platform::LogInfo(line);
  }
  // Save dialog capabilities are probed once, before the window exists, and
  // logged so the tests can assert which containers this build can write
  // (規劃/22/14 §3).
  (void)nv::platform::ProbeEncoders();
  nv::platform::LogInfo("save: encoders=" + nv::platform::EncoderList());
  if (g_test_drag_over) {
    g_drag_over = true;
    nv::platform::LogInfo("drop: enter (simulated)");
  }

  EnablePerMonitorDpi();
  ReportStartupPhase("dpi");

  // Settings are loaded before the window exists so the window class can carry
  // the themed background brush used by the pre-D3D first paint.
  g_state = std::make_unique<WindowState>();
  ReportStartupPhase("state");
  g_settings = nv::platform::LoadSettings();
  nv::i18n::Language language = nv::i18n::SystemLanguage();
  if (!g_settings.language.empty() &&
      !nv::i18n::ParseLanguage(g_settings.language, &language)) {
    language = nv::i18n::SystemLanguage();
  }
  if (g_language_override >= 0) {
    language = g_language_override == 0 ? nv::i18n::Language::kChinese
                                        : nv::i18n::Language::kEnglish;
  }
  ApplyLanguage(nullptr, language, false);
  // `--strip=` is a session-only override; otherwise the saved mode wins
  // (規劃/22/02 §3.3 single source of truth).
  if (!g_strip_override) {
    nv::ui::StripMode saved = nv::ui::StripMode::kPinned;
    if (nv::ui::ParseStripMode(g_settings.thumbstrip_mode, &saved)) {
      g_strip_mode = saved;
    }
  }
  // v3.0 (規劃/26 §2.5): the folder order is fixed (Explorer natural order), so
  // settings.sort_mode is read into Settings and kept on disk for migration but
  // no longer drives anything.
  nv::platform::LogInfo("ui: strip_mode=" +
                        Narrow(nv::ui::StripModeName(g_strip_mode)) +
                        " birdseye=" + (g_settings.birdseye ? "1" : "0"));

  // Single instance (規劃/22/15): forward the requested path to the instance
  // that already exists and exit. The mutex name includes a DataDir hash so a
  // portable and an installed copy never swallow each other (坑 10); the
  // message-only receiver exists before the main window so a launch during
  // device initialisation is not lost (坑 7).
  if (g_settings.single_instance) {
    const std::wstring mutex_name =
        nv::platform::InstanceMutexName(nv::platform::DataDir());
    const std::wstring receiver_class =
        nv::platform::InstanceReceiverClass(nv::platform::DataDir());
    g_instance_lock = std::make_unique<nv::platform::SingleInstanceLock>();
    bool already_running = false;
    if (!g_instance_lock->Acquire(mutex_name, &already_running)) {
      nv::platform::LogWarn("single_instance: mutex unavailable; opening normally");
    } else if (already_running) {
      HWND existing = nv::platform::InstanceReceiver::Find(receiver_class);
      bool forwarded = false;
      if (existing != nullptr) {
        // Best effort: without this the receiver can only flash its window.
        nv::platform::AllowForegroundFromWindow(existing);
        std::wstring error;
        // Forward an absolute path: the other instance's working directory may
        // differ from ours.
        const std::wstring forwarded_path =
            open_path.empty() ? std::wstring() : AbsolutePath(open_path).wstring();
        forwarded = nv::platform::ForwardInstanceMessage(existing, forwarded_path,
                                                         2000, &error);
        if (forwarded) {
          nv::platform::LogInfo(
              "single_instance: forwarded to existing window");
          if (!open_path.empty()) {
            nv::platform::LogInfo("single_instance: open=" +
                                  Narrow(open_path.wstring()));
          } else {
            nv::platform::LogInfo("single_instance: forwarded activate");
          }
          nv::platform::LogShutdown();
          return 0;
        }
        nv::platform::LogWarn("single_instance: forward failed: " +
                              Narrow(error) + "; opening normally");
      } else {
        nv::platform::LogWarn(
            "single_instance: mutex exists but no receiver; opening normally");
      }
      // Do not create a receiver here: forwarding failed, so this process is a
      // normal extra window and must not hijack later launches.
      (void)forwarded;
    } else {
      g_instance_receiver = std::make_unique<nv::platform::InstanceReceiver>();
      if (g_instance_receiver->Create(receiver_class)) {
        nv::platform::LogInfo("single_instance: receiver ready");
      } else {
        nv::platform::LogWarn("single_instance: receiver creation failed");
        g_instance_receiver.reset();
      }
    }
  }
  g_state->renderer.SetTheme(g_settings.dark_theme);
  g_state->renderer.SetCheckerboard(g_settings.checkerboard);
  g_state->renderer.SetCheckerOpacity(g_settings.checker_opacity);
  g_state->renderer.SetPixelMode(g_settings.pixel_mode);
  // 22/18 test hooks: forced interpolation / proxy switch must be in place
  // before the first frame so screenshots are deterministic.
  g_state->renderer.SetForcedQuality(g_force_quality);
  if (g_force_proxy >= 0) {
    g_state->renderer.SetProxyEnabled(g_force_proxy != 0);
  }
  g_state->renderer.SetHighContrast(nv::platform::IsHighContrastEnabled());
  ApplyFullscreenSettings();
  g_background_brush =
      CreateSolidBrush(g_state->renderer.background_colorref());

  // DWrite is device independent and costs ~2 ms, so it is created before the
  // window: the GDI first frame and the first D2D frame then share one text
  // context (規劃/22/22 §1 step 9).
  g_ui = std::make_unique<nv::ui::UiRoot>();
  if (!g_ui->Create()) {
    nv::platform::LogError("ui: DWrite factory creation failed");
  }
  g_ui->SetDpiScale(nv::platform::DpiScaleForWindow(nullptr));
  g_ui->SetTheme(g_settings.dark_theme, nv::platform::IsHighContrastEnabled());
  // Warm the DWrite formats here: this overlaps the device-initialization
  // thread instead of landing inside the first WM_SIZE.
  g_ui->WarmUp();
  ReportStartupPhase("ui");

  WNDCLASSEXW window_class{};
  window_class.cbSize = sizeof(window_class);
  window_class.style = CS_HREDRAW | CS_VREDRAW;
  window_class.lpfnWndProc = WindowProc;
  window_class.hInstance = instance;
  window_class.hCursor = LoadCursorW(nullptr, IDC_ARROW);
  // Brand icon from the .rc (IDI_ZOREVIEW); the small one drives the title bar
  // and Alt+Tab at the system's small-icon size.
  window_class.hIcon = static_cast<HICON>(LoadImageW(
      instance, MAKEINTRESOURCEW(101), IMAGE_ICON, 0, 0,
      LR_DEFAULTSIZE | LR_SHARED));
  window_class.hIconSm = static_cast<HICON>(LoadImageW(
      instance, MAKEINTRESOURCEW(101), IMAGE_ICON,
      GetSystemMetrics(SM_CXSMICON), GetSystemMetrics(SM_CYSMICON), LR_SHARED));
  window_class.hbrBackground = g_background_brush;
  window_class.lpszClassName = kWindowClass;

  if (RegisterClassExW(&window_class) == 0) {
    return 1;
  }

  HWND hwnd = CreateWindowExW(
      0, kWindowClass, nv::i18n::Text(nv::i18n::StringId::kAppTitle), WS_OVERLAPPEDWINDOW,
      CW_USEDEFAULT, CW_USEDEFAULT, kDefaultWidth, kDefaultHeight,
      nullptr, nullptr, instance, nullptr);
  if (hwnd == nullptr) {
    g_state.reset();
    return 1;
  }
  g_main_window = hwnd;
  // The circles start visible and fade out if nothing happens for five seconds.
  ArmArrowIdle(hwnd);
  ReportStartupPhase("window");
  ApplyWindowFrame(hwnd);
  ApplyThemeState(hwnd);
  // The more panel owns the toggle; the saved value is applied once the window
  // exists so a restart restores the z-order (規劃/22/15).
  if (g_settings.topmost) ApplyTopmost(hwnd, true);

  g_state->disk_cache = std::make_unique<nv::DiskCache>(
      nv::platform::DataDir() / L"cache" / L"images");
  g_state->cache_writer =
      std::make_unique<nv::DiskCacheWriter>(g_state->disk_cache.get());
  g_state->thumbs = std::make_unique<nv::ThumbnailService>(
      hwnd, kThumbnailDone, kThumbEdge, 1, 64ull * 1024 * 1024,
      nv::platform::DataDir() / L"cache" / L"thumbs");
  // Three full-size decode workers plus one dedicated preview worker
  // (規劃/22/13 §3.2, 規劃/25 §7.13-1): a preview never queues behind a full
  // decode, and with the prefetch gate in DecodeService (at most one full-frame
  // or two tier prefetches in flight, and none while a current request is
  // pending) the third worker exists so a current decode never waits behind two
  // prefetches.
  g_state->decode =
      std::make_unique<nv::DecodeService>(hwnd, kDecodeDone, 3, kPreviewDone);
  nv::platform::LogInfo("decode: workers=3 prefetch_cap=full:1 tier:2");
  // V3-04: tier prefetches read the display-entry disk cache from their worker
  // instead of decoding a stored neighbour again (規劃/25 §7.5).
  if (g_display_cache_mode != DisplayCacheMode::kOff) {
    g_state->decode->SetTierCache(g_state->disk_cache.get());
  }
  // EXIF worker (規劃/22/06): one low-priority thread; the info panel never
  // reads metadata on the UI thread.
  g_exif = std::make_unique<nv::ExifService>(hwnd, kExifDone, 8);
  // Save worker (規劃/22/14): one thread so a large encode never blocks the UI
  // thread or the decode workers. The size estimate lives in the system dialog
  // since V3.1-06 (§06c), so there is no second estimate worker any more.
  g_save_service = std::make_unique<nv::platform::SaveService>();
  g_save_service->Start(hwnd, kSaveDone);

  // OLE drop target: the only drop path (DragAcceptFiles was removed). Hover
  // transitions repaint once; Drop routes the first path through the same
  // OnDroppedPaths() the --simulate-drop hook uses (規劃/22/03 §4.1).
  g_drop_target = nv::platform::DropTargetPtr(new nv::platform::DropTarget());
  std::wstring drop_error;
  if (!g_drop_target->Register(
          hwnd,
          [hwnd](const std::vector<std::filesystem::path>& paths) {
            OnDroppedPaths(hwnd, paths);
          },
          [hwnd](bool hovering) {
            g_drag_over = hovering;
            nv::platform::LogInfo(hovering ? "drop: enter" : "drop: leave");
            RefreshUi(hwnd, false);
            InvalidateRect(hwnd, nullptr, FALSE);
          },
          &drop_error)) {
    nv::platform::LogError("drop: register failed: " + Narrow(drop_error));
  } else {
    nv::platform::LogInfo("drop: register ok");
  }

  ApplyDpiScale(hwnd);
  if (!RestoreWindowPlacement(hwnd)) {
    CenterWindow(hwnd, kDefaultWidth, kDefaultHeight);
  }

  // Start the first decode before the device exists: the decode runs on its
  // own worker thread and overlaps the driver initialization. LoadPath skips
  // the image caches until the renderer can draw, which is exactly the case.
  if (!open_path.empty()) OpenFile(hwnd, open_path);
  ReportStartupPhase("opening");

  // DWM plays a fade/slide transition on the first show; skip it so the window
  // appears at once.
  const BOOL transitions_disabled = TRUE;
  DwmSetWindowAttribute(hwnd, DWMWA_TRANSITIONS_FORCEDISABLED,
                        &transitions_disabled, sizeof(transitions_disabled));

  // The window is revealed here; Paint() fills it with the theme background
  // with GDI in the same message turn (before DWM's next composition), so the
  // first visible frame is already themed.
  const bool start_maximized =
      g_start_maximized ||
      (g_settings.remember_window && g_settings.window_saved &&
       g_settings.window_maximized);
  if (start_maximized) {
    g_window_state.SetMaximized(true);
    ShowWindow(hwnd, SW_SHOWMAXIMIZED);
  } else {
    ShowWindow(hwnd, show_command == 0 ? SW_SHOWNORMAL : show_command);
  }
  ReportStartupPhase("first_frame");
  UpdateWindow(hwnd);
  ReportStartupPhase("gdi_frame");

  // The window can now be activated, so hand any request that arrived while it
  // was being created to the normal open/activate path (規劃/22/15 §5.3).
  if (g_instance_receiver) {
    g_instance_receiver->SetHandler(
        [](const std::wstring& path) { HandleInstanceRequest(path); });
  }

  // --move-window: apply a deterministic rectangle and then send the
  // WM_EXITSIZEMOVE the system sends when the user finishes a drag, so the
  // "save on resize" path is testable without a real mouse (規劃/22/15 §3.1).
  if (g_test_move_window_valid) {
    SetWindowPos(hwnd, nullptr, g_test_move_window_rect.left,
                 g_test_move_window_rect.top,
                 g_test_move_window_rect.right - g_test_move_window_rect.left,
                 g_test_move_window_rect.bottom - g_test_move_window_rect.top,
                 SWP_NOZORDER | SWP_NOACTIVATE);
    SendMessageW(hwnd, WM_EXITSIZEMOVE, 0, 0);
    nv::platform::LogInfo("test: window moved");
  }

  auto bundle = device_future.get();
  ReportStartupPhase("device_ready");
  if (!g_state->renderer.Initialize(hwnd, std::move(bundle))) {
    // The renderer that draws every custom card is the one that failed, so the
    // failure is shown with GDI and then the process exits normally.
    nv::platform::LogError("renderer: D3D11/D2D initialization failed");
    const int code = RunFatalErrorWindow(
        hwnd,
        L"Failed to initialize the graphics device (D3D11/D2D).\n\n"
        L"Update the display driver or run ZoreView on a machine with "
        L"Direct3D 11 support.");
    CloseInfoWindowForAppClose();
    DestroyWindow(hwnd);
    g_state.reset();
    return code != 0 ? code : 1;
  }
  ReportStartupPhase("renderer");

  // --rotation: seed the view before the overlay is installed. A later
  // SetImage(reset_view=true) keeps the rotation, so the first image opens at
  // the requested angle.
  if (g_initial_rotation_steps != 0) {
    g_state->renderer.SetRotation(g_initial_rotation_steps);
    nv::platform::LogInfo("test: rotation=" +
                          std::to_string(nv::render::RotationDegrees(
                              g_state->renderer.rotation())));
  }

  // Icons need the D2D factory for their path geometry; the renderer owns the
  // one created with the device bundle.
  if (g_ui != nullptr) g_ui->context().SetD2DFactory(g_state->renderer.d2d_factory());

  // Overlay layers are drawn by the renderer in a fixed order; the rail lives in
  // the bottom-panel layer and every other panel is owned by UiRoot. The
  // callback must be installed after Initialize(), which resets renderer state.
  g_state->renderer.SetOverlay(
      [](ID2D1DeviceContext* context, const D2D1_RECT_F& /*content*/,
         bool dark, bool high_contrast, nv::render::OverlayLayer layer) {
        if (context == nullptr || g_ui == nullptr) return;
        if (layer == nv::render::OverlayLayer::kBottomPanels) {
          const nv::ui::Rect strip = StripRect();
          const float opacity = EffectiveStripOpacity();
          if (!strip.empty() && g_state != nullptr && StripShown() &&
              opacity > 0.01f) {
            const D2D1_RECT_F bounds =
                D2D1::RectF(strip.left, strip.top, strip.right, strip.bottom);
            g_state->strip.Render(context, bounds, dark, high_contrast, opacity);
          }
          // v3.0 (規劃/26 §2.4): the bird's eye is above the rail but below the
          // rail tooltip, the bottom bar and the prompt layer. The strip is
          // drawn here first, then this panel, then UiRoot's bottom panels.
          if (g_state != nullptr) {
            const nv::ui::MinimapLayout& minimap = g_ui->layout().minimap;
            if (minimap.visible) {
              nv::ui::DrawMinimap(context, g_ui->context(), minimap,
                                  g_state->renderer.minimap_photo(),
                                  g_state->renderer.rotation(),
                                  g_ui->colors());
            }
          }
        }
        g_ui->Draw(context, layer);
      });

  // The strip was laid out against a zero-width client before the renderer
  // existed; recompute it with the real view width, then repaint through D3D.
  // A preview produced before the device existed is applied first so it is
  // visible in the very first D2D frame (規劃/22/13 §3.2).
  ApplyPendingPreview(hwnd);
  if (!open_path.empty()) UpdateFolderUi(hwnd, open_path);
  ApplyDpiScale(hwnd);
  ApplyWindowFrame(hwnd);
  if (g_start_fullscreen) EnterFullscreen(hwnd);
  InvalidateRect(hwnd, nullptr, TRUE);
  UpdateWindow(hwnd);
  ReportStartupPhase("shown");

  // --- Deterministic error/loading hooks (規劃/22/11 §10) -------------------
  // Applied after the first real frame so the cards are laid out against the
  // final client size and the renderer can draw them.
  if (g_force_error) {
    g_forced_error_path = open_path;
    g_force_error_sticky = true;
    nv::UiError forced =
        nv::MakeUiError(g_force_error_kind, g_forced_error_path,
                        L"forced error (test hook)");
    forced.code = 0x1u;
    ShowError(hwnd, forced);
  }
  for (const std::wstring& text : g_force_toasts) {
    ShowToast(hwnd, text);
  }
  if (g_test_force_confirm) SetTimer(hwnd, kConfirmTimer, 700, nullptr);
  if (g_force_loading) {
    // Simulate a slow decode: the indicator appears after the same 400 ms delay
    // as a real load and (optionally) disappears after --force-loading <ms>.
    // A requested duration shorter than the delay models a fast decode and
    // must not show the indicator at all (規劃/22/11 §4.4).
    if (g_suppress_loading) {
      nv::platform::LogInfo("load: done ms=" +
                            std::to_string(g_force_loading_ms) + " shown=0");
    } else {
      g_pending_path = open_path.empty()
                           ? std::filesystem::path(L"forced-loading")
                           : open_path;
      BeginLoading(hwnd, false);
      if (g_force_loading_ms > 0) {
        SetTimer(hwnd, kLoadingEndTimer,
                 static_cast<UINT>(g_force_loading_ms), nullptr);
      }
    }
  }

  if (!g_test_save_as.empty() || !g_test_save_to.empty()) {
    // Wait for the first image and for the queued rotation keys: a fresh decode
    // of a small sample is well under the base delay, and one chord costs
    // 350 ms.
    UINT delay = 1800;
    if (!g_test_keys.empty()) {
      delay += static_cast<UINT>(g_test_keys_interval_ms * g_test_keys.size()) + 600;
    }
    SetTimer(hwnd, kSaveAsTimer, delay, nullptr);
  }
  if (!g_test_save_options.empty()) {
    // Open after the first image is on screen; the estimate then needs 250 ms
    // of debounce plus one encode before a dump can assert its value.
    SetTimer(hwnd, kSaveOptionsTimer, 1200, nullptr);
  }
  if (g_test_open_settings || !g_test_settings_actions.empty() ||
      !g_test_settings_dump.empty() || !g_test_settings_screenshot.empty()) {
    // Open after the first image frame so the main window is live behind the
    // dialog; the step timer then spaces every activation.
    SetTimer(hwnd, kSettingsTimer, 900, nullptr);
  }
  if (g_test_click_index >= 0 || g_test_scroll_notches != 0 ||
      !g_test_rename_to.empty() || g_test_delete_current ||
      g_test_open_rename_dialog || !g_test_ole_drop.empty() ||
      g_test_drag_current || g_test_drag_strip_index >= 0 ||
      g_test_context_menu) {
    SetTimer(hwnd, kTestActionTimer, 1200, nullptr);
  }
  if (!g_test_click_hits.empty()) {
    // The first click waits for the window and the first image; the rest are
    // spaced so each one can repaint before the next lands. When keys are also
    // queued the clicks must wait for them, otherwise a click on a panel the
    // keys open would land before the panel exists.
    UINT delay = 1200;
    // v3.0 (規劃/26 §2.1): the info window opens at ~900 ms; its scroll hook and
    // clicks must run after it exists.
    if (g_test_open_info_window || g_settings.info_window_visible) {
      delay = std::max<UINT>(delay, 2200);
    }
    if (!g_test_keys.empty()) {
      delay += 900 + static_cast<UINT>(g_test_keys_interval_ms * g_test_keys.size());
    }
    // The save-options card opens at 1200 ms; clicking it must come after.
    if (!g_test_save_options.empty()) delay = std::max<UINT>(delay, 1700);
    // V3.1-06 (§06e): the rename overlay opens on the 1200 ms test-action tick;
    // a click on its buttons must land after that.
    if (g_test_open_rename_dialog) delay = std::max<UINT>(delay, 2400);
    SetTimer(hwnd, kClickHitTimer, delay, nullptr);
  }
  if (!g_test_keys.empty()) {
    // Keys start after the first image is on screen.
    g_keys_wait_image = !open_path.empty();
    // Only take over the dump when no other hook needs its own delay; with a
    // combined run the normal fixed schedule below still applies. An explicit
    // --dump-at always wins: the two-stage checks need a snapshot taken after
    // the full decode replaced the preview, which is much later than the keys.
    g_keys_owns_dump = !g_layout_dump_path.empty() &&
                       g_test_dump_at_ms == 0 && g_test_click_hits.empty() &&
                       g_test_drops.empty() && g_test_ole_drop.empty() &&
                       g_test_save_options.empty();
    SetTimer(hwnd, kSendKeysTimer, static_cast<UINT>(g_test_keys_start_ms),
             nullptr);
  }
  // Hover hooks own the dump: it is scheduled from the hook once the synthetic
  // move (or the whole reveal/hide cycle) has actually run, so a slow ASan
  // start cannot beat it.
  if (!g_layout_dump_path.empty() &&
      (g_test_hover_thumb >= 0 || g_test_hover_hotzone || g_test_hover_cycle ||
       g_test_hover_bottom || g_test_drag_to_bottom || g_test_hover_fs) &&
      g_test_keys.empty() && g_test_click_hits.empty() &&
      g_test_drops.empty() && g_test_ole_drop.empty() &&
      g_test_save_options.empty()) {
    g_hover_owns_dump = true;
  }
  if (!g_test_drops.empty()) {
    // Drops start after the first frame so the card exists when they land.
    SetTimer(hwnd, kTestDropTimer, 600, nullptr);
  }
  if (g_test_hover_thumb >= 0 || g_test_hover_hotzone || g_test_hover_cycle ||
      g_test_hover_bottom || g_test_drag_to_bottom || g_test_hover_fs) {
    // Hover starts after the first image; the tooltip needs a further 500 ms,
    // so a layout dump scheduled below waits for it.
    SetTimer(hwnd, kHoverTestTimer, 1200, nullptr);
  }
  if (!g_screenshot_path.empty()) {
    SetTimer(hwnd, kScreenshotTimer,
             static_cast<UINT>(g_screenshot_delay_ms), nullptr);
  }
  if (g_test_fullscreen) {
    SetTimer(hwnd, kFullscreenTestTimer, 700, nullptr);
  }
  // v3.0 (規劃/26 §2.1): the info window opens after the first image; the
  // screenshot/key hooks wait for it. A previous session's "open" state uses the
  // same timer so it is restored only once a file is on screen.
  if (g_test_open_info_window || g_settings.info_window_visible) {
    g_info_restore_pending = g_settings.info_window_visible;
    // Early enough that --click-hit/--info-key land after the window exists, and
    // retried every 200 ms until the first image is up.
    SetTimer(hwnd, kInfoWindowTimer, 900, nullptr);
  }
  if (!g_test_info_screenshot.empty()) {
    SetTimer(hwnd, kInfoScreenshotTimer, 2600, nullptr);
  }
  if (g_test_info_key != 0) {
    SetTimer(hwnd, kInfoKeyTimer, 2000, nullptr);
  }
  if (g_test_info_scroll != 0) {
    // After the info window opens (~900 ms) and before --click-hit (2200 ms), so
    // a click on a below-the-fold row lands on a row that is actually visible.
    SetTimer(hwnd, kInfoScrollTimer, 1500, nullptr);
  }
  // Folder watching starts with the first frame and runs for the whole session
  // (22/10); the handler itself pauses while minimized/modal. V3-05 picks the
  // source here: a local disk is watched with ReadDirectoryChangesW, everything
  // else keeps the 2 s poll. SyncWatchBaseline re-selects it on navigation.
  UpdateWatchSource();
  if (g_test_dump_at_ms > 0) {
    SetTimer(hwnd, kDumpAtTimer, static_cast<UINT>(g_test_dump_at_ms), nullptr);
  }
  if (g_test_quit_at_ms > 0) {
    SetTimer(hwnd, kQuitTimer, static_cast<UINT>(g_test_quit_at_ms), nullptr);
  }
  if (g_test_minimize_at_ms > 0) {
    SetTimer(hwnd, kMinimizeTimer, static_cast<UINT>(g_test_minimize_at_ms),
             nullptr);
  }
  if (g_test_restore_at_ms > 0) {
    SetTimer(hwnd, kRestoreTimer, static_cast<UINT>(g_test_restore_at_ms),
             nullptr);
  }
  if (!g_layout_dump_path.empty() && g_test_dump_at_ms == 0 &&
      !g_keys_owns_dump && !g_hover_owns_dump) {
    // With --click-hit the dump must happen after the queued clicks, otherwise
    // it would record the state before the panel was opened.
    UINT delay = 900;
    if (!g_test_click_hits.empty()) {
      delay += 400 + static_cast<UINT>(160 * g_test_click_hits.size());
    }
    // Queued drops start at 600 ms and are 500 ms apart; wait for the last
    // decode before writing the dump.
    if (!g_test_drops.empty()) {
      delay += 600 + static_cast<UINT>(500 * g_test_drops.size()) + 400;
    }
    // The synthetic OLE drop runs at 1200 ms; give it time to decode.
    if (!g_test_ole_drop.empty()) delay += 700;
    // --send-keys starts at 1200 ms, 350 ms per chord; wait for the last one
    // plus one decode repaint.
    if (!g_test_keys.empty()) {
      delay += 1200 + static_cast<UINT>(g_test_keys_interval_ms * g_test_keys.size()) + 900;
    }
    if (g_test_hover_thumb >= 0 || g_test_hover_hotzone ||
        g_test_hover_bottom || g_test_drag_to_bottom || g_test_hover_fs) {
      delay = std::max<UINT>(delay, 2400);
    }
    // The held-drag case runs press -> pan -> bar on the hover timer and needs
    // the dump after the last move.
    if (g_test_drag_to_bottom) delay = std::max<UINT>(delay, 2800);
    // --simulate-drag-out / --simulate-strip-drag run on the 1200 ms test-action
    // timer; the dump must wait for them (規劃/22/19 §7).
    if (g_test_drag_current || g_test_drag_strip_index >= 0) {
      delay = std::max<UINT>(delay, 1800);
    }
    // The reveal+leave cycle needs the 700 ms phase gap plus the 400 ms grace
    // and the 160 ms fade before the dump records the hidden state.
    if (g_test_hover_cycle) delay = std::max<UINT>(delay, 4000);
    // The save-options hook opens at 1200 ms; the estimate needs its 250 ms
    // debounce plus one encode, so the dump waits past that.
    if (!g_test_save_options.empty()) delay = std::max<UINT>(delay, 2600);
    // v3.0 (規劃/26 §2.1): the info window opens at 1200 ms (hook or restored
    // "was open" state) and the scroll hook runs at 1800 ms; the dump must see
    // the final state.
    if (g_test_open_info_window || g_settings.info_window_visible ||
        !g_test_info_screenshot.empty() || g_test_info_key != 0 ||
        g_test_info_scroll != 0) {
      delay = std::max<UINT>(delay, 3200);
    }
    SetTimer(hwnd, kLayoutDumpTimer, delay, nullptr);
  }

  MSG message{};
  while (GetMessageW(&message, nullptr, 0, 0) > 0) {
    TranslateMessage(&message);
    DispatchMessageW(&message);
  }

  nv::platform::LogInfo("ZoreView shutting down");
  if (g_state->thumbs) g_state->thumbs->Shutdown();
  if (g_state->decode) g_state->decode->Shutdown();
  if (g_state->cache_writer) g_state->cache_writer->Shutdown();
  g_state->strip.ReleaseDeviceResources();
  g_state->renderer.Shutdown();
  g_state.reset();
  // DWrite factory and cached text formats; released after the last frame.
  if (g_ui != nullptr) {
    g_ui->Shutdown();
    g_ui.reset();
  }
  if (g_background_brush != nullptr) {
    DeleteObject(g_background_brush);
    g_background_brush = nullptr;
  }
  if (ole_initialized) OleUninitialize();
  // The receiver is a separate message-only window and outlives the main
  // window, so release it here; the mutex is closed by the lock destructor.
  if (g_instance_receiver) {
    g_instance_receiver->Destroy();
    g_instance_receiver.reset();
  }
  g_instance_lock.reset();
  nv::platform::LogShutdown();
  return static_cast<int>(message.wParam);
}
