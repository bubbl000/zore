#requires -Version 5.1
<#
  V3-07 switch-speed harness (A/B/C, measurement only; product code untouched).

  For every (app, sample set, scenario, repeat):
    1. hard-links the 31-image set into a fresh directory (cold app cache for
       ZoreView, ZoreView and 2345, whose caches are path-scoped);
    2. launches the app on img00, restores it if maximized and forces a
       1280x800 window at (120,120);
    3. records the 1440x960 top-left region at ~60 fps (ddagrab, x264 -qp 0);
    4. injects real SendInput arrow keys at the scenario cadence and flashes a
       24x24 sync marker on every key;
    5. drains until the window is pixel-stable, stops the recording, and writes
       run.json (key timestamps, window facts, log/trace snapshots, peaks).

  Analysis is separate: analyze_switch_video.py.

  ASCII only (Windows PowerShell 5.1 reads BOM-less scripts as ANSI), and the
  file must keep its UTF-8 BOM because the exe paths contain CJK characters.
#>
param(
  [string]$Apps = 'ours,replica,2345',
  [string]$Sets = '24mp12,24mp25,8k',
  [string]$Scenarios = 'slow,s3,s4',
  [int]$Repeats = 3,
  [string]$Samples = 'E:\temp\nv-v3-07-samples',
  [string]$Out = '',
  # Extra switches appended to our own command line (A/B arms, e.g.
  # '--prefetch=off'). Ignored for the two foreign apps.
  [string]$AppArgs = '',
  [switch]$KeepVideo
)

$ErrorActionPreference = 'Stop'
Add-Type -Path (Join-Path $PSScriptRoot 'SwitchHarness.cs') `
  -ReferencedAssemblies System.Windows.Forms, System.Drawing
[NvSwitch]::MakeDpiAware()

$NewRoot = (Resolve-Path (Join-Path $PSScriptRoot '..\..\..')).Path
$OursExe = Join-Path $NewRoot 'build\Release\zoreview.exe'
$ReplicaExe = 'E:\06-xiangmu\new\看图软件复刻\build\zoreview.exe'
$PicExe = 'E:\01_软件中枢\办公协作\2345Pic\2345PicViewer.exe'
$FFMPEG = 'E:\02-AI\ffmpeg7.1.1\bin\ffmpeg.exe'

if (-not $Out) {
  $Out = Join-Path (Join-Path $PSScriptRoot '..\..\output\switch-speed') (Get-Date -Format 'yyyyMMdd-HHmmss')
}
New-Item -ItemType Directory -Force $Out | Out-Null
$Links = Join-Path $Out 'links'
$Work = Join-Path $Out 'work'
$Videos = Join-Path $Out 'videos'
New-Item -ItemType Directory -Force $Links, $Work, $Videos | Out-Null

$Rect = @(120, 120, 1280, 800)
$MarkerName = 'Local\NvSwitchMarker-' + [guid]::NewGuid().ToString('N')
$script:results = New-Object System.Collections.ArrayList
$script:marker = $null
$script:markerEvent = $null

# ---------------------------------------------------------------- helpers
function Kill-Apps {
  foreach ($name in @('zoreview', 'zoreview', '2345PicViewer', '2345Pic')) {
    Get-Process -Name $name -ErrorAction SilentlyContinue |
      Stop-Process -Force -ErrorAction SilentlyContinue
  }
  Start-Sleep -Milliseconds 350
}

function Start-Marker {
  $script:markerEvent = [System.Threading.EventWaitHandle]::new(
    $false, [System.Threading.EventResetMode]::AutoReset, $MarkerName)
  $script:marker = Start-Process powershell -ArgumentList @(
    '-NoProfile', '-WindowStyle', 'Hidden', '-ExecutionPolicy', 'Bypass',
    '-File', (Join-Path $PSScriptRoot 'sync_marker.ps1'), '-EventName', $MarkerName) -PassThru
  Start-Sleep -Milliseconds 3500
}

function Stop-Marker {
  if ($script:marker -and -not $script:marker.HasExited) {
    Stop-Process -Id $script:marker.Id -Force -ErrorAction SilentlyContinue
  }
  if ($script:markerEvent) { $script:markerEvent.Dispose() }
}

function New-SampleDir([string]$set, [string]$tag) {
  $dir = Join-Path $Links ($set + '\' + $tag)
  if (Test-Path $dir) { Remove-Item $dir -Recurse -Force }
  New-Item -ItemType Directory -Force $dir | Out-Null
  $source = Join-Path $Samples $set
  $made = 0
  foreach ($file in Get-ChildItem $source -Filter 'img*.jpg' | Sort-Object Name) {
    New-Item -ItemType HardLink -Path (Join-Path $dir $file.Name) -Value $file.FullName | Out-Null
    $made++
  }
  if ($made -lt 31) { throw "hardlink set incomplete for $set/${tag}: $made" }
  return $dir
}

function Wait-Window([int]$processId, [int]$timeoutMs) {
  $deadline = [DateTime]::UtcNow.AddMilliseconds($timeoutMs)
  while ([DateTime]::UtcNow -lt $deadline) {
    $h = [NvSwitch]::FindLargestWindowOfPid($processId, 200 * 120)
    if ($h -ne [IntPtr]::Zero) { return $h }
    Start-Sleep -Milliseconds 100
  }
  return [IntPtr]::Zero
}

function Wait-LogPattern([string]$path, [string]$pattern, [int]$timeoutMs) {
  # The apps keep their log/trace open for writing; a plain ReadAllText asks
  # for FileShare.Read and hits a sharing violation, so read with ReadWrite.
  $deadline = [DateTime]::UtcNow.AddMilliseconds($timeoutMs)
  while ([DateTime]::UtcNow -lt $deadline) {
    if (Test-Path $path) {
      try {
        $fs = [System.IO.File]::Open($path, [System.IO.FileMode]::Open,
          [System.IO.FileAccess]::Read,
          ([System.IO.FileShare]::ReadWrite -bor [System.IO.FileShare]::Delete))
        try {
          $reader = New-Object System.IO.StreamReader($fs, [System.Text.Encoding]::UTF8)
          $text = $reader.ReadToEnd()
        } finally { $fs.Dispose() }
        if ($text -match $pattern) { return $true }
      } catch {}
    }
    Start-Sleep -Milliseconds 80
  }
  return $false
}

function Start-App([string]$app, [string]$image, [string]$runId) {
  $dir = Join-Path $Work ($app + '\' + $runId)
  New-Item -ItemType Directory -Force $dir | Out-Null
  $info = @{ app = $app; dir = $dir; log = $null; trace = $null; process = $null }
  if ($app -eq 'ours') {
    $exe = Join-Path $dir 'zoreview.exe'
    Copy-Item $OursExe $exe -Force
    $info.log = Join-Path $dir 'logs\zoreview.log'
    $exeArgs = @('--portable', '--language', 'en', '--quit-at', '900000')
    if ($AppArgs -ne '') { $exeArgs += $AppArgs.Split(' ') }
    $exeArgs += $image
    $info.process = Start-Process $exe -ArgumentList $exeArgs -PassThru
  } elseif ($app -eq 'replica') {
    $lad = Join-Path $dir 'lad'
    $temp = Join-Path $dir 'temp'
    New-Item -ItemType Directory -Force $lad, $temp | Out-Null
    $oldLad = $env:LOCALAPPDATA; $oldTemp = $env:TEMP; $oldTmp = $env:TMP; $oldTrace = $env:PICVIEW_TRACE
    $env:LOCALAPPDATA = $lad
    $env:TEMP = $temp
    $env:TMP = $temp
    $env:PICVIEW_TRACE = '1'
    $info.trace = Join-Path $temp 'picview-trace.log'
    $info.process = Start-Process $ReplicaExe -ArgumentList ('"' + $image + '"') -PassThru `
      -RedirectStandardError (Join-Path $dir 'stderr.log') `
      -RedirectStandardOutput (Join-Path $dir 'stdout.log')
    $env:LOCALAPPDATA = $oldLad; $env:TEMP = $oldTemp; $env:TMP = $oldTmp; $env:PICVIEW_TRACE = $oldTrace
  } else {
    $info.process = Start-Process $PicExe -ArgumentList ('"' + $image + '"') -PassThru `
      -RedirectStandardError (Join-Path $dir 'stderr.log') `
      -RedirectStandardOutput (Join-Path $dir 'stdout.log')
  }
  return $info
}

function Wait-Ready($info) {
  if ($info.app -eq 'ours') {
    if (-not (Wait-LogPattern $info.log 'load: done ms=' 25000)) {
      throw 'ours: first image did not decode'
    }
    Start-Sleep -Milliseconds 400
  } elseif ($info.app -eq 'replica') {
    if (-not (Wait-LogPattern $info.trace 'ui: apply|ui: completion' 25000)) {
      throw 'replica: no ui: apply'
    }
    Start-Sleep -Milliseconds 400
  } else {
    Start-Sleep -Milliseconds 3000
  }
}

function Resolve-Window($info) {
  if ($info.app -eq '2345') {
    $h = [NvSwitch]::FindWindowByClassPrefix('RCImageViewerFrame', 200 * 120)
    if ($h -ne [IntPtr]::Zero) { return $h }
  }
  $h = Wait-Window $info.process.Id 15000
  if ($h -eq [IntPtr]::Zero) {
    $h = [NvSwitch]::FindWindowByTitlePart('2345', 200 * 120)
  }
  if ($h -eq [IntPtr]::Zero) { throw ("no window for " + $info.app) }
  return $h
}

function Start-Capture([string]$path) {
  $psi = New-Object System.Diagnostics.ProcessStartInfo
  $psi.FileName = $FFMPEG
  $psi.Arguments = '-hide_banner -loglevel error -f lavfi -i ddagrab=0:framerate=60 ' +
    '-vf hwdownload,format=bgra,crop=1440:960:0:0 -t 240 -c:v libx264 -preset ultrafast -qp 0 -y "' + $path + '"'
  $psi.RedirectStandardInput = $true
  $psi.RedirectStandardError = $true
  $psi.UseShellExecute = $false
  $psi.CreateNoWindow = $true
  return [System.Diagnostics.Process]::Start($psi)
}

function Stop-Capture($proc) {
  try { $proc.StandardInput.WriteLine('q') } catch {}
  if (-not $proc.WaitForExit(8000)) { try { $proc.Kill() } catch {} }
  try { $proc.StandardError.ReadToEnd() | Out-Null } catch {}
}

function Save-Shot([string]$path) {
  [NvSwitch]::SaveScreenshot(0, 0, 1440, 960, $path)
}

# ---------------------------------------------------------------- one run
function Invoke-Run {
  param(
    [string]$app, [string]$set, [string]$scenario, [int]$repeat,
    [string]$sampleDir, [int]$count, [int]$intervalMs, [int]$direction,
    [string]$mode, [int]$split
  )
  $runId = "$app-$set-$scenario-r$repeat"
  $runDir = Join-Path $Out ('runs\' + $runId)
  New-Item -ItemType Directory -Force $runDir | Out-Null
  Write-Host ("run {0} count={1} interval={2}" -f $runId, $count, $intervalMs)

  Kill-Apps
  $image = Join-Path $sampleDir 'img00.jpg'
  $info = Start-App $app $image $runId
  $hwnd = [IntPtr]::Zero
  $ff = $null
  $tail = $null
  $mem = $null
  try {
    Wait-Ready $info
    $hwnd = Resolve-Window $info
    [void][NvSwitch]::RestoreIfMaximized($hwnd)
    $placed = [NvSwitch]::PlaceWindow($hwnd, $Rect[0], $Rect[1], $Rect[2], $Rect[3], 3)
    [void][NvSwitch]::ForceForeground($hwnd)
    Start-Sleep -Milliseconds 500

    $client = [NvSwitch]::ClientRectOf($hwnd)
    $rectNow = [NvSwitch]::RectOf($hwnd)
    $pidTarget = [NvSwitch]::PidOf($hwnd)
    Save-Shot (Join-Path $runDir 'before.png')

    $video = Join-Path $Videos ($runId + '.mkv')
    $ff = Start-Capture $video
    Start-Sleep -Milliseconds 800

    $clock = [System.Diagnostics.Stopwatch]::StartNew()
    if ($info.app -eq 'ours') {
      $tail = [NvSwitch+LogTail]::new()
      $tail.Start($info.log)
    }
    $mem = [NvSwitch+MemoryWatch]::new()
    $mem.Start($pidTarget, 20)

    $blocks = New-Object System.Collections.ArrayList
    if ($mode -eq 'slow') {
      $t0 = $clock.Elapsed.TotalMilliseconds
      $first = [NvSwitch]::RunKeys($hwnd, $direction, $split, $intervalMs, $MarkerName)
      [void]$blocks.Add(@{ phase = 'S1'; hits = $first; startMs = $t0 })
      Start-Sleep -Milliseconds 1600
      $t0 = $clock.Elapsed.TotalMilliseconds
      $second = [NvSwitch]::RunKeys($hwnd, -$direction, $count - $split, $intervalMs, $MarkerName)
      [void]$blocks.Add(@{ phase = 'S2'; hits = $second; startMs = $t0 })
      [void][NvSwitch]::WaitStable($client[0], $client[1], $client[2], $client[3], 800, 8000, 100)
    } else {
      $t0 = $clock.Elapsed.TotalMilliseconds
      $hits = [NvSwitch]::RunKeys($hwnd, $direction, $count, $intervalMs, $MarkerName)
      [void]$blocks.Add(@{ phase = $scenario; hits = $hits; startMs = $t0 })
      [void][NvSwitch]::WaitStable($client[0], $client[1], $client[2], $client[3], 1200, 30000, 100)
    }
    Start-Sleep -Milliseconds 600
    $runMs = $clock.ElapsedMilliseconds
    $mem.Stop()
    if ($tail) { $tail.Stop() }
    Stop-Capture $ff
    $ff = $null
    Start-Sleep -Milliseconds 250

    $keys = @()
    $index = 0
    foreach ($block in $blocks) {
      foreach ($h in $block.hits) {
        $keys += [ordered]@{
          index = $index
          phase = $block.phase
          t0_ms = [math]::Round($h.T0Ms, 2)
          t0_global_ms = [math]::Round($block.startMs + $h.T0Ms, 2)
          foreground_ok = $h.ForegroundOk
        }
        $index++
      }
    }

    $logEvents = @()
    if ($tail) {
      foreach ($entry in $tail.Snapshot()) {
        $parts = $entry.Split("`t", 2)
        if ($parts.Count -lt 2) { continue }
        $line = $parts[1]
        if ($line -match 'load: start|load: done|load: shown|memory cache hit|disk cache hit|display cache hit|preview: shown|load: cancelled|decoded:|open request') {
          $logEvents += [ordered]@{ t_ms = [double]$parts[0]; line = $line }
        }
      }
      if (Test-Path $info.log) { Copy-Item $info.log (Join-Path $runDir 'app.log') -Force }
    }
    if ($info.trace -and (Test-Path $info.trace)) {
      Copy-Item $info.trace (Join-Path $runDir 'trace.log') -Force
    }
    foreach ($extra in @('stderr.log', 'stdout.log')) {
      $p = Join-Path $info.dir $extra
      if (Test-Path $p) { Copy-Item $p (Join-Path $runDir $extra) -Force }
    }

    $run = [ordered]@{
      app = $app
      sample_set = $set
      scenario = $scenario
      repeat = $repeat
      mode = $mode
      split = if ($mode -eq 'slow') { $split } else { 0 }
      slow_split = if ($mode -eq 'slow') { $split } else { $null }
      direction = $direction
      interval_ms = $intervalMs
      key_count = $count
      sample_dir = $sampleDir
      window_rect = $rectNow
      client_rect = $client
      window_placed = $placed
      run_ms = $runMs
      foreground_all = -not [bool]($keys | Where-Object { -not $_.foreground_ok })
      keys = $keys
      log_events = $logEvents
      peak_private_mb = [math]::Round($mem.PeakPrivate / 1MB, 1)
      peak_ws_mb = [math]::Round($mem.PeakWs / 1MB, 1)
      video = $video
      notes = @()
    }
    if (-not $placed) { $run.notes += 'window not placed at 1280x800' }
    $run | ConvertTo-Json -Depth 6 | Set-Content -Encoding UTF8 (Join-Path $runDir 'run.json')
    Save-Shot (Join-Path $runDir 'after.png')

    [void]$script:results.Add($run)
    Write-Host ("  peak priv={0}MB ws={1}MB fg={2} placed={3}" -f `
      $run.peak_private_mb, $run.peak_ws_mb, $run.foreground_all, $placed)
  }
  finally {
    if ($ff) { Stop-Capture $ff }
    if ($mem) { $mem.Stop() }
    if ($tail) { $tail.Stop() }
    if ($hwnd -ne [IntPtr]::Zero) {
      [NvSwitch]::PostMessageClose($hwnd)
      Start-Sleep -Milliseconds 300
    }
    if ($info.process -and -not $info.process.HasExited) {
      try { $info.process.Kill() } catch {}
    }
    Kill-Apps
  }
}

# ---------------------------------------------------------------- main
if (-not (Test-Path (Join-Path $Samples 'samples.json'))) {
  throw "samples not found under $Samples"
}

$appList = $Apps.Split(',')
$setList = $Sets.Split(',')
$scenarioList = $Scenarios.Split(',')

Start-Marker
try {
  foreach ($set in $setList) {
    foreach ($scenario in $scenarioList) {
      for ($repeat = 1; $repeat -le $Repeats; $repeat++) {
        $tag = "$scenario-r$repeat"
        $sampleDir = New-SampleDir $set $tag
        foreach ($app in $appList) {
          if ($scenario -eq 'slow') {
            Invoke-Run -app $app -set $set -scenario 'slow' -repeat $repeat -sampleDir $sampleDir `
              -count 40 -intervalMs 1200 -direction 1 -mode 'slow' -split 20
          } elseif ($scenario -eq 's3') {
            Invoke-Run -app $app -set $set -scenario 's3' -repeat $repeat -sampleDir $sampleDir `
              -count 20 -intervalMs 200 -direction 1 -mode 'fast' -split 0
          } elseif ($scenario -eq 's4') {
            Invoke-Run -app $app -set $set -scenario 's4' -repeat $repeat -sampleDir $sampleDir `
              -count 30 -intervalMs 200 -direction 1 -mode 'fast' -split 0
          }
        }
      }
    }
  }
} finally {
  Stop-Marker
  Kill-Apps
}

$script:results | ConvertTo-Json -Depth 8 | Set-Content -Encoding UTF8 (Join-Path $Out 'results.json')
Write-Host ("wrote " + (Join-Path $Out 'results.json'))
