#requires -Version 5.1
<#
  V3-01 baseline measurement (read-only; no product changes).

  Creates a throw-away portable ZoreView directory, runs the release binary
  against generated 24 MP / 8K JPEGs, injects real WM_KEYDOWN navigation at a
  fixed cadence, polls private bytes, mutates a 2000-file folder from outside,
  and writes one JSON result file. All sample files live outside the repo.

  Prerequisite layout (made by make_v3_baseline_samples.py):
    $Samples\single24_12\24mp_12mb.jpg
    $Samples\single24_25\24mp_25mb.jpg
    $Samples\single8k\8k.jpg
    $Samples\flip24\flip01..12.jpg
    $Samples\flip8k\01..10.jpg
    $Samples\watch2000\w0000..w1999.bmp

  Example:
    python tests\tools\make_v3_baseline_samples.py C:\temp\nv-v3-samples
    powershell -ExecutionPolicy Bypass -File tests\tools\run_v3_baseline.ps1 `
      -Exe build\Release\zoreview.exe -Samples C:\temp\nv-v3-samples `
      -Out C:\temp\nv-v3-results -Repeats 3

  ASCII only: Windows PowerShell 5.1 reads BOM-less scripts as ANSI.
#>
param(
  [Parameter(Mandatory = $true)][string]$Exe,
  [Parameter(Mandatory = $true)][string]$Samples,
  [Parameter(Mandatory = $true)][string]$Out,
  [int]$Repeats = 3,
  [int]$PacedMs = 1200,
  [int]$RapidMs = 350
)

$ErrorActionPreference = 'Stop'
if (-not ([System.Management.Automation.PSTypeName]'NvV301Win32').Type) {
  Add-Type @"
using System;
using System.Runtime.InteropServices;
public static class NvV301Win32 {
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
$script:all = [ordered]@{}

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

function Wait-Log([string]$pattern, [int]$timeoutMs, [System.Diagnostics.Process]$p) {
  $sw = [System.Diagnostics.Stopwatch]::StartNew()
  while ($sw.ElapsedMilliseconds -lt $timeoutMs) {
    Update-Peak $p
    if ((Read-Log) -match $pattern) { return $true }
    Start-Sleep -Milliseconds 15
  }
  return $false
}

function Wait-Marker([long]$offset, [string]$pattern, [int]$timeoutMs,
                     [System.Diagnostics.Process]$p) {
  $sw = [System.Diagnostics.Stopwatch]::StartNew()
  $seg = ''
  while ($sw.ElapsedMilliseconds -lt $timeoutMs) {
    Update-Peak $p
    $seg = Read-Segment $offset
    if ($seg -match $pattern) {
      return @{ ms = $sw.ElapsedMilliseconds; text = $seg }
    }
    Start-Sleep -Milliseconds 10
  }
  return @{ ms = $null; text = $seg }
}

function Start-App([string]$image, [string[]]$extra = @()) {
  $arguments = @('--portable', '--language', 'en', '--quit-at', '60000') + $extra + @($image)
  $p = Start-Process -FilePath $WorkExe -ArgumentList $arguments -PassThru
  $script:peakPrivate = 0
  $hwnd = [IntPtr]::Zero
  $deadline = [DateTime]::UtcNow.AddSeconds(15)
  while ([DateTime]::UtcNow -lt $deadline) {
    Update-Peak $p
    try {
      $p.Refresh()
      if ($p.MainWindowHandle -ne 0) { $hwnd = $p.MainWindowHandle }
    } catch {}
    if ($hwnd -ne [IntPtr]::Zero -and (Test-Path $LogPath) -and ((Read-Log) -match 'decoded:')) { break }
    Start-Sleep -Milliseconds 25
  }
  if ($hwnd -eq [IntPtr]::Zero) { throw "no main window for $image" }
  if (-not ((Read-Log) -match 'decoded:')) { throw "no first decoded image for $image" }
  return @{ process = $p; hwnd = $hwnd }
}

function Post-Key([System.IntPtr]$hwnd, [int]$vk) {
  [void][NvV301Win32]::PostMessage($hwnd, 0x0100, [IntPtr]$vk, [IntPtr]0)
  Start-Sleep -Milliseconds 25
  [void][NvV301Win32]::PostMessage($hwnd, 0x0101, [IntPtr]$vk, [IntPtr]0)
}

function Close-App([System.Diagnostics.Process]$p, [System.IntPtr]$hwnd) {
  [void][NvV301Win32]::PostMessage($hwnd, 0x0010, [IntPtr]::Zero, [IntPtr]::Zero)
  if (-not $p.WaitForExit(8000)) {
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

function Memory-Stats([string]$text) {
  $values = [regex]::Matches($text, 'peak_private_mb=([0-9.]+)') |
    ForEach-Object { [double]$_.Groups[1].Value }
  $max = 0
  foreach ($v in $values) { if ($v -gt $max) { $max = $v } }
  return $max
}

function Single-Scenario([string]$label, [string]$image) {
  Reset-Data
  $app = Start-App $image
  [void](Wait-Log 'load: done ms=' 20000 $app.process)
  Sleep-Poll 900 $app.process
  [void](Wait-Log 'startup: first_image=' 3000 $app.process)
  $closed = Close-App $app.process $app.hwnd
  $text = Read-Log
  $cache = Cache-Stats
  $item = [ordered]@{
    label = $label
    image = (Split-Path -Leaf $image)
    image_bytes = (Get-Item $image).Length
    load_done_ms = [double](First-Match $text 'load: done ms=([0-9.]+)')
    load_shown_after_ms = $null
    startup_first_image_ms = [double](First-Match $text 'startup: first_image=([0-9.]+)ms')
    app_peak_private_mb = [math]::Round((Memory-Stats $text), 1)
    ext_peak_private_mb = [math]::Round($script:peakPrivate / 1MB, 1)
    bitmap = First-Match $text 'renderer: bitmap ([0-9x]+)'
    bitmap_mb = [double](First-Match $text 'renderer: bitmap [0-9x]+ bgra=([0-9.]+)MB')
    disk_skip_mb = [int](First-Match $text 'disk cache write skipped: frame is ([0-9]+) MB')
    folder_files = [int](First-Match $text 'folder: ([0-9]+) files')
    folder_scan_ms = [double](First-Match $text 'folder: [0-9]+ files in ([0-9.]+)ms')
    decoded_count = ([regex]::Matches($text, 'decoded:')).Count
    memory_hit_count = ([regex]::Matches($text, 'memory cache hit:')).Count
    disk_hit_count = ([regex]::Matches($text, 'disk cache hit:')).Count
    cancelled_count = ([regex]::Matches($text, 'load: cancelled path=')).Count
    closed_cleanly = $closed
  }
  $shown = [regex]::Match($text, 'load: shown path=[^\r\n]+ after_ms=([0-9.]+)')
  if ($shown.Success) { $item.load_shown_after_ms = [double]$shown.Groups[1].Value }
  foreach ($key in $cache.Keys) { $item[$key] = $cache[$key] }
  return $item
}

function Flip-Scenario([string]$label, [string]$image, [int]$keys, [int]$intervalMs,
                       [int]$backKeys = 0) {
  Reset-Data
  $app = Start-App $image
  $perKey = @()
  $forwardMs = @()
  $hitSources = @()
  for ($i = 1; $i -le $keys; $i++) {
    Sleep-Poll $intervalMs $app.process
    $offset = Log-Length
    $sw = [System.Diagnostics.Stopwatch]::StartNew()
    Post-Key $app.hwnd 0x27
    $m = Wait-Marker $offset 'load: done ms=|load: cancelled path=|memory cache hit:|disk cache hit:' 10000 $app.process
    $marker = 'timeout'
    if ($m.text -match 'load: done ms=') { $marker = 'decoded' }
    elseif ($m.text -match 'memory cache hit:') { $marker = 'memory' }
    elseif ($m.text -match 'disk cache hit:') { $marker = 'disk' }
    elseif ($m.text -match 'load: cancelled path=') { $marker = 'cancelled' }
    $done = First-Match $m.text 'load: done ms=([0-9.]+)'
    if ($done) { $forwardMs += [double]$done }
    $hitSources += $marker
    $perKey += [ordered]@{
      index = $i
      direction = 'forward'
      key_to_marker_ms = $m.ms
      marker = $marker
      app_load_done_ms = if ($done) { [double]$done } else { $null }
      path = First-Match $m.text 'path=([^\r\n]+)'
    }
    $left = $intervalMs - $sw.ElapsedMilliseconds
    if ($left -gt 0) { Sleep-Poll $left $app.process }
  }
  for ($i = 1; $i -le $backKeys; $i++) {
    Sleep-Poll $intervalMs $app.process
    $offset = Log-Length
    $sw = [System.Diagnostics.Stopwatch]::StartNew()
    Post-Key $app.hwnd 0x25
    $m = Wait-Marker $offset 'memory cache hit:|load: done ms=|load: cancelled path:' 10000 $app.process
    $marker = 'timeout'
    if ($m.text -match 'memory cache hit:') { $marker = 'memory' }
    elseif ($m.text -match 'load: done ms=') { $marker = 'decoded' }
    elseif ($m.text -match 'load: cancelled path=') { $marker = 'cancelled' }
    $done = First-Match $m.text 'load: done ms=([0-9.]+)'
    $hitSources += $marker
    $perKey += [ordered]@{
      index = $i
      direction = 'back'
      key_to_marker_ms = $m.ms
      marker = $marker
      app_load_done_ms = if ($done) { [double]$done } else { $null }
      path = First-Match $m.text 'path=([^\r\n]+)'
    }
    $left = $intervalMs - $sw.ElapsedMilliseconds
    if ($left -gt 0) { Sleep-Poll $left $app.process }
  }
  Sleep-Poll 1000 $app.process
  $closed = Close-App $app.process $app.hwnd
  $text = Read-Log
  $cache = Cache-Stats
  $sorted = @($forwardMs | Sort-Object)
  $median = $null
  $p95 = $null
  if ($sorted.Count -gt 0) {
    $median = $sorted[[int][math]::Floor(($sorted.Count - 1) / 2)]
    $p95 = $sorted[[int][math]::Ceiling($sorted.Count * 0.95) - 1]
  }
  $item = [ordered]@{
    label = $label
    image = (Split-Path -Leaf $image)
    image_bytes = (Get-Item $image).Length
    interval_ms = $intervalMs
    keys = $keys
    back_keys = $backKeys
    forward_load_done_ms = $forwardMs
    forward_median_ms = $median
    forward_p95_ms = $p95
    key_results = $perKey
    hit_sources = $hitSources
    app_peak_private_mb = [math]::Round((Memory-Stats $text), 1)
    ext_peak_private_mb = [math]::Round($script:peakPrivate / 1MB, 1)
    bitmap = First-Match $text 'renderer: bitmap ([0-9x]+)'
    bitmap_mb = [double](First-Match $text 'renderer: bitmap [0-9x]+ bgra=([0-9.]+)MB')
    decoded_count = ([regex]::Matches($text, 'decoded:')).Count
    memory_hit_count = ([regex]::Matches($text, 'memory cache hit:')).Count
    disk_hit_count = ([regex]::Matches($text, 'disk cache hit:')).Count
    cancelled_count = ([regex]::Matches($text, 'load: cancelled path=')).Count
    disk_skip_count = ([regex]::Matches($text, 'disk cache write skipped: frame is')).Count
    folder_files = [int](First-Match $text 'folder: ([0-9]+) files')
    folder_scan_ms = [double](First-Match $text 'folder: [0-9]+ files in ([0-9.]+)ms')
    closed_cleanly = $closed
  }
  foreach ($key in $cache.Keys) { $item[$key] = $cache[$key] }
  return $item
}

function Watch-Scenario([int]$repeat, [int[]]$waits = @(), [string]$label = 'watch2000') {
  $base = Join-Path $Samples 'watch2000'
  if (-not (Test-Path $base)) { throw "watch baseline folder missing: $base" }
  $watch = Join-Path $Out ("watch-{0}-{1}" -f $repeat, $label)
  Remove-Item $watch -Recurse -Force -ErrorAction SilentlyContinue
  Copy-Item $base $watch -Recurse
  Reset-Data
  $image = Join-Path $watch 'w0000.bmp'
  # V3-05: this baseline measures the legacy 2 s poll, so pin the source with
  # --watch-poll; the ReadDirectoryChangesW path is covered by run_watch_check.
  $app = Start-App $image @('--watch-poll')
  Sleep-Poll 2200 $app.process
  $mutations = @()
  $ops = @(
    @{ kind = 'add'; run = { Copy-Item (Join-Path $watch 'w0001.bmp') (Join-Path $watch 'a_new.bmp') } },
    @{ kind = 'rename'; run = { Rename-Item (Join-Path $watch 'w0100.bmp') 'w0100_renamed.bmp' } },
    @{ kind = 'delete'; run = { Remove-Item (Join-Path $watch 'w0200.bmp') } }
  )
  for ($i = 0; $i -lt $ops.Count; $i++) {
    $waitMs = 2500
    if ($waits.Count -gt $i) { $waitMs = $waits[$i] }
    if ($waitMs -gt 0) { Sleep-Poll $waitMs $app.process }
    $op = $ops[$i]
    $offset = Log-Length
    $sw = [System.Diagnostics.Stopwatch]::StartNew()
    & $op.run
    $opMs = $sw.ElapsedMilliseconds
    $m = Wait-Marker $offset 'watch: rescan files=|watch: current=removed|watch: folder=' 6000 $app.process
    $latency = $sw.ElapsedMilliseconds
    $files = First-Match $m.text 'watch: rescan files=([0-9]+)'
    $index = First-Match $m.text 'watch: rescan files=[0-9]+ index=([0-9]+)'
    $mutations += [ordered]@{
      kind = $op.kind
      op_ms = $opMs
      visible_latency_ms = $latency
      marker = if ($m.text -match 'watch: rescan') { 'rescan' } elseif ($m.text -match 'watch: current=removed') { 'removed' } else { 'timeout' }
      files = if ($files) { [int]$files } else { $null }
      index = if ($index) { [int]$index } else { $null }
    }
  }
  $closed = Close-App $app.process $app.hwnd
  $text = Read-Log
  $cache = Cache-Stats
  $item = [ordered]@{
    label = $label
    repeat = $repeat
    mutations = $mutations
    app_peak_private_mb = [math]::Round((Memory-Stats $text), 1)
    ext_peak_private_mb = [math]::Round($script:peakPrivate / 1MB, 1)
    folder_files = [int](First-Match $text 'folder: ([0-9]+) files')
    folder_scan_ms = [double](First-Match $text 'folder: [0-9]+ files in ([0-9.]+)ms')
    decoded_count = ([regex]::Matches($text, 'decoded:')).Count
    watch_rescan_count = ([regex]::Matches($text, 'watch: rescan files=')).Count
    closed_cleanly = $closed
    phase_sweep = $false
  }
  foreach ($key in $cache.Keys) { $item[$key] = $cache[$key] }
  return $item
}

function Save-Log([string]$name) {
  $dir = Join-Path $Out 'logs'
  New-Item -ItemType Directory -Path $dir -Force | Out-Null
  if (Test-Path $LogPath) {
    Copy-Item $LogPath (Join-Path $dir ($name + '.log')) -Force
  }
}

$sha = (Get-FileHash $Exe -Algorithm SHA256).Hash
$all = [ordered]@{
  generated_at = (Get-Date).ToString('o')
  exe = $Exe
  exe_sha256 = $sha
  samples = $Samples
  repeats = $Repeats
  paced_ms = $PacedMs
  rapid_ms = $RapidMs
  singles = @()
  flips = @()
  watch = @()
}

for ($r = 1; $r -le $Repeats; $r++) {
  Write-Host ("single 24mp_12 repeat {0}" -f $r)
  $item = Single-Scenario 'single_24mp_12' (Join-Path $Samples 'single24_12\24mp_12mb.jpg')
  $item.repeat = $r
  $all.singles += $item
  Save-Log ("single_24mp_12-r{0}" -f $r)
  Write-Host ("  load={0}ms peak={1}MB" -f $item.load_done_ms, $item.ext_peak_private_mb)

  Write-Host ("single 24mp_25 repeat {0}" -f $r)
  $item = Single-Scenario 'single_24mp_25' (Join-Path $Samples 'single24_25\24mp_25mb.jpg')
  $item.repeat = $r
  $all.singles += $item
  Save-Log ("single_24mp_25-r{0}" -f $r)
  Write-Host ("  load={0}ms peak={1}MB" -f $item.load_done_ms, $item.ext_peak_private_mb)

  Write-Host ("single 8k repeat {0}" -f $r)
  $item = Single-Scenario 'single_8k' (Join-Path $Samples 'single8k\8k.jpg')
  $item.repeat = $r
  $all.singles += $item
  Save-Log ("single_8k-r{0}" -f $r)
  Write-Host ("  load={0}ms peak={1}MB" -f $item.load_done_ms, $item.ext_peak_private_mb)

  Write-Host ("flip 24mp paced repeat {0}" -f $r)
  $item = Flip-Scenario 'flip_24mp_paced' (Join-Path $Samples 'flip24\flip01.jpg') 10 $PacedMs 3
  $item.repeat = $r
  $all.flips += $item
  Save-Log ("flip_24mp_paced-r{0}" -f $r)
  Write-Host ("  median={0}ms p95={1}ms peak={2}MB memhit={3}" -f $item.forward_median_ms, $item.forward_p95_ms, $item.ext_peak_private_mb, $item.memory_hit_count)

  Write-Host ("flip 24mp rapid repeat {0}" -f $r)
  $item = Flip-Scenario 'flip_24mp_rapid' (Join-Path $Samples 'flip24\flip01.jpg') 10 $RapidMs 0
  $item.repeat = $r
  $all.flips += $item
  Save-Log ("flip_24mp_rapid-r{0}" -f $r)
  Write-Host ("  median={0}ms p95={1}ms cancel={2} peak={3}MB" -f $item.forward_median_ms, $item.forward_p95_ms, $item.cancelled_count, $item.ext_peak_private_mb)

  Write-Host ("flip 8k paced repeat {0}" -f $r)
  $item = Flip-Scenario 'flip_8k_paced' (Join-Path $Samples 'flip8k\01.jpg') 10 $PacedMs 0
  $item.repeat = $r
  $all.flips += $item
  Save-Log ("flip_8k_paced-r{0}" -f $r)
  Write-Host ("  median={0}ms p95={1}ms peak={2}MB" -f $item.forward_median_ms, $item.forward_p95_ms, $item.ext_peak_private_mb)

  Write-Host ("watch2000 repeat {0}" -f $r)
  $item = Watch-Scenario $r
  $all.watch += $item
  Save-Log ("watch2000-r{0}" -f $r)
  Write-Host ("  peak={0}MB scan={1}ms" -f $item.ext_peak_private_mb, $item.folder_scan_ms)
}

# One exploratory phase sweep: the 2 s poll makes the visible latency depend on
# where the mutation lands in the timer phase. Not part of the 3 repeats.
Write-Host "watch2000 phase sweep (0/1800/100 ms after a poll)"
$phase = Watch-Scenario 1 @(0, 1800, 100) 'watch2000_phase'
$phase['phase_sweep'] = $true
$all.watch += $phase
Save-Log 'watch2000-phase'

$json = $all | ConvertTo-Json -Depth 12
$jsonPath = Join-Path $Out 'v3-01-results.json'
Set-Content -Path $jsonPath -Value $json -Encoding UTF8
Write-Host ""
Write-Host ("wrote {0}" -f $jsonPath)

