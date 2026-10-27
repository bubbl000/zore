# v3.0 (plan 26 section 2.9): self-drawn right-click menu (#12/#18/#19).
#
# The native TrackPopupMenu is gone: the menu is an in-window popup, so the
# checks can read its geometry from --layout-dump and drive it with the real
# mouse / --send-keys. Everything here is ASCII (pitfall 60/124).
#
# Geometry is physical client pixels; the harness is per-monitor DPI aware
# (pitfall 139) and parks the pointer before every run (pitfall 304).

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

$work = Join-Path $env:TEMP ("nv-context-" + [guid]::NewGuid().ToString("N"))
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
function CheckHas([string]$text, [string]$pattern, [string]$name) {
  Check ($text -match [regex]::Escape($pattern)) ("{0} (expected '{1}')" -f $name, $pattern)
}
function CheckNotHas([string]$text, [string]$pattern, [string]$name) {
  Check (-not ($text -match [regex]::Escape($pattern))) ("{0} (unexpected '{1}')" -f $name, $pattern)
}

Add-Type -AssemblyName System.Drawing
Add-Type @"
using System;
using System.Runtime.InteropServices;
public static class CtxNative {
  [DllImport("user32.dll")] public static extern bool SetProcessDpiAwarenessContext(IntPtr v);
  [DllImport("user32.dll")] public static extern bool SetCursorPos(int x, int y);
  [DllImport("user32.dll")] public static extern void mouse_event(uint f, uint dx, uint dy, uint d, UIntPtr e);
  [DllImport("user32.dll")] public static extern bool ClientToScreen(IntPtr h, ref POINT p);
  [DllImport("user32.dll")] public static extern bool GetWindowRect(IntPtr h, out RECT r);
  [DllImport("user32.dll")] public static extern bool SetForegroundWindow(IntPtr h);
  [StructLayout(LayoutKind.Sequential)] public struct RECT { public int Left, Top, Right, Bottom; }
  [StructLayout(LayoutKind.Sequential)] public struct POINT { public int X, Y; }
}
"@
[CtxNative]::SetProcessDpiAwarenessContext([IntPtr](-4)) | Out-Null

$bitmap = New-Object System.Drawing.Bitmap(800, 600)
$graphics = [System.Drawing.Graphics]::FromImage($bitmap)
$graphics.Clear([System.Drawing.Color]::FromArgb(46, 112, 164))
$graphics.Dispose()
$image = Join-Path $work "sample.bmp"
$bitmap.Save($image, [System.Drawing.Imaging.ImageFormat]::Bmp)
$bitmap.Dispose()

$settingsIni = Join-Path $work "settings.ini"
$layoutPath = Join-Path $work "layout.txt"
function LogPath() { Join-Path $work "logs\zoreview.log" }

function ReadValues([string]$path) {
  $values = @{}
  if (-not (Test-Path $path)) { return $values }
  # The dump is UTF-8 without a BOM; the labels contain an ellipsis.
  foreach ($line in Get-Content $path -Encoding UTF8) {
    $parts = $line.Split("=", 2)
    if ($parts.Count -eq 2) { $values[$parts[0]] = $parts[1] }
  }
  return $values
}
function RectOf([hashtable]$values, [string]$key) {
  if (-not $values.ContainsKey($key)) { return $null }
  $n = $values[$key].Split(",") | ForEach-Object { [double]$_ }
  if ($n.Count -ne 4) { return $null }
  return @{ L = $n[0]; T = $n[1]; R = $n[2]; B = $n[3]; W = $n[2] - $n[0]; H = $n[3] - $n[1] }
}
function Inside([hashtable]$inner, [hashtable]$outer) {
  if ($null -eq $inner -or $null -eq $outer) { return $false }
  return ($inner.L -ge $outer.L - 0.5) -and ($inner.T -ge $outer.T - 0.5) -and
         ($inner.R -le $outer.R + 0.5) -and ($inner.B -le $outer.B + 0.5)
}
function ClientRect([hashtable]$values) {
  if (-not $values.ContainsKey("client")) { return $null }
  $parts = $values["client"].Split("x")
  if ($parts.Count -ne 2) { return $null }
  $w = [double]$parts[0]; $h = [double]$parts[1]
  return @{ L = 0; T = 0; R = $w; B = $h; W = $w; H = $h }
}

# Runs one throw-away copy and waits for it to exit (no live input needed).
function RunDump([string]$label, [string[]]$extra, [int]$dumpAt = 2600,
                 [switch]$NoImage) {
  [CtxNative]::SetCursorPos(5, 5) | Out-Null
  Remove-Item $settingsIni -ErrorAction SilentlyContinue
  Remove-Item (LogPath) -ErrorAction SilentlyContinue
  $dump = Join-Path $work ("dump-" + $label + ".txt")
  Remove-Item $dump -ErrorAction SilentlyContinue
  $arguments = @("--portable", "--language", "en", "--layout-dump", $dump,
                 "--dump-at", "$dumpAt") + $extra
  if (-not $NoImage) { $arguments += @($image) }
  $process = Start-Process -FilePath $appExe -ArgumentList $arguments -PassThru
  for ($i = 0; $i -lt 200; $i++) {
    Start-Sleep -Milliseconds 100
    if (Test-Path $dump) { break }
  }
  Start-Sleep -Milliseconds 250
  $process.Refresh()
  if (-not $process.HasExited) {
    $null = $process.CloseMainWindow()
    if (-not $process.WaitForExit(5000)) { $process.Kill() }
  }
  Start-Sleep -Milliseconds 150
  return @{
    Values = (ReadValues $dump)
    Log = $(if (Test-Path (LogPath)) { Get-Content (LogPath) -Raw } else { "" })
  }
}

# Starts a live window for real-mouse cases; the dump lands at $dumpAt.
function StartLive([string]$label, [string[]]$extra, [int]$dumpAt = 4500) {
  [CtxNative]::SetCursorPos(5, 5) | Out-Null
  Remove-Item $settingsIni -ErrorAction SilentlyContinue
  Remove-Item (LogPath) -ErrorAction SilentlyContinue
  $dump = Join-Path $work ("live-" + $label + ".txt")
  Remove-Item $dump -ErrorAction SilentlyContinue
  $arguments = @("--portable", "--language", "en", "--layout-dump", $dump,
                 "--dump-at", "$dumpAt") + $extra + @($image)
  $process = Start-Process -FilePath $appExe -ArgumentList $arguments -PassThru
  $hwnd = [IntPtr]::Zero
  for ($i = 0; $i -lt 100; $i++) {
    Start-Sleep -Milliseconds 100
    $process.Refresh()
    if ($process.MainWindowHandle -ne [IntPtr]::Zero) { $hwnd = $process.MainWindowHandle; break }
  }
  if ($hwnd -eq [IntPtr]::Zero) { $process.Kill(); throw "$label no window" }
  return @{ Proc = $process; Hwnd = $hwnd; Dump = $dump }
}
function WaitDump($case, [int]$timeoutMs = 12000) {
  for ($i = 0; $i -lt ($timeoutMs / 100); $i++) {
    Start-Sleep -Milliseconds 100
    if (Test-Path $case.Dump) { break }
  }
  Start-Sleep -Milliseconds 200
  return (ReadValues $case.Dump)
}
function MoveClient($case, [double]$x, [double]$y) {
  $point = New-Object CtxNative+POINT
  $point.X = [int][math]::Round($x)
  $point.Y = [int][math]::Round($y)
  [CtxNative]::ClientToScreen($case.Hwnd, [ref]$point) | Out-Null
  [CtxNative]::SetCursorPos($point.X, $point.Y) | Out-Null
  Start-Sleep -Milliseconds 150
}
function ClickLeft($case, [double]$x, [double]$y) {
  [CtxNative]::SetForegroundWindow($case.Hwnd) | Out-Null
  Start-Sleep -Milliseconds 150
  MoveClient $case $x $y
  [CtxNative]::mouse_event(0x0002, 0, 0, 0, [UIntPtr]::Zero)
  Start-Sleep -Milliseconds 60
  [CtxNative]::mouse_event(0x0004, 0, 0, 0, [UIntPtr]::Zero)
  Start-Sleep -Milliseconds 300
}
function RightClick($case, [double]$x, [double]$y) {
  [CtxNative]::SetForegroundWindow($case.Hwnd) | Out-Null
  Start-Sleep -Milliseconds 150
  MoveClient $case $x $y
  [CtxNative]::mouse_event(0x0008, 0, 0, 0, [UIntPtr]::Zero)
  Start-Sleep -Milliseconds 60
  [CtxNative]::mouse_event(0x0010, 0, 0, 0, [UIntPtr]::Zero)
  Start-Sleep -Milliseconds 300
}
function EndLive($case) {
  $case.Proc.Refresh()
  if (-not $case.Proc.HasExited) {
    $null = $case.Proc.CloseMainWindow()
    if (-not $case.Proc.WaitForExit(5000)) { $case.Proc.Kill() }
  }
  Start-Sleep -Milliseconds 250
}
function LogText($case) {
  if (Test-Path (LogPath)) { return (Get-Content (LogPath) -Raw) }
  return ""
}

$menuLabelsPattern = '^Save as.*\|Copy image\|Copy path\|-\|Rename\|Delete to Recycle Bin\|-\|Set as desktop wallpaper\|Show in File Explorer\|-\|Settings.*$'

try {
  # --- 1. The hook opens the self-drawn menu below the point -----------------
  $open = RunDump "open" @("--context-menu", "640,400") 2600
  $v = $open.Values
  Check ($v["popup"] -eq "context") "the right-click menu opens as an in-window popup"
  Check ([int]$v["menu_rows"] -eq 11) ("the menu has 8 commands and 3 separators (got {0} rows)" -f $v["menu_rows"])
  $labels = $v["menu_labels"]
  Check ($labels -match $menuLabelsPattern) ("the rows keep their order and labels: {0}" -f $labels)
  Check (-not ($labels -match 'Open file')) "the menu has no 'Open file' entry (#18)"
  Check (-not ($labels -match 'Language')) "the menu has no language submenu (#12)"
  Check ($v["menu_above"] -eq "0") "a click in the middle opens the menu downwards"
  $client = ClientRect $v
  $menu = RectOf $v "menu"
  Check (Inside $menu $client) "the menu stays inside the client"
  Check ([math]::Abs($menu.L - 640) -le 1) ("the menu starts at the click x (got {0})" -f $menu.L)
  Check ($menu.T -ge 400) ("the menu starts below the click (got {0})" -f $menu.T)
  $title = RectOf $v "title"
  Check ($menu.T -ge $title.B) "the menu never overlaps the title bar"
  CheckHas $open.Log "ui: context_menu x=640 y=400" "opening the menu is logged"

  # --- 2. Near the right/bottom edge it flips left and up --------------------
  $flip = RunDump "flip" @("--context-menu", "1200,700") 2600
  $flipMenu = RectOf $flip.Values "menu"
  Check ($flip.Values["menu_above"] -eq "1") "a click near the bottom edge opens the menu upwards"
  Check ($flipMenu.R -le 1201) ("the flipped menu stays left of the click (right={0})" -f $flipMenu.R)
  Check ($flipMenu.B -le 701) ("the raised menu stays above the click (bottom={0})" -f $flipMenu.B)
  Check (Inside $flipMenu (ClientRect $flip.Values)) "the flipped menu stays inside the client"

  # --- 3. A right-click on the title bar opens below it ----------------------
  $top = RunDump "top" @("--context-menu", "640,10") 2600
  $topMenu = RectOf $top.Values "menu"
  $topTitle = RectOf $top.Values "title"
  Check ($top.Values["menu_above"] -eq "0") "a title-bar click opens the menu downwards"
  Check ($topMenu.T -ge $topTitle.B) "the menu is clamped below the title bar"

  # --- 4. Keyboard: Down/Down/Enter runs the second command -------------------
  $keys = RunDump "keys" @("--context-menu", "640,400",
                           "--send-keys", "down,down,enter",
                           "--send-keys-at", "1700") 3400
  Check ($keys.Values["popup"] -eq "none") "Enter closes the menu"
  CheckHas $keys.Log "ui: menu_hover=0" "Down highlights the first command"
  CheckHas $keys.Log "ui: menu_hover=1" "the second Down moves by one row"
  CheckHas $keys.Log "cmd: menu=context.copy_image" "Enter runs the highlighted command"
  # V3.1-06 (§06d): the copy confirmation is visible, and it is posted after the
  # menu closed instead of being painted underneath it.
  CheckHas $keys.Log ("ui: toast=" + "Image copied") `
    "copying the image reports success"
  Check ($keys.Values["toast_visible"] -eq "1") "the copy toast is on screen"
  Check ($keys.Values["toast_text"] -match "Image copied") `
    ("the dump carries the copy toast text (got '{0}')" -f $keys.Values["toast_text"])

  # Copy path: the third command in the same menu.
  $copyPath = RunDump "copy-path" @("--context-menu", "640,400",
                                    "--send-keys", "down,down,down,enter",
                                    "--send-keys-at", "1700") 3800
  CheckHas $copyPath.Log "cmd: menu=context.copy_path" "Enter on the third row copies the path"
  Check ($copyPath.Values["toast_text"] -match "Path copied") `
    ("the path copy also confirms (got '{0}')" -f $copyPath.Values["toast_text"])

  # --- 5. Up from nothing wraps to the last row (Settings) -------------------
  $up = RunDump "up" @("--context-menu", "640,400",
                       "--send-keys", "up,enter",
                       "--send-keys-at", "1700") 3400
  CheckHas $up.Log "ui: menu_hover=10" "Up from nothing selects the last command"
  Check ($up.Values["settings_open"] -eq "1") "Enter on Settings opens the settings window"

  # --- 6. Escape closes the menu --------------------------------------------
  $esc = RunDump "esc" @("--context-menu", "640,400",
                         "--send-keys", "esc",
                         "--send-keys-at", "1700") 2600
  Check ($esc.Values["popup"] -eq "none") "Escape closes the right-click menu"
  CheckHas $esc.Log "cmd: escape=popup" "Escape closes exactly the popup layer"

  # --- 7. Without an image only Settings is selectable -----------------------
  $empty = RunDump "empty" @("--context-menu", "640,400",
                             "--send-keys", "down,enter",
                             "--send-keys-at", "1700") 3400 -NoImage
  CheckHas $empty.Log "ui: menu_hover=10" "Down skips the disabled rows when there is no image"
  Check ($empty.Values["settings_open"] -eq "1") "Settings stays usable without an image"

  # --- 8. A real right-button click opens the menu ---------------------------
  $real = StartLive "real" @() 4500
  Start-Sleep -Milliseconds 1500
  RightClick $real 500 500
  $realValues = WaitDump $real
  $realMenu = RectOf $realValues "menu"
  Check ($realValues["popup"] -eq "context") "a real right-click opens the self-drawn menu"
  Check ([math]::Abs($realMenu.L - 500) -le 1) ("the real click anchors the menu at x=500 (got {0})" -f $realMenu.L)
  CheckHas (LogText $real) "ui: context_menu x=500 y=500" "the real right-click goes through WM_CONTEXTMENU"
  EndLive $real

  # --- 9. Clicking outside closes the menu without running a command --------
  $outside = StartLive "outside" @("--context-menu", "640,400") 4500
  Start-Sleep -Milliseconds 1600
  ClickLeft $outside 200 600
  $outsideValues = WaitDump $outside
  Check ($outsideValues["popup"] -eq "none") "clicking outside closes the menu"
  CheckNotHas (LogText $outside) "cmd: menu=context." "the outside click runs no menu command"
  EndLive $outside

  # --- 10. Clicking a row runs its command (Settings) -----------------------
  # The row rectangle comes from the identical static case 1 geometry.
  $row10 = RectOf $v "menu_row_10"
  $rowClick = StartLive "row" @("--context-menu", "640,400") 4500
  Start-Sleep -Milliseconds 1600
  ClickLeft $rowClick (($row10.L + $row10.R) / 2.0) (($row10.T + $row10.B) / 2.0)
  $rowValues = WaitDump $rowClick
  Check ($rowValues["popup"] -eq "none") "clicking a row closes the menu"
  Check ($rowValues["settings_open"] -eq "1") "clicking the Settings row opens the settings window"
  CheckHas (LogText $rowClick) "cmd: menu=context.settings" "the row click runs its command once"
  EndLive $rowClick

  # --- 11. Pixel evidence: the menu paints inside its panel only ------------
  $shots = Join-Path $repo "tests\output\context-menu"
  New-Item -ItemType Directory -Force -Path $shots | Out-Null
  $plainShot = Join-Path $shots "plain.png"
  $menuShot = Join-Path $shots "menu.png"
  $hcShot = Join-Path $shots "menu-hc.png"
  foreach ($shot in @($plainShot, $menuShot, $hcShot)) { Remove-Item $shot -ErrorAction SilentlyContinue }
  [CtxNative]::SetCursorPos(5, 5) | Out-Null
  $plainProcess = Start-Process -FilePath $appExe -ArgumentList @(
    "--portable", "--language", "en", "--screenshot", $plainShot,
    "--shot-delay", "1800", $image) -PassThru
  if (-not $plainProcess.WaitForExit(30000)) { $plainProcess.Kill(); throw "plain screenshot did not exit" }
  [CtxNative]::SetCursorPos(5, 5) | Out-Null
  $menuProcess = Start-Process -FilePath $appExe -ArgumentList @(
    "--portable", "--language", "en", "--context-menu", "640,400",
    "--screenshot", $menuShot, "--shot-delay", "1800", $image) -PassThru
  if (-not $menuProcess.WaitForExit(30000)) { $menuProcess.Kill(); throw "menu screenshot did not exit" }
  [CtxNative]::SetCursorPos(5, 5) | Out-Null
  $hcProcess = Start-Process -FilePath $appExe -ArgumentList @(
    "--portable", "--language", "en", "--force-high-contrast",
    "--context-menu", "640,400", "--screenshot", $hcShot,
    "--shot-delay", "1800", $image) -PassThru
  if (-not $hcProcess.WaitForExit(30000)) { $hcProcess.Kill(); throw "hc screenshot did not exit" }
  Start-Sleep -Milliseconds 200
  Check (Test-Path $plainShot) "the plain screenshot is written"
  Check (Test-Path $menuShot) "the menu screenshot is written"
  Check (Test-Path $hcShot) "the high-contrast menu screenshot is written"
  if ((Test-Path $plainShot) -and (Test-Path $menuShot)) {
    $plain = [System.Drawing.Bitmap]::new($plainShot)
    $withMenu = [System.Drawing.Bitmap]::new($menuShot)
    try {
      $insidePlain = $plain.GetPixel(800, 500).ToArgb()
      $insideMenu = $withMenu.GetPixel(800, 500).ToArgb()
      $outsidePlain = $plain.GetPixel(100, 700).ToArgb()
      $outsideMenu = $withMenu.GetPixel(100, 700).ToArgb()
      Check ($insidePlain -ne $insideMenu) "the menu panel paints over the picture"
      Check ($outsidePlain -eq $outsideMenu) "nothing outside the menu panel changes"
    } finally {
      $plain.Dispose()
      $withMenu.Dispose()
    }
  }
  if (Test-Path $hcShot) {
    $hc = [System.Drawing.Bitmap]::new($hcShot)
    try {
      Check ($hc.Width -ge 1280) "the high-contrast menu screenshot is a full frame"
    } finally {
      $hc.Dispose()
    }
  }

  # --- 12. The native menu is gone ------------------------------------------
  $sourcePath = Join-Path $repo "src\app\main.cpp"
  $source = Get-Content $sourcePath -Raw
  Check (-not ($source -match 'CreatePopupMenu')) "CreatePopupMenu is gone from main.cpp"
  Check (-not ($source -match 'TrackPopupMenu')) "TrackPopupMenu is gone from main.cpp"
} finally {
  if (-not $KeepOutput) {
    Remove-Item $work -Recurse -Force -ErrorAction SilentlyContinue
  } else {
    Write-Host ("Output kept in {0}" -f $work)
  }
}

Write-Host ""
if ($script:failures -eq 0) {
  Write-Host ("OK: {0} context-menu checks passed" -f $script:checks) -ForegroundColor Green
} else {
  Write-Host ("FAILED: {0} of {1} context-menu checks failed" -f $script:failures, $script:checks) -ForegroundColor Red
  exit 1
}
