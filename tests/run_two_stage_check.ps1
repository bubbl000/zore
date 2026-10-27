# v2.0 S3 (plan 22/13): two-stage loading.
#
# Covers the preview policy, the RAW camera-embedded JPEG preview, the shared
# thumbnail cache preview (a CR3 stores an H.265 preview, which WIC cannot open,
# so its second open uses the cached thumbnail), the "preview" badge, the
# preserved view on replacement, the failure path that keeps the preview and the
# export block while only the preview exists.
#
# Every run uses a throw-away portable directory, so the developer's
# %LOCALAPPDATA% settings and caches are untouched (22/00 rule 10). State is
# asserted from --layout-dump and the application log.
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

# The samples live in a folder whose name is not ASCII. Find it by content.
$samples = $null
foreach ($root in @($repo, (Split-Path -Parent $repo))) {
  foreach ($dir in Get-ChildItem -LiteralPath $root -Directory -ErrorAction SilentlyContinue) {
    if (Test-Path (Join-Path $dir.FullName "images\jpeg\photo_1600.jpg")) {
      $samples = $dir.FullName
      break
    }
  }
  if ($null -ne $samples) { break }
}
if ($null -eq $samples) { throw "sample folder not found near $repo" }

$cr2Source = Join-Path $samples "raw\Canon\EOS 5D Mark IV\B13A0729.CR2"
$cr3Source = Join-Path $samples "raw\Canon\Canon EOS R5m2\RAW.CR3"
foreach ($sample in @($cr2Source, $cr3Source)) {
  if (-not (Test-Path -LiteralPath $sample)) {
    throw "RAW sample not found: $sample"
  }
}

$work = Join-Path $env:TEMP ("nv-twostage-" + [guid]::NewGuid().ToString("N"))
New-Item -ItemType Directory -Path $work | Out-Null
$appExe = Join-Path $work "zoreview.exe"
Copy-Item $App $appExe
$appDirectory = Split-Path -Parent $App
Get-ChildItem $appDirectory -Filter "clang_rt.asan*.dll" -ErrorAction SilentlyContinue |
  ForEach-Object { Copy-Item $_.FullName (Join-Path $work $_.Name) }

$cr2 = Join-Path $work "sample.CR2"
$cr3 = Join-Path $work "sample.CR3"
$photo = Join-Path $work "photo.jpg"
Copy-Item -LiteralPath $cr2Source $cr2
Copy-Item -LiteralPath $cr3Source $cr3
Copy-Item (Join-Path $samples "images\jpeg\photo_1600.jpg") $photo

# JPEG -> JPEG -> CR2 navigation folder: the RAW is not an immediate neighbour,
# so its full decode is not running as a prefetch when the preview starts.
$nav = Join-Path $work "nav"
New-Item -ItemType Directory -Path $nav | Out-Null
Copy-Item $photo (Join-Path $nav "a.jpg")
Copy-Item $photo (Join-Path $nav "b.jpg")
Copy-Item -LiteralPath $cr2Source (Join-Path $nav "c.CR2")

$shots = Join-Path $repo "tests\output\two-stage"
New-Item -ItemType Directory -Path $shots -Force | Out-Null
$dumpPath = Join-Path $work "layout.txt"
$logPath = Join-Path $work "logs\zoreview.log"

$script:checks = 0
$script:failures = 0

Add-Type -AssemblyName System.Windows.Forms | Out-Null
Add-Type -AssemblyName System.Drawing | Out-Null
function ParkCursor() {
  [System.Windows.Forms.Cursor]::Position = (New-Object System.Drawing.Point(5, 5))
}

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

function ReadDump([string]$path) {
  $values = @{}
  if (-not (Test-Path $path)) { return $values }
  foreach ($line in Get-Content $path -Encoding UTF8) {
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

function Point([hashtable]$values, [string]$key) {
  if (-not $values.ContainsKey($key)) { return $null }
  $parts = $values[$key].Split(",")
  if ($parts.Count -ne 2) { return $null }
  return @{ X = [double]$parts[0]; Y = [double]$parts[1] }
}

function StartApp([string[]]$extra, [string]$image, [int]$dumpAt = 0,
                  [int]$quitAt = 0, [string]$shot = "", [int]$shotDelay = 0) {
  Remove-Item (Join-Path $work "settings.ini") -Force -ErrorAction SilentlyContinue
  Remove-Item $dumpPath -Force -ErrorAction SilentlyContinue
  Remove-Item $logPath -Force -ErrorAction SilentlyContinue

  if ($shot -ne "") {
    $png = Join-Path $shots ($shot + ".png")
    Remove-Item $png -Force -ErrorAction SilentlyContinue
    ParkCursor
    $arguments = @("--portable", "--language", "en", "--screenshot", $png,
                   "--shot-delay", [string]$shotDelay)
    $arguments += $extra
    $arguments += $image
  } else {
    $arguments = @("--portable", "--language", "en")
    if ($dumpAt -gt 0) {
      $arguments += @("--layout-dump", $dumpPath, "--dump-at", [string]$dumpAt,
                      "--quit-at", [string]$quitAt)
    } elseif ($quitAt -gt 0) {
      # Cache warm-up run: no dump, just keep the process alive long enough for
      # the background thumbnail to be saved to the disk cache.
      $arguments += @("--quit-at", [string]$quitAt)
    } else {
      $arguments += @("--layout-dump", $dumpPath)
    }
    $arguments += $extra
    $arguments += $image
  }

  $quoted = ($arguments | ForEach-Object {
    if ($_ -match '\s') { '"' + $_ + '"' } else { $_ }
  }) -join ' '
  $process = Start-Process -FilePath $appExe -ArgumentList $quoted -PassThru
  if (-not $process.WaitForExit(60000)) {
    $process.Kill()
    throw "zoreview.exe did not exit in time"
  }
  Start-Sleep -Milliseconds 120
  $text = ""
  if (Test-Path $logPath) { $text = Get-Content $logPath -Raw }
  $shotFile = $null
  if ($shot -ne "") { $shotFile = Join-Path $shots ($shot + ".png") }
  return @{ Values = (ReadDump $dumpPath); Log = $text; Shot = $shotFile }
}

try {
  # --- 1. RAW at fit: the preview is a placeholder, the half frame is final ---
  # V3.1-11 (option C): the camera JPEG is shown only until a LibRaw half-size
  # frame lands, so every *real* frame shares the full frame's rendering.
  $preview = StartApp @() $cr2 1600 1800
  $v = $preview.Values
  Check ($v["raw_mode"] -eq "half") "the CR2 ends on a LibRaw half-size frame"
  Check ($v["preview_active"] -eq "0") "the preview is not a placeholder any more"
  Check ($v["preview_badge_visible"] -eq "0") "no preview badge on the placeholder"
  Check ($v["has_image"] -eq "1") "the placeholder makes the image area live"
  Check ($v["loading_shown"] -eq "0") "the loading chip does not cover the frame"
  Check (@("1024", "1280", "1600") -contains $v["display_edge"]) `
    ("the fit edge is a display tier, not the 1920 preview (got {0})" -f $v["display_edge"])
  Check ($v["image_width"] -eq "6744" -and $v["image_height"] -eq "4502") `
    "the title/info size comes from the file"
  Check ($preview.Log -match "preview: shown source=embedded") `
    "the log reports the embedded preview"
  Check ($preview.Log -match "raw: preview as placeholder edge=") `
    "the log reports the placeholder, never 'as final'"
  Check ($preview.Log -match "raw: half-size request edge=") `
    "the placeholder is followed by a LibRaw half-size decode"
  Check (-not ($preview.Log -match "raw: preview as final")) `
    "the camera JPEG is never the final frame (V3.1-11)"
  $doneMatch = [regex]::Match($preview.Log, "load: done ms=(\d+)")
  Check $doneMatch.Success "the load reports completion"
  if ($doneMatch.Success) {
    Check ([int]$doneMatch.Groups[1].Value -lt 800) `
      ("the placeholder load is fast (ms={0})" -f $doneMatch.Groups[1].Value)
  }

  # --- 2. 1:1 asks for real pixels: a full decode replaces the preview ------
  $sharp = StartApp @("--send-keys", "1", "--send-keys-at", "1200") $cr2 4200 4800
  $v = $sharp.Values
  Check ($v["raw_mode"] -eq "full") "a 1:1 view upgrades the RAW to the full decode"
  Check ($v["zoom_percent"] -eq "100") "the upgrade lands at 100%"
  Check ($v["image_width"] -eq "6744" -and $v["image_height"] -eq "4502") `
    "the full frame has the file's dimensions"
  Check ($sharp.Log -match "upgrade: full frame applied") "the log reports the upgrade"
  Check ($sharp.Log -match "renderer: bitmap 6744x4502") `
    "the full frame is uploaded once for 1:1"
  # --- 3. A normal JPEG never flashes a preview ----------------------------
  $jpeg = StartApp @() $photo 500 700
  $v = $jpeg.Values
  Check ($v["preview_active"] -eq "0") "a small JPEG has no preview"
  Check (-not ($jpeg.Log -match "preview:")) "a small JPEG logs no preview work"

  # --- 4. A failed full decode keeps the preview + error card -------------
  # Only the v3.0 path can fail a decode now: with the preview as the final fit
  # frame there is no decode to fail, so the hook runs with --raw-preview=off.
  $failed = StartApp @("--raw-preview=off", "--force-preview-fail") $cr2 5000 5400
  $v = $failed.Values
  Check ($v["preview_active"] -eq "1") "a failed decode keeps the preview on screen"
  Check ($v["preview_failed"] -eq "1") "the badge reports the failed load"
  Check ($v["preview_badge_visible"] -eq "1") "the failed badge stays visible"
  Check ($v["error_open"] -eq "1") "the error card is shown over the preview"
  Check ($v["error_kind"] -eq "corrupt") "the failure is classified"
  Check ($v["image_width"] -eq "6744" -and $v["image_height"] -eq "4502") `
    "the preview keeps reporting the file size after the failure"
  Check ($failed.Log -match "preview: failed; keeping preview") `
    "the log reports the kept preview"

  # --- 5. The final preview frame at 200% DPI -----------------------------
  $scaled = StartApp @("--force-dpi-scale", "200") $cr2 1800 2000
  $v = $scaled.Values
  Check (@("preview", "half") -contains $v["raw_mode"]) `
    "the 200% run is on the preview placeholder or the half frame"
  Check ($v["preview_badge_visible"] -eq "0") "no badge at 200% either"
  Check ($scaled.Log -match "raw: preview as placeholder edge=") `
    "the 200% run also treats the preview as a placeholder"
  Check (-not ($scaled.Log -match "raw: preview as final")) `
    "the 200% run never makes the camera JPEG final"

  # --- 6. CR3 (H.265 preview) uses the thumbnail cache on the second open -
  $warm = StartApp @("--strip=pinned") $cr3 0 7000
  Check ($warm.Log -match "thumbnail ready") "the first CR3 open caches a thumbnail"
  Check ($warm.Log -match "no embedded JPEG preview \(format=6\)") `
    "the CR3 H.265 preview is reported as unsupported"
  # V3.1-04a: the cached 128 px thumbnail is still the immediate placeholder, but
  # the load no longer stops there — the H.265 preview is unavailable, so the
  # decision turns it into a half-size request. The state assertions are on the
  # log (a 400 ms dump races the decision) plus the settled dump below.
  $cached = StartApp @() $cr3 400 700
  Check ($cached.Log -match "preview: shown source=cache") `
    "the second CR3 open shows the cached thumbnail first"
  Check ($cached.Log -match "preview: unavailable \(LibRaw: no embedded JPEG preview") `
    "the H.265 CR3 reports no embedded JPEG preview"
  # V3.1-04a: an H.265-only CR3 has no embedded JPEG, so the fallback is a
  # half-size LibRaw request — still no 8480x5650 frame at fit.
  $half = StartApp @() $cr3 2600 3200
  $halfValues = $half.Values
  Check ($halfValues["raw_mode"] -eq "half") "the H.265 CR3 falls back to the half-size decode"
  Check ($half.Log -match "raw: preview too small") "the log reports the too-small preview"
  Check (-not ($half.Log -match "renderer: bitmap 8480x5650")) `
    "the H.265 CR3 never uploads the full sensor frame at fit"
  Check ([int]$halfValues["display_edge"] -le 2048) `
    ("the half-size frame stays at a display edge ({0})" -f $halfValues["display_edge"])

  # --- 7. An export from a display frame decodes the file first -----------
  # V3.1-04a: at fit the CR3 is a half-size display frame (current_is_tier=1), so
  # copying must force the full decode rather than ship the tier.
  $export = StartApp @("--send-keys", "Ctrl+C", "--send-keys-at", "2500") `
    $cr3 4200 4800
  $v = $export.Values
  Check ($v["has_image"] -eq "1") "the export test has a frame on screen"
  Check ($export.Log -match "export: forced full decode") `
    "copying from a display frame forces the full decode"
  Check (-not ($export.Log -match "preview: export blocked while loading")) `
    "the copy is not blocked once the frame is final"

  # --- 8. The full frame keeps the manual view set on the preview --------
  $navFile = Join-Path $nav "a.jpg"
  # V3.1-04a: 1:1 is what swaps the promoted preview for the file's pixels, and
  # the view must land exactly at 100 % of the file, centred.
  $preserve = StartApp @("--send-keys", "Right,Right,1") `
    $navFile 6000 6400
  $v = $preserve.Values
  Check ($v["file_name"] -eq "c.CR2") "navigation reached the CR2"
  Check ($v["raw_mode"] -eq "full") "the 1:1 view decoded the CR2"
  Check ($v["zoom_percent"] -eq "100" -and $v["fit"] -eq "0") `
    "the 1:1 view is 100% of the file"
  Check ($preserve.Log -match "upgrade: requested full frame") `
    "the 1:1 view asked for the full frame"
  Check ($preserve.Log -match "upgrade: full frame applied") `
    "the 200% view upgraded the preview to the full frame"
  $view = Point $v "view_center"
  $contentCenter = Point $v "content_center"
  if ($null -ne $view -and $null -ne $contentCenter) {
    CheckNear $view.X $contentCenter.X 2.0 "the view stays centred horizontally"
    CheckNear $view.Y $contentCenter.Y 2.0 "the view stays centred vertically"
  } else {
    Check $false "the view centre is exported by --layout-dump"
  }

  # --- 9. Screenshots: preview badge, sharp frame, failed preview --------
  # Drop the portable cache first: the earlier pinned-strip run cached 128 px
  # thumbnails for every sample in the work folder, and the screenshot should
  # show the embedded JPEG preview a first-time open gets.
  Remove-Item (Join-Path $work "cache") -Recurse -Force -ErrorAction SilentlyContinue
  $shotsRun = StartApp @() $cr2 0 0 "raw-preview" 700
  Check ((Test-Path $shotsRun.Shot) -and ((Get-Item $shotsRun.Shot).Length -gt 2000)) `
    "screenshot raw-preview (the preview is the final frame)"
  $shotsRun = StartApp @() $cr2 0 0 "sharp" 4000
  Check ((Test-Path $shotsRun.Shot) -and ((Get-Item $shotsRun.Shot).Length -gt 2000)) `
    "screenshot sharp"
  $shotsRun = StartApp @("--raw-preview=off", "--force-preview-fail") $cr2 0 0 "preview-failed" 4000
  Check ((Test-Path $shotsRun.Shot) -and ((Get-Item $shotsRun.Shot).Length -gt 2000)) `
    "screenshot preview-failed"
} finally {
  Remove-Item $work -Recurse -Force -ErrorAction SilentlyContinue
}

Write-Host ""
if ($script:failures -gt 0) {
  Write-Host ("FAILED: {0}/{1} checks" -f $script:failures, $script:checks) -ForegroundColor Red
  exit 1
}
Write-Host ("OK: {0} two-stage checks passed" -f $script:checks)
exit 0
