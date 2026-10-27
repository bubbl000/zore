#requires -Version 5.1
<#
  V3-03 transition-frame check (ASCII only).

  Generates a 24 MP JPEG, warms its shared thumbnail in a throw-away portable
  directory, then opens it twice:
    - warm cache: must log "preview: shown source=cache" and show the badge;
    - empty cache: must not show a preview at all.

  The existing S3-13 script (run_two_stage_check.ps1, 54 checks) is not changed;
  this is the extra V3-03 acceptance for the 24 MP / 12 MB JPEG case.
#>
param(
  [string]$App = ".\build\Release\zoreview.exe"
)

$ErrorActionPreference = "Stop"
$repo = Split-Path -Parent $PSScriptRoot
Set-Location $repo
if (-not (Test-Path $App)) { throw "Application not found: $App" }
$App = (Resolve-Path $App).Path

Add-Type -AssemblyName System.Drawing
if (-not ([System.Management.Automation.PSTypeName]'V303Sample').Type) {
  Add-Type -ReferencedAssemblies System.Drawing -TypeDefinition @"
using System;
using System.Drawing;
using System.Drawing.Imaging;
using System.Runtime.InteropServices;
public static class V303Sample {
  public static void MakeNoisyJpeg(string path, int width, int height, long quality) {
    using (Bitmap bitmap = new Bitmap(width, height, PixelFormat.Format24bppRgb)) {
      Rectangle rect = new Rectangle(0, 0, width, height);
      BitmapData data = bitmap.LockBits(rect, ImageLockMode.WriteOnly,
                                        PixelFormat.Format24bppRgb);
      try {
        byte[] buffer = new byte[data.Stride * height];
        new Random(20261026).NextBytes(buffer);
        Marshal.Copy(buffer, 0, data.Scan0, buffer.Length);
      } finally {
        bitmap.UnlockBits(data);
      }
      ImageCodecInfo jpeg = null;
      foreach (ImageCodecInfo codec in ImageCodecInfo.GetImageEncoders()) {
        if (codec.FormatID == ImageFormat.Jpeg.Guid) { jpeg = codec; break; }
      }
      EncoderParameters parameters = new EncoderParameters(1);
      parameters.Param[0] = new EncoderParameter(Encoder.Quality, quality);
      bitmap.Save(path, jpeg, parameters);
      parameters.Dispose();
    }
  }
}
"@
}

$work = Join-Path $env:TEMP ("nv-v303-" + [guid]::NewGuid().ToString("N"))
New-Item -ItemType Directory -Path $work | Out-Null
$appExe = Join-Path $work "zoreview.exe"
Copy-Item $App $appExe
$appDirectory = Split-Path -Parent $App
Get-ChildItem $appDirectory -Filter "clang_rt.asan*.dll" -ErrorAction SilentlyContinue |
  ForEach-Object { Copy-Item $_.FullName (Join-Path $work $_.Name) }

$images = Join-Path $work "images"
New-Item -ItemType Directory -Path $images | Out-Null
$large = Join-Path $images "large.jpg"
$neighbor = Join-Path $images "neighbor.jpg"
$logPath = Join-Path $work "logs\zoreview.log"
$dumpPath = Join-Path $work "layout.txt"

function New-Jpeg([string]$path, [int]$width, [int]$height, [int]$seed) {
  $bitmap = New-Object System.Drawing.Bitmap($width, $height)
  $graphics = [System.Drawing.Graphics]::FromImage($bitmap)
  try {
    $rect = New-Object System.Drawing.Rectangle(0, 0, $width, $height)
    $c1 = [System.Drawing.Color]::FromArgb(30 + $seed, 80, 140)
    $c2 = [System.Drawing.Color]::FromArgb(220, 160 - $seed, 40)
    $brush = New-Object System.Drawing.Drawing2D.LinearGradientBrush($rect, $c1, $c2, 35.0)
    try { $graphics.FillRectangle($brush, $rect) } finally { $brush.Dispose() }
    $pen = New-Object System.Drawing.Pen([System.Drawing.Color]::White, 3)
    try {
      for ($x = 0; $x -lt $width; $x += 97) {
        $graphics.DrawLine($pen, $x, 0, $x + 41, $height)
      }
    } finally { $pen.Dispose() }
  } finally {
    $graphics.Dispose()
    $bitmap.Save($path, [System.Drawing.Imaging.ImageFormat]::Jpeg)
    $bitmap.Dispose()
  }
}

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

function ReadDump([string]$path) {
  $values = @{}
  if (-not (Test-Path $path)) { return $values }
  foreach ($line in Get-Content $path -Encoding UTF8) {
    $parts = $line.Split("=", 2)
    if ($parts.Count -eq 2) { $values[$parts[0]] = $parts[1] }
  }
  return $values
}

function Reset-State {
  Remove-Item (Join-Path $work "settings.ini") -Force -ErrorAction SilentlyContinue
  Remove-Item $dumpPath -Force -ErrorAction SilentlyContinue
  Remove-Item $logPath -Force -ErrorAction SilentlyContinue
}

function Run-App([string[]]$extra, [int]$dumpAt = 0, [int]$quitAt = 0) {
  Reset-State
  $arguments = @("--portable", "--language", "en")
  if ($dumpAt -gt 0) {
    $arguments += @("--layout-dump", $dumpPath, "--dump-at", [string]$dumpAt)
  }
  if ($quitAt -gt 0) { $arguments += @("--quit-at", [string]$quitAt) }
  $arguments += $extra
  $arguments += $large
  $quoted = ($arguments | ForEach-Object {
    if ($_ -match '\s') { '"' + $_ + '"' } else { $_ }
  }) -join ' '
  $process = Start-Process -FilePath $appExe -ArgumentList $quoted -PassThru
  if (-not $process.WaitForExit(60000)) {
    $process.Kill()
    throw "zoreview.exe did not exit in time"
  }
  Start-Sleep -Milliseconds 150
  $text = ""
  if (Test-Path $logPath) { $text = Get-Content $logPath -Raw }
  return @{ Values = (ReadDump $dumpPath); Log = $text; Exit = $process.ExitCode }
}

try {
  [V303Sample]::MakeNoisyJpeg($large, 6000, 4000, 70)
  New-Jpeg $neighbor 800 600 90
  $largeSize = (Get-Item $large).Length
  Check ($largeSize -gt (1MB)) ("the generated 24 MP JPEG is not a tiny file ({0:N0} B)" -f $largeSize)

  # 1. Warm the shared thumbnail with a pinned strip (no layout dump needed).
  $warm = Run-App @("--strip=pinned") 0 7000
  Check ($warm.Log -match "thumbnail ready: large\.jpg") "the warm run caches the 24 MP thumbnail"
  $thumbFiles = @(Get-ChildItem (Join-Path $work "cache\thumbs") -Recurse -File -ErrorAction SilentlyContinue)
  Check ($thumbFiles.Count -ge 1) "the warm run writes a disk thumbnail"

  # 2. Warm cache: the cached 160 px frame must be shown before the full decode.
  # V3.1-07 S3 raised kThumbEdge 128 -> 160 (the transition-frame size), so the
  # preview is 160 px on the long edge; the aspect of a 6000x4000 file makes the
  # short edge 106 or 107 depending on the decoder's rounding.
  $hit = Run-App @() 100 900
  $v = $hit.Values
  Check ($hit.Log -match "preview: shown source=cache") "the warm open logs a cache preview"
  $thumbSize = $null
  if ($hit.Log -match "size=(\d+)x(\d+) target=6000x4000") {
    $thumbSize = @([int]$Matches[1], [int]$Matches[2])
  }
  Check ($thumbSize -and $thumbSize[0] -eq 160 -and $thumbSize[1] -ge 105 -and $thumbSize[1] -le 108) `
    "the cache preview knows the 24 MP target size (thumb 160 px)"
  Check ($hit.Log -notmatch "preview: shown source=embedded") "the warm open does not use an embedded preview"
  Check ($v["preview_badge_visible"] -eq "1" -or [double]$v["preview_opacity"] -gt 0.01) "the preview badge is visible"

  # 3. Cold cache: no preview, no badge (the negative control).
  Remove-Item (Join-Path $work "cache") -Recurse -Force -ErrorAction SilentlyContinue
  $miss = Run-App @() 100 900
  $v = $miss.Values
  Check ($miss.Log -notmatch "preview: shown") "the cold open does not log a preview"
  Check ($v["preview_badge_visible"] -eq "0") "the cold open has no preview badge"
  Check ($v["preview_active"] -eq "0") "the cold open does not activate a preview"
  Check ($v["has_image"] -eq "1" -or $v["loading_shown"] -eq "1") "the cold open is still loading or already shows the full image"
}
finally {
  Remove-Item $work -Recurse -Force -ErrorAction SilentlyContinue
}

Write-Host ""
if ($script:failures -gt 0) {
  Write-Host ("FAILED: {0} of {1} checks" -f $script:failures, $script:checks) -ForegroundColor Red
  exit 1
}
Write-Host ("OK: {0} transition checks passed" -f $script:checks) -ForegroundColor Green
exit 0



