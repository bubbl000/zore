# v2.0 S2 (plan 22/12): animation frame pill, playback state machine, frame
# stepping and animated WebP.
#
# Every run copies the executable and the samples into a throw-away portable
# directory, so the developer's %LOCALAPPDATA% settings/caches are untouched
# (22/00 rule 10). State is asserted from --layout-dump and the application log;
# pixel equality of the WebP frames is covered by zoreview_formattests.
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

# The samples live in the project's sample folder, whose name is not ASCII. Find
# it by content instead of hard-coding the name so this script stays ASCII.
$images = $null
foreach ($root in @($repo, (Split-Path -Parent $repo))) {
  foreach ($dir in Get-ChildItem -LiteralPath $root -Directory -ErrorAction SilentlyContinue) {
    if (Test-Path (Join-Path $dir.FullName "images\gif\anim.gif")) {
      $images = Join-Path $dir.FullName "images"
      break
    }
  }
  if ($null -ne $images) { break }
}
if ($null -eq $images) { throw "image sample folder not found near $repo" }

$work = Join-Path $env:TEMP ("nv-anim-" + [guid]::NewGuid().ToString("N"))
New-Item -ItemType Directory -Path $work | Out-Null
$appExe = Join-Path $work "zoreview.exe"
Copy-Item $App $appExe
$appDirectory = Split-Path -Parent $App
Get-ChildItem $appDirectory -Filter "clang_rt.asan*.dll" -ErrorAction SilentlyContinue |
  ForEach-Object { Copy-Item $_.FullName (Join-Path $work $_.Name) }

$gif = Join-Path $work "anim.gif"
$apng = Join-Path $work "anim.png"
$staticGif = Join-Path $work "static.gif"
$webp = Join-Path $work "anim.webp"
$webpOnce = Join-Path $work "anim_loop1.webp"
$photo = Join-Path $work "photo.jpg"
Copy-Item (Join-Path $images "gif\anim.gif") $gif
Copy-Item (Join-Path $images "gif\static.gif") $staticGif
Copy-Item (Join-Path $images "apng\anim.png") $apng
Copy-Item (Join-Path $images "webp\anim.webp") $webp
Copy-Item (Join-Path $images "webp\anim_loop1.webp") $webpOnce
Copy-Item (Join-Path $images "jpeg\photo_1600.jpg") $photo
$shots = Join-Path $repo "tests\output\anim"
New-Item -ItemType Directory -Path $shots -Force | Out-Null
$dumpPath = Join-Path $work "layout.txt"

$script:checks = 0
$script:failures = 0

# Screenshot runs must not inherit a pointer that happens to rest on a thumbnail
# (it would draw a tooltip into the frame). Park the real cursor off-window.
Add-Type -AssemblyName System.Windows.Forms | Out-Null
function ParkCursor() {
  [System.Windows.Forms.Cursor]::Position =
    (New-Object System.Drawing.Point(5, 5))
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

function ClientRect([hashtable]$values) {
  if (-not $values.ContainsKey("client")) { return $null }
  $parts = $values["client"].Split("x")
  if ($parts.Count -ne 2) { return $null }
  $w = [double]$parts[0]
  $h = [double]$parts[1]
  return @{ L = 0; T = 0; R = $w; B = $h; W = $w; H = $h }
}

function StartApp([string[]]$extra) {
  Remove-Item (Join-Path $work "settings.ini") -Force -ErrorAction SilentlyContinue
  Remove-Item $dumpPath -Force -ErrorAction SilentlyContinue
  $log = Join-Path $work "logs\zoreview.log"
  Remove-Item $log -Force -ErrorAction SilentlyContinue
  $arguments = @("--portable", "--language", "en")
  $arguments += $extra
  $process = Start-Process -FilePath $appExe -ArgumentList $arguments -PassThru
  if (-not $process.WaitForExit(40000)) {
    $process.Kill()
    throw "zoreview.exe did not exit in time"
  }
  Start-Sleep -Milliseconds 120
  $text = ""
  if (Test-Path $log) { $text = Get-Content $log -Raw }
  return @{ Values = (ReadDump $dumpPath); Log = $text }
}

# Default run: the app writes the dump ~900 ms after the first frame; queued
# keys/clicks push it later automatically.
function RunDump([string[]]$extra, [string]$image) {
  return StartApp (@("--layout-dump", $dumpPath) + $extra + @($image))
}

# Runs that need a later snapshot (natural loop end, minimise/restore) use
# --dump-at without exiting, then --quit-at.
function RunLongDump([string[]]$extra, [string]$image, [int]$dumpAt,
                     [int]$quitAt) {
  return StartApp (@("--layout-dump", $dumpPath, "--dump-at",
                     [string]$dumpAt, "--quit-at", [string]$quitAt) +
                   $extra + @($image))
}

function RunShot([string]$name, [string[]]$extra, [string]$image,
                 [int]$delay) {
  ParkCursor
  $png = Join-Path $shots ($name + ".png")
  Remove-Item $png -Force -ErrorAction SilentlyContinue
  $arguments = @("--portable") + $extra + @("--screenshot", $png,
                                            "--shot-delay", [string]$delay,
                                            $image)
  $process = Start-Process -FilePath $appExe -ArgumentList $arguments -PassThru
  if (-not $process.WaitForExit(40000)) {
    $process.Kill()
    throw "screenshot run did not exit: $name"
  }
  Start-Sleep -Milliseconds 100
  Check ((Test-Path $png) -and ((Get-Item $png).Length -gt 2000)) `
    ("screenshot $name")
}

try {
  # --- 1. Animated WebP: pill appears and plays ----------------------------
  $base = RunDump @() $webp
  $v = $base.Values
  $dpi = [double]$v["dpi"]
  Check ($v["anim_visible"] -eq "1") "the animated WebP shows the frame pill"
  Check ($v["anim_buttons"] -eq "1") "the pill has frame buttons"
  Check ($v["anim_frames"] -eq "4") "the WebP reports four frames"
  Check ($v["anim_state"] -eq "playing") "animation autoplays on open"
  Check ($v["anim_first_frame_only"] -eq "0") "a playable file is not degraded"
  Check ($base.Log -match "webp: animation frames=4 loop=2") `
    "the WebP animation is decoded with loop=2"
  Check ($base.Log -match "animation: started frames=4 loop=2 source=webp") `
    "playback starts with the WebP source"

  $pill = Rect $v "anim_pill"
  $content = Rect $v "content"
  $bottom = Rect $v "bottom"
  Check (Inside $pill $content) "the pill stays inside the image content"
  Check ($pill.B -le ($bottom.T - 12 * $dpi) + 0.5) `
    "the pill sits 12 DIP above the bottom stack"
  CheckNear $pill.H (36 * $dpi) 0.6 "the pill is 36 DIP tall"
  CheckNear $pill.W (248 * $dpi) 0.6 "the pill is 248 DIP wide"
  $prev = Rect $v "anim_prev"
  $label = Rect $v "anim_label_rect"
  $next = Rect $v "anim_next"
  $toggle = Rect $v "anim_toggle"
  CheckNear $prev.W (28 * $dpi) 1.2 "the previous button is 28 DIP"
  CheckNear $toggle.H (28 * $dpi) 1.2 "the play button is 28 DIP tall"
  Check ($prev.R -le $label.L + 0.5) "the label starts after the previous button"
  Check ($label.R -le $next.L + 0.5) "the label ends before the next button"
  Check ($next.R -le $toggle.L + 0.5) "next precedes play/pause"
  Check (Inside $toggle $pill) "all pill buttons stay inside the pill"

  # A pinned rail moves the pill up; it must never cover the rail.
  $pinned = RunDump @("--strip=pinned") $webp
  $pinnedPill = Rect $pinned.Values "anim_pill"
  $pinnedStrip = Rect $pinned.Values "strip"
  Check ($pinnedPill.B -le ($pinnedStrip.T - 12 * $dpi) + 0.5) `
    "the pill clears a pinned thumbnail rail"

  # --- 2. Fullscreen -------------------------------------------------------
  $full = RunDump @("--fullscreen", "--force-anim", "paused") $webp
  $fullPill = Rect $full.Values "anim_pill"
  $fullClient = ClientRect $full.Values
  Check ($full.Values["fullscreen"] -eq "1") "the fullscreen run is fullscreen"
  Check (Inside $fullPill $fullClient) "the pill stays inside the fullscreen client"
  CheckNear $fullPill.B ($fullClient.B - 12 * $dpi) 1.0
  Check ($full.Values["anim_state"] -eq "paused") "--force-anim pauses playback"

  # --- 3. Forced pill states (deterministic screenshots/dumps) -------------
  $ended = RunDump @("--force-anim", "ended") $webp
  Check ($ended.Values["anim_state"] -eq "ended") "--force-anim ended reports ended"
  Check ($ended.Values["anim_ended"] -eq "1") "the ended flag is set"
  Check ($ended.Values["anim_frame"] -eq "4") "the ended state sits on the last frame"
  Check ($ended.Values["anim_label"] -match "4 / 4") "the ended label shows 4 / 4"
  Check ($ended.Values["anim_label"] -match "Ended") "the ended label names the state"

  $truncated = RunDump @("--force-anim", "truncated") $webp
  Check ($truncated.Values["anim_truncated"] -eq "1") "the truncated flag is shown"
  Check ($truncated.Values["anim_label"] -match "Truncated") `
    "the truncated badge is visible"

  $first = RunDump @("--force-anim", "first") $webp
  Check ($first.Values["anim_visible"] -eq "1") `
    "a first-frame-only animation still shows the pill"
  Check ($first.Values["anim_buttons"] -eq "0") `
    "the degraded pill has no buttons"
  Check ($first.Values["anim_first_frame_only"] -eq "1") `
    "the first-frame-only state is reported"
  Check ($first.Values["anim_label"] -eq "First frame only") `
    "the degraded pill says First frame only"
  $hiddenClick = RunDump @("--force-anim", "first", "--click-hit",
                           "anim.toggle") $webp
  Check ($hiddenClick.Log -match "click=anim.toggle skipped=hidden") `
    "the degraded pill has no click target"

  # --- 4. Play / pause / step --------------------------------------------
  $paused = RunDump @("--send-keys", "space") $gif
  Check ($paused.Values["anim_state"] -eq "paused") "Space pauses a playing GIF"
  Check ($paused.Log -match "animation: paused") "pausing is logged"

  $resumed = RunDump @("--send-keys", "space,space") $gif
  Check ($resumed.Values["anim_state"] -eq "playing") "Space resumes playback"
  Check ($resumed.Log -match "animation: resumed") "resuming is logged"

  $step = RunDump @("--force-anim", "paused", "--send-keys", "period") $gif
  Check ($step.Values["anim_state"] -eq "paused") "stepping keeps playback paused"
  Check ($step.Values["anim_frame"] -eq "2") "period advances one frame"
  Check ($step.Log -match "animation: frame=2/4") "the step is logged"

  $back = RunDump @("--force-anim", "paused", "--send-keys", "comma") $gif
  Check ($back.Values["anim_frame"] -eq "4") "comma wraps backwards to the last frame"

  $clickNext = RunDump @("--force-anim", "paused", "--click-hit", "anim.next") $gif
  Check ($clickNext.Values["anim_frame"] -eq "2") "the next button advances a frame"
  Check ($clickNext.Log -match "cmd: id=anim.next") "the next button is logged"
  $clickPrev = RunDump @("--force-anim", "paused", "--click-hit", "anim.prev") $gif
  Check ($clickPrev.Values["anim_frame"] -eq "4") "the previous button wraps back"
  $clickPlay = RunDump @("--force-anim", "paused", "--click-hit", "anim.toggle") $gif
  Check ($clickPlay.Values["anim_state"] -eq "playing") `
    "the play button resumes playback"

  # --- 5. Loop end and replay --------------------------------------------
  $natural = RunLongDump @() $webpOnce 3000 3400
  Check ($natural.Values["anim_state"] -eq "ended") `
    "a finite loop ends on its own (loop=1)"
  Check ($natural.Values["anim_frame"] -eq "4") "the natural end stops on frame 4"
  Check ($natural.Log -match "animation: ended frame=4/4") "the end is logged"

  $replay = RunDump @("--force-anim", "ended", "--click-hit", "anim.toggle") $webp
  Check ($replay.Values["anim_state"] -eq "playing") "Play from Ended replays"
  # Playback keeps advancing after the click; the log is the deterministic
  # evidence that the replay began at frame 0 ("resumed frame=1").
  Check ([int]$replay.Values["anim_frame"] -ge 1) `
    "the replay advanced from the first frame"
  Check ($replay.Log -match "animation: resumed frame=1") "the replay is logged"

  # --- 6. Minimise pauses; restore only resumes what was playing ----------
  $minimised = RunLongDump @("--minimize-at", "1800", "--restore-at", "3000") `
    $gif 3600 4000
  Check ($minimised.Log -match "animation: paused minimized") `
    "minimising pauses the animation"
  Check ($minimised.Log -match "animation: resumed restored") `
    "restoring resumes it"
  Check ($minimised.Values["anim_state"] -eq "playing") `
    "the animation is playing again after restore"

  $pausedMin = RunLongDump @("--send-keys", "space", "--minimize-at", "2400",
                             "--restore-at", "3200") $gif 4000 4400
  Check ($pausedMin.Log -notmatch "animation: resumed restored") `
    "a manually paused animation is not resumed by restore"
  Check ($pausedMin.Values["anim_state"] -eq "paused") `
    "it stays paused after minimising and restoring"

  # --- 7. Frame updates never reset the view ------------------------------
  $zoomA = RunDump @("--force-anim", "paused", "--send-keys", "plus") $gif
  $zoomB = RunDump @("--force-anim", "paused", "--send-keys", "plus,period") $gif
  Check ([math]::Abs([double]$zoomA.Values["zoom"] -
                     [double]$zoomB.Values["zoom"]) -lt 0.001) `
    "stepping does not change the zoom"
  Check ($zoomB.Values["fit"] -eq "0") "the zoomed view stays manual after a step"

  $rotated = RunDump @("--rotation", "90", "--force-anim", "paused",
                       "--send-keys", "period") $webp
  Check ($rotated.Values["rotation"] -eq "1") "stepping keeps the 90 degree rotation"
  Check ($rotated.Values["display_width"] -eq "100") `
    "the rotated display keeps the swapped dimensions"
  Check ($rotated.Values["anim_frame"] -eq "2") "the frame advanced while rotated"

  $info = RunDump @("--send-keys", "i,period") $gif
  Check ($info.Values["info_open"] -eq "1") "the info panel stays open while stepping"
  Check ($info.Values["anim_state"] -eq "paused") "stepping pauses with the panel open"

  # --- 8. Formats and static files ---------------------------------------
  $gifRun = RunDump @() $gif
  Check ($gifRun.Values["anim_visible"] -eq "1") "the GIF shows the pill"
  Check ($gifRun.Values["anim_frames"] -eq "4") "the GIF reports four frames"
  Check ($gifRun.Log -match "source=gif") "the GIF playback source is logged"

  $apngRun = RunDump @() $apng
  Check ($apngRun.Values["anim_frames"] -eq "4") "the APNG reports four frames"
  Check ($apngRun.Log -match "source=apng") "the APNG playback source is logged"

  $static = RunDump @("--send-keys", "period") $photo
  Check ($static.Values["anim_visible"] -eq "0") "a still image has no pill"
  Check ($static.Log -match "cmd: blocked=animation.next") `
    "frame stepping is blocked without an animation"
  $staticGifRun = RunDump @() $staticGif
  Check ($staticGifRun.Values["anim_visible"] -eq "0") `
    "a single-frame GIF has no pill"

  # --- 9. Localisation and DPI -------------------------------------------
  $zh = StartApp @("--layout-dump", $dumpPath, "--language", "zh",
                   "--force-anim", "ended", $webp)
  Check ($zh.Values["anim_label"] -match "4 / 4") "the frame counter is language neutral"
  Check ($zh.Values["anim_label"] -match ([string][char]0x5DF2 + [string][char]0x7ED3 + [string][char]0x675F)) `
    "the ended badge is localised in Chinese"

  foreach ($scale in @(100, 125, 150, 200)) {
    $scaled = RunDump @("--force-dpi-scale", [string]$scale, "--force-anim",
                        "paused") $webp
    $s = [double]$scaled.Values["dpi"]
    $sp = Rect $scaled.Values "anim_pill"
    CheckNear $sp.H (36 * $s) 1.0 ("the pill is 36 DIP at {0}%" -f $scale)
    CheckNear $sp.W (248 * $s) 1.0 ("the pill is 248 DIP at {0}%" -f $scale)
    Check (Inside $sp (Rect $scaled.Values "content")) `
      ("the pill stays in the content at {0}%" -f $scale)
  }

  # --- 10. Screenshots ----------------------------------------------------
  RunShot "webp-playing" @("--strip=pinned") $webp 900
  RunShot "gif-paused" @("--strip=pinned", "--send-keys", "space") $gif 1400
  RunShot "webp-ended" @("--strip=pinned", "--force-anim", "ended") $webp 900
  RunShot "pill-fullscreen" @("--force-anim", "paused", "--fullscreen") $webp 900
  foreach ($shotScale in @(100, 125, 150, 200)) {
    RunShot ("pill-dpi" + $shotScale) @("--force-anim", "paused",
      "--force-dpi-scale", [string]$shotScale) $webp 900
  }
  RunShot "pill-first-frame" @("--force-anim", "first") $webp 900
  RunShot "pill-zh" @("--language", "zh", "--force-anim", "ended") $webp 900

  # --- 11. HEIC / multi-page TIFF are static (V3-06 maintenance) -----------
  # WIC decodes frame 0 only; frame_count>1 means the container holds several
  # images, not a playable animation. They must not show the pill and must be
  # cached: before the fix they were re-decoded on every visit and each HEIC
  # decode left HEIF-codec handles behind (mixed soak, HANDOFF pitfall 77).
  $heicDir = Join-Path $work "heicdir"
  New-Item -ItemType Directory -Path $heicDir | Out-Null
  Copy-Item (Join-Path $images "heic\example.heic") (Join-Path $heicDir "a.heic")
  Copy-Item (Join-Path $images "jpeg\photo_1600.jpg") (Join-Path $heicDir "b.jpg")
  $heicRun = RunDump @("--send-keys", "Right,Left") (Join-Path $heicDir "a.heic")
  Check ($heicRun.Values["anim_visible"] -eq "0") "HEIC does not show the animation pill"
  Check ($heicRun.Values["info_frames"] -eq "2") "HEIC keeps its container frame count"
  Check (([regex]::Matches($heicRun.Log, "decoded:")).Count -eq 1) `
    "only HEIC needs a decode (its JPEG neighbour was prefetched)"
  Check ($heicRun.Log -match "memory cache hit: .*a\.heic") `
    "revisiting HEIC is a memory-cache hit (not re-decoded)"

  $tiffRun = RunDump @() (Join-Path $images "tiff\multipage.tif")
  Check ($tiffRun.Values["anim_visible"] -eq "0") `
    "multi-page TIFF does not show the animation pill"
  Check ($tiffRun.Values["info_frames"] -eq "3") `
    "multi-page TIFF keeps its page count in the info card"
} finally {
  Remove-Item -Recurse -Force $work -ErrorAction SilentlyContinue
}

Write-Host ""
if ($script:failures -gt 0) {
  Write-Host ("FAILED: {0} of {1} animation checks failed" -f $script:failures,
              $script:checks) -ForegroundColor Red
  exit 1
}
Write-Host ("OK: {0} animation checks passed" -f $script:checks) -ForegroundColor Green
exit 0
