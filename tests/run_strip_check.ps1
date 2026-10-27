# v2.0 S2 (plan 22/08): thumbnail rail modes, auto fade, hot zone and the
# hover tooltip.
#
# The script copies the executable into a throw-away portable directory, so it
# never touches the developer's %LOCALAPPDATA% settings or caches (22/00 rule
# 10). Everything is asserted from --layout-dump plus the application log; no
# pixel comparison is needed for the fade because the dump reports the opacity.
#
# ASCII only, like the other UI scripts (HANDOFF pitfall 60/124).

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

$work = Join-Path $env:TEMP ("nv-strip-" + [guid]::NewGuid().ToString("N"))
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

# --- Samples ---------------------------------------------------------------
Add-Type -AssemblyName System.Drawing
# The auto rail reveals when the pointer is over the window's bottom edge. A
# previous hover case moves the real cursor there, so every run starts by
# parking the pointer in the top-left corner; otherwise the "default is hidden"
# case would race with wherever the last test left it.
Add-Type @"
using System;
using System.Runtime.InteropServices;
public class StripCursor {
  [DllImport("user32.dll")]
  public static extern bool SetCursorPos(int x, int y);
}
"@ | Out-Null
[StripCursor]::SetCursorPos(5, 5) | Out-Null

$samples = @(
  @{ Name = "a_wide.bmp";   W = 800; H = 600 },
  @{ Name = "b_tall.bmp";   W = 200; H = 1600 },
  @{ Name = "c_square.bmp"; W = 400; H = 400 }
)
foreach ($sample in $samples) {
  $bitmap = New-Object System.Drawing.Bitmap($sample.W, $sample.H)
  $graphics = [System.Drawing.Graphics]::FromImage($bitmap)
  $graphics.Clear([System.Drawing.Color]::FromArgb(40, 110, 170))
  $graphics.Dispose()
  $path = Join-Path $work $sample.Name
  $bitmap.Save($path, [System.Drawing.Imaging.ImageFormat]::Bmp)
  $bitmap.Dispose()
}
$firstImage = Join-Path $work "a_wide.bmp"
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

function Inside([hashtable]$inner, [hashtable]$outer) {
  if ($null -eq $inner -or $null -eq $outer) { return $false }
  return ($inner.L -ge $outer.L - 0.5) -and ($inner.T -ge $outer.T - 0.5) -and
         ($inner.R -le $outer.R + 0.5) -and ($inner.B -le $outer.B + 0.5)
}

function ClientRect([hashtable]$values) {
  if (-not $values.ContainsKey("client")) { return $null }
  $parts = $values["client"].Split("x")
  if ($parts.Count -ne 2) { return $null }
  $w = [double]$parts[0]
  $h = [double]$parts[1]
  return @{ L = 0; T = 0; R = $w; B = $h; W = $w; H = $h }
}

# Every run is a fresh process and starts from default settings: the fade and
# tooltip are per-run state, and the mode is pinned explicitly where needed.
function RunDump([string]$label, [string[]]$extra) {
  Remove-Item (Join-Path $work "settings.ini") -Force -ErrorAction SilentlyContinue
  Remove-Item $dumpPath -Force -ErrorAction SilentlyContinue
  # Park the real pointer away from the centred window before every launch; the
  # hover cases move it themselves once the app runs.
  [StripCursor]::SetCursorPos(5, 5) | Out-Null
  $log = Join-Path $work "logs\zoreview.log"
  Remove-Item $log -Force -ErrorAction SilentlyContinue

  $arguments = @("--portable", "--language", "en", "--layout-dump", $dumpPath)
  $arguments += $extra
  $arguments += $firstImage
  $process = Start-Process -FilePath $appExe -ArgumentList $arguments -PassThru
  if (-not $process.WaitForExit(30000)) {
    $process.Kill()
    throw "zoreview.exe did not exit in time: $label"
  }
  Start-Sleep -Milliseconds 120
  if (-not (Test-Path $dumpPath)) { throw "no layout dump for $label" }
  $text = ""
  if (Test-Path $log) { $text = Get-Content $log -Raw }
  return @{ Values = (ReadDump $dumpPath); Log = $text }
}

try {
  # --- 1. Default is auto and starts faded out ------------------------------
  $auto = RunDump "default" @()
  $v = $auto.Values
  $dpi = [double]$v["dpi"]
  Check ($v["strip_mode"] -eq "auto") "the default rail mode is auto"
  Check ($v["strip_shown"] -eq "0") "the auto rail starts faded out"
  Check ([double]$v["strip_opacity"] -eq 0) "the faded-out rail has zero opacity"
  Check ($v["strip_interactive"] -eq "0") "the faded-out rail owns no clicks"
  Check ($v["strip_counts"] -eq "0") "the auto rail reserves no image space"
  $strip = Rect $v "strip"
  $content = Rect $v "content"
  $bottom = Rect $v "bottom"
  Check ($strip.W -gt 0) "the auto rail keeps a rectangle for the hot zone"
  CheckNear $strip.H (76 * $dpi) 0.6 "the rail is 76 DIP tall"
  Check ([math]::Abs($content.B - $bottom.T) -lt 0.5) "the auto rail gives its space back to the image"
  Check (-not ($auto.Log -match "thumbnail ready:")) "a hidden rail requests no previews"

  # --- 2. Pinned ------------------------------------------------------------
  $pinned = RunDump "pinned" @("--strip=pinned")
  $p = $pinned.Values
  Check ($p["strip_mode"] -eq "pinned") "--strip=pinned selects pinned"
  Check ($p["strip_shown"] -eq "1") "the pinned rail is shown"
  Check ([double]$p["strip_opacity"] -eq 1) "the pinned rail is opaque"
  Check ($p["strip_interactive"] -eq "1") "the pinned rail owns clicks"
  Check ($p["strip_counts"] -eq "1") "the pinned rail reserves image space"
  $pStrip = Rect $p "strip"
  $pContent = Rect $p "content"
  Check ([math]::Abs($pContent.B - $pStrip.T) -lt 0.5) "the content ends above the pinned rail"
  Check ([math]::Abs($pStrip.B - (Rect $p "bottom").T) -lt 0.5) "the rail sits on the bottom bar"

  # --- 3. Off ---------------------------------------------------------------
  $off = RunDump "off" @("--strip=off")
  $o = $off.Values
  Check ($o["strip_mode"] -eq "off") "--strip=off selects off"
  Check ((Rect $o "strip").W -eq 0) "the off rail has no rectangle"
  Check ($o["strip_counts"] -eq "0") "the off rail reserves no space"
  Check (-not ($off.Log -match "thumbnail ready:")) "the off rail requests no previews"

  # --- 4. Hot zone reveals the auto rail ------------------------------------
  $hot = RunDump "hotzone" @("--strip=auto", "--hover-hotzone")
  $h = $hot.Values
  Check ($h["strip_shown"] -eq "1") "the 8 DIP hot zone reveals the rail"
  Check ([double]$h["strip_opacity"] -ge 0.99) "the reveal fades the rail in"
  Check ($h["strip_interactive"] -eq "1") "the revealed rail owns clicks"
  Check ($h["strip_counts"] -eq "0") "revealing the auto rail does not move the image"
  Check ([math]::Abs((Rect $h "content").B - (Rect $h "bottom").T) -lt 0.5) `
    "the revealed auto rail still overlays the image"
  Check ($hot.Log -match "ui: strip=show") "the reveal is logged"

  # --- 4b. v3.0 §2.10: the bottom bar belongs to the reveal area ------------
  # The hot zone used to be the bottom 8 DIP of the rail slot only; it is now
  # the union of the slot and the bottom bar. A held button never reveals it:
  # a drag that crosses the bar has to keep panning.
  $bar = RunDump "bottom-bar" @("--strip=auto", "--hover-bottom")
  Check ($bar.Values["strip_shown"] -eq "1") "moving onto the bottom bar reveals the rail"
  Check ($bar.Log -match "test: hover_bottom at=") "the bottom-bar move is logged"
  $held = RunDump "held-drag" @("--strip=auto", "--drag-to-bottom")
  Check ($held.Values["dragging"] -eq "1") "the pan is still running when the pointer reaches the bar"
  Check ($held.Values["strip_shown"] -eq "0") "a held drag never reveals the rail"

  # --- 5. Leaving hides it after the grace period ---------------------------
  $cycle = RunDump "cycle" @("--strip=auto", "--hover-cycle")
  Check ($cycle.Log -match "ui: strip=show") "the cycle reveals the rail"
  Check ($cycle.Log -match "ui: strip=hide") "leaving starts the fade out"
  Check ($cycle.Log -match "ui: strip=hidden") "the fade out completes"
  Check ([double]$cycle.Values["strip_opacity"] -eq 0) "the rail is fully transparent again"
  Check ($cycle.Values["strip_shown"] -eq "0") "the hidden rail is no longer shown"

  # --- 6. Hover tooltip -----------------------------------------------------
  $tip = RunDump "tooltip" @("--strip=pinned", "--hover-thumb", "1")
  $t = $tip.Values
  Check ($t["strip_hover"] -eq "1") "the second slot is hovered"
  Check ($t["strip_tooltip"] -eq "b_tall.bmp") "the tooltip shows the file name"
  $panel = Rect $t "strip_tooltip_panel"
  $client = ClientRect $t
  Check ($panel.W -gt 0) "the tooltip has a rectangle"
  Check (Inside $panel $client) "the tooltip stays inside the client area"
  Check ($t["strip_tooltip_above"] -eq "1") "the tooltip prefers the space above the slot"
  Check ($panel.B -le (Rect $t "strip").T + 0.5) "the tooltip sits above the rail"
  CheckNear $panel.H (26 * $dpi) 0.6 "the tooltip is 26 DIP tall"
  Check ($panel.W -le (320 * $dpi) + 0.5) "the tooltip is at most 320 DIP wide"

  # The first slot is at the left edge: the panel must clamp into the client
  # instead of hanging outside it.
  $edge = RunDump "tooltip-edge" @("--strip=pinned", "--hover-thumb", "0")
  $edgePanel = Rect $edge.Values "strip_tooltip_panel"
  Check ($edge.Values["strip_tooltip"] -eq "a_wide.bmp") "the first slot tooltip shows its name"
  Check ($edgePanel.L -ge (4 * $dpi) - 0.5) "the tooltip clamps at the left edge"
  Check (Inside $edgePanel (ClientRect $edge.Values)) "the clamped tooltip stays inside the client"

  # --- 7. Auto mode with a hovered slot: tooltip works once visible ---------
  $autoTip = RunDump "auto-tooltip" @("--strip=auto", "--hover-thumb", "1")
  Check ($autoTip.Values["strip_shown"] -eq "1") "hovering a slot reveals the auto rail"
  Check ($autoTip.Values["strip_tooltip"] -eq "b_tall.bmp") "the auto rail shows the same tooltip"
}
finally {
  Remove-Item -Recurse -Force $work -ErrorAction SilentlyContinue
}

Write-Host ""
if ($script:failures -gt 0) {
  Write-Host ("FAILED: {0} of {1} strip checks failed" -f $script:failures, $script:checks) -ForegroundColor Red
  exit 1
}
Write-Host ("OK: {0} strip checks passed" -f $script:checks) -ForegroundColor Green
exit 0
