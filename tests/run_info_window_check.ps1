# v3.0 (plan 26 §2.1 / item #1): the EXIF info card is its own top-level window.
#
# This replaces tests/run_info_panel_check.ps1: the v2.0 overlay had no HWND, so
# the window lifecycle (own Escape, drag/remembered position, hide while
# minimised/fullscreen, close with the main window) is what this run checks.
# Everything is asserted from --layout-dump, --print-settings and the log; the
# card itself is captured with --info-screenshot (PrintWindow), which is
# independent of --screenshot (that one still only captures the main window).
#
# The file contains non-ASCII sample labels, so it MUST keep its UTF-8 BOM
# (HANDOFF pitfall 60 / v3.0 pitfall 306).

param(
  [string]$App = ".\build\Release\zoreview.exe",
  [switch]$KeepOutput,
  [switch]$SkipRaw
)

$ErrorActionPreference = "Stop"
$repo = Split-Path -Parent $PSScriptRoot
Set-Location $repo

if (-not (Test-Path $App)) {
  throw "Application not found: $App (build first: tools\build.ps1)"
}
$App = (Resolve-Path $App).Path

$output = Join-Path $repo "tests\output\info-window"
New-Item -ItemType Directory -Force -Path $output | Out-Null

$work = Join-Path $env:TEMP ("nv-info-window-" + [guid]::NewGuid().ToString("N"))
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

function CheckEqual($actual, $expected, [string]$name) {
  Check ($actual -eq $expected) `
    ("{0} (actual='{1}' expected='{2}')" -f $name, $actual, $expected)
}

function CheckNear([double]$actual, [double]$expected, [double]$tolerance,
                   [string]$name) {
  Check ([math]::Abs($actual - $expected) -le $tolerance) `
    ("{0} (actual={1:0.##} expected={2:0.##})" -f $name, $actual, $expected)
}

function CheckHas([string]$text, [string]$pattern, [string]$name) {
  Check ($text.Contains($pattern)) ("{0} (expected '{1}')" -f $name, $pattern)
}

function ReadDump([string]$path) {
  $values = @{}
  if (-not (Test-Path $path)) { return $values }
  $lines = [System.IO.File]::ReadAllLines($path,
    [System.Text.Encoding]::UTF8)
  foreach ($line in $lines) {
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

function WorkAreas([hashtable]$values) {
  $areas = @()
  if (-not $values.ContainsKey("work_areas")) { return $areas }
  foreach ($part in $values["work_areas"].Split(";")) {
    if ([string]::IsNullOrWhiteSpace($part)) { continue }
    $numbers = $part.Split(",") | ForEach-Object { [double]$_ }
    if ($numbers.Count -eq 4) {
      $areas += ,@{ L = $numbers[0]; T = $numbers[1]; R = $numbers[2];
                    B = $numbers[3] }
    }
  }
  return $areas
}

function OverlapsAny([hashtable]$rect, $areas) {
  foreach ($area in $areas) {
    $w = [math]::Min($rect.R, $area.R) - [math]::Max($rect.L, $area.L)
    $h = [math]::Min($rect.B, $area.B) - [math]::Max($rect.T, $area.T)
    if ($w -ge 100 -and $h -ge 100) { return $true }
  }
  return $false
}

function ContainingArea([hashtable]$rect, $areas) {
  $best = $null
  $bestArea = 0.0
  foreach ($area in $areas) {
    $w = [math]::Min($rect.R, $area.R) - [math]::Max($rect.L, $area.L)
    $h = [math]::Min($rect.B, $area.B) - [math]::Max($rect.T, $area.T)
    if ($w -le 0 -or $h -le 0) { continue }
    if (($w * $h) -gt $bestArea) { $bestArea = $w * $h; $best = $area }
  }
  return $best
}

function LogText() {
  $log = Join-Path $work "logs\zoreview.log"
  if (Test-Path $log) { return (Get-Content $log -Raw) }
  return ""
}

# Samples: copied into one folder so navigation order is deterministic.
$source = Join-Path $repo "..\测试文件夹\images\exif"
if (-not (Test-Path $source)) {
  throw "EXIF samples missing: run python 测试文件夹\tools\make-exif-samples.py"
}
$exifDir = Join-Path $work "exif"
New-Item -ItemType Directory -Path $exifDir | Out-Null
foreach ($name in @("exif_full.jpg", "exif_none.png", "exif_partial.jpg",
                    "exif_png.png", "exif_south.jpg")) {
  Copy-Item (Join-Path $source $name) (Join-Path $exifDir $name)
}
$full = Join-Path $exifDir "exif_full.jpg"
$partial = Join-Path $exifDir "exif_partial.jpg"
$none = Join-Path $exifDir "exif_none.png"
$png = Join-Path $exifDir "exif_png.png"
$south = Join-Path $exifDir "exif_south.jpg"

function Invoke-App([string]$label, [string[]]$extra, [string]$image = $full,
                    [switch]$KeepSettings) {
  if (-not $KeepSettings) {
    Remove-Item (Join-Path $work "settings.ini") -Force -ErrorAction SilentlyContinue
  }
  $log = Join-Path $work "logs\zoreview.log"
  Remove-Item $log -Force -ErrorAction SilentlyContinue
  $dump = Join-Path $work ("dump-" + $label + ".txt")
  Remove-Item $dump -Force -ErrorAction SilentlyContinue

  $arguments = @("--portable", "--language", "en", "--layout-dump", "`"$dump`"")
  $arguments += $extra
  if ($image) { $arguments += "`"$image`"" }

  $process = Start-Process -FilePath $appExe -ArgumentList $arguments -PassThru
  if (-not $process.WaitForExit(90000)) {
    $process.Kill()
    throw "zoreview.exe did not exit in time: $label"
  }
  Start-Sleep -Milliseconds 150
  return @{ Values = (ReadDump $dump); Log = (LogText); Dump = $dump }
}

# Screenshot-only run: --info-screenshot captures the card and --quit-at ends the
# process afterwards (a layout dump would end it before the capture).
function Invoke-Shot([string]$label, [string[]]$extra, [string]$target) {
  Remove-Item (Join-Path $work "settings.ini") -Force -ErrorAction SilentlyContinue
  $log = Join-Path $work "logs\zoreview.log"
  Remove-Item $log -Force -ErrorAction SilentlyContinue
  $arguments = @("--portable", "--language", "en",
                 "--info-screenshot", "`"$target`"", "--quit-at", "4600")
  $arguments += $extra
  $arguments += "`"$full`""
  $process = Start-Process -FilePath $appExe -ArgumentList $arguments -PassThru
  if (-not $process.WaitForExit(60000)) {
    $process.Kill()
    throw "zoreview.exe did not exit in time: $label"
  }
  Start-Sleep -Milliseconds 150
  return @{ Log = (LogText) }
}

function PrintSettings() {
  $path = Join-Path $work "print-settings.txt"
  Remove-Item $path -Force -ErrorAction SilentlyContinue
  $process = Start-Process -FilePath $appExe `
    -ArgumentList @("--portable", "--print-settings", "`"$path`"") -PassThru
  if (-not $process.WaitForExit(30000)) {
    $process.Kill()
    throw "zoreview.exe did not exit in time: print-settings"
  }
  Start-Sleep -Milliseconds 100
  return (ReadDump $path)
}

function Shot([string]$label, [string[]]$extra, [string]$name) {
  $path = Join-Path $output $name
  Remove-Item $path -Force -ErrorAction SilentlyContinue
  $result = Invoke-Shot $label $extra $path
  Check (Test-Path $path) ("the screenshot exists: " + $name)
  return $result
}

try {
  # --- 1. A fresh session keeps the card closed ---------------------------
  $case = Invoke-App "hidden" @()
  Check ($case.Values["info_open"] -eq "0") "a fresh session starts with the card hidden"
  Check ($case.Values["info_window_saved"] -eq "0") "no position is stored yet"
  $printed = PrintSettings
  Check ($printed["info_window_visible"] -eq "0") "the setting stays hidden"
  Check ($printed["info_window_saved"] -eq "0") "the setting has no placement"

  # --- 2. It opens as a separate top-level window -------------------------
  $case = Invoke-App "open" @("--info-window")
  $values = $case.Values
  CheckEqual $values["info_open"] "1" "--info-window opens the card"
  CheckEqual $values["info_window_shown"] "1" "the card is on screen"
  CheckEqual $values["info_rows"] "25" "the card has 25 lines (4 groups + 21 rows)"
  CheckEqual $values["info_exif_ok"] "1" "the EXIF read succeeded"
  CheckHas $case.Log "ui: info_window=open" "opening is logged"
  CheckHas $case.Log "exif: done ok=1" "the worker reported a result"
  CheckHas $case.Log "exif: request queued" "the read ran on the worker"

  $dpi = [double]$values["info_window_dpi"]
  Check ($dpi -gt 0.0) "the card reports its DPI scale"
  $info = Rect $values "info"
  $close = Rect $values "info_close"
  $copyAll = Rect $values "info_copy_all"
  $body = Rect $values "info_body"
  $footer = Rect $values "info_footer"
  $header = Rect $values "info_header"
  CheckNear $info.W (360 * $dpi) 0.7 "the window is 360 DIP wide"
  Check (Inside $close $info) "the close button is inside the window"
  Check (Inside $copyAll $info) "copy-all is inside the window"
  Check (Inside $body $info) "the body is inside the window"
  CheckNear $close.W (22 * $dpi) 0.6 "the close button is 22 DIP"
  CheckNear $copyAll.H (28 * $dpi) 0.6 "copy-all is 28 DIP tall"
  Check ($body.T -ge $header.B - 0.5) "the body starts below the header"
  Check ($body.B -le $footer.T + 0.5) "the body ends above the footer"
  Check ($close.R -ge ($info.R - 17 * $dpi) - 0.5) "close sits at the top-right"
  Check ($copyAll.R -ge ($info.R - 17 * $dpi) - 0.5) "copy-all sits at the bottom-right"

  $mainRect = Rect $values "window_rect"
  $infoRect = Rect $values "info_window_rect"
  Check ($null -ne $infoRect -and ($infoRect.W -gt 0) -and ($infoRect.H -gt 0)) `
    "the card has its own screen rectangle"
  Check (($infoRect.L -ne $mainRect.L) -or ($infoRect.T -ne $mainRect.T)) `
    "it is not the main window rectangle"
  $areas = WorkAreas $values
  Check (OverlapsAny $infoRect $areas) "the card starts inside a monitor work area"
  $area = ContainingArea $infoRect $areas
  if ($null -ne $area) {
    Check ($infoRect.H -le (($area.B - $area.T) * 0.7) + 1.5) `
      "the card is capped at 70% of the work area"
  }

  # --- 3. I / Alt+Enter / the bottom button drive the same window ---------
  $case = Invoke-App "ikey" @("--send-keys", "I")
  CheckEqual $case.Values["info_open"] "1" "I opens the card"
  CheckHas $case.Log "cmd: view=info 1" "I logs view=info"
  CheckHas $case.Log "ui: info_window=open" "I creates the window"

  $case = Invoke-App "altenter" @("--send-keys", "Alt+Enter")
  CheckEqual $case.Values["info_open"] "1" "Alt+Enter opens the card"

  $case = Invoke-App "bottom" @("--click-hit", "bottom.info")
  CheckEqual $case.Values["info_open"] "1" "the bottom bar button opens the card"
  $case = Invoke-App "bottom2" @("--click-hit", "bottom.info", "--click-hit", "bottom.info")
  CheckEqual $case.Values["info_open"] "0" "a second click closes it again"

  # --- 4. Close button and Escape belong to the window --------------------
  $case = Invoke-App "close" @("--info-window", "--click-hit", "info.close")
  CheckEqual $case.Values["info_open"] "0" "the close button closes the card"
  CheckHas $case.Log "ui: info_window=closed" "closing is logged"
  $printed = PrintSettings
  Check ($printed["info_window_visible"] -eq "0") "closing stores the hidden state"

  $case = Invoke-App "iescape" @("--info-window", "--info-key", "escape")
  CheckEqual $case.Values["info_open"] "0" "Escape in the card closes it"
  CheckHas $case.Log "test: info_key escape sent" "the key reached the card"
  CheckHas $case.Log "ui: info_window=closed" "the card handled Escape itself"

  # The main window must not steal that Escape (single owner, 規劃/27 坑 219).
  $case = Invoke-App "mainesc" @("--info-window", "--send-keys", "Escape")
  CheckEqual $case.Values["info_open"] "1" "Escape in the main window leaves the card open"
  CheckHas $case.Log "cmd: escape=none" "the main window had no layer to close"

  # `I` closes it too: opening the card moves focus to it, so the main window
  # never sees the second press; the same key is delivered to the card here.
  $case = Invoke-App "ikey-close" @("--info-window", "--info-key", "i")
  CheckEqual $case.Values["info_open"] "0" "I in the card closes it again"
  CheckHas $case.Log "test: info_key i sent" "the I key reached the card"

  # --- 5. Every EXIF value is formatted as before -------------------------
  CheckEqual $values["info_name"] "exif_full.jpg" "file name row"
  Check ($values["info_path"].EndsWith("exif_full.jpg")) "path row is absolute"
  Check ($values["info_path"] -match "^\w:") "path row starts with a drive"
  CheckEqual $values["info_format"] "JPG" "format row"
  CheckEqual $values["info_dimension"] "1600 × 1000" "dimension row"
  CheckEqual $values["info_depth"] "8-bit / none" "bit depth / alpha row"
  CheckEqual $values["info_frames"] "1" "frame count row"
  CheckEqual $values["info_camera"] "Canon EOS R5" "make + model without duplicates"
  CheckEqual $values["info_lens"] "RF24-70mm F2.8 L IS USM" "lens row"
  CheckEqual $values["info_taken"] "2024-08-12 14:33:05" "date taken is normalised"
  CheckEqual $values["info_exposure"] "1/125 s" "exposure row"
  CheckEqual $values["info_aperture"] "f/2.8" "aperture row"
  CheckEqual $values["info_iso"] "ISO 400" "ISO row"
  CheckEqual $values["info_focal"] "50 mm" "focal length row"
  CheckEqual $values["info_bias"] "+0.3 EV" "exposure bias row"
  CheckEqual $values["info_flash"] "Did not fire" "flash row"
  CheckEqual $values["info_metering"] "Pattern" "metering row"
  CheckEqual $values["info_whitebalance"] "Auto" "white balance row"
  CheckHas $values["info_gps"] "31.230278, 121.473611" "GPS row"
  CheckEqual $values["info_line_0"] "h:File" "line 0 is the file header"
  CheckEqual $values["info_line_5"] "h:Image" "line 5 is the image header"
  CheckEqual $values["info_line_10"] "h:Camera" "line 10 is the camera header"
  CheckEqual $values["info_line_23"] "h:Color" "line 23 is the color header"

  # --- 6. Missing tags render an em dash, never a blank or an error -------
  $case = Invoke-App "partial" @("--info-window") $partial
  CheckEqual $case.Values["info_camera"] "Nikon Z 6" "partial: camera present"
  CheckEqual $case.Values["info_taken"] "2022-12-24 18:30:00" "partial: date present"
  CheckEqual $case.Values["info_iso"] "—" "partial: missing ISO shows an em dash"
  CheckEqual $case.Values["info_exposure"] "—" "partial: missing exposure is dashed"
  CheckEqual $case.Values["info_gps"] "—" "partial: missing GPS is dashed"
  CheckEqual $case.Values["info_icc"] "—" "partial: no ICC profile is dashed"
  CheckEqual $case.Values["info_rows"] "25" "partial: the row set never shrinks"

  $case = Invoke-App "none" @("--info-window") $none
  CheckEqual $case.Values["info_exif_ok"] "0" "no EXIF: the read reports none"
  CheckEqual $case.Values["info_camera"] "—" "no EXIF: camera is dashed"
  CheckEqual $case.Values["info_format"] "PNG" "no EXIF: format still known"
  CheckEqual $case.Values["info_dimension"] "800 × 500" "no EXIF: size from decode"
  CheckHas $case.Log "exif: done ok=0" "no EXIF is not an error toast"

  $case = Invoke-App "png" @("--info-window") $png
  CheckEqual $case.Values["info_camera"] "FUJIFILM X-T5" "PNG eXIf: camera"
  CheckEqual $case.Values["info_iso"] "ISO 800" "PNG eXIf: ISO"
  CheckEqual $case.Values["info_bias"] "-0.7 EV" "PNG eXIf: negative bias"

  $case = Invoke-App "south" @("--info-window") $south
  CheckHas $case.Values["info_gps"] "-33.860000, -151.210000" `
    "S/W GPS coordinates are negative"

  # --- 7. The card follows the current file --------------------------------
  $case = Invoke-App "nav" @("--send-keys", "I,Right,Right")
  CheckEqual $case.Values["folder_index"] "2" "two Rights land on the third file"
  CheckEqual $case.Values["info_name"] "exif_partial.jpg" "the card follows the file"
  CheckEqual $case.Values["info_camera"] "Nikon Z 6" "no stale camera from the first file"
  CheckEqual $case.Values["info_iso"] "—" "no stale ISO from the first file"
  CheckEqual $case.Values["info_open"] "1" "the card stays open while navigating"

  # --- 8. Scrolling --------------------------------------------------------
  # At 200% the 70% work-area cap bites, so the 25-row body really scrolls.
  $case = Invoke-App "scroll" @("--info-window", "--force-dpi-scale", "200",
                               "--info-scroll", "3")
  Check ([double]$case.Values["info_scroll"] -gt 0) "the scroll hook is applied"
  Check ([int]$case.Values["info_first_visible"] -ge 5) "the first visible line moved"
  CheckEqual $case.Values["info_scrollable"] "1" "the body reports scrolling"

  $case = Invoke-App "scrollmax" @("--info-window", "--force-dpi-scale", "200",
                                  "--info-scroll", "100")
  CheckNear ([double]$case.Values["info_scroll"]) `
    ([double]$case.Values["info_scroll_max"]) 0.6 "scrolling clamps at the end"

  $case = Invoke-App "scrollzero" @("--info-window", "--force-dpi-scale", "200",
                                   "--info-scroll", "0")
  CheckEqual $case.Values["info_first_visible"] "0" "no scroll starts at line 0"

  # --- 9. Copy all / copy one row -----------------------------------------
  Set-Clipboard -Value "nv-clear"
  $case = Invoke-App "copyall" @("--info-window", "--click-hit", "info.copy_all")
  CheckHas $case.Log "ui: info_copy=all" "copy-all reports the copy"
  $clipboard = Get-Clipboard -Raw
  CheckHas $clipboard "Name: exif_full.jpg" "copy-all contains the file name"
  CheckHas $clipboard "Camera: Canon EOS R5" "copy-all contains the camera"
  CheckHas $clipboard "ISO: ISO 400" "copy-all contains the ISO"
  Check (-not $clipboard.Contains("h:File")) "copy-all skips the group headers"
  Check (-not $clipboard.Contains("—")) "copy-all skips missing values"
  Check (($clipboard -split "`n").Count -ge 19) "copy-all has one line per value"
  CheckEqual $case.Values["info_open"] "1" "copy-all keeps the card open"

  Set-Clipboard -Value "nv-clear"
  # Row 16 (ISO) is below the fold: scroll first, then right-click it through the
  # real WM_CONTEXTMENU path.
  $case = Invoke-App "copyrow" @("--info-window", "--info-scroll", "3",
                                 "--click-hit", "info.copy_row.16")
  CheckHas $case.Log "ui: info_copy=row 16" "the row context menu copies that row"
  CheckEqual ((Get-Clipboard -Raw).Trim()) "ISO 400" "the copied row value is exact"

  # A plain left click on a row only moves the card (caption), it must not copy.
  Set-Clipboard -Value "nv-clear"
  $case = Invoke-App "rowleft" @("--info-window", "--click-hit", "info.row.16")
  CheckHas $case.Log "test: click=info.row.16" "the row click was delivered"
  CheckEqual $case.Values["info_open"] "1" "a row click keeps the card open"
  CheckEqual ((Get-Clipboard -Raw).Trim()) "nv-clear" "a row click copies nothing"

  # --- 10. Four DPI scales -------------------------------------------------
  foreach ($scale in @(100, 125, 150, 200)) {
    $case = Invoke-App ("dpi" + $scale) @("--info-window", "--force-dpi-scale", "$scale")
    $d = [double]$case.Values["info_window_dpi"]
    $info = Rect $case.Values "info"
    $close = Rect $case.Values "info_close"
    $copyAll = Rect $case.Values "info_copy_all"
    CheckNear $d ($scale / 100.0) 0.01 "$scale%: the card reports the forced DPI"
    CheckNear $info.W (360 * $d) 0.7 "$scale%: the window width scales"
    Check (Inside $close $info) "$scale%: close is inside the window"
    Check (Inside $copyAll $info) "$scale%: copy-all is inside the window"
    CheckNear $close.W (22 * $d) 0.7 "$scale%: close scales with DPI"
    CheckNear $copyAll.H (28 * $d) 0.7 "$scale%: copy-all scales with DPI"
  }

  # --- 11. Localisation and high contrast ---------------------------------
  $case = Invoke-App "zh" @("--language", "zh", "--info-window")
  CheckEqual $case.Values["info_camera"] "Canon EOS R5" "zh: values are unchanged"
  CheckEqual $case.Values["info_line_10"] "h:拍摄" "zh: the camera header is localised"
  CheckHas $case.Values["info_exposure"] "秒" "zh: the exposure unit is localised"
  CheckEqual $case.Values["info_window_shown"] "1" "zh: the card is on screen"

  $case = Invoke-App "hc" @("--force-high-contrast", "--info-window")
  CheckHas $case.Log "ui: high_contrast=1" "high contrast is active"
  CheckEqual $case.Values["info_open"] "1" "the card opens in high contrast"

  # --- 12. Screenshots of the card itself ---------------------------------
  Shot "shot-en" @("--info-window") "info-en.png"
  Shot "shot-zh" @("--language", "zh", "--info-window") "info-zh.png"
  Shot "shot-hc" @("--force-high-contrast", "--info-window") "info-hc.png"
  Shot "shot-light" @("--send-keys", "T", "--info-window") "info-light.png"
  Shot "shot-scrolled" @("--info-window", "--force-dpi-scale", "200",
                        "--info-scroll", "3") "info-scrolled.png"
  Shot "shot-dpi150" @("--info-window", "--force-dpi-scale", "150") "info-dpi150.png"

  # --- 13. Fullscreen and minimised follow the owner ----------------------
  $case = Invoke-App "fs" @("--info-window", "--fullscreen", "--dump-at", "3000",
                            "--quit-at", "4200")
  CheckEqual $case.Values["fullscreen"] "1" "fullscreen is active"
  CheckEqual $case.Values["info_open"] "1" "the card still exists in fullscreen"
  CheckEqual $case.Values["info_window_shown"] "0" "the card hides in fullscreen"

  $case = Invoke-App "fs-exit" @("--info-window", "--send-keys", "F11,F11")
  CheckEqual $case.Values["fullscreen"] "0" "fullscreen was left again"
  CheckEqual $case.Values["info_window_shown"] "1" "leaving fullscreen shows the card again"

  $case = Invoke-App "minimized" @("--info-window", "--minimize-at", "1600",
                                   "--dump-at", "2600", "--quit-at", "3800")
  CheckEqual $case.Values["info_window_shown"] "0" "minimising hides the card"

  $case = Invoke-App "restored" @("--info-window", "--minimize-at", "1600",
                                  "--restore-at", "2200", "--dump-at", "3000",
                                  "--quit-at", "4200")
  CheckEqual $case.Values["info_window_shown"] "1" "restoring shows the card again"

  # Closing the main window closes the card but keeps the "was open" intent.
  $case = Invoke-App "appclose" @("--info-window", "--quit-at", "3000")
  CheckHas $case.Log "ui: info_window=app_close" "the app closes the card first"
  CheckHas $case.Log "ui: info_window=closed (app)" "the close came from the app"
  $printed = PrintSettings
  Check ($printed["info_window_visible"] -eq "1") "the intent survives an app close"

  # --- 14. Position is remembered and clamped -----------------------------
  $case = Invoke-App "moved" @("--info-window", "--info-geometry", "320,240")
  $moved = Rect $case.Values "info_window_rect"
  CheckEqual ([int]$moved.L) 320 "the card moved to the requested x"
  CheckEqual ([int]$moved.T) 240 "the card moved to the requested y"
  CheckHas $case.Log "ui: info_window=moved 320,240" "the move is persisted"
  $printed = PrintSettings
  CheckEqual $printed["info_window"] "320,240" "the placement is stored"
  Check ($printed["info_window_saved"] -eq "1") "the placement marker is set"
  Check ($printed["info_window_visible"] -eq "1") "the open state is stored"

  # The next launch restores it without --info-window.
  $case = Invoke-App "restore" @() $full -KeepSettings
  CheckEqual $case.Values["info_open"] "1" "the next launch reopens the card"
  $restored = Rect $case.Values "info_window_rect"
  CheckEqual ([int]$restored.L) 320 "the restored x matches"
  CheckEqual ([int]$restored.T) 240 "the restored y matches"

  # An off-screen stored position is clamped into a work area instead.
  @"
[general]
version=1
[window]
info_visible=1
info_x=-9999
info_y=-9999
info_saved=1
"@ | Set-Content -Path (Join-Path $work "settings.ini") -Encoding ASCII
  $case = Invoke-App "offscreen" @() $full -KeepSettings
  CheckEqual $case.Values["info_open"] "1" "the off-screen card still opens"
  $clamped = Rect $case.Values "info_window_rect"
  $areas = WorkAreas $case.Values
  Check (OverlapsAny $clamped $areas) "the off-screen position is clamped on screen"
  Check ($clamped.L -ge -32768) "the clamped x is a sane coordinate"
  CheckHas $case.Log "ui: info_window=open" "the clamped window is logged"

  # --- 15. RAW metadata still comes from LibRaw ---------------------------
  $raw = Join-Path $repo "..\测试文件夹\raw\Canon\EOS 5D Mark IV\B13A0729.CR2"
  if (-not $SkipRaw -and (Test-Path $raw)) {
    $case = Invoke-App "raw" @("--info-window") $raw
    CheckHas $case.Values["info_camera"] "EOS 5D Mark IV" "RAW: camera from LibRaw"
    CheckEqual $case.Values["info_iso"] "ISO 100" "RAW: ISO from LibRaw"
    CheckEqual $case.Values["info_exposure"] "1/5000 s" "RAW: shutter from LibRaw"
    CheckEqual $case.Values["info_aperture"] "f/2.8" "RAW: aperture from LibRaw"
  } else {
    Write-Host "skip RAW metadata check (sample missing or -SkipRaw)"
  }

  # --- Summary -------------------------------------------------------------
  Write-Host ""
  if ($script:failures -eq 0) {
    Write-Host ("info window checks passed: {0}/{0}" -f $script:checks) `
      -ForegroundColor Green
  } else {
    Write-Host ("info window checks: {0} ok, {1} failed" -f `
      ($script:checks - $script:failures), $script:failures) -ForegroundColor Red
  }
} finally {
  if (-not $KeepOutput) {
    Remove-Item $work -Recurse -Force -ErrorAction SilentlyContinue
  } else {
    Write-Host ("kept: " + $work)
  }
}

if ($script:failures -ne 0) {
  exit 1
}
