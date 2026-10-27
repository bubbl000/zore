# Startup empty state, format line and drag & drop (v2.0 S1)

> Planning: `規劃/22/03` (implementation), `22/20` (overlay stack),
> `22/11` (error/state feedback). Code: `src/ui/empty_state.*`,
> `src/core/format_summary.*`, `src/platform/drag_drop.*`,
> `src/ui/layout.cpp` (`ComputeEmptyLayout`), `src/app/main.cpp`.

---

## 1. What is live

| Area | State |
| --- | --- |
| Empty card (icon, title, format line, primary button, footer) | **done** |
| Format line derived from build + system codecs | **done** |
| "All formats" modal card | **done** (read-only list, Esc/Close) |
| Drag hover highlight (accent wash, 2 px border, hint pill) | **done** (OLE) |
| Drop a file / a folder / an empty folder / an unsupported file | **done** |
| Folder picker (`IFileOpenDialog` + `FOS_PICKFOLDERS`) | **done** |
| Keyboard: Tab / Shift+Tab focus, Enter activates | **done** |
| GDI first frame with a simplified card | **done** |
| Error card for a failed decode | `22/11` (not in this stage) |
| UIA names for the three buttons | not in v2.0 (`22/03` §5) |

The empty card is visible exactly when no image owns the content area **and** no
decode is in flight. A load therefore never flashes the card behind the image
(`g_pending_path` in `main.cpp`).

## 2. Four states

| State | When | Card |
| --- | --- | --- |
| `empty` | nothing opened yet | title "Drop an image here" |
| `empty` + drag | an OLE drag hovers the window | same card + accent wash/border/hint |

> **V3.1-10（2026-10-27）**：拖入时的高亮（accent wash）改成**画在卡片之上**
> （`ui_root.cpp` 的 `kImageArea` 里先 `DrawEmptyCard` 再 `DrawDragWash`），
> 因为之前它压在卡片下面，用户拖图片进来时看到的是"卡片没被点亮"。
> 代价是 8% 的 accent 会轻微染到卡片和按钮上——这正是"高亮在最上层"的意思。
> 文案同时从"拖放图片或文件夹到此处"改成 **"拖放图片到此处"**
> （`Drop an image here`）；拖文件夹仍然有效，只是不再在提示里宣传。
| `folder` | a folder was opened but has no supported image | title "No supported images in this folder" + hint |
| `unsupported` | the last drop was not a decodable image | normal title + transient red note (3 s) |

An unsupported drop while an image is open does **not** clear the picture; it
shows a toast instead (`ui: toast=...`).

## 3. Layout

`ComputeEmptyLayout(content, dpi, {kind, format_lines, has_note})` in
`ui/layout.cpp` returns every rectangle; drawing and hit-testing read that one
frame (same rule as the rest of the chrome).

```text
content rectangle (title bar, bottom bar and a pinned rail already removed)
┌──────────────────────────────────── card (max 520 DIP, centred) ────────────┐
│ (28 padding)                                                                 │
│                      [ 64 x 64 image icon ]                                  │
│                      Drop an image here                   20 semibold       │
│        Supports JPG / PNG / ... (1–2 lines, wrapped by measurement)          │
│                      N formats in total                    11 muted           │
│                      [ 132 x 36 primary button ]            accent            │
│             Open folder                 All formats           12 text buttons  │
└──────────────────────────────────────────────────────────────────────────────┘
```

* Vertical rhythm: `28 padding + 64 icon + 20 gap + 28 title + 12 gap +
  18 x lines + 4 gap + 16 count + (note: 6 + 18) + 22 section + 36 button +
  14 gap + 24 footer + 28 padding`.
* One-line card = 314 DIP; every extra format line adds 18 DIP; the note adds
  24 DIP.
* If the content rectangle is shorter than the card, the icon (and its 20 DIP
  gap) is dropped once (`compact`) so the primary button is never clipped.
* Every size lives in `ui/metrics.h` (`kEmpty*`, `kFormats*`); nothing here is a
  hard-coded pixel value.
* The format line is wrapped by `WrapFormatNames()` using real text
  measurements, so English (longer) uses two lines and Chinese one. A line that
  still does not fit is ellipsized; the "N formats in total" line carries the
  complete count.

## 4. Format line derivation (the important part)

`core/format_summary.*` never contains a hand-written list of format names. It
groups `SupportedExtensions()` through `FormatRouter::FormatForExtension()` and
advertises a family only when this build/system can really decode it:

| Family | Decodable when |
| --- | --- |
| JPG / PNG / WebP / BMP / GIF / TIFF / ICO | WIC (always in this build) |
| PSD (PSD/PSB) | the in-tree decoder is compiled (always) |
| SVG | Direct2D is present (always) |
| TGA / PCX / PNM / XBM / XPM / WBMP | FreeImage is linked |
| RAW | LibRaw **or** FreeImage is linked (and a RAW extension exists) |
| HEIC/HEIF, AVIF | a WIC decoder claims `.heic`/`.heif`/`.avif` (Store extensions) |
| HDR/EXR/PFM | the router maps those extensions **and** FreeImage is linked |
| JP2/J2K/JPC | FreeImage is linked, or a WIC JP2 codec is installed |
| JXR/WDP | FreeImage is linked, or a WIC JPEG XR codec is installed |
| MNG/JNG | FreeImage is linked (first frame only) |

* The HEIF/AVIF check enumerates the installed WIC decoder components and reads
  their declared file extensions — no sample file and no registry guessing
  (`wic::DecoderSupportsExtension`). JP2/JXR use the same probe, with
  FreeImage as the fallback.
* A family with no routed extension is dropped, so the line never advertises a
  format the router cannot open.
* The count ("52 formats in total") is the number of **decodable** extensions,
  so a Debug `-NoFreeImage` build does not claim TGA/RAW/HDR/JP2/MNG.
* The "All formats" card lists every routed family, including unavailable ones,
  with "needs system codec" or "not built" instead of a false claim.
* `--layout-dump` prints `format_count` and `format_highlights`, and the log has
  one `format: freeimage=... libraw=... heif=... avif=... extensions=...` line.

## 5. Drag & drop (OLE, not WM_DROPFILES)

`DragAcceptFiles`/`WM_DROPFILES` only deliver the path after the mouse is
released, so it cannot draw a hover state. It was removed completely; the only
drop path is `platform::DropTarget` (`IDropTarget`):

* `main.cpp` initializes **OLE** (`OleInitialize`) instead of a bare
  `CoInitializeEx`; `RegisterDragDrop` fails without it (規劃/22/03 坑 1).
* `DragEnter/DragOver` set `DROPEFFECT_COPY` (otherwise the cursor shows a
  "forbidden" badge), `DragLeave` clears the state, `Drop` reads `CF_HDROP`.
* Only the hover **transitions** repaint; `DragOver` itself is free, so a drag
  never repaints per frame (坑 3).
* Before the window is destroyed the target is revoked
  (`RevokeDragDrop`) and its reference released.
* Reference counting follows the OLE contract (`AddRef` on every handed-out
  interface, `Release` deletes at zero).

Path dispatch (one function for OLE and `--simulate-drop`, `OnDroppedPaths`):

```text
first path only (multi-select opens the first item, 規劃/22/03 §4.3)
is directory      -> BuildFolderListFromFolder()
                     empty  -> folder-empty state (no decode requested)
                     else   -> open index 0
is a regular file and its extension is routed -> OpenFile()
otherwise         -> unsupported note (empty) / toast (image open)
```

Long/UNC/Unicode paths stay `std::filesystem::path` end to end; no URL
decoding.

## 6. Keyboard and focus

* `Tab` / `Shift+Tab` cycle focus: primary → open folder → all formats.
* `Enter` activates the focused button; the primary is focused by default.
* The focus ring is 2 px `accent` (system highlight colour in high contrast).
* `Ctrl+O` keeps working with no image (the command dispatch is independent of
  `has_image`, 規劃/22/03 坑 16).
* `Esc` is still owned by `OverlayStack`: the all-formats card closes first,
  and a modal never closes on an outside click.

## 7. All-formats card

`ui/empty_state.*`: 420 DIP wide, centred, height derived from the row count
(capped at 420 DIP and at the client minus 16 DIP margins); rows that do not fit
are not laid out, so drawing and hit-testing always agree. One row per family:
name, extensions, and a reason for unavailable families. The footer shows
`Esc to close · N in total` and a Close button. It is an `OverlayKind::kFormats`
modal: it dims the rest of the window (scrim), blocks image commands and is
closed by Esc, the Close button, or opening a file.

## 8. GDI first frame

Before D3D is ready, `UiRoot::PaintGdiBackground` fills the chrome bands plus a
simplified card: card background, accent primary button, title text in the
system UI font. It reads the same `EmptyLayout` as D2D, so there is one set of
coordinates (規劃/22/03 坑 15).

## 9. Test hooks

| Hook | Purpose |
| --- | --- |
| `--simulate-drop <path>` | runs `OnDroppedPaths()` once (repeatable; 600 ms first, 500 ms apart) — never publicized |
| `--simulate-ole-drop <path>` | builds a real `CF_HDROP` data object with the shell and drives the registered `IDropTarget` (`DragEnter/DragOver/Drop`) — verifies ReadPaths, effects and hover without a mouse |
| `--simulate-drag-over` | enters the hover state without an OLE drag (screenshot/dump) |
| `--layout-dump <file>` | extended with `empty`, `empty_kind`, `empty_lines`, `empty_compact`, `empty_focus`, `drag_over`, `loading`, `formats_open`, `formats_rows`, `format_count`, `format_highlights`, and the rectangles `empty_card/title/formats/primary/open_folder/all_formats/note`, `drag_hint`, `formats_panel`, `formats_close`, `formats_row_N` |
| `--click-hit empty.all_formats` | opens the modal card through the real command path (`formats.close` closes it) |

The primary/open-folder buttons intentionally have no click hook: they open
native modal dialogs and would hang an automated run. Their hit rectangles are
covered by unit tests, and the command itself is the same
`ActivateEmptyAction()` the mouse uses.

## 10. Log lines

```text
format: freeimage=1 libraw=1 heif=1 avif=1 extensions=52 routed=52
drop: register ok
drop: enter
drop: leave
drop: path=<path> dir=0|1
folder: open <path> files=N
empty: state=folder
empty: state=unsupported note=Unsupported format: .txt
empty: note=cleared
empty: focus=1
empty: activate=formats
cmd: formats=closed
test: drop=<path>                (hook only)
test: ole DragEnter hr=0x... effect=1
test: ole DragOver effect=1
test: ole Drop hr=0x... effect=1  (hook only, real IDropTarget calls)
```

## 11. Tests

* `zoreview_tests` (1034 checks) adds pure-function checks for
  `ComputeEmptyLayout`, its DPI scaling, the compact fallback, hit order,
  `BuildFormatSummary` with full/minimal capabilities, and
  `ComputeFormatPanelLayout`.
* `tests/run_empty_state_check.ps1` — **125 checks**: default state, drag
  overlay, the real `IDropTarget` path via `--simulate-ole-drop`,
  file/folder/empty-folder/unsupported/missing drops, unsupported while an image
  is open, the all-formats card and its close, forced 100/125/150/200 % DPI,
  Chinese, maximized, plus the screenshots under `tests/output/empty-state/`.
* Existing suites that must stay green: `run_ui_chrome_check` (86),
  `run_ui_shell_check` (41), `run_ui_check`, `run_format_matrix` (198/198),
  release/install/shell checks.

## 12. Known limitations

* A decode failure still shows the old message box; the in-window error card is
  `22/11`.
* Multi-select drops open the first item only; there is no queue UI.
* There is no recent-files list and no "open file" drag-out (`22/19`).
* The empty card has no UI Automation provider in v2.0; it is keyboard
  reachable with a visible focus ring instead.
* The unsupported note lives for 3 s and is not persisted.
* The OLE target is tested in-process with a real `CF_HDROP` data object
  (`--simulate-ole-drop`); a cross-process Explorer drag is verified manually
  only (it needs shell automation).
