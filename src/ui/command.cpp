#include "ui/command.h"

#include <windows.h>

namespace nv::ui {

const char* CommandName(CommandId id) {
  switch (id) {
    case CommandId::kNone:
      return "none";
    case CommandId::kPrev:
      return "prev";
    case CommandId::kNext:
      return "next";
    case CommandId::kFirst:
      return "first";
    case CommandId::kLast:
      return "last";
    case CommandId::kZoomIn:
      return "zoom.in";
    case CommandId::kZoomOut:
      return "zoom.out";
    case CommandId::kFit:
      return "zoom.fit";
    case CommandId::kActual:
      return "zoom.actual";
    case CommandId::kRotateLeft:
      return "rotate.left";
    case CommandId::kRotateRight:
      return "rotate.right";
    case CommandId::kToggleFullscreen:
      return "view.fullscreen";
    case CommandId::kToggleInfo:
      return "view.info";
    case CommandId::kToggleStrip:
      return "view.strip";
    case CommandId::kToggleAnimation:
      return "animation.toggle";
    case CommandId::kFramePrev:
      return "animation.prev";
    case CommandId::kFrameNext:
      return "animation.next";
    case CommandId::kCount:
      break;
  }
  return "none";
}

CommandState QueryCommand(CommandId id, const CommandContext& context) {
  CommandState state;
  switch (id) {
    case CommandId::kPrev:
    case CommandId::kNext:
    case CommandId::kFirst:
    case CommandId::kLast:
      // 單張圖：箭頭隱藏、Home/End no-op。導航也要求真的有一張圖上屏。
      // "next"/"ask" may leave a one-image folder for a sibling folder.
      state.enabled = context.has_image &&
                      (context.file_count > 1 || context.wrap_crosses_folders);
      break;

    case CommandId::kZoomIn:
    case CommandId::kZoomOut:
    case CommandId::kFit:
    case CommandId::kActual:
      state.enabled = context.has_image;
      state.checked = id == CommandId::kFit && context.fit;
      break;

    case CommandId::kRotateLeft:
    case CommandId::kRotateRight:
      // View rotation works for any picture, including a single-file folder.
      state.enabled = context.has_image;
      break;

    case CommandId::kToggleFullscreen:
      // Fullscreen has no image dependency (the empty card is usable in it).
      state.enabled = true;
      state.checked = context.fullscreen;
      break;

    case CommandId::kToggleInfo:
      state.enabled = context.has_image;
      state.checked = context.info_open;
      break;

    case CommandId::kToggleStrip:
      // Sorting/strip toggles stay usable without a picture (22/02 §3.2).
      state.enabled = true;
      state.checked = context.strip_visible;
      break;

    case CommandId::kToggleAnimation:
      state.enabled = context.has_multiple_frames;
      state.checked = context.animation_playing;
      break;

    case CommandId::kFramePrev:
    case CommandId::kFrameNext:
      state.enabled = context.has_multiple_frames;
      break;

    case CommandId::kNone:
    case CommandId::kCount:
      break;
  }
  return state;
}

CommandId CommandForKey(unsigned virtual_key, bool ctrl, bool shift, bool alt) {
  // F11 is the window-level fullscreen key and works regardless of modifiers
  // the user happens to hold (規劃/22/09 §4).
  if (virtual_key == VK_F11) return CommandId::kToggleFullscreen;

  // Alt+Enter = info panel. Windows delivers Alt combinations as
  // WM_SYSKEYDOWN, which the window procedure forwards to this same table.
  if (alt && !ctrl && virtual_key == VK_RETURN) return CommandId::kToggleInfo;

  if (ctrl && !alt) {
    if (virtual_key == '0') return CommandId::kFit;
    if (virtual_key == '1') return CommandId::kActual;
    if (virtual_key == 'T' && !shift) return CommandId::kToggleStrip;
    // View rotation (規劃/22/05 §3.5). Shifted variants stay unbound so a
    // future Ctrl+Shift shortcut never collides.
    if (virtual_key == 'L' && !shift) return CommandId::kRotateLeft;
    if (virtual_key == 'R' && !shift) return CommandId::kRotateRight;
    return CommandId::kNone;
  }
  if (alt) return CommandId::kNone;

  switch (virtual_key) {
    case VK_LEFT:
    case VK_PRIOR:  // PgUp
      return CommandId::kPrev;
    case VK_RIGHT:
    case VK_NEXT:  // PgDn
      return CommandId::kNext;
    case VK_HOME:
      return CommandId::kFirst;
    case VK_END:
      return CommandId::kLast;

    // Both the main-row key (with '=' and '+') and the numeric keypad.
    case VK_OEM_PLUS:
    case VK_ADD:
      return CommandId::kZoomIn;
    case VK_OEM_MINUS:
    case VK_SUBTRACT:
      return CommandId::kZoomOut;

    case '0':
      return CommandId::kFit;
    case '1':
      return CommandId::kActual;

    // 'I' only when it is not a Ctrl/Alt chord; plain T/C stay with the
    // theme/checkerboard toggles (22/09 §4 and 坑 1).
    case 'I':
      return shift ? CommandId::kNone : CommandId::kToggleInfo;

    case VK_SPACE:
      // 動畫時是暫停/繼續，否則在 Dispatch 裡退化成「下一張」(§4, 坑 14).
      return CommandId::kToggleAnimation;

    case VK_OEM_COMMA:
      return CommandId::kFramePrev;
    case VK_OEM_PERIOD:
      return CommandId::kFrameNext;

    case VK_RETURN:
      // Empty-state Enter is handled by the app before this table; with a
      // picture Enter toggles fullscreen (§4).
      return CommandId::kToggleFullscreen;

    default:
      return CommandId::kNone;
  }
}

}  // namespace nv::ui
