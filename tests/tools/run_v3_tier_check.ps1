#requires -Version 5.1
<#
.SYNOPSIS
    V3-04 display-tier checks (瑕忓妰/25 搂7.1-搂7.5 + 搂7.4b).

.DESCRIPTION
    Functional, timing and sharpness evidence for the display-tier chain, with
    the chain off (the default and the V3-04b fallback) and on
    (--display-cache on/force).

    IMPORTANT: the first image of a process is always decoded at full
    resolution. LoadPath runs before Renderer::Initialize on purpose (瑕忓妰/09
    startup decision: the decode overlaps driver initialization), so there is no
    content rectangle to choose a tier from. Every scenario here therefore
    opens the first file and then navigates once with VK_RIGHT, which is the
    first load that can use a tier.

    Measured:
      - key -> frame latency (wall clock until load: done / display cache hit /
        memory cache hit appears),
      - the decoded load: done value when the path decoded,
      - private peak (25 ms polling),
      - cache\images file count and bytes.

    Asserted:
      - a tier is requested, decoded, cached in memory and stored on disk,
      - a second process re-opens the same file from the disk tier,
      - --display-cache=off never requests a tier,
      - the layout dump reports display_edge / current_is_tier / cache_source,
      - sharpness A/B against a LANCZOS reference (off vs on screenshots).

.PREREQUISITES
      python tests\tools\make_v3_baseline_samples.py C:\temp\nv-v3-04-samples
      python tests\tools\make_quality_sample.py C:\temp\nv-v3-04-samples\stripes8k.png
      optional real photo pair: decode any RAW with
        zoreview_cli --decode <raw> --out photo30.bgra
      and save it as C:\temp\nv-v3-04-samples\photo30mp.jpg (30 MP, real detail).

.EXAMPLE
    powershell -ExecutionPolicy Bypass -File tests\tools\run_v3_tier_check.ps1 `
      -Exe build\Release\zoreview.exe -Samples C:\temp\nv-v3-04-samples `
      -Out C:\temp\nv-v3-04-results -Repeats 3
#>
param(
    [Parameter(Mandatory = $true)][string]$Exe,
    [Parameter(Mandatory = $true)][string]$Samples,
    [Parameter(Mandatory = $true)][string]$Out,
    [int]$Repeats = 3,
    [int]$PacedMs = 1200,
    [switch]$SkipSharpness
)

$ErrorActionPreference = 'Stop'

if (-not ("NvV304Win32" -as [type])) {
Add-Type @"
using System;
using System.Runtime.InteropServices;
public static class NvV304Win32 {
  [DllImport("user32.dll", SetLastError=true)]
  public static extern bool PostMessage(IntPtr hWnd, uint Msg, IntPtr wParam, IntPtr lParam);
}
"@
}

$Work = Join-Path $Out 'work'
New-Item -ItemType Directory -Path $Work -Force | Out-Null
$WorkExe = Join-Path $Work 'zoreview.exe'
Get-Process zoreview -ErrorAction SilentlyContinue |
  Where-Object { $_.Path -eq $WorkExe } |
  Stop-Process -Force -ErrorAction SilentlyContinue
Start-Sleep -Milliseconds 200
Copy-Item $Exe $WorkExe -Force
$LogPath = Join-Path $Work 'logs\zoreview.log'
$script:peakPrivate = 0
$script:failures = 0
$script:checks = 0
$results = [ordered]@{}

function Check([bool]$condition, [string]$name) {
  $script:checks++
  if ($condition) {
    Write-Host "ok   $name"
  } else {
    Write-Host "FAIL $name"
    $script:failures++
  }
}

function Update-Peak([System.Diagnostics.Process]$p) {
  if ($null -eq $p) { return }
  try {
    $p.Refresh()
    if (-not $p.HasExited -and $p.PrivateMemorySize64 -gt $script:peakPrivate) {
      $script:peakPrivate = $p.PrivateMemorySize64
    }
  } catch {}
}

function Sleep-Poll([int]$ms, [System.Diagnostics.Process]$p) {
  $sw = [System.Diagnostics.Stopwatch]::StartNew()
  while ($sw.ElapsedMilliseconds -lt $ms) {
    Update-Peak $p
    $left = $ms - $sw.ElapsedMilliseconds
    if ($left -gt 15) { $left = 15 }
    if ($left -gt 0) { Start-Sleep -Milliseconds $left }
  }
}

function Reset-Data {
  Remove-Item (Join-Path $Work 'logs') -Recurse -Force -ErrorAction SilentlyContinue
  Remove-Item (Join-Path $Work 'cache') -Recurse -Force -ErrorAction SilentlyContinue
  Remove-Item (Join-Path $Work 'settings.ini') -Force -ErrorAction SilentlyContinue
}

function Log-Length {
  if (Test-Path $LogPath) { return (Get-Item $LogPath).Length }
  return 0
}

function Read-LogBytes {
  if (-not (Test-Path $LogPath)) { return $null }
  try {
    $fs = [System.IO.File]::Open($LogPath, [System.IO.FileMode]::Open,
                                 [System.IO.FileAccess]::Read,
                                 [System.IO.FileShare]::ReadWrite)
    try {
      $length = [int]$fs.Length
      if ($length -le 0) { return (New-Object byte[] 0) }
      $buffer = New-Object byte[] $length
      [void]$fs.Read($buffer, 0, $length)
      return $buffer
    } finally { $fs.Dispose() }
  } catch { return $null }
}

function Read-Segment([long]$offset) {
  $bytes = Read-LogBytes
  if ($null -eq $bytes -or $offset -ge $bytes.Length) { return '' }
  return [System.Text.Encoding]::UTF8.GetString($bytes, [int]$offset, $bytes.Length - [int]$offset)
}

function Read-Log {
  $bytes = Read-LogBytes
  if ($null -eq $bytes) { return '' }
  return [System.Text.Encoding]::UTF8.GetString($bytes)
}

function First-Match([string]$text, [string]$pattern, [int]$group = 1) {
  $m = [regex]::Match($text, $pattern)
  if ($m.Success) { return $m.Groups[$group].Value }
  return $null
}

function Wait-Marker([long]$offset, [string]$pattern, [int]$timeoutMs,
                     [System.Diagnostics.Process]$p) {
  $sw = [System.Diagnostics.Stopwatch]::StartNew()
  $seg = ''
  while ($sw.ElapsedMilliseconds -lt $timeoutMs) {
    Update-Peak $p
    $seg = Read-Segment $offset
    if ($seg -match $pattern) {
      return @{ ms = $sw.ElapsedMilliseconds; text = $seg; matched = $true }
    }
    Start-Sleep -Milliseconds 10
  }
  return @{ ms = $null; text = $seg; matched = $false }
}

# Terminal markers of one navigation. `memory cache hit` is a first-class result
# (a prefetched tier or full frame is already in the LRU), so it must end the
# wait exactly like a decode does.
$NavMarkers = 'load: done ms=|display cache hit:|memory cache hit:|load: failed'

# Log writes are buffered, so an offset taken right before a key press can miss
# lines that are flushed a moment later. Counting occurrences instead of
# matching a byte offset makes the measurement independent of the flush timing:
# wait until the marker count grew, and report the elapsed time.
function Wait-Count([int]$baseline, [string]$pattern, [int]$timeoutMs,
                    [System.Diagnostics.Process]$p) {
  $sw = [System.Diagnostics.Stopwatch]::StartNew()
  $count = $baseline
  while ($sw.ElapsedMilliseconds -lt $timeoutMs) {
    Update-Peak $p
    $count = ([regex]::Matches((Read-Log), $pattern)).Count
    if ($count -gt $baseline) {
      return @{ ms = $sw.ElapsedMilliseconds; count = $count; matched = $true }
    }
    Start-Sleep -Milliseconds 10
  }
  return @{ ms = $null; count = $count; matched = $false }
}

function Start-App([string]$image, [string[]]$extra = @(), [int]$readyTimeoutMs = 20000) {
  $arguments = @('--portable', '--language', 'en', '--quit-at', '120000') + $extra + @($image)
  $p = Start-Process -FilePath $WorkExe -ArgumentList $arguments -PassThru
  $script:peakPrivate = 0
  $hwnd = [IntPtr]::Zero
  $deadline = [DateTime]::UtcNow.AddMilliseconds($readyTimeoutMs)
  while ([DateTime]::UtcNow -lt $deadline) {
    Update-Peak $p
    try {
      $p.Refresh()
      if ($p.MainWindowHandle -ne 0) { $hwnd = $p.MainWindowHandle }
    } catch {}
    if ($hwnd -ne [IntPtr]::Zero -and (Test-Path $LogPath) -and
        ((Read-Log) -match 'decoded:|load: done ms=|display cache hit:')) { break }
    Start-Sleep -Milliseconds 25
  }
  if ($hwnd -eq [IntPtr]::Zero) { throw "no main window for $image" }
  return @{ process = $p; hwnd = $hwnd }
}

function Post-Key([System.IntPtr]$hwnd, [int]$vk) {
  [void][NvV304Win32]::PostMessage($hwnd, 0x0100, [IntPtr]$vk, [IntPtr]0)
  Start-Sleep -Milliseconds 20
  [void][NvV304Win32]::PostMessage($hwnd, 0x0101, [IntPtr]$vk, [IntPtr]0)
}

function Close-App([System.Diagnostics.Process]$p, [System.IntPtr]$hwnd) {
  [void][NvV304Win32]::PostMessage($hwnd, 0x0010, [IntPtr]::Zero, [IntPtr]::Zero)
  if (-not $p.WaitForExit(10000)) {
    try { $p.Kill() } catch {}
    [void]$p.WaitForExit(3000)
    return $false
  }
  return $true
}

function Cache-Stats {
  $result = @{}
  foreach ($name in @('images', 'thumbs')) {
    $dir = Join-Path $Work "cache\$name"
    $files = @(Get-ChildItem $dir -Recurse -File -ErrorAction SilentlyContinue)
    $result[$name + '_files'] = $files.Count
    $sum = 0
    foreach ($f in $files) { $sum += $f.Length }
    $result[$name + '_bytes'] = $sum
  }
  return $result
}

function Percentile([double[]]$values, [double]$q) {
  if ($values.Count -eq 0) { return 0 }
  $sorted = @($values | Sort-Object)
  $index = [Math]::Ceiling($q * $sorted.Count) - 1
  if ($index -lt 0) { $index = 0 }
  return [double]$sorted[$index]
}

function Save-Log([string]$name) {
  $dir = Join-Path $Out 'logs'
  New-Item -ItemType Directory -Path $dir -Force | Out-Null
  if (Test-Path $LogPath) {
    Copy-Item $LogPath (Join-Path $dir ($name + '.log')) -Force
  }
}

# Opens <pair>\a<ext>, then navigates once with VK_RIGHT and measures the second
# (tier-capable) load. The caller owns Reset-Data so the disk-tier scenario can
# keep the cache between two runs.
function Invoke-Nav([string]$label, [string]$pairDir, [string]$ext,
                    [string[]]$extra = @(), [int]$settleMs = 600) {
  $image = Join-Path $pairDir ('a' + $ext)
  $app = Start-App $image $extra
  [void](Wait-Marker 0 'load: done ms=|display cache hit:|memory cache hit:' 25000 $app.process)
  Sleep-Poll 250 $app.process
  $baseline = ([regex]::Matches((Read-Log), $NavMarkers)).Count
  $offset = Log-Length
  Post-Key $app.hwnd 0x27  # VK_RIGHT
  $m = Wait-Count $baseline $NavMarkers 15000 $app.process
  $after = Read-Log
  $segment = if ($offset -lt $after.Length) { $after.Substring([Math]::Min($offset, $after.Length)) } else { '' }
  Sleep-Poll $settleMs $app.process
  $text = Read-Log
  $stats = Cache-Stats
  $entry = [pscustomobject][ordered]@{
    nav_ms = $m.ms
    load_done_ms = [double](First-Match $segment 'load: done ms=([0-9.]+)')
    tier_request = ([regex]::Matches($segment, 'display tier request:')).Count
    tier_hit = ([regex]::Matches($segment, 'display cache hit:')).Count
    full_mem_hit = ([regex]::Matches($segment, 'memory cache hit:')).Count
    tier_hit_disk = ([regex]::Matches($segment, 'display cache hit: .*source=disk')).Count
    tier_hit_mem = ([regex]::Matches($segment, 'display cache hit: .*source=mem')).Count
    decoded = ([regex]::Matches($segment, 'decoded:')).Count
    prefetch_disk = ([regex]::Matches($text, 'prefetch: tier disk hit')).Count
    tier_encoded_total = ([regex]::Matches($text, 'disk cache save: .*edge=')).Count
    images_files = $stats['images_files']
    images_bytes = $stats['images_bytes']
    peak_private_mb = [Math]::Round($script:peakPrivate / 1MB, 1)
  }
  [void](Close-App $app.process $app.hwnd)
  Save-Log $label
  return $entry
}

function Invoke-Flip([string]$label, [string]$startImage, [int]$keys,
                     [string[]]$extra = @()) {
  $app = Start-App $startImage $extra
  [void](Wait-Marker 0 'load: done ms=|display cache hit:|memory cache hit:' 25000 $app.process)
  Sleep-Poll 300 $app.process
  $latencies = New-Object System.Collections.Generic.List[double]
  $decoded = New-Object System.Collections.Generic.List[double]
  for ($i = 0; $i -lt $keys; $i++) {
    $baseline = ([regex]::Matches((Read-Log), $NavMarkers)).Count
    $before = (Read-Log).Length
    Post-Key $app.hwnd 0x27  # VK_RIGHT
    $m = Wait-Count $baseline $NavMarkers 15000 $app.process
    if ($null -ne $m.ms) { $latencies.Add([double]$m.ms) }
    $after = Read-Log
    $segment = if ($before -lt $after.Length) { $after.Substring($before) } else { '' }
    $done = First-Match $segment 'load: done ms=([0-9.]+)'
    if ($null -ne $done) { $decoded.Add([double]$done) }
    $rest = $PacedMs - [double]$m.ms
    if ($rest -gt 0) { Sleep-Poll ([int]$rest) $app.process }
  }
  Sleep-Poll 800 $app.process
  $text = Read-Log
  $stats = Cache-Stats
  $entry = [pscustomobject][ordered]@{
    nav_count = $latencies.Count
    nav_p50_ms = [Math]::Round((Percentile $latencies.ToArray() 0.5), 1)
    nav_p95_ms = [Math]::Round((Percentile $latencies.ToArray() 0.95), 1)
    decoded_count = $decoded.Count
    decoded_p50_ms = [Math]::Round((Percentile $decoded.ToArray() 0.5), 1)
    decoded_p95_ms = [Math]::Round((Percentile $decoded.ToArray() 0.95), 1)
    tier_hits = ([regex]::Matches($text, 'display cache hit:')).Count
    tier_encoded = ([regex]::Matches($text, 'disk cache save: .*edge=')).Count
    images_files = $stats['images_files']
    images_bytes = $stats['images_bytes']
    peak_private_mb = [Math]::Round($script:peakPrivate / 1MB, 1)
  }
  [void](Close-App $app.process $app.hwnd)
  Save-Log $label
  return $entry
}

# Layout dump of the *second* image, so the tier fields are the tier path.
function Invoke-Dump([string]$label, [string]$pairDir, [string]$ext,
                     [string[]]$extra = @()) {
  $image = Join-Path $pairDir ('a' + $ext)
  $dump = Join-Path $Work ("dump-$label.txt")
  Remove-Item $dump -Force -ErrorAction SilentlyContinue
  $app = Start-App $image (@($extra) + @('--send-keys', 'Right', '--layout-dump',
                                        $dump, '--quit-at', '9000'))
  $deadline = [DateTime]::UtcNow.AddSeconds(12)
  while ([DateTime]::UtcNow -lt $deadline) {
    Update-Peak $app.process
    if ((Test-Path $dump) -and ((Get-Item $dump).Length -gt 0)) { break }
    Start-Sleep -Milliseconds 50
  }
  Sleep-Poll 200 $app.process
  $dumpText = if (Test-Path $dump) { Get-Content $dump -Raw } else { '' }
  $entry = [pscustomobject][ordered]@{
    display_edge = First-Match $dumpText 'display_edge=(\d+)'
    current_is_tier = First-Match $dumpText 'current_is_tier=(\d+)'
    cache_source = First-Match $dumpText 'cache_source=(\S+)'
    content = First-Match $dumpText 'content=([0-9.,]+)'
    tier_hits = ([regex]::Matches((Read-Log), 'display cache hit:')).Count
    prefetch_disk = ([regex]::Matches((Read-Log), 'prefetch: tier disk hit')).Count
  }
  [void](Close-App $app.process $app.hwnd)
  Save-Log $label
  return $entry
}

function Invoke-Sharpness([string]$label, [string]$pairDir, [string]$ext,
                          [string]$referenceImage, [string]$contentRect) {
  $png = Join-Path $Out ("sharp-$label")
  New-Item -ItemType Directory -Path $png -Force | Out-Null
  $off = Join-Path $png 'off.png'
  $on = Join-Path $png 'on.png'
  Remove-Item $off, $on -Force -ErrorAction SilentlyContinue
  $image = Join-Path $pairDir ('a' + $ext)
  Reset-Data
  $app = Start-App $image @('--display-cache=off', '--send-keys', 'Right',
                            '--screenshot', $off, '--shot-delay', '2800')
  [void]$app.process.WaitForExit(25000)
  Reset-Data
  $app = Start-App $image @('--display-cache=on', '--send-keys', 'Right',
                            '--screenshot', $on, '--shot-delay', '2800')
  [void]$app.process.WaitForExit(25000)
  Save-Log ("sharp-" + $label + "-on")
  if (-not (Test-Path $off) -or -not (Test-Path $on)) {
    Check $false "sharpness $label : both screenshots exist"
    return
  }
  $parts = $contentRect -split ','
  if ($parts.Count -lt 4) {
    Check $false "sharpness $label : content rect in the dump"
    return
  }
  $left = [double]$parts[0]
  $top = [double]$parts[1]
  $w = [double]$parts[2] - $left
  $h = [double]$parts[3] - $top
  $script = Join-Path $PSScriptRoot 'tier_sharpness.py'
  $output = & python $script $referenceImage $off $on $left $top $w $h 2>&1
  $text = ($output | Out-String).Trim()
  Write-Host $text
  Check ($text -match 'geometry=\d+x\d+') "sharpness $label : geometry"
  Check ($text -match 'on_error=[0-9.]+') "sharpness $label : tier error measured"
  # A tier is resampled twice (source -> tier with Fant, tier -> content with
  # cubic), so the error against a single-pass LANCZOS reference grows; what
  # matters is that it stays below a visible amount (mean abs diff <= 2 of 255)
  # and that the sharpness proxy does not collapse (瑕忓妰/25 搂7.4b/搂7.9-7).
  $errorText = First-Match $text 'on_error=([0-9.]+)'
  if ($null -ne $errorText) {
    Check ([double]$errorText -le 2.0) "sharpness $label : mean error <= 2/255"
  }
  $gradientText = First-Match $text 'gradient_ratio=([0-9.]+)'
  if ($null -ne $gradientText) {
    Check ([double]$gradientText -ge 0.90) "sharpness $label : sharpness kept >= 90 %"
  }
}

# --- pair folders: two identical copies so the second load is the same picture
function Prepare-Pair([string]$name, [string]$source) {
  if (-not (Test-Path $source)) { return $null }
  $dir = Join-Path $Work ("pair-" + $name)
  New-Item -ItemType Directory -Path $dir -Force | Out-Null
  $ext = [System.IO.Path]::GetExtension($source)
  $a = Join-Path $dir ('a' + $ext)
  $b = Join-Path $dir ('b' + $ext)
  Copy-Item $source $a -Force
  Copy-Item $source $b -Force
  return @{ dir = $dir; ext = $ext; reference = $a }
}

$pair24 = Prepare-Pair '24mp' (Join-Path $Samples 'single24_12\24mp_12mb.jpg')
$flipDir = Join-Path $Samples 'flip24'
$flipStart = Join-Path $flipDir 'flip01.jpg'
$pairPhoto = Prepare-Pair 'photo' (Join-Path $Samples 'photo30mp.jpg')
foreach ($check in @($pair24, $pairPhoto)) {
  if ($null -eq $check) { throw "samples missing under $Samples" }
}
if (-not (Test-Path $flipStart)) { throw "sample missing: $flipStart" }

# --- 1. second-image load: full decode vs tier -------------------------------
Write-Host '--- second image (tier-capable): off vs on ---'
$navOff = @()
$navOn = @()
for ($r = 1; $r -le $Repeats; $r++) {
  Reset-Data
  $navOff += Invoke-Nav "nav-off-$r" $pair24.dir $pair24.ext @('--display-cache=off')
  Reset-Data
  $navOn += Invoke-Nav "nav-on-$r" $pair24.dir $pair24.ext @('--display-cache=on')
}
$results['nav_off'] = $navOff
$results['nav_on'] = $navOn
$navOffP50 = Percentile ($navOff | ForEach-Object { $_.nav_ms }) 0.5
$navOnP50 = Percentile ($navOn | ForEach-Object { $_.nav_ms }) 0.5
$navRatio = if ($navOffP50 -gt 0) { $navOnP50 / $navOffP50 } else { 0 }
Write-Host ("second-image nav p50: off={0:N0} ms, on={1:N0} ms, ratio={2:N2}" -f $navOffP50, $navOnP50, $navRatio)
Check (($navOn | Measure-Object -Property tier_request -Sum).Sum -ge $Repeats) 'each tier load requested a tier'
Check (($navOn | Measure-Object -Property tier_hit -Sum).Sum -ge 1) 'the tier navigation is served from a tier entry'
Check (($navOn | Measure-Object -Property images_files -Maximum).Maximum -ge 1) 'a tier was written to cache\images'
Check (($navOn | Measure-Object -Property images_bytes -Maximum).Maximum -gt 0) 'the tier entry has bytes'
Check (($navOff | Measure-Object -Property tier_request -Sum).Sum -eq 0) '--display-cache=off never requests a tier'
Check ($navRatio -le 0.7) 'tier load is at least 30 % faster than the full decode'

# --- 2. flip 10 forward ----------------------------------------------------
Write-Host '--- flip 10 forward: off vs on ---'
$flipOff = @()
$flipOn = @()
for ($r = 1; $r -le $Repeats; $r++) {
  Reset-Data
  $flipOff += Invoke-Flip "flip-off-$r" $flipStart 10 @('--display-cache=off')
  Reset-Data
  $flipOn += Invoke-Flip "flip-on-$r" $flipStart 10 @('--display-cache=on')
}
$results['flip_off'] = $flipOff
$results['flip_on'] = $flipOn
$flipOffP95 = Percentile ($flipOff | ForEach-Object { $_.nav_p95_ms }) 0.5
$flipOnP95 = Percentile ($flipOn | ForEach-Object { $_.nav_p95_ms }) 0.5
Write-Host ("flip nav p95: off={0:N0} ms, on={1:N0} ms" -f $flipOffP95, $flipOnP95)
Check ($flipOnP95 -le $flipOffP95) 'tier navigation p95 is not slower than the full-frame path'
Check (($flipOn | Measure-Object -Property tier_encoded -Sum).Sum -ge 1) 'neighbour tiers were encoded to disk'
Check (($flipOff | Measure-Object -Property tier_hits -Sum).Sum -eq 0) 'off mode has no tier hit'

# --- 3. disk tier read by a second process ---------------------------------
Write-Host '--- second process reads the tier from disk (prefetch) ---'
Reset-Data
$warm = Invoke-Nav 'disk-warm' $pair24.dir $pair24.ext @('--display-cache=on') 1500
$cool = Invoke-Nav 'disk-cool' $pair24.dir $pair24.ext @('--display-cache=on')
$results['disk_warm'] = $warm
$results['disk_cool'] = $cool
Check ($warm.images_files -ge 1) 'the warm run stored a tier'
Check ($warm.tier_encoded_total -ge 1) 'the warm run encoded a tier payload'
# A new process has an empty memory cache: the prefetch worker must find the
# stored tier on disk instead of decoding the neighbour again.
Check ($cool.prefetch_disk -ge 1) 'a second process reads the tier from disk'
Check ($cool.nav_ms -le 100) 'the navigated frame is on screen from the prefetched tier'

# --- 3b. 8K: the frame is >64 MB, so the full-frame path cannot prefetch -----
# NOTE: samples\single8k\8k.jpg is the 1.1 MB synthetic 7680x4320 sample of
# V3-01 (tests\output\8k.jpg). Its decode time is not representative of a real
# 20 MB 8K photo; the memory/disk/prefetch numbers are (126.6 MB per full frame).
Write-Host '--- 8K second image: off vs on ---'
$pair8k = Prepare-Pair '8k' (Join-Path $Samples 'single8k\8k.jpg')
if ($null -ne $pair8k) {
  $kOff = @()
  $kOn = @()
  for ($r = 1; $r -le $Repeats; $r++) {
    Reset-Data
    $kOff += Invoke-Nav "8k-off-$r" $pair8k.dir $pair8k.ext @('--display-cache=off')
    Reset-Data
    $kOn += Invoke-Nav "8k-on-$r" $pair8k.dir $pair8k.ext @('--display-cache=on')
  }
  $results['8k_off'] = $kOff
  $results['8k_on'] = $kOn
  $kOffP50 = Percentile ($kOff | ForEach-Object { $_.nav_ms }) 0.5
  $kOnP50 = Percentile ($kOn | ForEach-Object { $_.nav_ms }) 0.5
  $kOffPeak = Percentile ($kOff | ForEach-Object { $_.peak_private_mb }) 0.5
  $kOnPeak = Percentile ($kOn | ForEach-Object { $_.peak_private_mb }) 0.5
  Write-Host ("8K nav p50: off={0:N0} ms, on={1:N0} ms; peak off={2:N0} MB, on={3:N0} MB" -f $kOffP50, $kOnP50, $kOffPeak, $kOnPeak)
  Check ($kOnP50 -lt $kOffP50) '8K tier navigation is faster than the full frame'
  Check ($kOnPeak -lt $kOffPeak) '8K tier navigation uses less private memory'
  Check (($kOn | Measure-Object -Property tier_request -Sum).Sum -ge $Repeats) '8K requested a tier'
  # Second process, same cache: the 8K tier must come from disk.
  $kCool = Invoke-Nav '8k-cool' $pair8k.dir $pair8k.ext @('--display-cache=on')
  $results['8k_cool'] = $kCool
  Check ($kCool.prefetch_disk -ge 1) '8K second process reads the tier from disk'
  Check ($kCool.nav_ms -le 30) '8K second open is on screen in <= 30 ms'
} else {
  Write-Host 'skip 8K: samples\single8k\8k.jpg missing'
}

# --- 4. default / explicit dump fields -------------------------------------
# V3-04b: the chain is on by default (規劃/25 §7.6/§7.7 are done), so the "off"
# baseline has to ask for it explicitly.
Write-Host '--- layout dump: explicit off / default (on) / cache hit ---'
Reset-Data
$dumpOff = Invoke-Dump 'dump-off' $pair24.dir $pair24.ext @('--display-cache=off')
Reset-Data
$dumpDefault = Invoke-Dump 'dump-default' $pair24.dir $pair24.ext @()
Reset-Data
$dumpOn = Invoke-Dump 'dump-on' $pair24.dir $pair24.ext @('--display-cache=on')
# The third run reuses the cache: it is the only dump that can report a hit.
$dumpHit = Invoke-Dump 'dump-hit' $pair24.dir $pair24.ext @('--display-cache=on')
$results['dump_off'] = $dumpOff
$results['dump_default'] = $dumpDefault
$results['dump_on'] = $dumpOn
$results['dump_hit'] = $dumpHit
Check ($dumpOff.current_is_tier -eq '0') 'off dump: current_is_tier=0'
Check ($dumpOff.display_edge -eq '0') 'off dump: display_edge=0'
Check ($dumpOff.tier_hits -eq 0) 'off run has no tier hit'
Check ($dumpDefault.current_is_tier -eq '1') 'default run uses a tier (chain is on by default)'
Check ([int]$dumpDefault.display_edge -ge 512) 'default run reports a real tier edge'
Check ($dumpOn.current_is_tier -eq '1') 'tier dump: current_is_tier=1'
Check ([int]$dumpOn.display_edge -ge 512) 'tier dump: display_edge is a real tier'
Check ($dumpOn.cache_source -match '^(none|mem)$') 'tier dump: cache_source is none or mem'
Check ($dumpHit.current_is_tier -eq '1') 'hit dump: current_is_tier=1'
Check ($dumpHit.cache_source -eq 'mem') 'hit dump: cache_source=mem (prefetched)'
Check ($dumpHit.display_edge -eq $dumpOn.display_edge) 'hit dump: same tier edge as the decode'

# --- 5. V3-04b: §7.6 upgrade / forced export, §7.7 proxy input, §7.13 --------
Write-Host '--- V3-04b: tab-upgrade, forced export, decode topology ---'

# Dump of the *second* image after also pressing a key, so the dump reflects a
# view change on top of the tier load.
function Invoke-KeyDump([string]$label, [string]$pairDir, [string]$ext,
                         [string]$keys, [string[]]$extra = @()) {
  $image = Join-Path $pairDir ('a' + $ext)
  $dump = Join-Path $Work ("dump-$label.txt")
  Remove-Item $dump -Force -ErrorAction SilentlyContinue
  $app = Start-App $image (@($extra) + @('--send-keys', $keys, '--layout-dump',
                                        $dump, '--quit-at', '12000'))
  $deadline = [DateTime]::UtcNow.AddSeconds(15)
  while ([DateTime]::UtcNow -lt $deadline) {
    Update-Peak $app.process
    if ((Test-Path $dump) -and ((Get-Item $dump).Length -gt 0)) { break }
    Start-Sleep -Milliseconds 50
  }
  Sleep-Poll 200 $app.process
  $dumpText = if (Test-Path $dump) { Get-Content $dump -Raw } else { '' }
  $entry = [pscustomobject][ordered]@{
    current_is_tier = First-Match $dumpText 'current_is_tier=(\d+)'
    display_edge = First-Match $dumpText 'display_edge=(\d+)'
    source_is_tier = First-Match $dumpText 'source_is_tier=(\d+)'
    cache_source = First-Match $dumpText 'cache_source=(\S+)'
    zoom = First-Match $dumpText 'zoom=([0-9.]+)'
    zoom_percent = First-Match $dumpText 'zoom_percent=(\d+)'
    fit = First-Match $dumpText 'fit=(\d+)'
    proxy_ready = First-Match $dumpText 'proxy_ready=(\d+)'
  }
  [void](Close-App $app.process $app.hwnd)
  Save-Log $label
  $entry | Add-Member -NotePropertyName log -NotePropertyValue (Read-Log)
  return $entry
}

# $7.6: a view that reaches 100 % of the file asks for the full frame once and
# keeps the tier on screen until it lands.
Reset-Data
$upgrade = Invoke-KeyDump 'upgrade-100' $pair24.dir $pair24.ext 'Right,1' @('--display-cache=on')
$results['upgrade_100'] = $upgrade
Check ([regex]::Matches($upgrade.log, 'upgrade: requested full frame').Count -ge 1) `
  'upgrade: the 100 % view requested the full frame'
Check ([regex]::Matches($upgrade.log, 'upgrade: full frame applied').Count -ge 1) `
  'upgrade: the full frame replaced the tier'
Check ($upgrade.current_is_tier -eq '0') 'upgrade: the frame on screen is the full frame'
Check ($upgrade.display_edge -eq '0') 'upgrade: display_edge is cleared'
Check ($upgrade.fit -eq '0') 'upgrade: the 100 % view is a manual view'
Check ([math]::Abs([double]$upgrade.zoom - 1.0) -le 0.001) `
  ("upgrade: the zoom is 1.000 of the file (actual={0})" -f $upgrade.zoom)
Check ($upgrade.zoom_percent -eq '100') 'upgrade: the title shows 100 %'
# The tier really was the first frame of that load, and the upgrade came after
# it (otherwise these checks could pass on the plain full-frame path).
$tierIndex = $upgrade.log.IndexOf('display tier request:')
$upgradeIndex = $upgrade.log.IndexOf('upgrade: requested full frame')
Check ($tierIndex -ge 0 -and $upgradeIndex -gt $tierIndex) `
  'upgrade: a tier was shown first, then the upgrade was requested'

# $7.6: a fit-scale capture keeps the tier (that is what the user sees), while a
# 100 % capture must be byte-identical to the full-frame path.
$pngDir = Join-Path $Out 'v3-04b'
New-Item -ItemType Directory -Path $pngDir -Force | Out-Null
function Invoke-Shot([string]$name, [string]$extraMode, [string]$keys) {
  $png = Join-Path $pngDir $name
  Remove-Item $png -Force -ErrorAction SilentlyContinue
  Reset-Data
  $app = Start-App (Join-Path $pair24.dir ('a' + $pair24.ext)) `
    @($extraMode, '--send-keys', $keys, '--screenshot', $png, '--shot-delay', '3000')
  [void]$app.process.WaitForExit(30000)
  Save-Log ('shot-' + [System.IO.Path]::GetFileNameWithoutExtension($name) + '-' + $extraMode)
  return $png
}
$shotOff = Invoke-Shot 'actual-off.png' '--display-cache=off' 'Right,1'
$shotOn = Invoke-Shot 'actual-on.png' '--display-cache=on' 'Right,1'
$hashOff = if (Test-Path $shotOff) { (Get-FileHash -Algorithm SHA256 $shotOff).Hash } else { '' }
$hashOn = if (Test-Path $shotOn) { (Get-FileHash -Algorithm SHA256 $shotOn).Hash } else { '' }
Check ($hashOff -ne '') 'screenshot parity: the off-mode 100 % capture exists'
Check ($hashOn -ne '') 'screenshot parity: the on-mode 100 % capture exists'
Check ($hashOff -eq $hashOn) 'screenshot parity: 100 % captures are byte-identical'
$fitOff = Invoke-Shot 'fit-off.png' '--display-cache=off' 'Right'
$fitOn = Invoke-Shot 'fit-on.png' '--display-cache=on' 'Right'
Check ((Test-Path $fitOff) -and (Test-Path $fitOn)) 'fit captures exist for the tier A/B'
if ((Test-Path $fitOff) -and (Test-Path $fitOn)) {
  $fitOffHash = (Get-FileHash -Algorithm SHA256 $fitOff).Hash
  $fitOnHash = (Get-FileHash -Algorithm SHA256 $fitOn).Hash
  Check ($fitOffHash -ne $fitOnHash) `
    'a fit-scale capture keeps the tier (not silently upgraded)'
}

# $7.6: save-as writes file pixels even when only the tier is on screen. The
# saved JPEG's dimensions are the proof; the log line names the forced decode.
$savedJpg = Join-Path $pngDir 'export-on.jpg'
Remove-Item $savedJpg -Force -ErrorAction SilentlyContinue
Reset-Data
$saveApp = Start-App (Join-Path $pair24.dir ('a' + $pair24.ext)) `
  @('--display-cache=on', '--send-keys', 'Right', '--save-as', $savedJpg)
$saveDeadline = [DateTime]::UtcNow.AddSeconds(20)
while ([DateTime]::UtcNow -lt $saveDeadline) {
  Update-Peak $saveApp.process
  if ((Test-Path $savedJpg) -and ((Get-Item $savedJpg).Length -gt 0)) {
    Start-Sleep -Milliseconds 300
    break
  }
  Start-Sleep -Milliseconds 100
}
$saveLog = Read-Log
[void](Close-App $saveApp.process $saveApp.hwnd)
Save-Log 'export-on'
Check ([regex]::Matches($saveLog, 'export: forced full decode ms=').Count -ge 1) `
  'export: a tier save forced a full decode'
Check (Test-Path $savedJpg) 'export: the save-as file was written'
if (Test-Path $savedJpg) {
  Add-Type -AssemblyName System.Drawing
  $saved = [System.Drawing.Image]::FromFile($savedJpg)
  try {
    $savedW = $saved.Width
    $savedH = $saved.Height
  } finally { $saved.Dispose() }
  Check ($savedW -eq 6000 -and $savedH -eq 4000) `
    ("export: the saved pixels are the file, not the tier ({0}x{1})" -f $savedW, $savedH)
}

# $7.7 wiring: the renderer knows the source is a tier (the proxy policy input),
# and the dump proves it.
Reset-Data
$tierSrc = Invoke-KeyDump 'dump-source-tier' $pair24.dir $pair24.ext 'Right' @('--display-cache=on')
$results['dump_source_tier'] = $tierSrc
Check ($tierSrc.current_is_tier -eq '1') 'tier dump: current_is_tier=1'
Check ($tierSrc.source_is_tier -eq '1') `
  'tier dump: the renderer was told the source is a tier (plan 7.7 input)'

# $7.13-3: one Detect per open. A prefetched neighbour is a cache hit, so the
# tier *decode* path (where the preview-size probe runs) needs --prefetch=off.
Reset-Data
$tierDecode = Invoke-KeyDump 'dump-tier-decode' $pair24.dir $pair24.ext 'Right' `
  @('--display-cache=on', '--prefetch=off')
$results['dump_tier_decode'] = $tierDecode
Check ($tierDecode.current_is_tier -eq '1') `
  'tier decode: the navigated frame was decoded at the tier (no prefetch)'
Check ([regex]::Matches($tierDecode.log, 'probe: preview reused tier probe').Count -ge 1) `
  'detect-once: the preview reused the tier probe instead of reopening the file'
# The first load of the process has no tier probe, so exactly one plain header
# read is expected; a second one would mean Detect() ran twice for one open.
Check ([regex]::Matches($tierDecode.log, 'probe: preview header read').Count -le 1) `
  'detect-once: at most the first load needed its own header read'

# $7.13-1: three decode workers with the prefetch caps.
Check ([regex]::Matches($tierSrc.log, 'decode: workers=3 prefetch_cap=full:1 tier:2').Count -eq 1) `
  'concurrency: three decode workers with a prefetch gate'

# --- 6. sharpness A/B ------------------------------------------------------
if (-not $SkipSharpness) {
  Write-Host '--- sharpness A/B at fit (tier Fant vs full + cubic) ---'
  $content = $dumpOff.content
  Invoke-Sharpness '24mp' $pair24.dir $pair24.ext $pair24.reference $content
  Invoke-Sharpness 'photo30mp' $pairPhoto.dir $pairPhoto.ext $pairPhoto.reference $content
  # The 3 px stripe sample is the aliasing/moire case of 22/18 (run_quality_check);
  # after the fit downscale both arms average it to a flat grey, so it cannot
  # discriminate a tier from a full frame and is not used here.
}

$json = Join-Path $Out 'v3-04-tier-results.json'
$results | ConvertTo-Json -Depth 6 | Set-Content -Path $json -Encoding UTF8
Write-Host "results: $json"

if ($script:failures -ne 0) {
  Write-Host ("{0} of {1} checks failed" -f $script:failures, $script:checks)
  exit 1
}
Write-Host ("all {0} tier checks passed" -f $script:checks)
exit 0
