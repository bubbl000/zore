# Bird's eye view (minimap, v3.0 #4)

> Planning: `規劃/26-v3.0-UI修正-方案.md §2.4`.
> Code: `src/ui/minimap.*` (pure geometry + painting), `src/ui/layout.cpp`
> (placement inside the one layout frame), `src/render/renderer.*` (the derived
> mini photo), `src/app/main.cpp` (input, hooks, dump).

---

## 1. What it is

A small copy of the whole picture in the lower-right corner, with the visible part
of the content area outlined. It is an **in-window overlay**, not a second
top-level window: `OverlayKind` is untouched, `Esc` has no new owner, and the
existing card/window architecture stays as it is (`規劃/27 §2`).

Layer order: above the thumbnail rail, **below** the rail tooltip, the bottom bar
and the toast / popup / modal layer. It is painted from the app's overlay callback
inside `kBottomPanels`: rail first, then the minimap, then `UiRoot`'s tooltip and
bottom bar, so a hover tooltip is never covered by the panel.

## 2. Where it sits

`ComputeMinimapLayout()` (`src/ui/minimap.h`) is the only geometry. Inputs are the
client/content/rail/bottom-bar rectangles from `LayoutFrame`, DPI, and three
renderer numbers: post-rotation display size (`display_width/height`), effective
scale (`zoom()`), and the picture centre (`ViewCenter()`).

* **Anchor**: directly above the rail slot (`strip.top − 8 DIP`); above the bottom
  bar when the rail is off; at the bottom edge with a 12 DIP margin in fullscreen.
  The panel is right-aligned with the same 12 DIP margin.
* **Size**: canvas fitted to the **rotated** display aspect, at most 220 × 180 DIP,
  never more than ~34 % of the content width or ~42 % of its height. Padding is
  3 DIP, radius 6 DIP.
* **Minimum**: the panel is not placed at all when the canvas would be narrower
  than 80 DIP or shorter than 16 DIP. That is why an extreme panorama (for
  example 20:1) simply has no minimap: a 7 px strip would be a dead control, not
  a navigation aid.
* The panel slides up rather than hanging over the bar on a very short window.

## 3. The photo

`Renderer::minimap_photo()`:

* a picture whose long edge is already ≤ **512 px** is used as-is (no copy);
* a larger picture is downscaled **once** on the GPU into a `512 × n` target
  bitmap, in the same way as the `22/18` 1/4 proxy (target retarget, one
  `DrawBitmap`, restore). It is re-baked when the frame changes
  (`SetImage`/`ClearImage`) and released when the panel is off;
* the bake only runs while the panel is on (`SetMinimapEnabled`), so an off
  minimap costs no VRAM;
* a failed bake falls back to the full-size bitmap — slower but correct.

This is a **derived copy of the current frame**, not the V3-04 display-grade
chain: no re-decode, no disk cache, about 0.6 MB at 512 × 320. The paint is a
single `DrawBitmap` from that small texture, so panning a 8K picture does not run
a full-size high-quality downscale every frame.

Without the bake the panel would have to minify 7680 px into ~180 px with
`HIGH_QUALITY_CUBIC` on every repaint; that cost is why the derived copy exists.

## 4. Interaction

The whole panel is one hit target (`HitId::kMinimap`), hit **before** the rail
and the image area, so a press never falls through to the picture behind it.

The viewport frame is a 12 % accent fill plus a 1.5 DIP accent stroke. It is
omitted when it covers the whole photo (fit): there it would only double the
panel border and tint the picture.

* Press **inside the frame**: drag the frame. Every move becomes
  `Renderer::PanBy(-dx × k, -dy × k)` with
  `k = display_per_canvas × zoom` — canvas pixels → display pixels → client
  pixels. No `SetZoom`/`Fit` call exists in this path, so dragging the frame can
  never make the picture grow (the 0.9 pitfall is avoided by construction).
* Press **outside the frame** (blank photo margin): the visible centre moves to
  the pressed picture point on press, then the same drag continues from there.
* A press without movement is a no-op (inside the frame) or the re-centre above.
* `WM_CAPTURECHANGED` ends the gesture, so Alt+Tab cannot leave the frame stuck.
* Mouse-up ends the drag; the panel stays where it is.
* The pointer gets the hand cursor over the panel.

The frame follows **every** pan, including the normal image drag. Pan is the one
view change that does not go through `RefreshUi()`, so the app refreshes the
layout once per move *while the panel is open*. With the panel off, image
dragging is unchanged.

On a window resize the effective fit scale depends on the content insets the
layout itself produces, so `RefreshUi()` runs a second layout pass while the panel
is open. Without it the frame would keep the pre-resize fraction until the next
input (regression covered by `run_minimap_check --move-window`).

## 5. Entry points

| Entry | Notes |
| --- | --- |
| 更多▾ → 鸟瞰图 | `kCmdToggleBirdseye`, persisted as `settings.birdseye` |
| `M` | same command, no modifier (T/C stay theme/checkerboard, I stays info) |
| Settings page | `birdseye` checkbox (unchanged) |

Default is **off**. Toggling keeps the existing toast feedback.

## 6. Test hooks and dump

| Hook | Purpose |
| --- | --- |
| `--click-hit minimap` | synthesized click on the frame centre |
| `--click-hit minimap.blank` | synthesized click on the photo's top-left (blank only at zoom > fit) |
| `--minimap-drag <dx>,<dy>` | real press → move → release on the frame; queued with the click hits, so the dump waits for it |

Dump keys (only while the panel is placed):

```text
minimap=1
minimap_panel=<l,t,r,b>
minimap_canvas=<l,t,r,b>
minimap_image=<l,t,r,b>            # where the photo is painted
minimap_viewport=<l,t,r,b>         # visible part of the picture, client px
minimap_view_display=<l,t,r,b>     # same region in rotated source pixels
minimap_scale=<display px per canvas px>
minimap_photo=1                    # a bitmap is available for painting
minimap_baked=1                    # the derived copy exists (>512 px sources)
```

Log lines:

```text
renderer: minimap 512x320 bgra=0.6MB bake=0.9ms   # once per frame change
ui: minimap drag=start | drag=end | drag=cancelled
ui: minimap center                                # blank click re-centred the view
```

## 7. Tests

* Unit: `tests/ui_layout_tests.cpp` → `TestMinimapLayout()` (panel placement,
  aspect following the rotation, clipped visible rect, drag conversion direction
  and scale, canvas→display mapping, fullscreen placement, tiny/panorama hiding,
  hit order).
* GUI: `tests/run_minimap_check.ps1` — setting default, placement at fit,
  more-panel switch (show/hide/persist), `M`, 1:1 frame size, drag (frame moves,
  zoom does not), blank click re-centre, 90° aspect, fullscreen non-overlap with
  the reveal buttons, panorama hiding, a two-screenshot paint check (nothing
  outside the panel changes, the panel rectangle is painted), and the three plan
  screenshots `output/minimap/minimap-{fit,actual,rot90}.png`.
