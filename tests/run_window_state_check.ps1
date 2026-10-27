# v2.0 S3 (plan 22/15): window placement persistence, maximized/fullscreen/
# minimized restore rules, always-on-top around fullscreen and the off-screen
# fallback.
#
# v3.0 (plan 26 section 2.6) adds real-mouse maximize/restore cases: the
# maximize button returns HTMAXBUTTON, so a real click never produces the
# client messages --click-hit posts.
#
# A throw-away copy of the executable runs in portable mode, so settings.ini,
# logs and caches live in the temp directory. The placement assertions read the
# window_rect/work_areas keys that --layout-dump now reports (physical pixels).
#
# Everything here is ASCII (pitfall 60/124).

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

$work = Join-Path $env:TEMP ("nv-window-" + [guid]::NewGuid().ToString("N"))
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

function LogPath() {
  Join-Path $work "logs\zoreview.log"
}

function RunApp([string]$label, [string[]]$arguments, [int]$timeout = 30000) {
  $logPath = LogPath
  Remove-Item $logPath -ErrorAction SilentlyContinue
  $all = @("--portable") + $arguments
  $process = Start-Process -FilePath $appExe -ArgumentList $all -PassThru
  if (-not $process.WaitForExit($timeout)) {
    $process.Kill()
    throw "zoreview.exe did not exit in time: $label"
  }
  Start-Sleep -Milliseconds 100
  if (Test-Path $logPath) { return (Get-Content $logPath -Raw) }
  return ""
}

function ReadValues([string]$path) {
  $values = @{}
  if (-not (Test-Path $path)) { return $values }
  foreach ($line in Get-Content $path) {
    $parts = $line.Split("=", 2)
    if ($parts.Count -eq 2) { $values[$parts[0]] = $parts[1] }
  }
  return $values
}

function WriteSettings([string]$path, [hashtable]$placement, [bool]$remember = $true, [bool]$topmost = $false) {
  $lines = @(
    "[general]",
    "version=1",
    "language=en",
    "remember_window=$(if ($remember) { 1 } else { 0 })",
    "always_on_top=$(if ($topmost) { 1 } else { 0 })",
    "single_instance=0",
    "")
  if ($null -ne $placement) {
    $lines += @(
      "[window]",
      "x=$($placement.X)",
      "y=$($placement.Y)",
      "w=$($placement.W)",
      "h=$($placement.H)",
      "maximized=$(if ($placement.Maximized) { 1 } else { 0 })",
      "saved=1",
      "")
  }
  Set-Content -Path $path -Encoding ASCII -Value $lines
}

$settingsIni = Join-Path $work "settings.ini"
$layoutPath = Join-Path $work "layout.txt"

function Layout([string]$label, [hashtable]$placement, [string[]]$extra = @(), [bool]$remember = $true, [bool]$topmost = $false) {
  WriteSettings $settingsIni $placement $remember $topmost
  Remove-Item $layoutPath -ErrorAction SilentlyContinue
  $log = RunApp $label (@("--layout-dump", $layoutPath) + $extra)
  if (-not (Test-Path $layoutPath)) { throw "no layout dump for $label" }
  return @{ Values = (ReadValues $layoutPath); Log = $log }
}

function Printed([string]$label) {
  $printPath = Join-Path $work "print.txt"
  Remove-Item $printPath -ErrorAction SilentlyContinue
  $log = RunApp $label @("--print-settings", $printPath)
  return @{ Values = (ReadValues $printPath); Log = $log }
}

function Rect([string]$text) {
  $n = $text.Split(",") | ForEach-Object { [int]$_ }
  if ($n.Count -ne 4) { return $null }
  return [pscustomobject]@{ L = $n[0]; T = $n[1]; R = $n[2]; B = $n[3]; W = $n[2] - $n[0]; H = $n[3] - $n[1] }
}

function WorkAreas([hashtable]$values) {
  $result = @()
  if (-not $values.ContainsKey("work_areas") -or [string]::IsNullOrEmpty($values["work_areas"])) { return $result }
  foreach ($item in $values["work_areas"].Split(";")) {
    $rect = Rect $item
    if ($null -ne $rect) { $result += $rect }
  }
  return $result
}

function InsideAny([object]$rect, [object[]]$areas) {
  foreach ($area in $areas) {
    if ($rect.L -ge $area.L -and $rect.T -ge $area.T -and
        $rect.R -le $area.R -and $rect.B -le $area.B) { return $true }
  }
  return $false
}

# --- Samples ----------------------------------------------------------------
Add-Type -AssemblyName System.Drawing
$bitmap = New-Object System.Drawing.Bitmap(640, 480)
$graphics = [System.Drawing.Graphics]::FromImage($bitmap)
$graphics.Clear([System.Drawing.Color]::FromArgb(30, 120, 80))
$graphics.Dispose()
$image = Join-Path $work "sample.bmp"
$bitmap.Save($image, [System.Drawing.Imaging.ImageFormat]::Bmp)
$bitmap.Dispose()

# --- Real-mouse helpers (v3.0, plan 26 section 2.6) -------------------------
# The maximize button is HTMAXBUTTON so Windows 11 can show snap layouts. That
# means a real caption click never produces the client messages --click-hit
# posts, so this section drives the actual mouse. The harness is per-monitor
# DPI aware (pitfall 139) and parks the pointer away from the window before
# every launch (pitfall 304).
if (-not ("WinStateNative" -as [type])) {
  Add-Type @"
using System;
using System.Runtime.InteropServices;
public static class WinStateNative {
  [DllImport("user32.dll")] public static extern bool SetProcessDpiAwarenessContext(IntPtr v);
  [DllImport("user32.dll")] public static extern bool SetCursorPos(int x, int y);
  [DllImport("user32.dll")] public static extern void mouse_event(uint f, uint dx, uint dy, uint d, UIntPtr e);
  [DllImport("user32.dll")] public static extern bool ClientToScreen(IntPtr h, ref POINT p);
  [DllImport("user32.dll")] public static extern bool GetWindowRect(IntPtr h, out RECT r);
  [DllImport("user32.dll")] public static extern bool GetClientRect(IntPtr h, out RECT r);
  [DllImport("user32.dll")] public static extern bool IsZoomed(IntPtr h);
  [DllImport("user32.dll")] public static extern bool IsIconic(IntPtr h);
  [DllImport("user32.dll")] public static extern bool SetForegroundWindow(IntPtr h);
  [DllImport("user32.dll")] public static extern IntPtr MonitorFromWindow(IntPtr h, uint flags);
  [DllImport("user32.dll")] public static extern bool GetMonitorInfoW(IntPtr m, ref MONITORINFO i);
  [StructLayout(LayoutKind.Sequential)] public struct RECT { public int Left, Top, Right, Bottom; }
  [StructLayout(LayoutKind.Sequential)] public struct POINT { public int X, Y; }
  [StructLayout(LayoutKind.Sequential)] public struct MONITORINFO { public int cbSize; public RECT rcMonitor; public RECT rcWork; public uint dwFlags; }
}
"@
}
[WinStateNative]::SetProcessDpiAwarenessContext([IntPtr](-4)) | Out-Null

# Starts a live window: it writes one layout dump at $dumpAt and then keeps
# running so the caller can drive real input.
function StartLive([string]$label, [int]$dumpAt = 2600) {
  [WinStateNative]::SetCursorPos(5, 5) | Out-Null
  Remove-Item $settingsIni -ErrorAction SilentlyContinue
  $log = LogPath
  Remove-Item $log -ErrorAction SilentlyContinue
  $dump = Join-Path $work ("live-" + $label + ".txt")
  Remove-Item $dump -ErrorAction SilentlyContinue
  $arguments = @("--portable", "--language", "en", "--layout-dump", $dump,
                 "--dump-at", "$dumpAt") + @($image)
  $process = Start-Process -FilePath $appExe -ArgumentList $arguments -PassThru
  $hwnd = [IntPtr]::Zero
  for ($i = 0; $i -lt 100; $i++) {
    Start-Sleep -Milliseconds 100
    $process.Refresh()
    if ($process.MainWindowHandle -ne [IntPtr]::Zero) { $hwnd = $process.MainWindowHandle; break }
  }
  if ($hwnd -eq [IntPtr]::Zero) { $process.Kill(); throw "no window for $label" }
  return @{ Proc = $process; Hwnd = $hwnd; Dump = $dump; Log = $log }
}

function WaitLiveDump($case, [int]$timeoutMs = 12000) {
  for ($i = 0; $i -lt ($timeoutMs / 100); $i++) {
    Start-Sleep -Milliseconds 100
    if (Test-Path $case.Dump) { break }
  }
  Start-Sleep -Milliseconds 200
  return (ReadValues $case.Dump)
}

function LiveLogText($case) {
  if (Test-Path $case.Log) { return (Get-Content $case.Log -Raw) }
  return ""
}

function Move-Client($case, [double]$x, [double]$y) {
  $point = New-Object WinStateNative+POINT
  $point.X = [int][math]::Round($x)
  $point.Y = [int][math]::Round($y)
  [WinStateNative]::ClientToScreen($case.Hwnd, [ref]$point) | Out-Null
  [WinStateNative]::SetCursorPos($point.X, $point.Y) | Out-Null
  Start-Sleep -Milliseconds 150
}

function Mouse-Down() { [WinStateNative]::mouse_event(0x0002, 0, 0, 0, [UIntPtr]::Zero) }
function Mouse-Up() { [WinStateNative]::mouse_event(0x0004, 0, 0, 0, [UIntPtr]::Zero) }

function Move-Screen([int]$x, [int]$y) {
  [WinStateNative]::SetCursorPos($x, $y) | Out-Null
  Start-Sleep -Milliseconds 150
}

function Click-Client($case, [double]$x, [double]$y) {
  [WinStateNative]::SetForegroundWindow($case.Hwnd) | Out-Null
  Start-Sleep -Milliseconds 150
  Move-Client $case $x $y
  Mouse-Down
  Start-Sleep -Milliseconds 60
  Mouse-Up
  Start-Sleep -Milliseconds 700
}

function DoubleClick-Client($case, [double]$x, [double]$y) {
  [WinStateNative]::SetForegroundWindow($case.Hwnd) | Out-Null
  Start-Sleep -Milliseconds 150
  Move-Client $case $x $y
  Mouse-Down
  Start-Sleep -Milliseconds 40
  Mouse-Up
  Start-Sleep -Milliseconds 70
  Mouse-Down
  Start-Sleep -Milliseconds 40
  Mouse-Up
  Start-Sleep -Milliseconds 700
}

function WindowRect($case) {
  $rect = New-Object WinStateNative+RECT
  [WinStateNative]::GetWindowRect($case.Hwnd, [ref]$rect) | Out-Null
  return $rect
}

function ClientSize($case) {
  $rect = New-Object WinStateNative+RECT
  [WinStateNative]::GetClientRect($case.Hwnd, [ref]$rect) | Out-Null
  return @{ W = $rect.Right - $rect.Left; H = $rect.Bottom - $rect.Top }
}

function WorkArea($case) {
  $monitor = [WinStateNative]::MonitorFromWindow($case.Hwnd, 2)
  $info = New-Object WinStateNative+MONITORINFO
  $info.cbSize = [System.Runtime.InteropServices.Marshal]::SizeOf($info)
  [WinStateNative]::GetMonitorInfoW($monitor, [ref]$info) | Out-Null
  return @{ W = $info.rcWork.Right - $info.rcWork.Left; H = $info.rcWork.Bottom - $info.rcWork.Top }
}

function EndLive($case) {
  $case.Proc.Refresh()
  if (-not $case.Proc.HasExited) {
    $null = $case.Proc.CloseMainWindow()
    if (-not $case.Proc.WaitForExit(5000)) { $case.Proc.Kill() }
  }
  Start-Sleep -Milliseconds 250
}

try {
  # --- 1. Layout dump reports the physical window rectangle -----------------
  $base = Layout "base" @{ X = 120; Y = 90; W = 1000; H = 700; Maximized = $false }
  $rect = Rect $base.Values["window_rect"]
  $areas = @(WorkAreas $base.Values)
  Check ($null -ne $rect) "the layout dump reports window_rect"
  Check ($areas.Count -ge 1) "the layout dump reports work_areas"
  CheckHas $base.Log "settings: window restored" "restoring a placement is logged"

  # --- 2. A valid placement is restored exactly ----------------------------
  Check ($rect.L -eq 120 -and $rect.T -eq 90 -and $rect.W -eq 1000 -and $rect.H -eq 700) "the saved rectangle is restored exactly"

  # --- 3. remember_window=0 starts centred, not at the saved corner --------
  $forgotten = Layout "remember-off" @{ X = 120; Y = 90; W = 1000; H = 700; Maximized = $false } @() $false
  $forgottenRect = Rect $forgotten.Values["window_rect"]
  $forgottenAreas = @(WorkAreas $forgotten.Values)
  Check (-not ($forgottenRect.L -eq 120 -and $forgottenRect.T -eq 90)) "remember-window off ignores the stored rectangle"
  Check (InsideAny $forgottenRect $forgottenAreas) "the centred window is fully inside a work area"

  # --- 4. An off-screen placement falls back to the centred default --------
  $offscreen = Layout "offscreen" @{ X = 4000; Y = 3000; W = 1000; H = 700; Maximized = $false }
  $offscreenRect = Rect $offscreen.Values["window_rect"]
  $offscreenAreas = @(WorkAreas $offscreen.Values)
  Check (-not ($offscreenRect.L -eq 4000)) "an off-screen rectangle is not restored"
  Check (InsideAny $offscreenRect $offscreenAreas) "the fallback window is fully inside a work area"
  CheckHas $offscreen.Log "settings: window placement invalid" "the off-screen fallback is logged"

  # --- 5. A partially visible placement is clamped into the work area ------
  $clamped = Layout "clamped" @{ X = -100; Y = 50; W = 1000; H = 700; Maximized = $false }
  $clampedRect = Rect $clamped.Values["window_rect"]
  $clampedAreas = @(WorkAreas $clamped.Values)
  Check ($clampedRect.W -eq 1000 -and $clampedRect.H -eq 700) "the clamped window keeps its size"
  Check ($clampedRect.L -ge 0) "the clamped window is not left of the work area"
  Check (InsideAny $clampedRect $clampedAreas) "the clamped window is fully inside a work area"

  # --- 6. A maximized placement restores maximized -------------------------
  $maximized = Layout "maximized" @{ X = 140; Y = 110; W = 900; H = 650; Maximized = $true }
  Check ($maximized.Values["state"] -eq "maximized") "a maximized placement restores maximized"
  Check ($maximized.Values["maximized"] -eq "1") "the layout reports maximized=1"

  # --- 7. WM_EXITSIZEMOVE saves the new rectangle --------------------------
  # No graceful close: only the resize notification can have written this.
  WriteSettings $settingsIni @{ X = 10; Y = 10; W = 800; H = 600; Maximized = $false }
  $resizeProcess = Start-Process -FilePath $appExe -ArgumentList @("--portable", "--move-window", "220,160,880,620", $image) -PassThru
  Start-Sleep -Seconds 3
  if (-not $resizeProcess.HasExited) { $resizeProcess.Kill() }
  $null = $resizeProcess.WaitForExit(8000)
  $resized = Printed "exitsize"
  Check ($resized.Values["window"] -eq "220,160,880x620") "WM_EXITSIZEMOVE stores the resized rectangle"
  Check ($resized.Values["window_saved"] -eq "1") "the resized rectangle is marked saved"

  # --- 8. Minimized close keeps the previous placement --------------------
  $minPlacement = @{ X = 150; Y = 120; W = 950; H = 650; Maximized = $false }
  WriteSettings $settingsIni $minPlacement
  $null = RunApp "minimized" @("--minimize-at", "1200", "--quit-at", "2600", $image)
  $afterMin = Printed "after-minimized"
  Check ($afterMin.Values["window"] -eq "150,120,950x650") "a minimized close does not overwrite the placement"

  # --- 9. Fullscreen close keeps the previous placement -------------------
  WriteSettings $settingsIni $minPlacement
  $null = RunApp "fullscreen" @("--fullscreen", "--quit-at", "2200", $image)
  $afterFull = Printed "after-fullscreen"
  Check ($afterFull.Values["window"] -eq "150,120,950x650") "a fullscreen close does not overwrite the placement"

  # --- 10. Always-on-top is suspended for fullscreen and restored after ----
  WriteSettings $settingsIni $minPlacement $true $true
  Remove-Item (LogPath) -ErrorAction SilentlyContinue
  $topLog = RunApp "topmost-fullscreen" @("--send-keys", "F11,F11", "--quit-at", "5000", $image)
  CheckHas $topLog "ui: topmost=1" "the saved always-on-top flag is applied"
  CheckHas $topLog "ui: topmost=0 (fullscreen)" "entering fullscreen drops the topmost band"
  CheckHas $topLog "ui: topmost=1 (fullscreen exit)" "leaving fullscreen restores the topmost band"

  # --- 11. A real caption click on maximize (v3.0, plan 26 section 2.6) ----
  # The maximize button returns HTMAXBUTTON, so a real click is a non-client
  # caption click: the client-only --click-hit path cannot see it. The press is
  # captured by the app and the client release dispatches the command.
  $live = StartLive "maximize"
  $liveValues = WaitLiveDump $live
  $liveButtons = $liveValues["buttons"].Split("|")
  $maxButton = Rect $liveButtons[3]
  $beforeRect = WindowRect $live
  $beforeClient = ClientSize $live
  $maxCenterX = ($maxButton.L + $maxButton.R) / 2.0
  $offsetFromRight = $beforeClient.W - $maxCenterX
  Click-Client $live $maxCenterX (($maxButton.T + $maxButton.B) / 2.0)
  Check ([WinStateNative]::IsZoomed($live.Hwnd)) "a real click on maximize maximizes the window"
  $maxLog = LiveLogText $live
  CheckHas $maxLog "cmd: id=title.maximize" "the real maximize click runs the normal command"
  CheckHas $maxLog "window: state=maximized" "the maximized state is recorded"
  $maxClient = ClientSize $live
  $workArea = WorkArea $live
  Check ($maxClient.W -eq $workArea.W -and $maxClient.H -eq $workArea.H) "the maximized client fills the work area"
  $maximizedRect = WindowRect $live
  Check (($maximizedRect.Right - $maximizedRect.Left) -gt ($beforeRect.Right - $beforeRect.Left)) "the maximized window is wider than the restored one"

  # A second real click restores; the button keeps its distance from the right
  # edge, so the maximized client width gives the restore point.
  Click-Client $live ($maxClient.W - $offsetFromRight) (($maxButton.T + $maxButton.B) / 2.0)
  Check (-not [WinStateNative]::IsZoomed($live.Hwnd)) "a second real click restores the window"
  $restoredRect = WindowRect $live
  Check ([math]::Abs($restoredRect.Left - $beforeRect.Left) -le 1 -and
         [math]::Abs($restoredRect.Top - $beforeRect.Top) -le 1 -and
         [math]::Abs($restoredRect.Right - $beforeRect.Right) -le 1 -and
         [math]::Abs($restoredRect.Bottom - $beforeRect.Bottom) -le 1) "restoring returns to the previous rectangle"
  CheckHas (LiveLogText $live) "window: state=normal" "the restored state is recorded"
  EndLive $live

  # --- 12. Releasing off the button cancels the press ----------------------
  $cancel = StartLive "cancel"
  $cancelValues = WaitLiveDump $cancel
  $cancelButtons = $cancelValues["buttons"].Split("|")
  $cancelButton = Rect $cancelButtons[3]
  $cancelCenterX = ($cancelButton.L + $cancelButton.R) / 2.0
  [WinStateNative]::SetForegroundWindow($cancel.Hwnd) | Out-Null
  Start-Sleep -Milliseconds 150
  Move-Client $cancel $cancelCenterX (($cancelButton.T + $cancelButton.B) / 2.0)
  Mouse-Down
  Start-Sleep -Milliseconds 120
  Move-Client $cancel ($cancelCenterX - 120) (($cancelButton.T + $cancelButton.B) / 2.0)
  Start-Sleep -Milliseconds 120
  Mouse-Up
  Start-Sleep -Milliseconds 600
  Check (-not [WinStateNative]::IsZoomed($cancel.Hwnd)) "releasing off the maximize button does not maximize"
  Check (-not ((LiveLogText $cancel) -match [regex]::Escape("cmd: id=title.maximize"))) "the cancelled press runs no command"
  EndLive $cancel

  # --- 13. Double-clicking the title bar still maximizes and restores ------
  $double = StartLive "double"
  $doubleValues = WaitLiveDump $double
  $doubleClientW = [double]$doubleValues["client"].Split("x")[0]
  DoubleClick-Client $double ($doubleClientW / 2.0) 16
  Check ([WinStateNative]::IsZoomed($double.Hwnd)) "double-clicking the title bar still maximizes"
  $doubleMaxW = (ClientSize $double).W
  DoubleClick-Client $double ($doubleMaxW / 2.0) 16
  Check (-not [WinStateNative]::IsZoomed($double.Hwnd)) "double-clicking the maximized title bar restores"
  EndLive $double

  # --- 14. Minimize and drag regressions -----------------------------------
  $minimize = StartLive "minimize"
  $minimizeValues = WaitLiveDump $minimize
  $minimizeButton = Rect ($minimizeValues["buttons"].Split("|")[2])
  Click-Client $minimize (($minimizeButton.L + $minimizeButton.R) / 2.0) (($minimizeButton.T + $minimizeButton.B) / 2.0)
  Check ([WinStateNative]::IsIconic($minimize.Hwnd)) "a real click on minimize still minimizes"
  CheckHas (LiveLogText $minimize) "cmd: id=title.minimize" "the minimize click keeps its command"
  EndLive $minimize

  $drag = StartLive "drag"
  $dragValues = WaitLiveDump $drag
  $dragClientW = [double]$dragValues["client"].Split("x")[0]
  $dragBefore = WindowRect $drag
  [WinStateNative]::SetForegroundWindow($drag.Hwnd) | Out-Null
  Start-Sleep -Milliseconds 150
  # Drive the drag in screen coordinates: the window moves under the cursor, so
  # client coordinates would chase it and multiply the distance.
  $dragStart = New-Object WinStateNative+POINT
  $dragStart.X = [int]($dragClientW / 2.0)
  $dragStart.Y = 16
  [WinStateNative]::ClientToScreen($drag.Hwnd, [ref]$dragStart) | Out-Null
  Move-Screen $dragStart.X $dragStart.Y
  Mouse-Down
  Start-Sleep -Milliseconds 200
  for ($step = 1; $step -le 4; $step++) {
    Move-Screen ($dragStart.X + 15 * $step) $dragStart.Y
  }
  Start-Sleep -Milliseconds 200
  Mouse-Up
  Start-Sleep -Milliseconds 700
  $dragAfter = WindowRect $drag
  Check ([math]::Abs(($dragAfter.Left - $dragBefore.Left) - 60) -le 12) "dragging the title bar still moves the window"
  Check (($dragAfter.Right - $dragAfter.Left) -eq ($dragBefore.Right - $dragBefore.Left)) "dragging the title bar keeps the width"
  EndLive $drag

  # --- 15. Hover alone never maximizes --------------------------------------
  $hover = StartLive "hover"
  $hoverValues = WaitLiveDump $hover
  $hoverButton = Rect ($hoverValues["buttons"].Split("|")[3])
  [WinStateNative]::SetForegroundWindow($hover.Hwnd) | Out-Null
  Start-Sleep -Milliseconds 150
  Move-Client $hover (($hoverButton.L + $hoverButton.R) / 2.0) (($hoverButton.T + $hoverButton.B) / 2.0)
  Start-Sleep -Milliseconds 900
  Check (-not [WinStateNative]::IsZoomed($hover.Hwnd)) "hovering the maximize button does not maximize"
  Check (-not ((LiveLogText $hover) -match [regex]::Escape("cmd: id=title.maximize"))) "hovering runs no maximize command"
  EndLive $hover

  # --- 16. The synthetic --click-hit path is unchanged ----------------------
  $synthetic = Layout "synthetic-maximize" $null @("--click-hit", "title.maximize")
  Check ($synthetic.Values["state"] -eq "maximized") "the synthetic --click-hit path still maximizes"
} finally {
  if (-not $KeepOutput) {
    Remove-Item $work -Recurse -Force -ErrorAction SilentlyContinue
  } else {
    Write-Host ("Output kept in {0}" -f $work)
  }
}

Write-Host ""
if ($script:failures -eq 0) {
  Write-Host ("OK: {0} window-state checks passed" -f $script:checks) -ForegroundColor Green
} else {
  Write-Host ("FAILED: {0} of {1} window-state checks failed" -f $script:failures, $script:checks) -ForegroundColor Red
  exit 1
}
