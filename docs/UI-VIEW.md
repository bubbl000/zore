# View rotation (v2.0 S1 / `22/05`)

> Display-only quarter turns. The decoded pixels, `ImageInfo`, the memory/disk
> caches and the file on disk stay upright; the renderer rotates the bitmap
> around its own centre. Everything here is one source of truth: `rotation_` in
> `src/render/renderer.*` and the pure math in `src/render/view_transform.h`.

---

## 1. Interaction

| Input | Action |
| --- | --- |
| `Ctrl+R` | rotate 90° clockwise |
| `Ctrl+L` | rotate 90° counter-clockwise |
| bottom-bar `↻` / `↺` | same commands (the accent dot was removed in v3.0, `規劃/26 §2.3`) |
| four quarter turns | return exactly to the original frame (`max_diff=0`) |

The 更多▾ panel used to carry a `重置旋转 / Reset rotation` row; v3.0 removed it
(`規劃/26 §2.5`). There is no `Ctrl+0` binding either, so the documented way back
to upright is the fourth quarter turn (or turning back the way you came).

Long key repeats are ignored (`lParam` bit 30): holding `Ctrl+R` rotates once,
not four times. The shortcut table is `ui/command.cpp: CommandForKey()`; the
availability decision is `QueryCommand()` (needs a picture, no folder
requirement).

Rotation survives fullscreen, maximize/restore, window resize, DPI changes and
animation frame updates (`SetImage(info, pixels, false)` never touches it).
Opening the next image keeps it when `settings.ini` `[ui] keep_rotation=1`
(default); with `0` each load resets the view. An empty state (`ClearImage()`)
always returns to 0°.

## 2. Geometry

```text
source size      W × H            (image_width()/image_height())
effective size   odd quarter turn ? H × W : W × H   (display_width/height)
fit scale        min(contentW / effectiveW, contentH / effectiveH)
```

`EffectiveOffset()` still centres the unrotated `W × H` rectangle in the
content, which is also the centre of the rotated bounding box: the picture does
not jump when the angle changes. In manual zoom mode the zoom and the image
centre are kept; the picture spins in place.

The transform is exact, not built from `sin/cos`:

```text
r=0 { 1, 0, 0, 1 }   r=1 { 0, 1,-1, 0 }   r=2 {-1, 0, 0,-1 }   r=3 { 0,-1, 1, 0 }
M' = T(-c) · R · T(c)          c = centre of the drawn image rectangle
```

so four quarter turns reproduce the original pixels bit-for-bit.

## 3. Drawing order

```text
Clear(background)
PushAxisAlignedClip(content)          // while the transform is identity
  SetTransform(rotation about c)
    checkerboard + DrawBitmap(dest)   // dest stays W × H, source pixels
  SetTransform(identity)              // MUST reset before the chrome
PopAxisAlignedClip
overlay(kImageArea → kBottomPanels → kTopPanels → kChrome)
```

The clip is pushed in screen space before the rotation transform, so the image
is clipped to the content rectangle even when rotated. The transform is reset
before every overlay layer: title bar, bottom bar, arrows and cards never
rotate.

## 4. Zoom anchors

Wheel zoom works in the unrotated image space. The cursor position is mapped
back first:

```text
p' = c + R(-angle) · (p - c)
```

then the existing anchor formula runs on `p'`. Without this, the zoom drifts
sideways after a rotation (the pixel under the cursor must stay under it).
`Renderer::SetZoom()` (percent menu) does the same for the content centre.

## 5. Exports

`Save as`, `Copy image` and `Set as desktop wallpaper` save **what the user
sees**: the app makes a temporary copy of the current image and rotates it with
`nv::RotateImageForView()` (`core/image_transform.cpp`, the same clockwise
convention as the renderer), then writes that copy. The decoded image, the LRU
and the disk cache are untouched.

The window title and the info panel keep the **source** dimensions. The info
panel appends the view angle, e.g. `400 × 300 (view 90°)`.

## 6. Settings

| Key (`[ui]`) | Values | Default |
| --- | --- | --- |
| `keep_rotation` | `0` / `1` | `1` |

The settings page (`22/07`) will expose the switch; there is no second copy of
the value in the app.

## 7. Test hooks

| Hook | Purpose |
| --- | --- |
| `--rotation <0\|90\|180\|270>` | deterministic initial view rotation |
| `--send-keys "Ctrl+R,Ctrl+R"` | real `WM_KEYDOWN` path for the shortcuts |
| `--click-hit bottom.rotate_left\|bottom.rotate_right` | real hit rectangle of the buttons |
| `--save-as <file>` | saves the current view (after queued keys) without the dialog |
| `--layout-dump` | adds `rotation`, `rotation_degrees`, `display_width`, `display_height`, `info_dimension` |

Logs:

```text
cmd: view=rotate right rotation=1 angle=90 display=300x400
cmd: view=rotate left  rotation=3 angle=270 display=300x400
cmd: view=rotate reset keep_rotation=0
cmd: blocked=rotate.right
saved image: <path> rotation=1
copied image: rotation=1
```

## 8. Pitfalls

1. **The transform must be reset before the overlay** — otherwise the title bar
   and bottom bar rotate with the picture.
2. **Fit uses the swapped size** — using `W × H` leaves a 90° image cropped or
   tiny.
3. **The clip is pushed before `SetTransform`** — pushing it after a rotation
   makes the clip a rotated quad.
4. **The wheel anchor must be un-rotated** — otherwise zoom drifts after a
   rotation.
5. **`SetImage(..., false)` must keep the rotation** — animation frames would
   otherwise snap back upright on every frame.
6. **`keep_rotation` resets once per load, not per frame** — do not put the
   reset inside `SetImage`.
7. **Export uses a temporary rotated copy** — never mutate `ImageInfo` or the
   cached image; the title/info/cache key must keep describing the source file.
8. **The rotate buttons stay enabled while rotated** — they perform another 90°
   and are never locked by the current angle. The accent dot that used to mark
   "rotated" is gone (v3.0, `規劃/26 §2.3`): two 4 DIP dots under the icons read
   as stray pixels.

## 9. Acceptance

```powershell
# 77 checks: Ctrl+L/R, buttons, turn-back to upright, keep_rotation, copy/save
# exports, angles 0/90/180/270 sampled on a four-colour marker, four-turn round
# trip, fullscreen, no-image blocking, optional 8K memory parity.
powershell -ExecutionPolicy Bypass -File tests\run_rotation_check.ps1

# ASan build (the script copies the ASan runtime next to the temp exe)
powershell -ExecutionPolicy Bypass -File tests\run_rotation_check.ps1 `
  -App .\build-asan\RelWithDebInfo\zoreview.exe

# Manual: four angles at the current DPI
.\build\Release\zoreview.exe --portable --rotation 90 "图.jpg"
```

Screenshots land in `tests\output\rotation\` (`angle-0/90/180/270/360.png`).
