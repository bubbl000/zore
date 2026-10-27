# v3.0 #4 (plan 26 section 2.4): bird's eye view (minimap).
#
# The minimap is an in-window overlay above the thumbnail rail: it shows the
# whole picture, outlines the visible part of the content area and pans the
# picture when the frame is dragged. The script asserts the geometry from
# --layout-dump, the behaviour from the log, and the painting from two
# screenshots (panel on/off).
#
# ASCII only, like the other UI scripts (HANDOFF pitfall 60/124). Every run is a
# throw-away portable directory, so the developer's settings are never touched.

param(
  [string]$App = ".\build\Release\zoreview.exe"
)

$ErrorActionPreference = "Stop"
$repo = Split-Path -Parent $PSScriptRoot
Set-Location $repo

if (-not (Test-Path $App)) {
  throw "Application not found: $App (build first: tools\build.ps1)"
}
$App = (Resolve-Path $App).Path

$work = Join-Path $env:TEMP ("nv-minimap-" + [guid]::NewGuid().ToString("N"))
New-Item -ItemType Directory -Path $work | Out-Null
$appExe = Join-Path $work "zoreview.exe"
Copy-Item $App $appExe
$appDirectory = Split-Path -Parent $App
Get-ChildItem $appDirectory -Filter "clang_rt.asan*.dll" -ErrorAction SilentlyContinue |
  ForEach-Object { Copy-Item $_.FullName (Join-Path $work $_.Name) }

$script:checks = 0
$script:failures = 0

function Check([bool]$condition, [string]$name) {
  $script:checks++
  if ($condition) {
    Write-Host ("ok   {0}" -f $name)
  } else {
    Write-Host ("FAIL {0}" -f $name) -ForegroundColor Red
    $script:failures++
  }
}

function CheckNear([double]$actual, [double]$expected, [double]$tolerance,
                   [string]$name) {
  Check ([math]::Abs($actual - $expected) -le $tolerance) `
    ("{0} (actual={1:0.##} expected={2:0.##})" -f $name, $actual, $expected)
}

function CheckHas([string]$text, [string]$pattern, [string]$name) {
  Check ($text -match [regex]::Escape($pattern)) ("{0} (expected '{1}')" -f $name, $pattern)
}

# --- Samples ---------------------------------------------------------------
Add-Type -AssemblyName System.Drawing
$photoPath = Join-Path $work "photo.jpg"
$panoramaPath = Join-Path $work "panorama.jpg"

function New-Sample([string]$path, [int]$width, [int]$height, [bool]$markers) {
  $bitmap = New-Object System.Drawing.Bitmap($width, $height)
  $graphics = [System.Drawing.Graphics]::FromImage($bitmap)
  $rect = New-Object System.Drawing.Rectangle(0, 0, $width, $height)
  $brush = New-Object System.Drawing.Drawing2D.LinearGradientBrush(
    $rect,
    [System.Drawing.Color]::FromArgb(30, 80, 160),
    [System.Drawing.Color]::FromArgb(200, 90, 60),
    [System.Drawing.Drawing2D.LinearGradientMode]::ForwardDiagonal)
  $graphics.FillRectangle($brush, $rect)
  if ($markers) {
    # Asymmetric markers make an accidental rotation obvious.
    $white = New-Object System.Drawing.SolidBrush(
      [System.Drawing.Color]::FromArgb(245, 245, 245))
    $graphics.FillRectangle($white, [int]($width * 0.08), [int]($height * 0.10),
                            [int]($width * 0.28), [int]($height * 0.20))
    $accent = New-Object System.Drawing.SolidBrush(
      [System.Drawing.Color]::FromArgb(240, 210, 60))
    $graphics.FillEllipse($accent, [int]($width * 0.55), [int]($height * 0.30),
                          [int]($width * 0.30), [int]($height * 0.40))
    $white.Dispose()
    $accent.Dispose()
  }
  $brush.Dispose()
  $graphics.Dispose()
  $bitmap.Save($path, [System.Drawing.Imaging.ImageFormat]::Jpeg)
  $bitmap.Dispose()
}

New-Sample $photoPath 3200 2000 $true
New-Sample $panoramaPath 4000 200 $false

# --- Helpers ---------------------------------------------------------------
function Seed([int]$birdseye) {
  $lines = @("[general]", "birdseye=$birdseye")
  Set-Content -Path (Join-Path $work "settings.ini") -Encoding ASCII -Value $lines
}

$dumpPath = Join-Path $work "layout.txt"

function ReadDump([string]$path) {
  $values = @{}
  if (-not (Test-Path $path)) { return $values }
  foreach ($line in Get-Content $path) {
    $parts = $line.Split("=", 2)
    if ($parts.Count -eq 2) { $values[$parts[0]] = $parts[1] }
  }
  return $values
}

function Rect([hashtable]$values, [string]$key) {
  if (-not $values.ContainsKey($key)) { return $null }
  $numbers = $values[$key].Split(",") | ForEach-Object { [double]$_ }
  if ($numbers.Count -ne 4) { return $null }
  return @{ L = $numbers[0]; T = $numbers[1]; R = $numbers[2]; B = $numbers[3];
            W = $numbers[2] - $numbers[0]; H = $numbers[3] - $numbers[1] }
}

function Intersects([hashtable]$a, [hashtable]$b) {
  if ($null -eq $a -or $null -eq $b) { return $false }
  return -not ($a.R -le $b.L -or $b.R -le $a.L -or $a.B -le $b.T -or $b.B -le $a.T)
}

function Inside([hashtable]$inner, [hashtable]$outer) {
  if ($null -eq $inner -or $null -eq $outer) { return $false }
  return ($inner.L -ge $outer.L - 0.5) -and ($inner.T -ge $outer.T - 0.5) -and
         ($inner.R -le $outer.R + 0.5) -and ($inner.B -le $outer.B + 0.5)
}

function RunDump([string]$label, [string[]]$argv, [string]$image, [int]$birdseye) {
  Seed $birdseye
  Remove-Item $dumpPath -Force -ErrorAction SilentlyContinue
  $log = Join-Path $work "logs\zoreview.log"
  Remove-Item $log -Force -ErrorAction SilentlyContinue
  $arguments = @("--portable", "--language", "en", "--strip=pinned",
                 "--layout-dump", $dumpPath)
  $arguments += $argv
  $arguments += $image
  $process = Start-Process -FilePath $appExe -ArgumentList $arguments -PassThru
  if (-not $process.WaitForExit(40000)) {
    $process.Kill()
    throw "zoreview.exe did not exit in time: $label"
  }
  Start-Sleep -Milliseconds 150
  if (-not (Test-Path $dumpPath)) { throw "no layout dump for $label" }
  $text = ""
  if (Test-Path $log) { $text = Get-Content $log -Raw }
  return @{ Values = (ReadDump $dumpPath); Log = $text }
}

function ClientRect([hashtable]$values) {
  if (-not $values.ContainsKey("client")) { return $null }
  $parts = $values["client"].Split("x")
  if ($parts.Count -ne 2) { return $null }
  $w = [double]$parts[0]
  $h = [double]$parts[1]
  return @{ L = 0; T = 0; R = $w; B = $h; W = $w; H = $h }
}

$shotDir = Join-Path $PSScriptRoot "output\minimap"
New-Item -ItemType Directory -Force -Path $shotDir | Out-Null

function RunShot([string]$name, [string[]]$argv, [string]$image, [int]$birdseye) {
  Seed $birdseye
  $png = Join-Path $shotDir ($name + ".png")
  Remove-Item $png -Force -ErrorAction SilentlyContinue
  $log = Join-Path $work "logs\zoreview.log"
  Remove-Item $log -Force -ErrorAction SilentlyContinue
  $arguments = @("--portable", "--language", "en", "--strip=pinned",
                 "--screenshot", $png, "--shot-delay", "3000")
  $arguments += $argv
  $arguments += $image
  $process = Start-Process -FilePath $appExe -ArgumentList $arguments -PassThru
  if (-not $process.WaitForExit(60000)) {
    $process.Kill()
    throw "zoreview.exe did not exit in time: $name"
  }
  Start-Sleep -Milliseconds 150
  if (-not (Test-Path $png)) { throw "no screenshot for $name" }
  return $png
}

# Copies the BGRA bytes of a PNG once; GetPixel per sample is far too slow for a
# 1280x800 comparison.
function Read-Pixels([string]$path) {
  $bitmap = New-Object System.Drawing.Bitmap($path)
  $width = $bitmap.Width
  $height = $bitmap.Height
  $rect = New-Object System.Drawing.Rectangle(0, 0, $width, $height)
  $data = $bitmap.LockBits($rect,
    [System.Drawing.Imaging.ImageLockMode]::ReadOnly,
    [System.Drawing.Imaging.PixelFormat]::Format32bppArgb)
  $stride = $data.Stride
  $bytes = New-Object byte[] ($stride * $height)
  [System.Runtime.InteropServices.Marshal]::Copy($data.Scan0, $bytes, 0,
    $bytes.Length)
  $bitmap.UnlockBits($data)
  $bitmap.Dispose()
  return @{ Bytes = $bytes; Stride = $stride; W = $width; H = $height }
}

try {
  # --- 1. Default is off ----------------------------------------------------
  $off = RunDump "default" @() $photoPath 0
  $v = $off.Values
  $dpi = [double]$v["dpi"]
  Check ($v["birdseye"] -eq "0") "the bird's eye setting defaults to off"
  Check ($v["minimap"] -eq "0") "no panel is placed while the setting is off"
  Check (-not $v.ContainsKey("minimap_panel")) "an off minimap reports no rectangles"
  Check (-not ($off.Log -match "renderer: minimap")) "an off minimap bakes no photo"

  # --- 2. Panel geometry at fit --------------------------------------------
  $on = RunDump "fit" @() $photoPath 1
  $v = $on.Values
  $dpi = [double]$v["dpi"]
  Check ($v["minimap"] -eq "1") "the setting places the panel"
  $panel = Rect $v "minimap_panel"
  $canvas = Rect $v "minimap_canvas"
  $image = Rect $v "minimap_image"
  $viewport = Rect $v "minimap_viewport"
  $content = Rect $v "content"
  $strip = Rect $v "strip"
  Check ($null -ne $panel -and $panel.W -gt 0 -and $panel.H -gt 0) "the panel has a rectangle"
  Check (Inside $panel $content) "the panel stays inside the picture area"
  Check ($panel.B -lt $strip.T + 0.5) "the panel floats above the rail slot"
  CheckNear ($content.R - $panel.R) (12 * $dpi) 0.6 "the panel is right-aligned"
  Check ($v["minimap_photo"] -eq "1") "the photo bitmap is available"
  Check ($v["minimap_baked"] -eq "1") "a large picture bakes a derived mini photo"
  CheckHas $on.Log "renderer: minimap 512x" "the bake runs once and is logged at most 512 px"
  $displayW = [double]$v["display_width"]
  $displayH = [double]$v["display_height"]
  CheckNear ($canvas.W / $canvas.H) ($displayW / $displayH) 0.02 "the canvas follows the display aspect"
  CheckNear $canvas.L $image.L 0.01 "the photo starts at the canvas edge"
  CheckNear $canvas.R $image.R 0.01 "the photo ends at the canvas edge"
  Check ($canvas.W -le 220 * $dpi + 0.6 -and $canvas.H -le 180 * $dpi + 0.6) "the panel keeps its size budget"
  Check (Inside $viewport $image) "the frame stays inside the photo"
  Check ($viewport.W -ge $image.W - 1.5 -and $viewport.H -ge $image.H - 1.5) "a fitted picture outlines the whole photo"
  $scale = [double]$v["minimap_scale"]
  Check ($scale -gt 1) "one canvas pixel covers several display pixels"

  # --- 2b. A resize rebuilds the frame against the new fit scale -----------
  # The fit scale depends on the content insets the layout produces, so the app
  # runs a second layout pass while the panel is open. Without it the frame
  # would keep the pre-resize fraction until the next input.
  $resized = RunDump "resize" @("--move-window", "120,80,1040,700") $photoPath 1
  $v = $resized.Values
  $image = Rect $v "minimap_image"
  $viewport = Rect $v "minimap_viewport"
  Check ($v["client"] -eq "1040x700") "the resized client is the requested one"
  Check ($v["minimap"] -eq "1") "a resize keeps the panel"
  Check ($viewport.W -ge $image.W - 1.5 -and $viewport.H -ge $image.H - 1.5) "a resized fit view still outlines the whole photo"

  # --- 3. The more-panel switch shows and hides the panel ------------------
  Seed 0
  $toggle = RunDump "toggle-on" @("--click-hit", "bottom.more",
    "--click-hit", "menu.0") $photoPath 0
  CheckHas $toggle.Log "ui: toggle birdseye=1" "the more panel switch logs the new state"
  Check ($toggle.Values["minimap"] -eq "1") "the more panel switch shows the panel"
  # The switch saves immediately, so a fresh process must read it back. Do not
  # seed the file here: that would make the persistence check pass by accident.
  $printPath = Join-Path $work "print.txt"
  Remove-Item $printPath -ErrorAction SilentlyContinue
  $process = Start-Process -FilePath $appExe -ArgumentList @("--portable",
    "--language", "en", "--print-settings", $printPath) -PassThru
  if (-not $process.WaitForExit(30000)) { $process.Kill(); throw "print-settings timed out" }
  $printed = ReadDump $printPath
  Check ($printed["birdseye"] -eq "1") "the switch state is persisted"
  $toggleOff = RunDump "toggle-off" @("--click-hit", "bottom.more",
    "--click-hit", "menu.0") $photoPath 1
  CheckHas $toggleOff.Log "ui: toggle birdseye=0" "the more panel switch can hide the panel"
  Check ($toggleOff.Values["minimap"] -eq "0") "the closed switch removes the panel"

  # --- 4. M shortcut --------------------------------------------------------
  $keyOn = RunDump "key-on" @("--send-keys", "m") $photoPath 0
  CheckHas $keyOn.Log "ui: toggle birdseye=1" "M turns the bird's eye on"
  Check ($keyOn.Values["minimap"] -eq "1") "M places the panel"
  $keyOff = RunDump "key-off" @("--send-keys", "m,m") $photoPath 0
  Check ($keyOff.Values["minimap"] -eq "0") "M twice returns to off"

  # --- 5. 1:1 frame ---------------------------------------------------------
  $actual = RunDump "actual" @("--send-keys", "1") $photoPath 1
  $v = $actual.Values
  $content = Rect $v "content"
  $image = Rect $v "minimap_image"
  $viewport = Rect $v "minimap_viewport"
  Check ($v["zoom"] -eq "1.000") "the 1:1 view is at 100%"
  $expectedW = $image.W * $content.W / $displayW
  $expectedH = $image.H * $content.H / $displayH
  CheckNear $viewport.W $expectedW 1.5 "the frame width is the visible fraction (1:1)"
  CheckNear $viewport.H $expectedH 1.5 "the frame height is the visible fraction (1:1)"
  Check ($viewport.W -lt $image.W * 0.6) "the 1:1 frame covers only part of the photo"
  # The frame must map the visible display rect back into the panel. Derive the
  # expected centre from the dump (content, picture centre, zoom) instead of
  # from the minimap code, so a mapping regression is visible here.
  $center = $v["view_center"].Split(",") | ForEach-Object { [double]$_ }
  $zoom = [double]$v["zoom"]
  $displayCenterX = $displayW / 2 + (($content.L + $content.R) / 2 - $center[0]) / $zoom
  $displayCenterY = $displayH / 2 + (($content.T + $content.B) / 2 - $center[1]) / $zoom
  $expectedCenterX = $image.L + $displayCenterX / $displayW * $image.W
  $expectedCenterY = $image.T + $displayCenterY / $displayH * $image.H
  CheckNear (($viewport.L + $viewport.R) / 2) $expectedCenterX 1.5 "the frame centre maps the visible centre (x)"
  CheckNear (($viewport.T + $viewport.B) / 2) $expectedCenterY 1.5 "the frame centre maps the visible centre (y)"

  # --- 6. Dragging the frame pans without zooming ---------------------------
  $drag = RunDump "drag" @("--send-keys", "1", "--minimap-drag", "40,-20") $photoPath 1
  $v = $drag.Values
  CheckHas $drag.Log "ui: minimap drag=start" "pressing the frame starts a minimap drag"
  CheckHas $drag.Log "ui: minimap drag=end" "releasing ends the minimap drag"
  $viewport2 = Rect $v "minimap_viewport"
  CheckNear $viewport2.L ($viewport.L + 40) 1.5 "dragging right moves the frame right"
  CheckNear $viewport2.T ($viewport.T - 20) 1.5 "dragging up moves the frame up"
  Check ($v["zoom"] -eq "1.000") "dragging the frame never changes the zoom (no self-magnify)"
  Check ($v["fit"] -eq "0") "dragging stays in the manual view"
  $scale = [double]$v["minimap_scale"]
  $before = $actual.Values["view_center"].Split(",") | ForEach-Object { [double]$_ }
  $after = $v["view_center"].Split(",") | ForEach-Object { [double]$_ }
  CheckNear ($after[0] - $before[0]) (-40 * $scale) 2.5 "the pan follows the drag distance (x)"
  CheckNear ($after[1] - $before[1]) (20 * $scale) 2.5 "the pan follows the drag distance (y)"

  # --- 7. A blank click recentres the view ---------------------------------
  $blank = RunDump "blank" @("--send-keys", "1", "--click-hit", "minimap.blank") $photoPath 1
  $v = $blank.Values
  CheckHas $blank.Log "ui: minimap center" "a blank click recentres the view"
  $image = Rect $v "minimap_image"
  $viewport3 = Rect $v "minimap_viewport"
  Check ($viewport3.L -le $image.L + 1.5) "the recentred frame reaches the photo's left edge"
  Check ($viewport3.T -le $image.T + 1.5) "the recentred frame reaches the photo's top edge"
  Check ($v["zoom"] -eq "1.000") "recentring never changes the zoom"

  # --- 8. Rotation swaps the panel aspect ----------------------------------
  $rot = RunDump "rot90" @("--rotation", "90", "--send-keys", "1") $photoPath 1
  $v = $rot.Values
  $canvas = Rect $v "minimap_canvas"
  $image = Rect $v "minimap_image"
  $viewport4 = Rect $v "minimap_viewport"
  $content = Rect $v "content"
  Check ($v["rotation"] -eq "1") "the view is rotated 90 degrees"
  Check ($v["display_width"] -eq "2000" -and $v["display_height"] -eq "3200") "the display size is swapped"
  Check ($canvas.H -gt $canvas.W) "a 90 degree view makes the panel portrait"
  CheckNear ($canvas.W / $canvas.H) (2000.0 / 3200.0) 0.02 "the portrait canvas keeps the rotated aspect"
  $expectedW = $image.W * $content.W / 2000.0
  CheckNear $viewport4.W $expectedW 1.5 "the rotated frame maps the visible width"

  # --- 9. Fullscreen never covers the top-right buttons --------------------
  $fs = RunDump "fullscreen" @("--fullscreen") $photoPath 1
  $v = $fs.Values
  Check ($v["fullscreen"] -eq "1") "the fullscreen case is fullscreen"
  Check ($v["minimap"] -eq "1") "fullscreen keeps the panel"
  $panel = Rect $v "minimap_panel"
  $fsExit = Rect $v "fs_exit"
  $fsClose = Rect $v "fs_close"
  Check (-not (Intersects $panel $fsExit)) "the panel never covers the exit-fullscreen button"
  Check (-not (Intersects $panel $fsClose)) "the panel never covers the close button"
  $strip = Rect $v "strip"
  CheckNear ($strip.T - $panel.B) (8 * $dpi) 0.6 "in fullscreen the panel floats above the rail slot"

  # --- 10. Extreme aspect / tiny window hides the panel -------------------
  $pano = RunDump "panorama" @() $panoramaPath 1
  Check ($pano.Values["minimap"] -eq "0") "an extreme panorama aspect hides the panel"
  Check (-not $pano.Values.ContainsKey("minimap_panel")) "a hidden panel reports no rectangles"

  # --- 11. The panel is actually painted in its rectangle -----------------
  $shotOff = RunShot "photo-off" @() $photoPath 0
  $shotOn = RunShot "photo-on" @() $photoPath 1
  $hashOff = (Get-FileHash $shotOff -Algorithm SHA256).Hash
  $hashOn = (Get-FileHash $shotOn -Algorithm SHA256).Hash
  Check ($hashOff -ne $hashOn) "the panel changes the rendered frame"
  $onDump = RunDump "paint" @() $photoPath 1
  $panel = Rect $onDump.Values "minimap_panel"
  $a = Read-Pixels $shotOff
  $b = Read-Pixels $shotOn
  $outsideDiff = 0
  $insideDiff = 0
  $insideTotal = 0
  $step = 4
  for ($y = 0; $y -lt $a.H; $y += $step) {
    for ($x = 0; $x -lt $a.W; $x += $step) {
      $inside = ($x -ge ($panel.L - 2)) -and ($x -le ($panel.R + 2)) -and
                ($y -ge ($panel.T - 2)) -and ($y -le ($panel.B + 2))
      $index = $y * $a.Stride + $x * 4
      $different = ($a.Bytes[$index] -ne $b.Bytes[$index]) -or
                   ($a.Bytes[$index + 1] -ne $b.Bytes[$index + 1]) -or
                   ($a.Bytes[$index + 2] -ne $b.Bytes[$index + 2])
      if ($inside) {
        $insideTotal++
        if ($different) { $insideDiff++ }
      } elseif ($different) {
        $outsideDiff++
      }
    }
  }
  Check ($outsideDiff -eq 0) ("nothing outside the panel changes (diff={0})" -f $outsideDiff)
  Check ($insideTotal -gt 1000) "the panel region is large enough to sample"
  Check ($insideDiff -gt ($insideTotal * 0.1)) ("the panel is painted inside its rectangle (diff={0}/{1})" -f $insideDiff, $insideTotal)

  # --- 12. The three plan screenshots --------------------------------------
  $fitShot = RunShot "minimap-fit" @() $photoPath 1
  $actualShot = RunShot "minimap-actual" @("--send-keys", "1") $photoPath 1
  $rotShot = RunShot "minimap-rot90" @("--rotation", "90", "--send-keys", "1") $photoPath 1
  $fitHash = (Get-FileHash $fitShot -Algorithm SHA256).Hash
  $actualHash = (Get-FileHash $actualShot -Algorithm SHA256).Hash
  $rotHash = (Get-FileHash $rotShot -Algorithm SHA256).Hash
  Check ($fitHash -ne $actualHash) "the 1:1 screenshot differs from the fit screenshot"
  Check ($fitHash -ne $rotHash) "the 90 degree screenshot differs from the fit screenshot"
  Check ($actualHash -ne $rotHash) "the 1:1 screenshot differs from the rotated one"
  Check (Test-Path (Join-Path $shotDir "minimap-fit.png")) "the fit screenshot is kept"
  Check (Test-Path (Join-Path $shotDir "minimap-actual.png")) "the 1:1 screenshot is kept"
  Check (Test-Path (Join-Path $shotDir "minimap-rot90.png")) "the 90 degree screenshot is kept"
} finally {
  Remove-Item $work -Recurse -Force -ErrorAction SilentlyContinue
}

Write-Host ""
if ($script:failures -gt 0) {
  Write-Host ("{0} check(s) failed" -f $script:failures) -ForegroundColor Red
  exit 1
}
Write-Host ("all {0} bird's eye checks passed" -f $script:checks)
exit 0
