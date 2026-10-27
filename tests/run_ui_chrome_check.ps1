# v2.0 S1 (plan 22/02): bottom bar commands, more panel, zoom menu, info panel,
# title-bar segment clicks and the persisted chrome settings.
#
# The script runs a throw-away copy of the executable in portable mode, so it
# never touches the developer's %LOCALAPPDATA% settings or caches (plan 22/00
# rule 10). All paths and strings here are ASCII on purpose: a UTF-8 BOM is
# then not required (HANDOFF pitfall 60/124).
#
# Everything is driven through --click-hit, which clicks the centre of the
# rectangle the UI actually drew, so the checks work at any DPI without
# composing scaled mouse coordinates (pitfall 139).

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

$work = Join-Path $env:TEMP ("nv-chrome-" + [guid]::NewGuid().ToString("N"))
New-Item -ItemType Directory -Path $work | Out-Null
$appExe = Join-Path $work "zoreview.exe"
Copy-Item $App $appExe
# An AddressSanitizer build needs its runtime DLL next to the executable; a
# plain copy would leave it behind in build-asan (HANDOFF pitfall 113).
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
  $found = $text -match [regex]::Escape($pattern)
  Check $found ("{0} (expected '{1}')" -f $name, $pattern)
}

function Log([string]$name = "zoreview") {
  # The application always writes logs\zoreview.log inside its portable data
  # directory; the file is cleared before every run so each log is readable on
  # its own.
  Join-Path $work "logs\zoreview.log"
}

# --- Sample images ---------------------------------------------------------
# Three BMPs with different pixel sizes and explicit timestamps, so name, size
# and date orders are all different and can be told apart.
Add-Type -AssemblyName System.Drawing
# v3.0 (規劃/26 §2.10): the auto rail reveals for the whole bottom bar, so the
# real pointer must not be left over the window's bottom edge — otherwise
# "the auto rail starts faded out" would depend on the previous run.
Add-Type @"
using System;
using System.Runtime.InteropServices;
public class ChromeCursor {
  [DllImport("user32.dll")] public static extern bool SetCursorPos(int x, int y);
}
"@ | Out-Null
$samples = @(
  @{ Name = "a_wide.bmp";   W = 800; H = 600;  Time = [datetime]"2020-01-01 10:00:00" },
  @{ Name = "b_tall.bmp";   W = 200; H = 1600; Time = [datetime]"2021-06-01 10:00:00" },
  @{ Name = "c_square.bmp"; W = 400; H = 400;  Time = [datetime]"2022-12-01 10:00:00" }
)
foreach ($sample in $samples) {
  $bitmap = New-Object System.Drawing.Bitmap($sample.W, $sample.H)
  $graphics = [System.Drawing.Graphics]::FromImage($bitmap)
  $graphics.Clear([System.Drawing.Color]::FromArgb(40, 90, 160))
  $graphics.Dispose()
  $path = Join-Path $work $sample.Name
  $bitmap.Save($path, [System.Drawing.Imaging.ImageFormat]::Bmp)
  $bitmap.Dispose()
  (Get-Item $path).LastWriteTime = $sample.Time
}
$firstImage = Join-Path $work "a_wide.bmp"

# --- Helpers ---------------------------------------------------------------
# Runs the application with the given arguments, waits for it to exit and
# returns its log text. Every run is a fresh process: persisted state is only
# observable across runs, which is exactly what the settings checks need.
function RunApp([string]$label, [string[]]$arguments) {
  $logPath = Log $label
  Remove-Item $logPath -ErrorAction SilentlyContinue
  $all = @("--portable", "--language", "en") + $arguments
  [ChromeCursor]::SetCursorPos(5, 5) | Out-Null
  $process = Start-Process -FilePath $appExe -ArgumentList $all -PassThru
  if (-not $process.WaitForExit(20000)) {
    $process.Kill()
    throw "zoreview.exe did not exit in time: $label"
  }
  Start-Sleep -Milliseconds 120
  if (Test-Path $logPath) { return (Get-Content $logPath -Raw) }
  return ""
}

$dumpPath = Join-Path $work "layout.txt"
function Dump([string]$label, [string[]]$clicks, [string]$image = $firstImage) {
  Remove-Item $dumpPath -ErrorAction SilentlyContinue
  $arguments = @("--layout-dump", $dumpPath)
  foreach ($click in $clicks) { $arguments += @("--click-hit", $click) }
  $arguments += $image
  $log = RunApp $label $arguments
  if (-not (Test-Path $dumpPath)) { throw "no layout dump for $label" }
  $values = @{}
  foreach ($line in Get-Content $dumpPath) {
    $parts = $line.Split("=", 2)
    if ($parts.Count -eq 2) { $values[$parts[0]] = $parts[1] }
  }
  return @{ Values = $values; Log = $log }
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

# The dump reports the client rectangle as "WxH" and it always starts at 0,0.
function ClientRect([hashtable]$values) {
  if (-not $values.ContainsKey("client")) { return $null }
  $parts = $values["client"].Split("x")
  if ($parts.Count -ne 2) { return $null }
  $w = [double]$parts[0]
  $h = [double]$parts[1]
  return @{ L = 0; T = 0; R = $w; B = $h; W = $w; H = $h }
}

function SettingsText([string]$label) {
  $path = Join-Path $work "settings-out.txt"
  Remove-Item $path -ErrorAction SilentlyContinue
  $log = RunApp $label @("--print-settings", $path)
  $values = @{}
  if (Test-Path $path) {
    foreach ($line in Get-Content $path) {
      $parts = $line.Split("=", 2)
      if ($parts.Count -eq 2) { $values[$parts[0]] = $parts[1] }
    }
  }
  return @{ Values = $values; Log = $log }
}

try {
  # --- 1. Settings defaults ------------------------------------------------
  $settings = SettingsText "print-defaults"
  Check ($settings.Values["thumbstrip_mode"] -eq "auto") "default thumbstrip mode is auto (22/08)"
  Check ($settings.Values["birdseye"] -eq "0") "birdseye defaults to off"
  Check ($settings.Values["topmost"] -eq "0") "topmost defaults to off"

  # --- 2. More panel geometry ---------------------------------------------
  $more = Dump "more-open" @("bottom.more")
  $m = $more.Values
  Check ($m["popup"] -eq "more") "clicking More opens the more panel"
  # v3.0 (plan 26 section 2.5): the panel is down to the two view switches —
  # always-on-top, reset rotation and the whole sort group are gone.
  Check ([int]$m["menu_rows"] -eq 2) ("more panel has 2 rows (got {0})" -f $m["menu_rows"])

  Check ($m["rotation"] -eq "0") "the more panel opens with an upright view"
  Check ($m["menu_above"] -eq "1") "the more panel opens above its button"
  $menu = Rect $m "menu"
  $client = ClientRect $m
  $bottom = Rect $m "bottom"
  Check (Inside $menu $client) "the more panel stays inside the client area"
  Check ($menu.B -le $bottom.T + 0.5) "the more panel never covers the bottom bar"
  Check ($menu.W -lt ($client.W * 0.5)) "the more panel is a narrow flyout"
  for ($i = 0; $i -lt 2; $i++) {
    $row = Rect $m ("menu_row_{0}" -f $i)
    Check (Inside $row $menu) ("menu row {0} is inside the panel" -f $i)
  }
  $row0 = Rect $m "menu_row_0"
  $row1 = Rect $m "menu_row_1"
  Check ([math]::Abs($row0.H - $row1.H) -lt 0.5) "menu rows share one height"
  Check ($row0.B -le $row1.T + 0.5) "menu rows are stacked downwards"

  # --- 2b. #11: the hamburger opens settings, not a popup ------------------
  # 規劃/26 §2.8: the ☰ title button used to fall through to the same handler
  # as right-click (the native context menu). It now opens the settings window
  # directly; 更多▾ stays the view-switch entry (asserted in section 2).
  $hamburger = Dump "hamburger" @("title.menu")
  Check ($hamburger.Values["settings_open"] -eq "1") "the hamburger opens the settings window (#11)"
  Check ($hamburger.Values["popup"] -eq "none") "the hamburger opens no popup"
  CheckHas $hamburger.Log "settings: opened" "opening settings from the hamburger is logged"

  # --- 2c. #3: the rotate glyphs sit centred and carry no accent dot -------
  # 規劃/26 §2.3: one large arc let Direct2D pick the other circle centre, so
  # both glyphs were painted ~4.75 DIP up and outwards; the dump only knows the
  # 16x16 boxes, so this check measures the rendered screenshot instead.
  # Two runs on purpose: --screenshot destroys the window when it fires
  # (TakeScreenshot), so a combined screenshot + --layout-dump loses the dump.
  $iconShot = Join-Path $work "rotate-icons.png"
  $iconDump = Join-Path $work "rotate-layout.txt"
  Remove-Item $iconShot, $iconDump -ErrorAction SilentlyContinue
  $null = RunApp "rotate-layout" @(
    "--force-dpi-scale", "200", "--layout-dump", $iconDump, $firstImage)
  $null = RunApp "rotate-shot" @(
    "--force-dpi-scale", "200",
    "--screenshot", $iconShot, "--shot-delay", "800", $firstImage)
  Check ((Test-Path $iconShot) -and (Test-Path $iconDump)) "the rotate icon screenshot and layout are written"
  if ((Test-Path $iconShot) -and (Test-Path $iconDump)) {
    $iconValues = @{}
    foreach ($line in Get-Content $iconDump) {
      $parts = $line.Split("=", 2)
      if ($parts.Count -eq 2) { $iconValues[$parts[0]] = $parts[1] }
    }
    $scale = [double]$iconValues["dpi"]
    $bar = Rect $iconValues "bottom"
    $right = $bar.R - 12.0 * $scale            # kBottomPadding
    $rotRight = @{ L = $right - 128.0 * $scale; R = $right - 88.0 * $scale
                   T = $bar.T; B = $bar.B }
    $rotLeft = @{ L = $right - 168.0 * $scale; R = $right - 128.0 * $scale
                  T = $bar.T; B = $bar.B }
    $bmp = [System.Drawing.Bitmap]::new($iconShot)
    try {
      $measure = {
        param($rect, $bitmap)
        $x0 = [int][math]::Round($rect.L); $x1 = [int][math]::Round($rect.R)
        $y0 = [int][math]::Round($rect.T); $y1 = [int][math]::Round($rect.B)
        $bg = $bitmap.GetPixel($x0 + 2, $y1 - 4)
        $minX = $x1; $maxX = -1; $minY = $y1; $maxY = -1; $green = 0
        for ($y = $y0 + 2; $y -lt $y1 - 2; $y++) {
          for ($x = $x0 + 2; $x -lt $x1 - 2; $x++) {
            $p = $bitmap.GetPixel($x, $y)
            if (($p.G -gt $p.R + 30) -and ($p.G -gt $p.B + 30)) { $green++ }
            $d = [math]::Abs($p.R - $bg.R) + [math]::Abs($p.G - $bg.G) +
                 [math]::Abs($p.B - $bg.B)
            if ($d -gt 90) {
              if ($x -lt $minX) { $minX = $x }
              if ($x -gt $maxX) { $maxX = $x }
              if ($y -lt $minY) { $minY = $y }
              if ($y -gt $maxY) { $maxY = $y }
            }
          }
        }
        return @{ MinX = $minX; MaxX = $maxX; MinY = $minY; MaxY = $maxY
                  Green = $green }
      }
      foreach ($pair in @(@{ Name = "left"; Rect = $rotLeft },
                          @{ Name = "right"; Rect = $rotRight })) {
        $m = & $measure $pair.Rect $bmp
        $cx = ($m.MinX + $m.MaxX) / 2.0
        $cy = ($m.MinY + $m.MaxY) / 2.0
        $wantX = ($pair.Rect.L + $pair.Rect.R) / 2.0
        $wantY = ($pair.Rect.T + $pair.Rect.B) / 2.0
        Check (($m.MaxX -ge 0) -and
               ([math]::Abs($cx - $wantX) -le 1.0) -and
               ([math]::Abs($cy - $wantY) -le 1.0)) `
          ("the rotate {0} glyph is centred (ink {1:N1},{2:N1} vs button {3:N1},{4:N1})" -f
            $pair.Name, $cx, $cy, $wantX, $wantY)
        Check ($m.Green -eq 0) ("the rotate {0} button has no accent dot" -f $pair.Name)
      }
    } finally {
      $bmp.Dispose()
    }
  }

  # --- 3. Smart thumbnails toggle: on (auto fade) <-> off (規劃/22/08 §3.2) ---
  # V3.1-09: the more-panel switch is "智能缩略图"; on = the rail fades in on
  # hover, off = no rail at all. `pinned` is only reachable from the settings
  # page now, so starting from the auto default the first toggle turns it off.
  $toggled = Dump "more-thumb" @("bottom.more", "menu.1")
  CheckHas $toggled.Log "ui: toggle thumbstrip=off" "the thumbnail toggle logs its state"
  Check ($toggled.Values["strip_mode"] -eq "off") "the toggle turns the rail off"
  Check ($toggled.Values["strip_shown"] -eq "0") "the off rail paints nothing"
  Check ($toggled.Values["strip_counts"] -eq "0") "the off rail reserves no space"
  Check ((Rect $toggled.Values "strip").H -eq 0) "the off rail has no rectangle"

  $persisted = SettingsText "print-thumb"
  Check ($persisted.Values["thumbstrip_mode"] -eq "off") "the thumbnail toggle is written to settings"

  $restart = Dump "restart-off" @()
  Check ($restart.Values["strip_mode"] -eq "off") "a restart restores the off state"
  Check ($restart.Values["strip_shown"] -eq "0") "the restored off rail stays hidden"

  # Toggling again returns to auto: the rail starts faded out but keeps its
  # rectangle for the 8 DIP hot zone and reserves no space.
  $auto = Dump "more-thumb-auto" @("bottom.more", "menu.1")
  CheckHas $auto.Log "ui: toggle thumbstrip=auto" "the toggle can return to auto"
  Check ($auto.Values["strip_mode"] -eq "auto") "the toggle switches back to auto"
  Check ($auto.Values["strip_shown"] -eq "0") "the auto rail starts faded out"
  Check ([double]$auto.Values["strip_opacity"] -eq 0) "the faded-out rail has zero opacity"
  Check ($auto.Values["strip_interactive"] -eq "0") "the faded-out rail owns no clicks"
  Check ([double](Rect $auto.Values "strip").W -gt 0) "the auto rail keeps its rectangle for the hot zone"
  Check ($auto.Values["strip_counts"] -eq "0") "the auto rail reserves no space"
  $contentAuto = Rect $auto.Values "content"
  $bottomAuto = Rect $auto.Values "bottom"
  Check ([math]::Abs($contentAuto.B - $bottomAuto.T) -lt 0.5) "the auto rail gives its space back to the image"

  # --- 4. Bird's eye toggle ------------------------------------------------
  $bird = Dump "more-birdseye" @("bottom.more", "menu.0")
  CheckHas $bird.Log "ui: toggle birdseye=1" "the bird's eye toggle logs its state"
  $birdBack = SettingsText "print-birdseye"
  Check ($birdBack.Values["birdseye"] -eq "1") "the bird's eye toggle is persisted"
  $null = Dump "more-birdseye-off" @("bottom.more", "menu.0")
  $birdReset = SettingsText "print-birdseye-off"
  Check ($birdReset.Values["birdseye"] -eq "0") "the bird's eye toggle can be switched back"

  # "Always on top" is a settings-page row now (v3.0 plan 26 section 2.5);
  # tests\run_settings_check.ps1 covers it through row.topmost.

  # --- 5. Zoom menu --------------------------------------------------------
  $zoom = Dump "percent-open" @("bottom.percent")
  Check ($zoom.Values["popup"] -eq "percent") "clicking the percentage opens the zoom menu"
  Check ([int]$zoom.Values["menu_rows"] -eq 5) "the zoom menu has 5 rows"
  Check ($zoom.Values["menu_above"] -eq "1") "the zoom menu opens above its button"
  $zoomMenu = Rect $zoom.Values "menu"
  Check ($zoomMenu.W -lt $menu.W) "the zoom menu is narrower than the more panel"
  Check ($zoom.Values["fit"] -eq "1") "the image still starts fitted"

  $hundred = Dump "percent-100" @("bottom.percent", "menu.2")
  CheckHas $hundred.Log "cmd: menu=zoom.100" "choosing 100% runs the zoom command"
  Check ([math]::Abs([double]$hundred.Values["zoom"] - 1.0) -lt 0.002) "100% sets the zoom to 1.0"
  Check ($hundred.Values["fit"] -eq "0") "choosing a zoom step leaves fit mode"
  Check ($hundred.Values["popup"] -eq "none") "choosing a row closes the menu"

  $fitBack = Dump "percent-fit" @("bottom.percent", "menu.0")
  CheckHas $fitBack.Log "cmd: menu=zoom.fit" "choosing fit runs the fit command"
  Check ($fitBack.Values["fit"] -eq "1") "fit mode is restored"

  # --- 6. Info card (v3.0: its own top-level window, plan 26 §2.1) ---------
  # The card now remembers "was open" across launches, so these toggle cases are
  # not about persistence: start them from a clean settings file.
  Remove-Item (Join-Path $work "settings.ini") -Force -ErrorAction SilentlyContinue
  $info = Dump "info-open" @("bottom.info")
  Check ($info.Values["info_open"] -eq "1") "the info button opens the card"
  CheckHas $info.Log "ui: info=1" "the info card logs its state"
  $infoRows = [int]$info.Values["info_rows"]
  Check ($infoRows -ge 4) ("the info card has rows (got {0})" -f $infoRows)
  $infoWindow = Rect $info.Values "info_window_rect"
  Check ($null -ne $infoWindow -and $infoWindow.W -gt 0 -and $infoWindow.H -gt 0) `
    "the info card has its own window rectangle"
  $infoClient = $info.Values["info_window_client"].Split("x")
  Check ([double]$infoClient[0] -gt 0 -and [double]$infoClient[1] -gt 0) `
    "the info card client area is reported"
  Check ($info.Values["info_window_shown"] -eq "1") "the info card is on screen"
  Remove-Item (Join-Path $work "settings.ini") -Force -ErrorAction SilentlyContinue
  $infoClosed = Dump "info-closed" @("bottom.info", "bottom.info")
  Check ($infoClosed.Values["info_open"] -eq "0") "a second click closes the info card"

  # --- 7. Title bar segment clicks ----------------------------------------
  Set-Clipboard -Value "nv-sentinel"
  $title = Dump "title-dimension" @("title.dimension")
  CheckHas $title.Log "ui: title_click=dimension" "clicking the dimension segment logs the action"
  CheckHas $title.Log "ui: toast=" "the copy action shows a toast"
  Start-Sleep -Milliseconds 120
  Check ((Get-Clipboard) -eq "800 x 600" -or (Get-Clipboard) -eq "800 $([char]0x00D7) 600") ("the dimension segment copies '800 x 600' (got '{0}')" -f (Get-Clipboard))

  Set-Clipboard -Value "nv-sentinel"
  $titlePath = Dump "title-file-name" @("title.file_name")
  CheckHas $titlePath.Log "ui: title_click=file_name" "clicking the file name logs the action"
  Start-Sleep -Milliseconds 120
  Check ((Get-Clipboard) -like "*a_wide.bmp") "the file name segment copies the full path"

  # v3.0 (plan 26 section 2.2): the caption keeps 文件名 / 尺寸 / 大小 only; the
  # index and the percentage moved to the bottom bar for good.
  Check ($title.Values["segments"] -eq "111") ("the title bar draws three segments (got '{0}')" -f $title.Values["segments"])

  # --- 8. Folder order is Explorer's natural order, fixed -------------------
  # v3.0 (plan 26 section 2.5): the sort group is gone and there is one order.
  # img2 must come before img10 exactly like in Explorer; the old comparison
  # was a lowercase ordinal, which put img10 first.
  $naturalDir = Join-Path $work "natural"
  New-Item -ItemType Directory -Force -Path $naturalDir | Out-Null
  Copy-Item $firstImage (Join-Path $naturalDir "img10.bmp")
  Copy-Item $firstImage (Join-Path $naturalDir "img2.bmp")
  $natural = Dump "natural-order" @() (Join-Path $naturalDir "img10.bmp")
  Check ([int]$natural.Values["folder_files"] -eq 2) "the natural-order folder lists two images"
  Check ([int]$natural.Values["folder_index"] -eq 1) ("img10 follows img2 in Explorer order (got index {0})" -f $natural.Values["folder_index"])
  Check ($natural.Values["file_name"] -eq "img10.bmp") "opening img10.bmp keeps it as the current file"

  # --- 9. DPI: the panels scale without leaving the client ----------------
  foreach ($scale in @(100, 125, 150, 200)) {
    $dpiFile = Join-Path $work ("layout-{0}.txt" -f $scale)
    Remove-Item $dpiFile -ErrorAction SilentlyContinue
    $log = RunApp ("dpi-force-{0}" -f $scale) @(
      "--force-dpi-scale", "$scale", "--layout-dump", $dpiFile,
      "--click-hit", "bottom.more", $firstImage)
    $values = @{}
    if (Test-Path $dpiFile) {
      foreach ($line in Get-Content $dpiFile) {
        $parts = $line.Split("=", 2)
        if ($parts.Count -eq 2) { $values[$parts[0]] = $parts[1] }
      }
    }
    $scaledMenu = Rect $values "menu"
    $scaledClient = ClientRect $values
    $scaledBottom = Rect $values "bottom"
    $scaledRow = Rect $values "menu_row_0"
    Check (Inside $scaledMenu $scaledClient) ("forced {0}%: menu inside the client" -f $scale)
    Check ($scaledMenu.B -le $scaledBottom.T + 0.5) ("forced {0}%: menu above the bottom bar" -f $scale)
    $expected = 32 * [double]$values["dpi"]
    Check ([math]::Abs($scaledRow.H - $expected) -lt 1.0) ("forced {0}%: rows scale with DPI ({1} px)" -f $scale, $scaledRow.H)
    Check ([math]::Abs([double]$values["dpi"] - ($scale / 100.0)) -lt 0.001) ("forced {0}%: the reported scale matches" -f $scale)
  }
}
finally {
  if (-not $KeepOutput) {
    Remove-Item $work -Recurse -Force -ErrorAction SilentlyContinue
  } else {
    Write-Host ("Output kept in {0}" -f $work)
  }
}

Write-Host ""
if ($script:failures -eq 0) {
  Write-Host ("OK: {0} chrome checks passed" -f $script:checks) -ForegroundColor Green
  exit 0
}
Write-Host ("FAILED: {0} of {1} chrome checks failed" -f $script:failures, $script:checks) -ForegroundColor Red
exit 1
