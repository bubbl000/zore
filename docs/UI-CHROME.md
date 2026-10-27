# UI chrome (v2.0 S1)

> `src/ui/` chrome behaviour that is more than geometry: the commands behind the
> title-bar segments and the bottom bar, the popup menus, the info panel and the
> toast feedback.
> Planning: `22/02` (title bar + bottom bar), `22/20` (overlay stack),
> `22/11` (error / state feedback), `22/09` (zoom commands), `22/05` (rotation).

---

## 1. What is live in S1

| Area | State |
| --- | --- |
| Title bar, 5 buttons | done (S0) |
| Title bar `☰` → settings | **done (v3.0)**: opens the settings window directly; right-click keeps the native menu and 更多▾ keeps the view switches (`規劃/26 §2.8`) |
| Fullscreen reveal | **done (v3.0)**: exit/close float in the top-right (160 ms fade, 3 s idle); the rail is reveal-only there (`規劃/26 §2.7`) |
| Title bar, 3 information segments | done (S0); v3.0 trimmed it to 3 (`規劃/26 §2.2`) |
| Title segment click → copy | **done (S1)**; the zoom menu is bottom-bar only since v3.0 |
| Bottom bar overview (ⓘ / percent / index) | **done (S1)**: ⓘ and percent are commands, index is display-only |
| Bottom bar view (fit / 1:1) | done (S0) |
| Bottom bar rotate `↺` / `↻` | **done (S1 05)**: 90° view rotation (the accent dot was removed in v3.0, `規劃/26 §2.3`) |
| 更多▾ flyout | **done (S1)**: 2 switches only (v3.0 removed always-on-top, reset rotation and the sort group — `規劃/26 §2.5`) |
| Zoom menu (percent button) | **done (S1)** |
| Info panel | **done (S2 06)**: 4 groups / 21 rows, EXIF worker, scroll, copy — see `docs/INFO-PANEL.md` |
| Bird's eye view (minimap) | **done (v3.0)**: overlay above the rail, frame drag pans without zooming — see `docs/MINIMAP.md` (`規劃/26 §2.4`) |
| Toast | **done (S1)** |
| Startup empty state, format line, all-formats card, OLE drop | **done (S1 03)** — see `docs/EMPTY-STATE.md` |

`22/05` lights up the rotate pair: `rotate_available = true`, the enabled state
still follows `has_image`. v3.0 dropped the accent dot that used to mark a
non-zero angle (`規劃/26 §2.3`) — the rotated picture already says so, and two
4 DIP dots under the icons read as stray pixels. The glyphs themselves are now
centred in their 16x16 box (a single large D2D arc had picked the other circle
centre and painted them ~4.75 DIP up and outwards). See `docs/UI-VIEW.md` for the
transform and the export rules.

## 2. Title bar segments

```text
[icon] file name · 1600 × 1000 · 2.4 MB                    ☰  ⛶  —  ▢  ✕
```

Priority when the window narrows (hidden from the right): file size, dimension.
The file name is never dropped; it is ellipsized (with 3 px of slack so DWrite
trimming does not eat the last characters).

| Segment | Left click | Copies / opens |
| --- | --- | --- |
| file name | copy full path | `E:\...\photo.jpg` |
| dimension | copy `W × H` | `1600 × 1000` |
| file size | copy formatted size | `2.4 MB` |
| empty area, double click, right click | unchanged native behaviour | — |

The index (`14/17`) and the zoom percentage used to be segments too; v3.0 moved
both to the bottom bar only (`規劃/26 §2.2`), so the caption keeps the file
identity and nothing that changes while zooming or navigating.

The leftmost `☰` is a button, not a segment: it opens the settings window
directly (v3.0, `規劃/26 §2.8`). Right-clicking the image still opens the native
context menu, and 更多▾ in the bottom bar stays the view-switch entry — three
entries, three jobs.

**How a click coexists with dragging.** The title bar stays `HTCAPTION`, so
dragging, double-click maximize/restore, the system menu and `Alt+Space` are all
still the operating system's. On a caption press Windows runs its own move loop
and, for a click that never moves, sends neither `WM_NCLBUTTONUP` nor
`WM_EXITSIZEMOVE` (verified on this host). The click is therefore confirmed in
three steps:

1. `WM_NCLBUTTONDOWN` records the client position and the window rectangle and
   starts a 30 ms poll (`kTitleClickTimer`).
2. While the left button is still down the poll returns; at release it waits
   `GetDoubleClickTime()` so the second half of a double-click cannot copy
   anything (`WM_NCLBUTTONDBLCLK` cancels outright).
3. The action runs only if the window rectangle is unchanged **and** the cursor
   stayed within `SM_CXDRAG` of the press point.

A drag that returns to its exact start position is therefore still a drag, and
window movement from any other source (snap, maximize) also cancels the click.

## 3. Bottom bar parts

44 DIP tall, left cluster `ⓘ · percent · index`, right cluster
`fit · 1:1 · ↺ · ↻ · more▾`.

| Part | DIP | Command | Disabled when |
| --- | --- | --- | --- |
| `ⓘ` | 28 | toggle the info panel | no image |
| percent | 56 | open the zoom menu | no image |
| index | 72 | none (display only) | always (not hit-testable) |
| fit | 64 | `Renderer::FitToWindow()` | no image |
| 1:1 | 56 | `Renderer::ActualSize()` | no image |
| `↺` | 40 | rotate left 90° (`CommandId::kRotateLeft`, `Ctrl+L`) | no image |
| `↻` | 40 | rotate right 90° (`CommandId::kRotateRight`, `Ctrl+R`) | no image |
| 更多▾ | 88 | toggle the more panel | **never** (the switches work without a picture) |

Hover paints `bg.hover`, press `bg.pressed`; the disabled pair uses
`text.muted`. All four view commands log `cmd: id=...` so scripts can assert
them without reading pixels.

## 4. Popup menus

One widget for both popups (`ui/menu.h`, `ui/menu.cpp`), one pure layout
function shared by drawing and hit-testing:

| Property | Value |
| --- | --- |
| Width | 240 DIP (more), 168 DIP (zoom) |
| Row height | 32 DIP (separators and group captions included) |
| Padding | 8 DIP around the rows, 4 DIP at the row ends |
| Gap to the anchor | 4 DIP |
| Anchor | above the owning part, left-aligned; right-aligned when it would overflow the right edge |
| Fallback | flips below the anchor when there is no room above; clamped inside the client with an 8 DIP margin |
| Rows | command / toggle / radio / separator / group caption |

Interaction:

* The owning button toggles the popup.
* A click on a selectable row runs its command and closes the popup.
* A click anywhere else (including the owning button, the image or the strip)
  closes the popup without acting — the overlay stack consumes it.
* `Esc` closes the popup before anything else (`OverlayStack::Escape()`).
* Rows highlight on hover; disabled, separator and caption rows never highlight
  and never take the click.

### 4.1 More panel rows

| # | Row | Kind | Effect |
| --- | --- | --- | --- |
| 0 | 鸟瞰图 / Bird's eye view | toggle | `settings.birdseye`; shows the minimap overlay above the rail (`規劃/26 §2.4`, `docs/MINIMAP.md`). `M` is the keyboard shortcut |
| 1 | 缩略图 / Thumbnails | toggle | `settings.thumbstrip_mode` = `pinned` ↔ `auto` (checked = always visible). `off` is settings-page only (`22/08`) |

v3.0 shrank the panel from ten rows to these two (`規劃/26 §2.5`): **always on
top** keeps its settings row (`topmost`), **reset rotation** has no dedicated
entry at all (the view rotation is a 4-step turn), and the whole **sort group**
is gone with `SortMode` itself — the folder has one order, Explorer's natural
name order (`core/folder_navigator.cpp`). No separator is left because two
switches have nothing to separate.

`帮助 / 关于` (`22/21`) is **not** in the panel yet: a dead row that can never be
enabled would be worse than a later addition.

### 4.2 Zoom menu rows

`适应窗口` (fit, checked while fitted), `50%`, `100%`, `200%`, `400%`. Choosing a
step leaves fit mode and keeps the view centre (`Renderer::SetZoom`), so the
image does not jump to a corner.

## 5. Info panel

Right-bottom overlay, 360 DIP wide, height **derived from the line count**
(`kInfoHeaderHeight + kInfoFooterHeight + 2 × kInfoPadding + lines ×
kInfoRowHeight`), capped at 70 % of the content height. See `docs/INFO-PANEL.md`
for the full model (groups, EXIF fields, scrolling, copy).

## 6. Toasts

`UiRoot::overlay().PushToast()` + a 2.6 s timer (`kToastTimer`), at most three
visible, newest at the bottom of the bottom stack. Every toast is logged as
`ui: toast=...` so scripts can assert the feedback without OCR.

Used by: copying a title segment, toggling a switch, changing the sort order.

## 7. State and persistence

`platform/settings.h` gained five fields, read at startup (a missing key keeps
the built-in default, so upgrades behave like a fresh install). The more panel
writes the first four; `zoom_step` is consumed by the keyboard zoom now and will
get its picker in 22/07:

| Key (`[ui]`) | Values | Default |
| --- | --- | --- |
| `thumbstrip_mode` | `pinned` / `auto` / `off` | `auto` (`22/08`: fade in on hover) |
| `birdseye` | `0` / `1` | `0` |
| `sort_mode` | `name` / `date` / `size` | `name` — **no consumer since v3.0**: kept on disk so a downgrade or a future version still finds the user's value |
| `topmost` | `0` / `1` | `0` |
| `keep_rotation` | `0` / `1` | `1` (`22/05`: keep the view rotation on the next image) |
| `zoom_step` | `1.05` / `1.10` / `1.15` / `1.25` | `1.15` |

`--strip=` / `--strip ` override `thumbstrip_mode` for **one session** and never
write the file (`g_strip_override`).

The folder order is fixed: `core/folder_navigator.cpp` sorts every listing with
`StrCmpLogicalW` (digits by value, case-insensitive) plus an ordinal tie-break,
which is the order Explorer shows. Switching files therefore never re-orders
anything; the open image is located by path in `BuildFolderList()` and by
`RescanFolder()` after a watch event (規劃/22/02 §7, v3.0 規劃/26 §2.5).

## 8. Test hooks

| Hook | Purpose |
| --- | --- |
| `--click-hit <name>` | synthesized click on a chrome target; repeatable, 1.2 s for the first then 160 ms apart |
| `--simulate-drop <path>` | runs the drop dispatcher once (repeatable, 600 ms first then 500 ms apart); see `docs/EMPTY-STATE.md` |
| `--simulate-ole-drop <path>` | drives the real `IDropTarget` with a shell `CF_HDROP` data object |
| `--simulate-drag-over` | enters the OLE hover state without a drag |
| `--print-settings [file]` | writes the persisted settings (`key=value`), stdout when no file |
| `--send-keys <seq>` | posts real key chords (`Right`, `Ctrl+T`, `Ctrl+R`, `Alt+Enter`, `+`, …); see `docs/NAVIGATION.md` and `docs/UI-VIEW.md` |
| `--rotation <0\|90\|180\|270>` | deterministic initial view rotation (`docs/UI-VIEW.md`) |
| `--save-as <file>` | saves the current view after the queued keys, without the dialog (`docs/UI-VIEW.md`) |
| `--hover-thumb <index>` | posts a real `WM_MOUSEMOVE` at a rail slot after the first image; the tooltip path (`22/08`) runs unchanged. It also moves the real cursor so `TME_LEAVE` cannot fire a spurious `WM_MOUSELEAVE` |
| `--hover-hotzone` | posts a move into the bottom 8 DIP band (still valid — v3.0 widened the reveal area to the rail slot + bottom bar) |
| `--hover-bottom` | posts a move onto the bottom bar centre; the wide reveal area must fade the auto rail in (v3.0, `規劃/26 §2.10`) |
| `--drag-to-bottom` | presses inside the image, pans 40 DIP, then drags onto the bottom bar while still held; the rail must stay hidden (`dragging=1`, `strip_shown=0`) |
| `--minimap-drag <dx>,<dy>` | presses the bird's eye frame, moves by dx/dy and releases; queued with the click hits, so the dump waits for it (v3.0, `規劃/26 §2.4`) |
| `--fullscreen` | enters fullscreen 700 ms after the first frame (reveal screenshots / checks) |
| `--hover-fs` | enters fullscreen if needed, then posts a real move into the top-right reveal zone |
| `--hover-cycle` | reveal via the hot zone, then leave; asserts the 400 ms grace + 160 ms fade out in one run |
| `--layout-dump <file>` | existing dump, extended with `birdseye`, `minimap`, `minimap_panel=`, `minimap_canvas=`, `minimap_image=`, `minimap_viewport=`, `minimap_view_display=`, `minimap_scale`, `minimap_photo`, `minimap_baked`, `topmost`, `info_open`, `info_rows`, `popup`, `settings_open`, `dragging`, `fs_reveal`, `fs_reveal_opacity`, `fs_exit=`, `fs_close=`, `menu_rows`, `menu_above`, `folder_files`, `folder_index`, `folder_path`, `file_name`, `folder_wrap`, `watch_interval`, `confirm_kind`, `confirm_target`, `confirm_remember`, `confirm_remember_row=`, `confirm_remember_box=`, `zoom`, `fit`, `zoom_min`, `zoom_max`, `zoom_percent`, `arrows_visible`, `arrows_left=`, `arrows_right=`, `rotation`, `rotation_degrees`, `display_width`, `display_height`, `info_dimension`, `strip_shown`, `strip_opacity`, `strip_interactive`, `strip_hover`, `strip_tooltip`, `strip_tooltip_above`, `strip_tooltip_panel=`, `info=`, `menu=`, `menu_row_N=`, and the empty-state keys listed in `docs/EMPTY-STATE.md` (`sort_mode` was removed in v3.0 with the sort UI) |
| `--dump-at <ms>` | writes `--layout-dump` at the given delay **without exiting** (folder-watch checks mutate files while the app runs) |
| `--quit-at <ms>` | posts `WM_CLOSE` at the given delay |
| `--minimize-at <ms>` | minimizes the window at the given delay (asserts the watch pause) |
| `--watch-interval <ms>` | folder polling interval, 100–10000; default 2000 (test only) |

Click target names: `bottom.info`, `bottom.percent`, `bottom.index`,
`bottom.fit`, `bottom.actual`, `bottom.rotate_left`, `bottom.rotate_right`,
`bottom.more`, `title.menu`, `title.fullscreen`, `title.minimize`,
`title.maximize`, `title.close`, `title.file_name`, `title.dimension`,
`title.file_size`, `fullscreen.exit`, `fullscreen.close`, `menu.<row>`,
`arrows.left`, `arrows.right`, `minimap`, `minimap.blank`.

The target rectangle always comes from the cached layout, so the hook works at
any DPI without composing scaled mouse coordinates (HANDOFF pitfall 139). Title
segments call the same `HandleTitleSegmentClick()` the real click ends up in —
the caption gesture itself cannot be faked with `PostMessage`; the five title
buttons do take the synthesized down/up path (same as `bottom.more`).

`tests/run_ui_chrome_check.ps1` drives all of the above (85 checks: geometry,
persistence, restart, zoom steps, info panel, title copies, the hamburger →
settings entry, the centred rotate glyphs, natural folder order, 100–200 % DPI)
in a throw-away portable copy of the executable.

## 9. Log lines

```text
ui: strip_mode=... sort=... birdseye=0      startup state
ui: more_panel=open|closed                  more button
ui: percent_menu=open|closed                percentage button
ui: info=1|0                                info button
ui: toggle thumbstrip=pinned|auto           more panel switch (22/08)
ui: strip=show|hide|hidden                  auto rail reveal / grace / fade end
ui: strip_hover=3                           hovered rail slot (-1 = none)
ui: strip_tooltip=photo.jpg                 hover tooltip shown
ui: toggle birdseye=1                       more panel switch (or M)
ui: minimap drag=start|end|cancelled        bird's eye frame drag (v3.0)
ui: minimap center                          blank click re-centred the view
renderer: minimap 512x320 bake=0.9ms        derived mini photo (v3.0)
ui: fs_reveal=show|hide                     fullscreen reveal pair (v3.0)
ui: topmost=1                               more panel switch
ui: title_click=dimension                   title segment click
ui: toast=Dimensions copied                 toast shown
cmd: menu=zoom.100                          popup command
sort: mode=date files=3 index=2             folder re-sorted
```
