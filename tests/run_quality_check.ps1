# v2.0 S3 (plan 22/18): scaling quality.
#
# Covers the interpolation state machine (linear while panning/zooming/animation,
# high-quality cubic at rest), pixel-mode magnification, the 1/4 display proxy,
# screenshot parity and the --force-quality / --proxy test hooks.
#
# The script runs a throw-away copy of the executable in portable mode and forces
# 125% DPI, so the layout numbers are reproducible on any host. All strings are
# ASCII, so the file needs no UTF-8 BOM (HANDOFF pitfalls 60/124).
#
# Reference comparisons need Python + Pillow (already used by the sample tools in
# this repository); they are skipped with a message when Pillow is unavailable.

param(
  [string]$App = ".\build\Release\zoreview.exe",
  [string]$Image8k = (Join-Path $PSScriptRoot "output\8k.jpg"),
  [switch]$SkipSlow,
  [switch]$KeepOutput
)

$ErrorActionPreference = "Stop"
$repo = Split-Path -Parent $PSScriptRoot
Set-Location $repo

if (-not (Test-Path $App)) {
  throw "Application not found: $App (build first: tools\build.ps1)"
}
$App = (Resolve-Path $App).Path

Add-Type -AssemblyName System.Drawing

$work = Join-Path $env:TEMP ("nv-quality-" + [guid]::NewGuid().ToString("N"))
New-Item -ItemType Directory -Path $work | Out-Null
$appExe = Join-Path $work "zoreview.exe"
Copy-Item $App $appExe
# AddressSanitizer builds need their runtime next to the copied executable.
$appDirectory = Split-Path -Parent $App
Get-ChildItem $appDirectory -Filter "clang_rt.asan*.dll" -ErrorAction SilentlyContinue |
  ForEach-Object { Copy-Item $_.FullName (Join-Path $work $_.Name) }

$outputDir = Join-Path $PSScriptRoot "output\quality"
New-Item -ItemType Directory -Force -Path $outputDir | Out-Null

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
  Check ($text -match [regex]::Escape($pattern)) `
    ("{0} (expected '{1}')" -f $name, $pattern)
}

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

function Point([hashtable]$values, [string]$key) {
  if (-not $values.ContainsKey($key)) { return $null }
  $numbers = $values[$key].Split(",") | ForEach-Object { [double]$_ }
  if ($numbers.Count -ne 2) { return $null }
  return @{ X = $numbers[0]; Y = $numbers[1] }
}

function LogText() {
  $log = Join-Path $work "logs\zoreview.log"
  if (Test-Path $log) { return (Get-Content $log -Raw) }
  return ""
}

function ClearState([switch]$PixelMode) {
  Remove-Item (Join-Path $work "settings.ini") -Force -ErrorAction SilentlyContinue
  Remove-Item (Join-Path $work "logs\zoreview.log") -Force -ErrorAction SilentlyContinue
  if ($PixelMode) {
    # A pre-seeded settings file is deterministic; the settings-window path is
    # covered by run_settings_check.ps1.
    Set-Content (Join-Path $work "settings.ini") `
      "[general]`nversion=1`n[view]`npixel_mode=1`n" -Encoding ASCII
  }
}

function StartApp([string[]]$extra, [string]$image, [int]$timeoutMs = 40000) {
  $arguments = @("--portable", "--language", "en", "--force-dpi-scale", "125",
                 "--strip=pinned") + $extra
  if ($image) { $arguments += $image }
  $process = Start-Process -FilePath $appExe -ArgumentList $arguments -PassThru
  if (-not $process.WaitForExit($timeoutMs)) {
    $process.Kill()
    throw ("zoreview.exe did not exit in time: {0}" -f ($extra -join " "))
  }
  Start-Sleep -Milliseconds 150
}

# Dump: --layout-dump exits after the queued hooks have run.
function Dump([string]$label, [string[]]$extra, [string]$image,
              [switch]$PixelMode, [int]$dpi = 125) {
  ClearState -PixelMode:$PixelMode
  $dump = Join-Path $work ("dump-" + $label + ".txt")
  Remove-Item $dump -Force -ErrorAction SilentlyContinue
  $arguments = @("--layout-dump", $dump, "--force-dpi-scale", "$dpi") + $extra
  StartApp $arguments $image
  return @{ Values = (ReadDump $dump); Log = (LogText); Dump = $dump }
}

# Timed run: --dump-at writes a live-state dump and --quit-at ends the process,
# so the log still contains the transitions that led there.
function Timed([string]$label, [string[]]$extra, [string]$image,
               [int]$dumpAt, [int]$quitAt, [switch]$PixelMode) {
  ClearState -PixelMode:$PixelMode
  $dump = Join-Path $work ("dump-" + $label + ".txt")
  Remove-Item $dump -Force -ErrorAction SilentlyContinue
  $arguments = @("--layout-dump", $dump, "--dump-at", "$dumpAt",
                 "--quit-at", "$quitAt") + $extra
  StartApp $arguments $image
  return @{ Values = (ReadDump $dump); Log = (LogText); Dump = $dump }
}

function Shot([string]$label, [int]$delay, [string[]]$extra, [string]$image,
              [switch]$PixelMode) {
  ClearState -PixelMode:$PixelMode
  $png = Join-Path $outputDir ($label + ".png")
  Remove-Item $png -Force -ErrorAction SilentlyContinue
  $arguments = @("--screenshot", $png, "--shot-delay", "$delay") + $extra
  StartApp $arguments $image
  return @{ Path = $png; Log = (LogText) }
}

function FileHash([string]$path) {
  if (-not (Test-Path $path)) { return "" }
  return (Get-FileHash -Algorithm SHA256 $path).Hash
}

function FirstIndex([string]$text, [string]$pattern) {
  $match = [regex]::Match($text, [regex]::Escape($pattern))
  if ($match.Success) { return $match.Index }
  return -1
}

function LastIndex([string]$text, [string]$pattern) {
  $matches = [regex]::Matches($text, [regex]::Escape($pattern))
  if ($matches.Count -gt 0) { return $matches[$matches.Count - 1].Index }
  return -1
}

function DistinctColors([string]$path, [double]$cx, [double]$cy, [int]$half) {
  if (-not (Test-Path $path)) { return -1 }
  $bitmap = [System.Drawing.Bitmap]::FromFile($path)
  try {
    $set = @{}
    $left = [int][math]::Max(0, [math]::Round($cx) - $half)
    $top = [int][math]::Max(0, [math]::Round($cy) - $half)
    $right = [int][math]::Min($bitmap.Width - 1, [math]::Round($cx) + $half)
    $bottom = [int][math]::Min($bitmap.Height - 1, [math]::Round($cy) + $half)
    for ($y = $top; $y -le $bottom; $y++) {
      for ($x = $left; $x -le $right; $x++) {
        $c = $bitmap.GetPixel($x, $y)
        $set[("{0},{1},{2}" -f $c.R, $c.G, $c.B)] = 1
      }
    }
    return $set.Count
  } finally {
    $bitmap.Dispose()
  }
}

# --- Samples ---------------------------------------------------------------
# 1600x1200 checker with 8 px blocks: fitted it is below 100% (cubic), and after
# "1,+,+" it is magnified, where NEAREST can only produce the two source colours;
# any interpolation creates a third.
$checkerPath = Join-Path $work "checker.png"
$checker = New-Object System.Drawing.Bitmap(1600, 1200)
$checkerGraphics = [System.Drawing.Graphics]::FromImage($checker)
$colorA = [System.Drawing.Color]::FromArgb(220, 30, 30)
$colorB = [System.Drawing.Color]::FromArgb(20, 240, 240)
$brushA = New-Object System.Drawing.SolidBrush($colorA)
$brushB = New-Object System.Drawing.SolidBrush($colorB)
$rectsA = New-Object 'System.Collections.Generic.List[System.Drawing.Rectangle]'
$rectsB = New-Object 'System.Collections.Generic.List[System.Drawing.Rectangle]'
for ($by = 0; $by -lt 150; $by++) {
  for ($bx = 0; $bx -lt 200; $bx++) {
    $rect = New-Object System.Drawing.Rectangle(($bx * 8), ($by * 8), 8, 8)
    if ((($bx + $by) % 2) -eq 0) { $rectsA.Add($rect) } else { $rectsB.Add($rect) }
  }
}
$checkerGraphics.FillRectangles($brushA, $rectsA.ToArray())
$checkerGraphics.FillRectangles($brushB, $rectsB.ToArray())
$checkerGraphics.Dispose()
$brushA.Dispose()
$brushB.Dispose()
$checker.Save($checkerPath, [System.Drawing.Imaging.ImageFormat]::Png)
$checker.Dispose()

$python = Get-Command python -ErrorAction SilentlyContinue
$hasPillow = $false
if ($null -ne $python) {
  & python -c "import PIL" 2>$null
  $hasPillow = ($LASTEXITCODE -eq 0)
}

$eightK = $null
if (Test-Path $Image8k) { $eightK = (Resolve-Path $Image8k).Path }

try {
  # --- 1. Automatic policy and the proxy ---------------------------------
  if ($null -eq $eightK) {
    Write-Host "skip 8K quality checks (tests\output\8k.jpg not generated)"
  } else {
    $base = Dump "8k-default" @() $eightK
    Check ($base.Values["quality"] -eq "good") "a static 8K fit renders with cubic"
    Check ($base.Values["quality_last"] -eq "good") "the last drawn frame was good"
    Check ($base.Values["quality_forced"] -eq "auto") "quality is automatic by default"
    Check ($base.Values["interactive"] -eq "0") "an idle view is not interactive"
    Check ($base.Values["proxy_enabled"] -eq "1") "the display proxy is enabled by default"
    Check ($base.Values["proxy_ready"] -eq "1") "the 1/4 proxy was baked for 8K"
    Check ($base.Values["proxy_active"] -eq "0") "a static frame draws the original, not the proxy"
    CheckHas $base.Log "render: quality=good" "the quality transition is logged"
    CheckHas $base.Log "renderer: proxy 1920x1080" "the 8K proxy is 1/4 of the source"

    $proxyLine = [regex]::Match($base.Log, "renderer: proxy ([0-9]+)x([0-9]+) bgra=([0-9.]+)MB bake=([0-9.]+)ms")
    Check $proxyLine.Success "the proxy bake reports its size and cost"
    if ($proxyLine.Success) {
      $proxyMb = [double]$proxyLine.Groups[3].Value
      $bakeMs = [double]$proxyLine.Groups[4].Value
      Check ($proxyMb -le 9.0) ("the 8K proxy stays under 9 MB (actual={0:0.0}MB)" -f $proxyMb)
      Check ($bakeMs -le 60.0) ("the proxy bakes in one interactive-frame budget (actual={0:0.0}ms)" -f $bakeMs)
    }

    $fast = Dump "8k-forced-fast" @("--force-quality", "fast") $eightK
    Check ($fast.Values["quality"] -eq "fast") "--force-quality fast wins over the static policy"
    Check ($fast.Values["quality_forced"] -eq "fast") "the forced quality is reported"
    Check ($fast.Values["proxy_active"] -eq "1") "interactive frames draw the 1/4 proxy"

    $good = Dump "8k-forced-good" @("--force-quality", "good") $eightK
    Check ($good.Values["quality"] -eq "good") "--force-quality good keeps cubic"
    Check ($good.Values["proxy_active"] -eq "0") "the good path never substitutes the proxy"

    $noProxy = Dump "8k-no-proxy" @("--proxy", "off") $eightK
    Check ($noProxy.Values["proxy_enabled"] -eq "0") "--proxy off is reported"
    Check ($noProxy.Values["proxy_ready"] -eq "0") "--proxy off skips the bake"
    Check (-not ($noProxy.Log -match [regex]::Escape("renderer: proxy"))) `
      "--proxy off logs no proxy allocation"

    $rotated = Dump "8k-rot90" @("--rotation", "90") $eightK
    Check ($rotated.Values["rotation"] -eq "1") "the rotation reached the renderer"
    Check ($rotated.Values["quality"] -eq "good") "a rotated static view still uses cubic"

    $fullscreen = Dump "8k-fullscreen" @("--fullscreen") $eightK
    Check ($fullscreen.Values["quality"] -eq "good") "fullscreen keeps cubic for a static view"

    $dpi200 = Dump "8k-dpi200" @() $eightK -dpi 200
    Check ($dpi200.Values["dpi"] -match "^2") "the 200% DPI override is active"
    Check ($dpi200.Values["quality"] -eq "good") "the quality choice is DPI independent"

    $small = Dump "small-default" @() $checkerPath
    Check ($small.Values["quality"] -eq "good") "a fitted image at ~50% uses cubic"
    Check ($small.Values["proxy_ready"] -eq "0") `
      "an image below the 4x proxy threshold never allocates a proxy"
  }

  # --- 2. Screenshot parity and reference quality -------------------------
  if ($null -eq $eightK) {
    Write-Host "skip 8K screenshot comparison"
  } else {
    $autoShot = Shot "8k-auto" 2200 @() $eightK
    $autoHash = FileHash $autoShot.Path
    Check ($autoShot.Path -and (Test-Path $autoShot.Path)) "the static screenshot exists"

    $autoShot2 = Shot "8k-auto-again" 2200 @() $eightK
    Check ((FileHash $autoShot2.Path) -eq $autoHash) "two static captures are byte-identical"
    CheckHas $autoShot.Log "render: quality=good" "the capture path renders good"

    $goodShot = Shot "8k-good" 2200 @("--force-quality", "good") $eightK
    $fastShot = Shot "8k-fast" 2200 @("--force-quality", "fast") $eightK
    Check ((FileHash $goodShot.Path) -eq $autoHash) `
      "a screenshot matches the static visual (auto == forced good)"
    Check ((FileHash $fastShot.Path) -ne $autoHash) `
      "--force-quality fast captures a different frame than good"

    if ($hasPillow) {
      $content = Rect $base.Values "content"
      $compare = & python (Join-Path $PSScriptRoot "tools\quality_compare.py") `
        $eightK $fastShot.Path $goodShot.Path $content.L $content.T $content.W $content.H
      $compare | ForEach-Object { Write-Host ("     {0}" -f $_) }
      $linear = ([regex]::Match(($compare -join " "), "linear=([0-9.]+)")).Groups[1].Value
      $cubic = ([regex]::Match(($compare -join " "), "cubic=([0-9.]+)")).Groups[1].Value
      $improvement = ([regex]::Match(($compare -join " "), "improvement=([0-9.]+)")).Groups[1].Value
      Check ($linear -ne "" -and $cubic -ne "") "the Lanczos comparison produced numbers"
      if ($linear -ne "" -and $cubic -ne "") {
        Check ([double]$cubic -lt [double]$linear) `
          ("cubic is closer to the Lanczos reference on the 8K photo (linear={0} cubic={1})" -f $linear, $cubic)
        Check ([double]$improvement -ge 5.0) `
          ("the photo error drops by at least 5% (actual={0}%)" -f $improvement)
      }
    } else {
      Write-Host "skip Lanczos comparison (Python Pillow not available)"
    }
  }

  # High-frequency content: linear aliases badly, cubic must be far closer.
  if ($hasPillow -and -not $SkipSlow) {
    $finePath = Join-Path $work "quality-fine.png"
    & python (Join-Path $PSScriptRoot "tools\make_quality_sample.py") $finePath | Out-Null
    if (Test-Path $finePath) {
      $fineDump = Dump "fine-default" @() $finePath
      $fineFast = Shot "fine-fast" 2600 @("--force-quality", "fast") $finePath
      $fineGood = Shot "fine-good" 2600 @("--force-quality", "good") $finePath
      $fineContent = Rect $fineDump.Values "content"
      $compare = & python (Join-Path $PSScriptRoot "tools\quality_compare.py") `
        $finePath $fineFast.Path $fineGood.Path $fineContent.L $fineContent.T `
        $fineContent.W $fineContent.H
      $compare | ForEach-Object { Write-Host ("     {0}" -f $_) }
      $joined = $compare -join " "
      $linear = ([regex]::Match($joined, "linear=([0-9.]+)")).Groups[1].Value
      $cubic = ([regex]::Match($joined, "cubic=([0-9.]+)")).Groups[1].Value
      $improvement = ([regex]::Match($joined, "improvement=([0-9.]+)")).Groups[1].Value
      Check ($linear -ne "" -and $cubic -ne "") "the fine-detail comparison produced numbers"
      if ($linear -ne "" -and $cubic -ne "") {
        Check ([double]$linear -ge 5.0) `
          ("the fine sample really exercises interpolation (linear={0})" -f $linear)
        Check ([double]$improvement -ge 30.0) `
          ("cubic reduces the fine-detail error by at least 30% (actual={0}%)" -f $improvement)
        Check ([double]$cubic -le 2.0) `
          ("the static fine-detail error is small (cubic={0})" -f $cubic)
      }
    } else {
      Check $false "the fine-detail sample was generated"
    }
  } else {
    Write-Host "skip the fine-detail comparison (Pillow missing or -SkipSlow)"
  }

  # --- 3. Pixel mode -------------------------------------------------------
  $pixelFit = Dump "pixel-fit" @() $checkerPath -PixelMode
  Check ($pixelFit.Values["has_image"] -eq "1") "pixel mode loads the checker"
  Check ([double]$pixelFit.Values["zoom"] -lt 1.0) "the fitted checker is below 100%"
  Check ($pixelFit.Values["quality"] -eq "good") `
    "pixel mode below 100% still uses cubic (no shrink aliasing)"

  $pixelZoom = Timed "pixel-zoom" @("--send-keys", "1,+,+") $checkerPath 2600 3200 -PixelMode
  Check ([double]$pixelZoom.Values["zoom"] -gt 1.0) "the zoom keys magnified the checker"
  Check ($pixelZoom.Values["quality"] -eq "pixel") `
    "pixel mode at or above 100% uses nearest neighbour"
  CheckHas $pixelZoom.Log "render: quality=pixel" "the nearest-neighbour frame is logged"

  $pixelShot = Shot "pixel-shot" 2800 @("--send-keys", "1,+,+") $checkerPath -PixelMode
  $pixelGood = Shot "pixel-good" 2800 @("--force-quality", "good", "--send-keys", "1,+,+") `
    $checkerPath -PixelMode
  $center = Point $pixelZoom.Values "view_center"
  if ($null -ne $center) {
    $nearestColors = DistinctColors $pixelShot.Path $center.X $center.Y 20
    $cubicColors = DistinctColors $pixelGood.Path $center.X $center.Y 20
    Check ($nearestColors -eq 2) `
      ("nearest neighbour keeps exactly the two source colours (actual={0})" -f $nearestColors)
    Check ($cubicColors -gt 10) `
      ("cubic produces interpolation colours at the same spot (actual={0})" -f $cubicColors)
  } else {
    Check $false "the pixel-mode dump reports the view centre"
  }

  # --- 4. Interaction and animation transitions ----------------------------
  if ($null -ne $eightK) {
    $zoom = Timed "8k-zoom" @("--send-keys", "+") $eightK 3000 3600
    Check ($zoom.Values["zoom"] -gt ($base.Values["zoom"] -as [double])) `
      "the zoom key changed the 8K scale"
    Check ($zoom.Values["interactive"] -eq "0") "the interaction window expired before the dump"
    Check ($zoom.Values["quality"] -eq "good") "the settled view is back to cubic"
    Check ((FirstIndex $zoom.Log "render: quality=fast") -ge 0) `
      "the zoom rendered at least one fast frame"
    Check ((LastIndex $zoom.Log "render: quality=good") -gt (FirstIndex $zoom.Log "render: quality=fast")) `
      "the log shows fast first and good after the interaction"
  }

  $animImages = $null
  foreach ($root in @($repo, (Split-Path -Parent $repo))) {
    foreach ($dir in Get-ChildItem -LiteralPath $root -Directory -ErrorAction SilentlyContinue) {
      if (Test-Path (Join-Path $dir.FullName "images\gif\anim.gif")) {
        $animImages = Join-Path $dir.FullName "images"
        break
      }
    }
    if ($null -ne $animImages) { break }
  }
  $animPath = $null
  if ($null -ne $animImages) {
    $animPath = Join-Path $animImages "gif\anim.gif"
  }
  if ($null -ne $animPath -and (Test-Path $animPath)) {
    # Space pauses playback at 1200 ms; the dump at 3200 ms must show a static
    # good frame with animating=0.
    $anim = Timed "anim-pause" @("--send-keys", "Space") (Resolve-Path $animPath).Path 3200 3800
    Check ($anim.Values["anim_state"] -eq "paused") "Space paused the animation"
    Check ($anim.Values["animating"] -eq "0") "a paused animation is no longer animating"
    Check ($anim.Values["quality"] -eq "good") "a paused animation upgrades to cubic"
    Check ((FirstIndex $anim.Log "render: quality=fast") -ge 0) `
      "playback frames used the fast path"
    Check ((LastIndex $anim.Log "render: quality=good") -gt (FirstIndex $anim.Log "render: quality=fast")) `
      "the animation log shows fast playback then good after pause"
  } else {
    Write-Host "skip animation quality check (anim.gif not found)"
  }

  # --- Summary -------------------------------------------------------------
  Write-Host ""
  Write-Host ("{0} checks, {1} failure(s)" -f $script:checks, $script:failures)
  Write-Host "Screenshots: $outputDir"
  if ($script:failures -ne 0) {
    Write-Host "Artifacts kept at $work" -ForegroundColor Yellow
    $KeepOutput = $true
    exit 1
  }
  Write-Host "OK: scaling quality checks passed"
} finally {
  if (-not $KeepOutput) {
    Remove-Item $work -Recurse -Force -ErrorAction SilentlyContinue
  }
}
