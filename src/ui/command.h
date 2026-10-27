#pragma once

#include <cstddef>

namespace nv::ui {

// Keyboard-driven commands from 規劃/22/09 §3/§4. The table below is the one
// place that maps a key chord to a command; the app turns the command into an
// action through a single Dispatch(), and the same Query() decides whether a
// button, menu row or shortcut is available.
//
// File commands (open/save/copy/delete/rename) keep their existing handlers:
// they predate this layer and have side effects (dialogs, shell operations)
// that are not part of the navigation/zoom scope of 22/09.
enum class CommandId {
  kNone = 0,

  // Navigation. kFirst/kLast are new in 22/09; PgUp/PgDn are synonyms of
  // kPrev/kNext (規劃/22/09 §4).
  kPrev,
  kNext,
  kFirst,
  kLast,

  // Zoom. The percent menu (22/02) uses the same renderer entry points.
  kZoomIn,
  kZoomOut,
  kFit,
  kActual,

  // View rotation (規劃/22/05): display-only quarter turns.
  kRotateLeft,
  kRotateRight,

  // View / animation.
  kToggleFullscreen,
  kToggleInfo,
  kToggleStrip,
  kToggleAnimation,
  kFramePrev,
  kFrameNext,

  kCount,
};

const char* CommandName(CommandId id);

// The state Query needs. Everything is derived by the app from the renderer,
// the folder navigator and the overlay stack; the UI module stays free of
// application state.
struct CommandContext {
  bool has_image = false;
  size_t file_count = 0;
  bool has_multiple_frames = false;
  bool animation_playing = false;
  bool fullscreen = false;
  bool info_open = false;
  bool strip_visible = false;
  bool fit = false;
  // Folder wrap can leave the folder at an edge, so a single-image folder is
  // still navigable under "next"/"ask" (規劃/22/10 §4.1).
  bool wrap_crosses_folders = false;
};

struct CommandState {
  bool enabled = false;
  bool checked = false;
};

// False for every command that needs a picture when there is none; navigation
// additionally needs at least two files (單張圖 Home/End 是 no-op, §5).
CommandState QueryCommand(CommandId id, const CommandContext& context);

// Maps one WM_KEYDOWN to a command, or kNone when the chord is not in the
// 22/09 table. `ctrl/shift/alt` come from the live keyboard state (or the
// --send-keys test hook). The '+/-' keys accept both the main row and the
// numeric keypad (規劃/22/09 坑 2).
CommandId CommandForKey(unsigned virtual_key, bool ctrl, bool shift, bool alt);

}  // namespace nv::ui
