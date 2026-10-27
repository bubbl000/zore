#requires -Version 5.1
<#
.SYNOPSIS
    M1-3 check: 8K JPEG decode and display, with memory measurement.

.DESCRIPTION
    - CLI: probe + decode the generated 7680x4320 sample, report decode_ms.
    - App: launch in --screenshot mode and poll the process every 100 ms to
      record peak working set / private bytes while the 8K bitmap is created.
    - Log: assert the renderer reports a 7680x4320 bitmap and that the app
      reports its own memory numbers at first image.

    Generate the sample first with tests\make_stress_samples.ps1.
#>
param(
    [string]$Image8k = (Join-Path $PSScriptRoot 'output\8k.jpg'),
    [string]$Exe = (Join-Path (Split-Path -Parent $PSScriptRoot) 'build\Release\zoreview.exe'),
    [string]$Cli = (Join-Path (Split-Path -Parent $PSScriptRoot) 'build\Release\zoreview_cli.exe'),
    [string]$Out = (Join-Path $PSScriptRoot 'output\8k-screen.png'),
    [int]$DwellMs = 3500,
    # The S0 budget (規劃/22/00 §5) is expressed as private bytes: <=350 MB for a
    # single 8K image. Working set tracks private almost 1:1 here (the 126.6 MB
    # D2D bitmap plus the decode buffers stay resident), so the WS ceiling is
    # only a "no second frame" guard.
    [double]$MaxPeakWsMB = 360,
    [double]$MaxPeakPrivateMB = 480
)

$ErrorActionPreference = 'Stop'
if (-not (Test-Path $Image8k)) { throw "8K sample not found: $Image8k (run make_stress_samples.ps1)" }
if (-not (Test-Path $Exe)) { throw "exe not found: $Exe" }

$logPath = Join-Path $env:LOCALAPPDATA 'ZoreView\logs\zoreview.log'
$logOffset = 0
if (Test-Path $logPath) { $logOffset = (Get-Item $logPath).Length }

Add-Type -AssemblyName System.Drawing

Write-Host '--- CLI ---'
$probe = & $Cli --probe $Image8k 2>&1
$probe | ForEach-Object { Write-Host $_ }
$tempRaw = Join-Path $env:TEMP 'nv_8k.bgra'
$decode = & $Cli --decode $Image8k --out $tempRaw 2>&1
$decode | Where-Object { $_ -match '^(size|stride|decode_ms|bytes):' } | ForEach-Object { Write-Host $_ }
$cliDecodeMs = [regex]::Match(($decode -join "`n"), 'decode_ms:\s*(\d+)').Groups[1].Value
$cliBytes = [regex]::Match(($decode -join "`n"), 'bytes:\s*(\d+)').Groups[1].Value

Write-Host ''
Write-Host '--- app ---'

# V3-04b: the display-tier chain is on by default, so the 8K full-frame path
# (where "current frame > 64 MB means no neighbour prefetch" lives) has to ask
# for it explicitly. Both modes are measured, and both must respect the S0
# memory budget: the tier run shows the default experience, the off run keeps
# the original full-frame rule covered.
function Measure-Run([string]$Label, [string[]]$Extra, [string]$Screenshot) {
    if (Test-Path $Screenshot) { Remove-Item $Screenshot -Force }
    $offset = 0
    if (Test-Path $logPath) { $offset = (Get-Item $logPath).Length }
    $clock = [System.Diagnostics.Stopwatch]::StartNew()
    $arguments = @("--screenshot `"$Screenshot`" --shot-delay $DwellMs") + $Extra + @("`"$Image8k`"")
    $process = Start-Process -FilePath $Exe -ArgumentList $arguments -PassThru
    $peakWs = 0
    $peakPrivate = 0
    while (-not $process.HasExited) {
        try {
            $process.Refresh()
            $ws = $process.WorkingSet64
            $priv = $process.PrivateMemorySize64
            if ($ws -gt $peakWs) { $peakWs = $ws }
            if ($priv -gt $peakPrivate) { $peakPrivate = $priv }
        } catch {
            break
        }
        Start-Sleep -Milliseconds 50
    }
    [void]$process.WaitForExit(10000)
    $clock.Stop()

    $runText = ''
    if (Test-Path $logPath) {
        $stream = [System.IO.File]::Open($logPath, 'Open', 'Read', 'ReadWrite')
        try {
            [void]$stream.Seek($offset, 'Begin')
            $reader = New-Object System.IO.StreamReader($stream)
            $runText = $reader.ReadToEnd()
            $reader.Dispose()
        } finally {
            $stream.Dispose()
        }
    }

    $shotOk = $false
    $shotWidth = 0
    $shotHeight = 0
    if (Test-Path $Screenshot) {
        $info = [System.Drawing.Image]::FromFile($Screenshot)
        try {
            $shotWidth = $info.Width
            $shotHeight = $info.Height
        } finally {
            $info.Dispose()
        }
        $shotOk = $true
    }

    $bitmap = [regex]::Match($runText, 'renderer: bitmap ([0-9x]+) bgra=([0-9.]+)MB')
    $memory = [regex]::Match($runText, 'memory: first_image ws_mb=([0-9.]+) private_mb=([0-9.]+) peak_ws_mb=([0-9.]+)')
    return @{
        label = $Label
        log = $runText
        exit = $process.ExitCode
        first_image = [regex]::Match($runText, 'startup: first_image=([0-9.]+)ms').Groups[1].Value
        bitmap = if ($bitmap.Success) { $bitmap.Groups[1].Value } else { '' }
        bitmap_mb = if ($bitmap.Success) { $bitmap.Groups[2].Value } else { '' }
        mem = $memory
        prefetch_skipped = [regex]::Matches($runText, 'prefetch skipped: current frame').Count
        prefetched = [regex]::Matches($runText, 'prefetched:').Count
        tier_hits = [regex]::Matches($runText, 'display cache hit:').Count
        shot_ok = $shotOk
        shot_w = $shotWidth
        shot_h = $shotHeight
        peak_ws_mb = [math]::Round($peakWs / 1MB, 1)
        peak_private_mb = [math]::Round($peakPrivate / 1MB, 1)
        total_ms = [math]::Round($clock.Elapsed.TotalMilliseconds)
    }
}

$runs = @()
$runs += Measure-Run 'default (tier)' @() $Out
$offShot = [System.IO.Path]::ChangeExtension($Out, '.off.png')
$runs += Measure-Run 'off (full frame)' @('--display-cache=off') $offShot

$text = $runs[0].log
$bitmapLine = [regex]::Match($text, 'renderer: bitmap ([0-9x]+) bgra=([0-9.]+)MB')
$memLine = $runs[0].mem
$firstImage = $runs[0].first_image
$p = @{ ExitCode = $runs[0].exit }

$cliSize = [regex]::Match(($probe -join "`n"), 'size:\s*(\d+x\d+)').Groups[1].Value
Write-Host ("cli_size={0} cli_decoded_bytes={1} cli_decode_ms={2}" -f $cliSize, $cliBytes, $cliDecodeMs)
foreach ($run in $runs) {
    Write-Host ("[{0}] app_exit={1} first_image_ms={2}" -f $run.label, $run.exit, $run.first_image)
    if ($run.bitmap) {
        Write-Host ("[{0}] bitmap={1} bitmap_mb={2}" -f $run.label, $run.bitmap, $run.bitmap_mb)
    }
    if ($run.mem.Success) {
        Write-Host ("[{0}] app_memory ws_mb={1} private_mb={2} peak_ws_mb={3}" -f $run.label, $run.mem.Groups[1].Value, $run.mem.Groups[2].Value, $run.mem.Groups[3].Value)
    }
    Write-Host ("[{0}] process_peak ws_mb={1} private_mb={2}" -f $run.label, $run.peak_ws_mb, $run.peak_private_mb)
    Write-Host ("[{0}] screenshot={1}x{2} prefetch_skipped={3} prefetched={4} tier_hits={5} total_ms={6}" -f $run.label, $run.shot_w, $run.shot_h, $run.prefetch_skipped, $run.prefetched, $run.tier_hits, $run.total_ms)
}
Write-Host ("peak_limit_ws={0}MB peak_limit_private={1}MB" -f $MaxPeakWsMB, $MaxPeakPrivateMB)

$failed = 0
foreach ($run in $runs) {
    if (-not $run.shot_ok) { Write-Host ("FAIL: [{0}] screenshot missing" -f $run.label); $failed++ }
    if ($run.bitmap -ne '7680x4320') {
        Write-Host ("FAIL: [{0}] expected bitmap 7680x4320, got '{1}'" -f $run.label, $run.bitmap); $failed++
    }
    if (-not $run.first_image) { Write-Host ("FAIL: [{0}] no first_image startup line" -f $run.label); $failed++ }
    if ($run.peak_ws_mb -gt $MaxPeakWsMB) {
        Write-Host ("FAIL: [{0}] peak working set {1}MB exceeds {2}MB" -f $run.label, $run.peak_ws_mb, $MaxPeakWsMB); $failed++
    }
    if ($run.peak_private_mb -gt $MaxPeakPrivateMB) {
        Write-Host ("FAIL: [{0}] peak private {1}MB exceeds {2}MB" -f $run.label, $run.peak_private_mb, $MaxPeakPrivateMB); $failed++
    }
}
# The >64 MB rule is a *full-frame* rule: with the tier chain off an 8K frame
# must not prefetch its neighbour at full resolution (two 127 MB frames would
# break the S0 budget), while the default tier prefetch is allowed and cheap.
if ($runs[1].prefetch_skipped -eq 0) {
    Write-Host 'FAIL: [off] expected prefetch to be skipped for an 8K frame'; $failed++
}
if ($runs[0].prefetched -eq 0 -and $runs[0].tier_hits -eq 0) {
    Write-Host 'FAIL: [default] expected a tier prefetch for the 8K neighbour'; $failed++
}
if ($failed -gt 0) { exit 1 }
Write-Host 'OK'
