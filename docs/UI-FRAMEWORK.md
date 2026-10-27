# UI framework (v2.0 S0)

> `src/ui/` — tokens, layout, hit stack, DWrite text, vector icons, title and
> bottom chrome, plus the borderless window state machine.
> Planning: `規劃/22-v2.0-实现方案/01` (framework), `20` (stacks), `22` (lifecycle).

---

## 1. One frame, one layout

```text
WindowProc ─┬─ WM_SIZE / WM_DPICHANGED / theme change / decode / zoom
            │        └─ RefreshUi() ── UiRoot::Update() ── LayoutFrame
            ├─ WM_PAINT ─────────────► Renderer::Render()
            │                              └─ OverlayFn(layer) ── UiRoot::Draw()
            └─ mouse / keyboard ─────► UiRoot::HitTest() ── HitId
```

* `UiRoot::Update()` is the only place that computes rectangles. Drawing and
  hit-testing read the same cached `LayoutFrame` / `TitleLayout` / `BottomLayout`,
  so a control can never be painted somewhere it cannot be clicked.
* Every size is written in DIP in `ui/metrics.h` and multiplied by the window DPI
  scale inside `layout.cpp`. Text sizes stay in DIP: `draw::DrawText()` applies
  the scale itself (a pixel-sized font passed as DIP is the classic 1.25x bug).
* Colours come only from `ui/theme_tokens.h`. High contrast replaces every token
  with a system colour and never keeps the green accent.

## 2. Layers (renderer side)

`nv::render::OverlayLayer` is drawn by `Renderer::DrawFrame()` in this order:

| layer | content | owner |
| --- | --- | --- |
| `kImageArea` | navigation arrows, empty-state and error cards | `22/03`, `22/09` (S1) |
| `kBottomPanels` | thumbnail rail, bottom bar | app + `ui/chrome` |
| `kTopPanels` | more/percent popups, info panel, toasts, modals | `22/20` (S1/S2) |
| `kChrome` | title bar (always last) | `ui/chrome` |

`Renderer::SetTopInset()` reserves the title bar and `SetBottomInset()` the bottom
bar plus the pinned rail; `ContentRect()` is what fit/zoom/pan use.

## 3. Layout formulas (as implemented)

```text
title      = 32 DIP            (hidden in fullscreen)
bottom     = 44 DIP            (hidden in fullscreen)
strip      = 76 DIP            (pinned: subtracted from content; auto: overlay; off: hidden)
content    = client - title - bottom - (pinned ? strip : 0)
stack_top  = content.bottom
pill       = centred, height 36, bottom = stack_top - 12          (needs an image)
toast      = centred, height 36, bottom = stack_top - 12 - (pill ? 48 : 0)
info       = width 320, right - 16, bottom = stack_top - 12, max height 60% content
arrows     = 40x40, 16 DIP inside the content edges
```

Fullscreen (`F11` / `--fullscreen`): `title = bottom = strip = 0` and
`content = client`; the pill/toast keep 12/16 DIP from the client bottom.

## 4. Title bar (32 DIP)

Five information segments, left to right: **file name · dimensions · file size ·
index · percent**. When the window narrows, segments are dropped in reverse
priority (file size → dimension → index → percent); the file name is never
dropped, it is ellipsized. Below 640 DIP the buttons shrink from 46 to 40 DIP.

Five buttons, 46x32 DIP each, right-aligned: menu (`☰`), fullscreen, minimize,
maximize/restore, close (danger on hover). Hover uses `bg.hover`, pressed
`bg.pressed`; the close button uses `danger` for both.

## 5. Bottom bar (44 DIP, S0 skeleton)

Left: `ⓘ` (info, S2), percent, index. Right: `适应` (fit), `1:1`, rotate left,
rotate right, `更多▾`. Controls whose command lands in a later stage are drawn
disabled and are not hit-testable (`LayoutInputs::*_available`).

## 6. Hit stack

`UiRoot::HitTest()` checks, in order:

1. modal open → nothing (the modal owns the pointer)
2. popup open → nothing here (the popup handled it; clicking outside closes it)
3. title buttons
4. bottom bar parts (disabled parts are skipped)
5. thumbnail rail
6. image area

`WM_NCHITTEST` uses only `UiRoot::HitTestTitleButtons()` plus the resize border:
the maximize button returns `HTMAXBUTTON` (keeps Win11 snap layouts and lets the
system run the click), the other four return `HTCLIENT` so the app draws their
pressed state, and the rest of the title bar returns `HTCAPTION` (drag, double
click, system menu all stay native).

## 7. Borderless window

`WS_OVERLAPPEDWINDOW` is kept so snap, resize, `Alt+Space` and `Alt+F4` remain
native; only the non-client area is removed.

| message | handling |
| --- | --- |
| `WM_NCCALCSIZE` (TRUE) | client = window; when maximized the client is clamped to the monitor work area (the maximized window rectangle is inflated by the invisible frame) |
| `WM_GETMINMAXINFO` | min track size 640x480 DIP; maximized size/position = work area |
| `WM_NCHITTEST` | resize border 6 DIP (not while maximized), title buttons, caption |
| `WM_NCACTIVATE` / `WM_NCPAINT` | suppress system non-client drawing, repaint the custom title bar |
| `WM_NCMOUSEMOVE` | hover for the `HTMAXBUTTON` maximize button |
| `WM_ACTIVATE` | active/inactive title bar colours |
| `WM_SYSCOMMAND` | `SC_KEYMENU` goes to `DefWindowProc` (`Alt+Space`) |

DWM: `DWMWA_WINDOW_CORNER_PREFERENCE = ROUND` (Win11) and `DONOTROUND` while
maximized/fullscreen; the shadow comes from the retained thick frame.
`--classic-frame` keeps the system frame for A/B comparison.

Window states live in `ui/window_state.h` (Normal / Maximized / Fullscreen).
Fullscreen saves the style, the rectangle and the maximized flag, and restores
exactly what was there before. `Escape` and `F11` go through the same
`WindowStateMachine`.

## 8. Escape and overlay priority

`OverlayStack::Escape()` closes exactly one layer per press:
settings window → modal → popup → info panel → fullscreen. The settings window is
a separate top-level window and is only tracked as a flag. `BlocksImageCommands()`
(modal) disables every image command; `BlocksNavigation()` (modal or popup) also
disables arrows/keyboard navigation.

Toolbar commands never bypass this: `WM_KEYDOWN` asks the stack before running
anything except `Escape`/`F11`.

## 9. Test hooks (S0)

| hook | purpose |
| --- | --- |
| `--borderless` (default) / `--classic-frame` | A/B the custom frame |
| `--strip=pinned|auto|off` (or `--strip <mode>`) | rail mode; `auto` currently shows the rail without the fade (S2) |
| `--layout-dump <file>` | writes the cached layout, window/client sizes and state, then exits |
| `--maximized` / `--fullscreen` | start in that window state |
| `--screenshot`, `--shot-delay`, `--bench-startup`, `--force-dpi-scale`, `--force-high-contrast` | unchanged from M1 |

`tests/run_ui_shell_check.ps1` drives them. **The harness must call
`SetProcessDpiAwarenessContext(-4)` before using `SetCursorPos` or posted
coordinates**, otherwise Windows virtualises them by the DPI scale and every
synthetic click lands 1.25x off target.

## 10. Not in S0

* Empty state card, navigation arrows, error cards (`kImageArea` is empty).
* More/percent popups, info panel and settings window (`OverlayStack` already
  models them; nothing opens them yet).
* Rail fade for `auto` mode, tooltips, keyboard focus ring and Tab order.
* Any UI Automation provider for the drawn controls: only the window title is
  exposed (`EVENT_OBJECT_NAMECHANGE`), as planned in `規劃/22/20 §3.3`.
