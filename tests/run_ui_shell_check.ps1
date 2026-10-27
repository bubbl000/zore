# v2.0 S0: custom-drawn chrome + borderless window behaviour checks.
#
# Scriptable part of 規劃/22/01 §8:
#   - layout rectangles at 100% and 125% (title 32 DIP, bottom 44 DIP, strip 76 DIP)
#   - five title segments, five right-aligned buttons, close button flush right
#   - pinned / auto / off thumbnail rail and the content rectangle each implies
#   - borderless invariants: client == window when normal, client == work area
#     when maximized, client == monitor when fullscreen
#   - title-bar buttons: fullscreen toggle and close (real mouse input)
#   - Escape priority: fullscreen exits once, a second Escape does nothing
#   - bottom bar 适应 / 1:1 commands
#
# The harness itself must be per-monitor DPI aware, otherwise SetCursorPos and
# posted coordinates are virtualised and every click lands 1.25x off target.

param(
    [string]$App = '',
    [string]$Image = '',
    [string]$OutputDirectory = ''
)

$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot
if ($App -eq '') { $App = Join-Path $root 'build\Release\zoreview.exe' }
if ($Image -eq '') { $Image = Join-Path $root '..\测试文件夹\images\jpeg\photo_1600.jpg' }
if ($OutputDirectory -eq '') { $OutputDirectory = Join-Path $root 'tests\output\ui-shell' }
New-Item -ItemType Directory -Force -Path $OutputDirectory | Out-Null
$logPath = Join-Path $env:LOCALAPPDATA 'ZoreView\logs\zoreview.log'

$script:failures = 0
function Assert([bool]$condition, [string]$name) {
    if ($condition) {
        Write-Host "OK   $name"
    } else {
        Write-Host "FAIL $name" -ForegroundColor Red
        $script:failures++
    }
}

# ASan builds: keep reports in one place (relative log_path, because an absolute
# one is split on the drive colon) and assert that none appear.
$isAsan = $App -match 'build-asan'
$asanDir = Join-Path $root 'tests\output\asan'
$asanBefore = 0
if ($isAsan) {
    New-Item -ItemType Directory -Force -Path $asanDir | Out-Null
    $asanBefore = @(Get-ChildItem -LiteralPath $asanDir -Filter 'asan.*' `
            -ErrorAction SilentlyContinue).Count
    $env:ASAN_OPTIONS = 'log_path=tests\output\asan\asan'
}

if (-not ('UiShellNative' -as [type])) {
    Add-Type @"
using System;
using System.Runtime.InteropServices;
public static class UiShellNative {
  [DllImport("user32.dll")] public static extern bool SetProcessDpiAwarenessContext(IntPtr value);
  [DllImport("user32.dll")] public static extern bool SetCursorPos(int x, int y);
  [DllImport("user32.dll")] public static extern void mouse_event(uint flags, uint dx, uint dy, uint data, UIntPtr extra);
  [DllImport("user32.dll")] public static extern bool SetForegroundWindow(IntPtr hwnd);
  [DllImport("user32.dll")] public static extern bool PostMessage(IntPtr hwnd, uint msg, IntPtr w, IntPtr l);
  [DllImport("user32.dll")] public static extern bool GetClientRect(IntPtr hwnd, out RECT rect);
  [DllImport("user32.dll")] public static extern bool ClientToScreen(IntPtr hwnd, ref POINT point);
  [StructLayout(LayoutKind.Sequential)] public struct RECT { public int Left, Top, Right, Bottom; }
  [StructLayout(LayoutKind.Sequential)] public struct POINT { public int X, Y; }
}
"@
}

# Per-monitor v2 awareness for this script's process (see the note above).
[UiShellNative]::SetProcessDpiAwarenessContext([IntPtr](-4)) | Out-Null
Add-Type -AssemblyName System.Windows.Forms | Out-Null

function Read-NewLogLines([int]$skip) {
    if (-not (Test-Path -LiteralPath $logPath)) { return @() }
    return @(Get-Content -LiteralPath $logPath -Encoding UTF8 | Select-Object -Skip $skip)
}

function Get-LogCount {
    if (-not (Test-Path -LiteralPath $logPath)) { return 0 }
    return @(Get-Content -LiteralPath $logPath -Encoding UTF8).Count
}

function Invoke-App {
    param([string[]]$Arguments)
    # Pin the rail for the geometry checks: since 22/08 the default mode only
    # fades the rail in on hover, which is covered by run_strip_check.ps1.
    # A caller-supplied --strip=auto/off later on the command line still wins.
    $full = @('--strip=pinned') + $Arguments + @($Image)
    $process = Start-Process -FilePath $App -ArgumentList $full -PassThru `
        -WorkingDirectory $root
    return $process
}

function Wait-ForWindow($process, [int]$timeoutMs = 15000) {
    $deadline = (Get-Date).AddMilliseconds($timeoutMs)
    while ((Get-Date) -lt $deadline) {
        Start-Sleep -Milliseconds 100
        $process.Refresh()
        if ($process.HasExited) { return }
        if ($process.MainWindowHandle -ne 0) { return }
    }
}

function Get-Dump([string]$name, [string[]]$Arguments) {
    # Cold starts (antivirus scanning, D3D device init) can push the first decode
    # past the app's default dump delay, which used to make every image-dependent
    # assertion fail in one go. Retry the same dump until the image is up.
    for ($attempt = 0; $attempt -lt 3; $attempt++) {
        $path = Join-Path $OutputDirectory "layout-$name.txt"
        Remove-Item -LiteralPath $path -ErrorAction SilentlyContinue
        $arguments = @('--layout-dump', $path) + $Arguments
        $process = Invoke-App -Arguments $arguments
        if (-not $process.WaitForExit(30000)) {
            $process.Kill()
            throw "layout dump '$name' timed out"
        }
        if (-not (Test-Path -LiteralPath $path)) {
            throw "layout dump '$name' was not written"
        }
        $map = @{}
        foreach ($line in Get-Content -LiteralPath $path -Encoding UTF8) {
            $index = $line.IndexOf('=')
            if ($index -gt 0) {
                $map[$line.Substring(0, $index)] = $line.Substring($index + 1)
            }
        }
        if ($map['has_image'] -eq '1' -or $attempt -eq 2) { return $map }
        Start-Sleep -Milliseconds 400
    }
    return $map
}

function Split-Rect([string]$text) {
    if ([string]::IsNullOrWhiteSpace($text)) { return @(0, 0, 0, 0) }
    return @($text -split ',' | ForEach-Object { [double]$_ })
}

# --- 1. Normal window: chrome geometry ---------------------------------------
$normal = Get-Dump 'normal' @()
Write-Host ("     dpi={0} client={1} window={2}" -f $normal['dpi'], $normal['client'], $normal['window'])
$scale = [double]$normal['dpi']
Assert ($normal['state'] -eq 'normal') 'starts in the normal state'
Assert ($normal['borderless'] -eq '1') 'borderless frame is the default'
Assert ($normal['client'] -eq $normal['window']) 'normal: client rectangle equals the window rectangle'
Assert ($normal['has_image'] -eq '1') 'the image was decoded before the dump'

$client = Split-Rect ($normal['client'] -replace 'x', ',')
$title = Split-Rect $normal['title']
$content = Split-Rect $normal['content']
$bottom = Split-Rect $normal['bottom']
$strip = Split-Rect $normal['strip']
$expectedTitle = 32.0 * $scale
$expectedBottom = 44.0 * $scale
$expectedStrip = 76.0 * $scale
Assert ([math]::Abs(($title[3] - $title[1]) - $expectedTitle) -le 1.0) `
    ("title bar is 32 DIP ({0} px at {1}x)" -f $expectedTitle, $scale)
Assert ([math]::Abs(($bottom[3] - $bottom[1]) - $expectedBottom) -le 1.0) `
    ("bottom bar is 44 DIP ({0} px)" -f $expectedBottom)
Assert ([math]::Abs(($strip[3] - $strip[1]) - $expectedStrip) -le 1.0) `
    ("pinned strip is 76 DIP ({0} px)" -f $expectedStrip)
Assert ([math]::Abs($content[1] - $title[3]) -le 1.0) 'content starts exactly below the title bar'
Assert ([math]::Abs($content[3] - $strip[1]) -le 1.0) 'content ends exactly above the pinned strip'
Assert ([math]::Abs($strip[3] - $bottom[1]) -le 1.0) 'strip sits exactly above the bottom bar'
Assert ([math]::Abs($bottom[3] - $client[1]) -le 1.0) 'bottom bar is flush with the client bottom'
Assert ($normal['strip_counts'] -eq '1') 'pinned strip reduces the content rectangle'

# Five right-aligned buttons; v3.0 trimmed the caption to three information
# segments (規劃/26 §2.2), so the segment string is "111" rather than "11111".
Assert ($normal['segments'] -eq '111') 'all three title segments are visible'
$buttons = @($normal['buttons'] -split '\|')
Assert ($buttons.Count -eq 5) 'five title-bar buttons'
$closeButton = Split-Rect $buttons[4]
$fullscreenButton = Split-Rect $buttons[1]
Assert ([math]::Abs($closeButton[2] - $client[0]) -le 1.0) 'close button ends at the right edge'
$buttonWidth = [math]::Abs($closeButton[2] - $closeButton[0])
Assert ([math]::Abs($buttonWidth - (46.0 * $scale)) -le 1.5) `
    ("title buttons are 46 DIP wide ({0} px)" -f $buttonWidth)

# --- 2. Rail modes ----------------------------------------------------------
$off = Get-Dump 'off' @('--strip=off')
$offContent = Split-Rect $off['content']
$offBottom = Split-Rect $off['bottom']
$offStrip = Split-Rect $off['strip']
Assert ($off['strip_mode'] -eq 'off') '--strip=off is parsed'
Assert ($off['strip_counts'] -eq '0') 'off: the rail does not reduce the content'
Assert (($offStrip[2] - $offStrip[0]) -le 0) 'off: no strip rectangle'
Assert ([math]::Abs($offContent[3] - $offBottom[1]) -le 1.0) 'off: content ends at the bottom bar'

$auto = Get-Dump 'auto' @('--strip=auto')
$autoContent = Split-Rect $auto['content']
$autoBottom = Split-Rect $auto['bottom']
$autoStrip = Split-Rect $auto['strip']
Assert ($auto['strip_mode'] -eq 'auto') '--strip=auto is parsed'
Assert ($auto['strip_counts'] -eq '0') 'auto: the rail overlays instead of reducing content'
Assert ([math]::Abs($autoContent[3] - $autoBottom[1]) -le 1.0) 'auto: content ends at the bottom bar'
Assert (($autoStrip[3] - $autoStrip[1]) -gt 0) 'auto: the rail still has a visible rectangle'

# --- 3. Maximized / fullscreen ---------------------------------------------
$maximized = Get-Dump 'maximized' @('--maximized')
$work = [System.Windows.Forms.Screen]::PrimaryScreen.WorkingArea
Write-Host ("     maximized client={0} window={1} workarea={2}x{3}" -f `
        $maximized['client'], $maximized['window'], $work.Width, $work.Height)
Assert ($maximized['state'] -eq 'maximized') 'maximized state is reported'
$maxClient = Split-Rect ($maximized['client'] -replace 'x', ',')
Assert ([math]::Abs($maxClient[0] - $work.Width) -le 1.0) 'maximized client width equals the work area'
Assert ([math]::Abs($maxClient[1] - $work.Height) -le 1.0) `
    'maximized client height equals the work area (taskbar stays visible)'
$maxTitle = Split-Rect $maximized['title']
Assert ([math]::Abs(($maxTitle[3] - $maxTitle[1]) - $expectedTitle) -le 1.0) `
    'maximized keeps the 32 DIP title bar'

$fullscreen = Get-Dump 'fullscreen' @('--fullscreen')
$screen = [System.Windows.Forms.Screen]::PrimaryScreen.Bounds
Write-Host ("     fullscreen client={0} screen={1}x{2}" -f $fullscreen['client'], $screen.Width, $screen.Height)
Assert ($fullscreen['state'] -eq 'fullscreen') 'fullscreen state is reported'
$fsClient = Split-Rect ($fullscreen['client'] -replace 'x', ',')
$fsContent = Split-Rect $fullscreen['content']
$fsTitle = Split-Rect $fullscreen['title']
$fsStrip = Split-Rect $fullscreen['strip']
Assert ([math]::Abs($fsClient[0] - $screen.Width) -le 1.0) 'fullscreen covers the whole monitor width'
Assert ([math]::Abs($fsClient[1] - $screen.Height) -le 1.0) 'fullscreen covers the whole monitor height'
Assert (($fsTitle[3] - $fsTitle[1]) -le 0) 'fullscreen hides the title bar'
# v3.0 (規劃/26 §2.7) reverses pitfall 146: the fullscreen rail keeps its
# rectangle (it hosts the bottom reveal hot zone) but starts hidden and owns no
# clicks until the pointer reaches the bottom edge.
Assert (($fsStrip[2] - $fsStrip[0]) -gt 0) 'fullscreen keeps the rail rectangle for the reveal hot zone'
Assert ($fullscreen['strip_shown'] -eq '0') 'fullscreen starts with the rail hidden'
Assert ($fullscreen['strip_interactive'] -eq '0') 'the hidden fullscreen rail owns no clicks'
Assert ([math]::Abs($fsContent[0] - 0) -le 1.0 -and [math]::Abs($fsContent[1] - 0) -le 1.0) `
    'fullscreen content starts at the client origin'

# --- 4. Escape priority (real key message) ---------------------------------
$skip = Get-LogCount
$process = Invoke-App -Arguments @('--fullscreen')
Wait-ForWindow $process
Start-Sleep -Milliseconds 1200
$hwnd = $process.MainWindowHandle
[UiShellNative]::PostMessage($hwnd, 0x0100, [IntPtr]0x1B, [IntPtr]0) | Out-Null
Start-Sleep -Milliseconds 700
[UiShellNative]::PostMessage($hwnd, 0x0100, [IntPtr]0x1B, [IntPtr]0) | Out-Null
Start-Sleep -Milliseconds 700
$log = Read-NewLogLines $skip
Assert (@($log | Where-Object { $_ -match 'cmd: escape=fullscreen' }).Count -eq 1) `
    'first Escape exits fullscreen'
Assert (@($log | Where-Object { $_ -match 'window: state=normal' }).Count -ge 1) `
    'the window returns to the normal state'
Assert (@($log | Where-Object { $_ -match 'cmd: escape=none' }).Count -eq 1) `
    'second Escape has nothing left to close'
if (-not $process.HasExited) {
    $process.CloseMainWindow() | Out-Null
    if (-not $process.WaitForExit(5000)) { $process.Kill() }
}

# --- 5. Title-bar buttons with real mouse input ----------------------------
function Invoke-Click([IntPtr]$window, [double]$clientX, [double]$clientY) {
    [UiShellNative]::SetForegroundWindow($window) | Out-Null
    Start-Sleep -Milliseconds 200
    $point = New-Object UiShellNative+POINT
    $point.X = [int][math]::Round($clientX)
    $point.Y = [int][math]::Round($clientY)
    [UiShellNative]::ClientToScreen($window, [ref]$point) | Out-Null
    [UiShellNative]::SetCursorPos($point.X, $point.Y) | Out-Null
    Start-Sleep -Milliseconds 150
    [UiShellNative]::mouse_event(0x0002, 0, 0, 0, [UIntPtr]::Zero)
    Start-Sleep -Milliseconds 80
    [UiShellNative]::mouse_event(0x0004, 0, 0, 0, [UIntPtr]::Zero)
    Start-Sleep -Milliseconds 500
}

$skip = Get-LogCount
$process = Invoke-App -Arguments @()
Wait-ForWindow $process
Start-Sleep -Milliseconds 1500
$hwnd = $process.MainWindowHandle
$rect = New-Object UiShellNative+RECT
[UiShellNative]::GetClientRect($hwnd, [ref]$rect) | Out-Null
$clientWidth = $rect.Right
$clientHeight = $rect.Bottom
Write-Host ("     client={0}x{1}" -f $clientWidth, $clientHeight)

$rowY = 16.0 * $scale
Invoke-Click $hwnd (($fullscreenButton[0] + $fullscreenButton[2]) / 2.0) $rowY
$log = Read-NewLogLines $skip
Assert (@($log | Where-Object { $_ -match 'cmd: id=title.fullscreen' }).Count -eq 1) `
    'clicking the title-bar fullscreen button runs the command'
Assert (@($log | Where-Object { $_ -match 'window: state=fullscreen' }).Count -ge 1) `
    'the fullscreen button enters fullscreen'

# Back to normal with Escape, then exercise the bottom bar.
[UiShellNative]::PostMessage($hwnd, 0x0100, [IntPtr]0x1B, [IntPtr]0) | Out-Null
Start-Sleep -Milliseconds 700

$skip = Get-LogCount
# Bottom bar: 适应 (fit) then 1:1. Right cluster from the right edge:
# 更多 88, ↻ 40, ↺ 40, 1:1 56, 适应 64 (DIP), 12 DIP padding.
$pad = 12.0 * $scale
$fitWidth = 64.0 * $scale
$actualWidth = 56.0 * $scale
$rotateWidth = 40.0 * $scale
$moreWidth = 88.0 * $scale
$bottomCenterY = $clientHeight - (44.0 * $scale) / 2.0
$actualX = $clientWidth - $pad - $moreWidth - 2 * $rotateWidth - $actualWidth / 2.0
$fitX = $clientWidth - $pad - $moreWidth - 2 * $rotateWidth - $actualWidth - $fitWidth / 2.0
Invoke-Click $hwnd $actualX $bottomCenterY
Invoke-Click $hwnd $fitX $bottomCenterY
$log = Read-NewLogLines $skip
Assert (@($log | Where-Object { $_ -match 'cmd: id=bottom.actual' }).Count -eq 1) `
    'clicking 1:1 runs the actual-size command'
Assert (@($log | Where-Object { $_ -match 'cmd: id=bottom.fit' }).Count -eq 1) `
    'clicking 适应 runs the fit command'

# Close button: the process must exit by itself.
$closeX = ($closeButton[0] + $closeButton[2]) / 2.0
Invoke-Click $hwnd $closeX $rowY
$exited = $process.WaitForExit(5000)
Assert ($exited) 'clicking the close button exits the process'
if (-not $exited) { $process.Kill() }

# --- 6. Chaos-free shutdown ------------------------------------------------
Start-Sleep -Milliseconds 300
$tail = @(Read-NewLogLines 0 | Select-Object -Last 6)
Assert (@($tail | Where-Object { $_ -match 'shutdown' }).Count -ge 1) `
    'the last shutdown path is logged'

# --- 7. AddressSanitizer (only when running the ASan build) -----------------
if ($isAsan) {
    $asanAfter = @(Get-ChildItem -LiteralPath $asanDir -Filter 'asan.*' `
            -ErrorAction SilentlyContinue)
    Assert ($asanAfter.Count -le $asanBefore) `
        ("no new AddressSanitizer report ({0} before, {1} after)" -f `
            $asanBefore, $asanAfter.Count)
    if ($asanAfter.Count -gt $asanBefore) {
        $asanAfter | Select-Object -Last 3 | ForEach-Object {
            Write-Host ("     report: {0}" -f $_.FullName)
        }
    }
}

if ($script:failures -ne 0) {
    Write-Host "$($script:failures) check(s) failed" -ForegroundColor Red
    exit 1
}
Write-Host 'OK: v2.0 S0 chrome, borderless window and Escape priority'
exit 0
