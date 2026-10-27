# v3.0 (規劃/26 §2.7): fullscreen reveal strip (#9) and the fullscreen
# thumbnail rail (#10).
#
# Real mouse input is required for the reveal: the app reveals on WM_MOUSEMOVE,
# and the 3 s idle deadline must be measured against real movements. The
# harness is per-monitor DPI aware (pitfall 139) and parks the pointer away
# from the window before every launch (pitfall 304).

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

$work = Join-Path $env:TEMP ("nv-fullscreen-" + [guid]::NewGuid().ToString("N"))
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

Add-Type -AssemblyName System.Drawing
Add-Type @"
using System;
using System.Runtime.InteropServices;
public static class FsNative {
  [DllImport("user32.dll")] public static extern bool SetProcessDpiAwarenessContext(IntPtr v);
  [DllImport("user32.dll")] public static extern bool SetCursorPos(int x, int y);
  [DllImport("user32.dll")] public static extern void mouse_event(uint f, uint dx, uint dy, uint d, UIntPtr e);
  [DllImport("user32.dll")] public static extern bool GetClientRect(IntPtr h, out RECT r);
  [DllImport("user32.dll")] public static extern bool ClientToScreen(IntPtr h, ref POINT p);
  [StructLayout(LayoutKind.Sequential)] public struct RECT { public int Left, Top, Right, Bottom; }
  [StructLayout(LayoutKind.Sequential)] public struct POINT { public int X, Y; }
}
"@
[FsNative]::SetProcessDpiAwarenessContext([IntPtr](-4)) | Out-Null

$bitmap = New-Object System.Drawing.Bitmap(800, 600)
$graphics = [System.Drawing.Graphics]::FromImage($bitmap)
$graphics.Clear([System.Drawing.Color]::FromArgb(40, 90, 160))
$graphics.Dispose()
$firstImage = Join-Path $work "sample.bmp"
$bitmap.Save($firstImage, [System.Drawing.Imaging.ImageFormat]::Bmp)
$bitmap.Dispose()

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
  $n = $values[$key].Split(",") | ForEach-Object { [double]$_ }
  if ($n.Count -ne 4) { return $null }
  return @{ L = $n[0]; T = $n[1]; R = $n[2]; B = $n[3]; W = $n[2] - $n[0]; H = $n[3] - $n[1] }
}
function Inside([hashtable]$inner, [hashtable]$outer) {
  if ($null -eq $inner -or $null -eq $outer) { return $false }
  return ($inner.L -ge $outer.L - 0.5) -and ($inner.T -ge $outer.T - 0.5) -and
         ($inner.R -le $outer.R + 0.5) -and ($inner.B -le $outer.B + 0.5)
}
# The dump reports the client rectangle as "WxH"; it always starts at 0,0.
function ClientRect([hashtable]$values) {
  if (-not $values.ContainsKey("client")) { return $null }
  $parts = $values["client"].Split("x")
  if ($parts.Count -ne 2) { return $null }
  $w = [double]$parts[0]; $h = [double]$parts[1]
  return @{ L = 0; T = 0; R = $w; B = $h; W = $w; H = $h }
}

# Starts one run. `--dump-at` writes the layout without exiting; the caller then
# drives real input before the dump lands.
function Start-Case([string]$label, [string[]]$extra, [int]$dumpAt) {
  [FsNative]::SetCursorPos(5, 5) | Out-Null
  Remove-Item (Join-Path $work "settings.ini") -ErrorAction SilentlyContinue
  $log = Join-Path $work "logs\zoreview.log"
  Remove-Item $log -ErrorAction SilentlyContinue
  $dump = Join-Path $work ("dump-" + $label + ".txt")
  Remove-Item $dump -ErrorAction SilentlyContinue
  $arguments = @("--portable", "--language", "en", "--layout-dump", $dump,
                 "--dump-at", "$dumpAt") + $extra + @($firstImage)
  $process = Start-Process -FilePath $appExe -ArgumentList $arguments -PassThru
  $hwnd = [IntPtr]::Zero
  for ($i = 0; $i -lt 80; $i++) {
    Start-Sleep -Milliseconds 100
    $process.Refresh()
    if ($process.MainWindowHandle -ne [IntPtr]::Zero) { $hwnd = $process.MainWindowHandle; break }
  }
  if ($hwnd -eq [IntPtr]::Zero) { $process.Kill(); throw "$label no window" }
  return @{ Proc = $process; Hwnd = $hwnd; Dump = $dump; Log = $log }
}

function MoveTo($case, [double]$x, [double]$y) {
  $point = New-Object FsNative+POINT
  $point.X = [int][math]::Round($x); $point.Y = [int][math]::Round($y)
  [FsNative]::ClientToScreen($case.Hwnd, [ref]$point) | Out-Null
  [FsNative]::SetCursorPos($point.X, $point.Y) | Out-Null
  Start-Sleep -Milliseconds 120
}
function Click() {
  [FsNative]::mouse_event(0x0002, 0, 0, 0, [UIntPtr]::Zero)
  Start-Sleep -Milliseconds 50
  [FsNative]::mouse_event(0x0004, 0, 0, 0, [UIntPtr]::Zero)
}
function Wait-Dump($case, [int]$timeoutMs = 8000) {
  for ($i = 0; $i -lt ($timeoutMs / 100); $i++) {
    Start-Sleep -Milliseconds 100
    if (Test-Path $case.Dump) { break }
  }
  Start-Sleep -Milliseconds 150
  return (ReadDump $case.Dump)
}
function End-Case($case) {
  $case.Proc.Refresh()
  if (-not $case.Proc.HasExited) {
    # Close the window with a posted WM_CLOSE so the app exits cleanly.
    [void]$case.Proc.CloseMainWindow()
    if (-not $case.Proc.WaitForExit(6000)) { $case.Proc.Kill() }
  }
  Start-Sleep -Milliseconds 250
}
function LogText($case) {
  if (Test-Path $case.Log) { return (Get-Content $case.Log -Raw) }
  return ""
}

try {
  # --- 1. Entering fullscreen reveals the pair for 3 s ---------------------
  $enter = Start-Case "enter" @("--fullscreen") 3200
  Start-Sleep -Milliseconds 1500
  $v = Wait-Dump $enter
  Check ($v["fullscreen"] -eq "1") "the window is fullscreen"
  Check ($v["fs_reveal"] -eq "1") "entering fullscreen reveals the exit pair"
  Check ([int]$v["fs_reveal_opacity"] -ge 50) "the reveal faded in"
  $exitRect = Rect $v "fs_exit"
  $closeRect = Rect $v "fs_close"
  $client = ClientRect $v
  Check ($null -ne $exitRect -and $exitRect.W -gt 0) "the exit button is laid out"
  Check ($null -ne $closeRect -and $closeRect.W -gt 0) "the close button is laid out"
  Check (Inside $exitRect $client) "the exit button is inside the client"
  Check (Inside $closeRect $client) "the close button is inside the client"
  Check ($closeRect.R -le $client.R + 0.5 -and $closeRect.T -le 20.0) "the pair sits in the top-right corner"
  Check ($v["strip_shown"] -eq "0") "the auto rail stays hidden on entry"
  CheckHas (LogText $enter) "ui: fs_reveal=show" "the reveal is logged"
  End-Case $enter

  # --- 2. The pair fades out 3 s after the last move inside it ------------
  $hide = Start-Case "hide" @("--fullscreen") 6500
  Start-Sleep -Milliseconds 1200
  $corner = Rect (ReadDump $enter.Dump) "fs_exit"
  $clientHide = ClientRect (ReadDump $enter.Dump)
  MoveTo $hide (($corner.L + $corner.R) / 2.0) (($corner.T + $corner.B) / 2.0)
  Start-Sleep -Milliseconds 600
  MoveTo $hide (($clientHide.L + $clientHide.R) / 2.0) (($clientHide.T + $clientHide.B) / 2.0)
  $v = Wait-Dump $hide
  Check ($v["fullscreen"] -eq "1") "the window is still fullscreen"
  Check ($v["fs_reveal"] -eq "0") "the pair hid itself after the idle deadline"
  CheckHas (LogText $hide) "ui: fs_reveal=hide" "the automatic hide is logged"
  End-Case $hide

  # --- 3. The revealed exit button leaves fullscreen -----------------------
  $exit = Start-Case "exit" @("--fullscreen") 4500
  Start-Sleep -Milliseconds 1200
  $button = Rect (ReadDump $enter.Dump) "fs_exit"
  MoveTo $exit (($button.L + $button.R) / 2.0) (($button.T + $button.B) / 2.0)
  Start-Sleep -Milliseconds 500
  Click
  $v = Wait-Dump $exit
  Check ($v["fullscreen"] -eq "0") "clicking the revealed exit button leaves fullscreen"
  CheckHas (LogText $exit) "window: state=normal" "leaving fullscreen is logged"
  End-Case $exit

  # --- 4. Fullscreen rail: hidden at first, reveal-only afterwards --------
  $pinnedHidden = Start-Case "strip-hidden" @("--strip=pinned", "--fullscreen") 2600
  Start-Sleep -Milliseconds 1800
  $v = Wait-Dump $pinnedHidden
  Check ($v["fullscreen"] -eq "1") "the pinned run is fullscreen"
  Check ($v["strip_mode"] -eq "pinned") "the pinned mode is active"
  Check ($v["strip_shown"] -eq "0") "a pinned rail starts hidden in fullscreen (reveal-only)"
  Check ($v["strip_interactive"] -eq "0") "the hidden fullscreen rail owns no clicks"
  End-Case $pinnedHidden

  $pinnedReveal = Start-Case "strip-reveal" @("--strip=pinned", "--fullscreen") 3600
  Start-Sleep -Milliseconds 1200
  $layout = ReadDump $pinnedHidden.Dump
  $bottomClient = ClientRect $layout
  MoveTo $pinnedReveal (($bottomClient.L + $bottomClient.R) / 2.0) ($bottomClient.B - 40)
  $v = Wait-Dump $pinnedReveal
  Check ($v["strip_shown"] -eq "1") "moving to the bottom edge reveals the fullscreen rail"
  Check ($v["strip_interactive"] -eq "1") "the revealed fullscreen rail owns clicks"
  Check ($v["fullscreen"] -eq "1") "revealing the rail keeps fullscreen"
  End-Case $pinnedReveal

  # --- 5. Fullscreen screenshot contains the revealed pair ----------------
  $shot = Join-Path $repo "tests\output\fullscreen\reveal.png"
  New-Item -ItemType Directory -Force -Path (Split-Path -Parent $shot) | Out-Null
  Remove-Item $shot -ErrorAction SilentlyContinue
  [FsNative]::SetCursorPos(5, 5) | Out-Null
  $process = Start-Process -FilePath $appExe -ArgumentList @(
    "--portable", "--language", "en", "--fullscreen",
    "--screenshot", $shot, "--shot-delay", "1800", $firstImage) -PassThru
  if (-not $process.WaitForExit(30000)) { $process.Kill(); throw "screenshot run did not exit" }
  Start-Sleep -Milliseconds 150
  Check (Test-Path $shot) "the fullscreen screenshot is written"
  if (Test-Path $shot) {
    $image = [System.Drawing.Bitmap]::new($shot)
    try {
      Check ($image.Width -ge 1280) "the screenshot is a full-width frame"
    } finally {
      $image.Dispose()
    }
  }
} finally {
  if (-not $KeepOutput) {
    Remove-Item $work -Recurse -Force -ErrorAction SilentlyContinue
  } else {
    Write-Host ("Output kept in {0}" -f $work)
  }
}

Write-Host ""
if ($script:failures -eq 0) {
  Write-Host ("OK: {0} fullscreen checks passed" -f $script:checks) -ForegroundColor Green
} else {
  Write-Host ("FAILED: {0} of {1} fullscreen checks failed" -f $script:failures, $script:checks) -ForegroundColor Red
  exit 1
}
