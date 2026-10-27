# Navigation, zoom and image arrows (v2.0 S1 / 22-09)

Status: implemented 2026-10-24. This document is the reference for the keyboard
table, the image-area previous/next circles and the zoom range. The full product
key table is `規劃/22-v2.0-实现方案/09-导航与缩放命令.md`; this file records what
is actually shipped and how it is verified.

## 1. Command layer

`src/ui/command.*` is the single keyboard → command table:

```cpp
enum class CommandId { kPrev, kNext, kFirst, kLast, kZoomIn, kZoomOut,
                       kFit, kActual, kToggleFullscreen, kToggleInfo,
                       kToggleStrip, kToggleAnimation, kFramePrev,
                       kFrameNext, ... };

CommandId CommandForKey(vk, ctrl, shift, alt);   // pure, unit-tested
CommandState QueryCommand(id, context);          // enabled / checked
```

The app builds a `CommandContext` from the renderer, the folder navigator and
the overlay stack and calls `DispatchCommand()`; every key, the image arrows and
`--send-keys` go through it. `QueryCommand` is the only availability decision:
without a picture every image command is `enabled=false` and the dispatch logs
`cmd: blocked=<name>` instead of acting. File commands (`Ctrl+O/S/C`,
`Del`, `F2`, …) keep their existing handlers because they open dialogs or shell
operations outside this scope.

## 2. Key table (shipped subset)

| Key | Command | Notes |
| --- | --- | --- |
| `←` / `PgUp` | prev | folder-edge policy: loop (default), next image folder, or ask |
| `→` / `PgDn` | next | folder-edge policy: loop (default), next image folder, or ask |
| `Home` / `End` | first / last | no-op for a single image |
| `Space` | animation toggle, else next | on a still image it opens the next file |
| `,` / `.` | previous / next frame | pauses playback first; animated images only |
| `+` / `=` / numpad `+` | zoom in | step is `zoom_step` (default 1.15) |
| `-` / numpad `-` | zoom out | |
| `0` / `Ctrl+0` | fit to window | |
| `1` / `Ctrl+1` | 100% | |
| `Ctrl+R` / `Ctrl+L` | rotate the view right / left 90° | display-only; see `docs/UI-VIEW.md` |
| `Ctrl+T` | thumbnail strip on/off | same command as the more panel switch |
| `I` / `Alt+Enter` | info panel | |
| `F11` / `Enter` | fullscreen | Enter is skipped while the empty card is shown |
| `Esc` | overlay stack | one press closes one layer |

Not in the table on purpose: plain `T` (theme), plain `C` (checkerboard),
`Ctrl+O/S/C/Shift+C/Shift+W`, `Del`, `F2`, `F5` — they are handled by their
existing code paths and unit tests assert the table does not steal them.

## 3. Zoom range and semantics

`src/render/zoom_math.h` holds the math so it can be unit-tested without a D3D
device:

```text
fit_scale = min(content_w / image_w, content_h / image_h)
min_zoom  = min(0.10, fit_scale)      <- an 8K image may fit at ~8%
max_zoom  = 32.0                      <- 3200%
percent   = lround(zoom * 100)
```

- Keyboard zoom switches from fit to manual first (the current fit scale becomes
  the base) and anchors on the **content centre**, so the picture does not jump.
- Wheel zoom keeps the cursor anchor. The default wheel mode is zoom; `Ctrl`+
  wheel reverses the direction.
- The percentage menu (22/02) uses `SetZoom()`, which applies the same clamp and
  centre anchor. `ActualSize()` always means exactly `1.0`.
- Reaching a limit is a no-op; there is no dialog.

## 4. Image-area arrows

`LayoutFrame::arrows_left/right` are the only geometry (56 DIP circles, 16 DIP
from the content edges, vertically centred). They are laid out only when
`arrows_visible` is true, which the app derives as *has an image and the folder
has more than one file*. Consequences:

- single image / empty state: no rectangles, no hit targets;
- fullscreen: visible (content is the whole client);
- `opacity` is 35 % at rest and 100 % on hover (instant, no animation);
- hit order is after the empty card and before the strip/image, so clicking an
  arrow never starts a pan;
- clicking dispatches `kPrev` / `kNext`, not a second implementation.

## 5. Settings

`[ui] zoom_step` stores one of `1.05` / `1.10` / `1.15` / `1.25` (default
`1.15`). `LoadSettings()` accepts only those values; anything else keeps the
default. `--print-settings` prints `zoom_step=1.15`.

## 6. Test hooks and logs

| Hook | Purpose |
| --- | --- |
| `--send-keys <seq>` | comma-separated chords posted as real `WM_KEYDOWN`/`WM_KEYUP`, e.g. `"F11,Right,Right,Esc"`, `"Ctrl+T,Ctrl+T"`, `"Alt+Enter"`, `"+"`. Simulated modifiers stand in for `GetKeyState`. |
| `--click-hit arrows.left` / `arrows.right` | clicks the centre of the cached arrow rectangle (skipped when hidden) |
| `--layout-dump` | adds `arrows_visible`, `arrows_left=`, `arrows_right=`, `zoom_min`, `zoom_max`, `zoom_percent` |

`--send-keys` waits for the first image to be on screen before posting (up to
10 s); navigation would otherwise only log `blocked`. With `--layout-dump` and no
other hook, the dump is scheduled 800 ms after the last chord instead of using a
fixed delay, so it always reflects the final state.

Logs:

```text
test: key=Right                         --send-keys hook
cmd: nav=next|prev|first|last           navigation
cmd: zoom=in|out|fit|actual             zoom (no numeric value: read the dump)
cmd: view=fullscreen|info 1|strip       view toggles
cmd: animation=toggle|prev next         animation keys
cmd: blocked=<command>                  QueryCommand said disabled
cmd: id=arrows.left|arrows.right        arrow click
animation: frame=N/M                    frame stepping
```

## 7. Verification

```powershell
# unit: command table, arrows geometry/hit, zoom math (in zoreview_tests)
.\build\Release\zoreview_tests.exe

# 68 checks: geometry at the current DPI, all navigation keys, zoom steps,
# view keys, arrow clicks, single-image and no-image disabling
powershell -ExecutionPolicy Bypass -File tests\run_navigation_check.ps1
```

## 8. Pitfalls

1. `+` must accept `VK_OEM_PLUS`, `VK_ADD` and the `=` key; the shift state is
   ignored for this chord.
2. Do not add a fixed 10 % floor: `min_zoom = min(0.10, fit_scale)` exists
   because an 8K image fits at ~8 % (see `zoom_math.h`).
3. Percent uses `lround`; truncation shows 99 % for 100 %.
4. The arrows must be hit **before** `kImage`, otherwise the first pixel of a
   click starts a pan.
5. A single-file folder hides the arrows and disables prev/next/first/last under
   `loop`; under `next`/`ask` they stay enabled because the step can leave the
   folder (22/10 §4.1).
6. `--send-keys` must not post before the first decode completes; the hook polls
   `has_image` first. It also resets the simulated modifiers on the tick after
   the last chord.
7. Alt combinations arrive as `WM_SYSKEYDOWN`; the window procedure forwards
   only `Alt+Enter` and lets `Alt+F4`/`Alt+Space` reach `DefWindowProc`.
