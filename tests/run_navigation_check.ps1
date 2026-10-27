# v2.0 S1 (plan 22/09): navigation keys, zoom keys, image-area arrows and the
# --send-keys hook.
#
# The script runs a throw-away copy of the executable in portable mode, so it
# never touches the developer's %LOCALAPPDATA% state. All strings are ASCII, so
# the file needs no UTF-8 BOM (HANDOFF pitfalls 60/124).
#
# Keys are posted through the real WM_KEYDOWN path (--send-keys); arrows are
# clicked through --click-hit, which uses the rectangle the UI actually drew.

param(
  [string]$App = ".\build\Release\zoreview.exe",
  [switch]$KeepOutput
)

$ErrorActionPreference = "Stop"
$repo = Split-Path -Parent $PSScriptRoot
Set-Location $repo

if (-not (Test-Path $App)) {
  throw "Application not found: $App (build first: tools\build.ps1)"
}
$App = (Resolve-Path $App).Path

$work = Join-Path $env:TEMP ("nv-nav-" + [guid]::NewGuid().ToString("N"))
New-Item -ItemType Directory -Path $work | Out-Null
$appExe = Join-Path $work "zoreview.exe"
Copy-Item $App $appExe
# AddressSanitizer builds need their runtime next to the copied executable
# (HANDOFF pitfall 113).
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
  Check ($text -match [regex]::Escape($pattern)) `
    ("{0} (expected '{1}')" -f $name, $pattern)
}

# --- Samples ---------------------------------------------------------------
Add-Type -AssemblyName System.Drawing
$samples = @(
  @{ Name = "a_wide.bmp";   W = 1600; H = 1200 },
  @{ Name = "b_square.bmp"; W = 900;  H = 900 },
  @{ Name = "c_tall.bmp";   W = 600;  H = 1400 }
)
foreach ($sample in $samples) {
  $bitmap = New-Object System.Drawing.Bitmap($sample.W, $sample.H)
  $graphics = [System.Drawing.Graphics]::FromImage($bitmap)
  $graphics.Clear([System.Drawing.Color]::FromArgb(30, 80, 140))
  $graphics.Dispose()
  $path = Join-Path $work $sample.Name
  $bitmap.Save($path, [System.Drawing.Imaging.ImageFormat]::Bmp)
  $bitmap.Dispose()
}
$firstImage = Join-Path $work "a_wide.bmp"

# A folder with exactly one image, for the "single image hides the arrows"
# case (the main folder has three).
$singleDir = Join-Path $work "single"
New-Item -ItemType Directory -Path $singleDir | Out-Null
Copy-Item $firstImage (Join-Path $singleDir "only.bmp")
$singleImage = Join-Path $singleDir "only.bmp"

# --- Helpers ---------------------------------------------------------------
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

function Inside([hashtable]$inner, [hashtable]$outer) {
  if ($null -eq $inner -or $null -eq $outer) { return $false }
  return ($inner.L -ge $outer.L - 0.5) -and
         ($inner.T -ge $outer.T - 0.5) -and
         ($inner.R -le $outer.R + 0.5) -and
         ($inner.B -le $outer.B + 0.5)
}

function LogText() {
  $log = Join-Path $work "logs\zoreview.log"
  if (Test-Path $log) { return (Get-Content $log -Raw) }
  return ""
}

# v3.0 (規劃/26 §2.10): the auto rail now reveals for the whole bottom bar, so a
# pointer parked over the window's bottom edge would show it at launch and make
# "the auto strip starts faded out" depend on where the last run left the mouse.
# Park the real pointer away from the centred window before every launch.
Add-Type @"
using System;
using System.Runtime.InteropServices;
public class NavCursor {
  [DllImport("user32.dll")] public static extern bool SetCursorPos(int x, int y);
}
"@ | Out-Null

# Runs one fresh process. `--layout-dump` is written after the queued keys
# finish, so the dump reflects the final state.
function RunCase([string]$label, [string[]]$extra, [string]$image = $firstImage) {
  # Each run starts from default settings; persisted mode is only meaningful
  # inside a single run for this check.
  Remove-Item (Join-Path $work "settings.ini") -Force -ErrorAction SilentlyContinue
  $log = Join-Path $work "logs\zoreview.log"
  Remove-Item $log -Force -ErrorAction SilentlyContinue
  $dump = Join-Path $work ("dump-" + $label + ".txt")
  Remove-Item $dump -Force -ErrorAction SilentlyContinue

  $arguments = @("--portable", "--language", "en", "--layout-dump", $dump)
  $arguments += $extra
  if ($image) { $arguments += $image }

  [NavCursor]::SetCursorPos(5, 5) | Out-Null
  $process = Start-Process -FilePath $appExe -ArgumentList $arguments -PassThru
  if (-not $process.WaitForExit(30000)) {
    $process.Kill()
    throw "zoreview.exe did not exit in time: $label"
  }
  Start-Sleep -Milliseconds 120
  return @{ Values = (ReadDump $dump); Log = (LogText); Dump = $dump }
}

function Keys([string]$label, [string]$sequence, [string]$image = $firstImage) {
  return RunCase $label @("--send-keys", $sequence) $image
}

try {
  # --- 1. Default geometry with a multi-image folder -----------------------
  $base = Keys "base" "Home"
  $values = $base.Values
  Check ($values["has_image"] -eq "1") "startup shows an image"
  Check ($values["arrows_visible"] -eq "1") "three files: arrows are visible"
  $dpi = [double]$values["dpi"]
  $content = Rect $values "content"
  $left = Rect $values "arrows_left"
  $right = Rect $values "arrows_right"
  CheckNear $left.W (56 * $dpi) 0.6 "left arrow is 56 DIP wide"
  CheckNear $left.H (56 * $dpi) 0.6 "left arrow is 56 DIP tall"
  CheckNear $right.W (56 * $dpi) 0.6 "right arrow is 56 DIP wide"
  CheckNear $left.L ($content.L + 16 * $dpi) 0.6 "left arrow is 16 DIP from content"
  CheckNear $right.R ($content.R - 16 * $dpi) 0.6 "right arrow is 16 DIP from content"
  CheckNear (($left.T + $left.B) / 2) (($content.T + $content.B) / 2) 0.6 `
    "arrows are vertically centred in the content"
  Check (Inside $left $content) "left arrow stays inside the content"
  Check (Inside $right $content) "right arrow stays inside the content"
  Check ($values["zoom_min"] -eq "0.1") "minimum zoom is 10% for a normal image"
  Check ($values["zoom_max"] -eq "32") "maximum zoom is 3200%"
  Check ($values["fit"] -eq "1") "startup uses fit mode"

  # --- 2. Navigation keys --------------------------------------------------
  $case = Keys "right" "Right"
  Check ($case.Values["folder_index"] -eq "1") "Right opens the next file"
  CheckHas $case.Log "cmd: nav=next" "Right logs nav=next"

  $case = Keys "left" "Right,Left"
  Check ($case.Values["folder_index"] -eq "0") "Left returns to the previous file"
  CheckHas $case.Log "cmd: nav=prev" "Left logs nav=prev"

  $case = Keys "end" "End"
  Check ($case.Values["folder_index"] -eq "2") "End opens the last file"
  CheckHas $case.Log "cmd: nav=last" "End logs nav=last"

  $case = Keys "home" "End,Home"
  Check ($case.Values["folder_index"] -eq "0") "Home opens the first file"
  CheckHas $case.Log "cmd: nav=first" "Home logs nav=first"

  $case = Keys "pgdn" "PgDn"
  Check ($case.Values["folder_index"] -eq "1") "PgDn is the next file"
  CheckHas $case.Log "cmd: nav=next" "PgDn logs nav=next"

  $case = Keys "pgup" "PgDn,PgUp"
  Check ($case.Values["folder_index"] -eq "0") "PgUp is the previous file"
  CheckHas $case.Log "cmd: nav=prev" "PgUp logs nav=prev"

  $case = Keys "space" "Space"
  Check ($case.Values["folder_index"] -eq "1") `
    "Space on a still image opens the next file"

  $case = Keys "right2" "Right,Right,Left"
  Check ($case.Values["folder_index"] -eq "1") "repeated navigation lands in order"

  # --- 3. Zoom keys --------------------------------------------------------
  $baseZoom = [double]$base.Values["zoom"]
  $case = Keys "plus" "+"
  Check ($case.Values["fit"] -eq "0") "plus leaves fit mode"
  $zoomPlus = [double]$case.Values["zoom"]
  Check ($zoomPlus -gt $baseZoom * 1.10) "plus zooms in by the configured step"
  CheckHas $case.Log "cmd: zoom=in" "plus logs zoom=in"

  $case = Keys "minus" "+,+,-"
  $zoomMinus = [double]$case.Values["zoom"]
  Check ($case.Values["fit"] -eq "0") "minus keeps manual zoom mode"
  CheckNear $zoomMinus $zoomPlus 0.02 "one minus cancels one plus"
  CheckHas $case.Log "cmd: zoom=out" "minus logs zoom=out"

  $case = Keys "ctrlfit" "+,Ctrl+0"
  Check ($case.Values["fit"] -eq "1") "Ctrl+0 returns to fit"
  CheckHas $case.Log "cmd: zoom=fit" "Ctrl+0 logs zoom=fit"

  $case = Keys "ctrlactual" "Ctrl+1"
  Check ($case.Values["fit"] -eq "0") "Ctrl+1 leaves fit mode"
  CheckNear ([double]$case.Values["zoom"]) 1.0 0.001 "Ctrl+1 is exactly 100%"
  Check ($case.Values["zoom_percent"] -eq "100") "Ctrl+1 reports 100%"
  CheckHas $case.Log "cmd: zoom=actual" "Ctrl+1 logs zoom=actual"

  $case = Keys "plainfit" "+,0"
  Check ($case.Values["fit"] -eq "1") "0 returns to fit"

  $case = Keys "plainactual" "1"
  CheckNear ([double]$case.Values["zoom"]) 1.0 0.001 "1 is exactly 100%"

  # --- 4. View keys --------------------------------------------------------
  $case = Keys "f11" "F11"
  Check ($case.Values["fullscreen"] -eq "1") "F11 enters fullscreen"
  $fsTitle = Rect $case.Values "title"
  Check ($fsTitle.H -eq 0) "fullscreen hides the title bar"

  $case = Keys "esc" "F11,Esc"
  Check ($case.Values["fullscreen"] -eq "0") "Esc leaves fullscreen"

  $case = Keys "enter" "Enter"
  Check ($case.Values["fullscreen"] -eq "1") "Enter toggles fullscreen"

  $case = Keys "info" "I"
  Check ($case.Values["info_open"] -eq "1") "I opens the info panel"
  CheckHas $case.Log "cmd: view=info 1" "I logs view=info"

  $case = Keys "altenter" "Alt+Enter"
  Check ($case.Values["info_open"] -eq "1") "Alt+Enter opens the info panel"

  # Ctrl+T maps auto <-> pinned (規劃/22/08 §3.2); every case starts from the
  # auto default because RunCase removes settings.ini.
  $case = Keys "strip" "Ctrl+T"
  Check ($case.Values["strip_mode"] -eq "pinned") "Ctrl+T pins the strip"
  Check ($case.Values["strip_counts"] -eq "1") "the pinned strip reserves space"
  Check ($case.Values["strip_shown"] -eq "1") "the pinned strip is shown"

  $case = Keys "strip2" "Ctrl+T,Ctrl+T"
  Check ($case.Values["strip_mode"] -eq "auto") "Ctrl+T toggles back to auto"
  Check ($case.Values["strip_shown"] -eq "0") "the auto strip starts faded out"
  Check ($case.Values["strip_interactive"] -eq "0") "the faded-out strip is not clickable"

  # --- 5. Image arrows -----------------------------------------------------
  $case = RunCase "arrow-right" @("--click-hit", "arrows.right")
  Check ($case.Values["folder_index"] -eq "1") "clicking the right arrow advances"
  CheckHas $case.Log "cmd: id=arrows.right" "the right arrow reports its hit id"
  CheckHas $case.Log "cmd: nav=next" "the right arrow dispatches nav=next"

  $case = RunCase "arrow-left" @("--click-hit", "arrows.right", "--click-hit", "arrows.left")
  Check ($case.Values["folder_index"] -eq "0") "clicking the left arrow goes back"
  CheckHas $case.Log "cmd: id=arrows.left" "the left arrow reports its hit id"

  # --- 6. Single image: no arrows, navigation disabled --------------------
  $case = Keys "single" "Right,Home" $singleImage
  Check ($case.Values["has_image"] -eq "1") "the single-image folder shows a picture"
  Check ($case.Values["arrows_visible"] -eq "0") `
    "a single image hides the arrows"
  Check ($case.Values["arrows_left"] -eq "0,0,0,0") `
    "a single image has no arrow rectangle"
  Check ($case.Values["folder_index"] -eq "0") "navigation is a no-op with one file"
  CheckHas $case.Log "cmd: blocked=next" "Right is blocked with one file"
  CheckHas $case.Log "cmd: blocked=first" "Home is blocked with one file"

  # --- 7. No image: every image command is disabled -----------------------
  $case = RunCase "no-image" @("--send-keys", "Right,Home,+,I") $null
  Check ($case.Values["has_image"] -eq "0") "no path starts in the empty state"
  Check ($case.Values["arrows_visible"] -eq "0") "the empty state has no arrows"
  CheckHas $case.Log "cmd: blocked=next" "Right is blocked without an image"
  CheckHas $case.Log "cmd: blocked=zoom.in" "plus is blocked without an image"
  CheckHas $case.Log "cmd: blocked=view.info" "I is blocked without an image"
  Check ($case.Values["folder_files"] -eq "0") "no folder was scanned without a path"

  # --- Summary -------------------------------------------------------------
  Write-Host ""
  Write-Host ("{0} checks, {1} failure(s)" -f $script:checks, $script:failures)
  if ($script:failures -ne 0) {
    Write-Host "Artifacts kept at $work" -ForegroundColor Yellow
    exit 1
  }
  Write-Host "OK: navigation checks passed"
} finally {
  if (-not $KeepOutput) {
    Remove-Item $work -Recurse -Force -ErrorAction SilentlyContinue
  }
}
