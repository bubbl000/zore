#requires -Version 5.1
<#
.SYNOPSIS
    M1-3 check: large folder (1000+ images) scan and thumbnail-strip virtualization.

.DESCRIPTION
    Opens an image in the middle of a large directory, optionally scrolls the
    strip, and asserts from the log that:
      - the folder scan completes quickly and lists every file;
      - only the visible window of thumbnails is requested (no full-folder walk);
      - no thumbnail fails;
      - the screenshot is written.

    Generate the folder first with tests\make_stress_samples.ps1.
#>
param(
    [string]$Folder = (Join-Path $PSScriptRoot 'output\bigfolder'),
    [string]$Exe = (Join-Path (Split-Path -Parent $PSScriptRoot) 'build\Release\zoreview.exe'),
    [string]$Out = (Join-Path $PSScriptRoot 'output\bigfolder-screen.png'),
    [int]$ScrollNotches = 40,
    [int]$DwellMs = 3500,
    [int]$MinFiles = 1000,
    [int]$MaxVisible = 64,
    [int]$MaxScanMs = 1000
)

$ErrorActionPreference = 'Stop'
if (-not (Test-Path $Folder)) { throw "folder not found: $Folder (run make_stress_samples.ps1)" }
if (-not (Test-Path $Exe)) { throw "exe not found: $Exe" }

$files = @(Get-ChildItem -LiteralPath $Folder -File | Where-Object { $_.Extension -match '^\.(jpg|jpeg|png|bmp|gif|webp|tif|tiff|ico)$' } | Sort-Object Name)
if ($files.Count -lt $MinFiles) { throw "folder has $($files.Count) images, need >= $MinFiles" }
$image = $files[[int][math]::Floor($files.Count / 2)].FullName

$logPath = Join-Path $env:LOCALAPPDATA 'ZoreView\logs\zoreview.log'
$logOffset = 0
if (Test-Path $logPath) { $logOffset = (Get-Item $logPath).Length }
if (Test-Path $Out) { Remove-Item $Out -Force }
Add-Type -AssemblyName System.Drawing

# --strip=pinned: virtualization is only observable while the rail is visible;
# the S2 default is auto (fade in on hover).
$arguments = "--strip=pinned --screenshot `"$Out`" --shot-delay $DwellMs"
if ($ScrollNotches -ne 0) { $arguments += " --scroll-notches $ScrollNotches" }
$arguments += " `"$image`""

$sw = [System.Diagnostics.Stopwatch]::StartNew()
$p = Start-Process -FilePath $Exe -ArgumentList $arguments -PassThru
if (-not $p.WaitForExit(20000)) {
    $p.Kill()
    throw "app did not exit within 20s"
}
$sw.Stop()

$text = ''
if (Test-Path $logPath) {
    $stream = [System.IO.File]::Open($logPath, 'Open', 'Read', 'ReadWrite')
    try {
        [void]$stream.Seek($logOffset, 'Begin')
        $reader = New-Object System.IO.StreamReader($stream)
        $text = $reader.ReadToEnd()
        $reader.Dispose()
    } finally {
        $stream.Dispose()
    }
}

$scan = [regex]::Match($text, 'folder:\s*(\d+) files in ([0-9.]+)ms')
$thumbs = [regex]::Matches($text, 'thumbs: visible=(\d+) requested=(\d+) scroll=(-?[0-9.]+)')
$failures = [regex]::Matches($text, 'thumbnail failed:').Count
$firstImage = [regex]::Match($text, 'startup: first_image=([0-9.]+)ms').Groups[1].Value

$maxVisible = 0
$maxRequested = 0
$maxScroll = 0.0
foreach ($m in $thumbs) {
    $v = [int]$m.Groups[1].Value
    $r = [int]$m.Groups[2].Value
    $s = [double]$m.Groups[3].Value
    if ($v -gt $maxVisible) { $maxVisible = $v }
    if ($r -gt $maxRequested) { $maxRequested = $r }
    if ($s -gt $maxScroll) { $maxScroll = $s }
}

$shot = Test-Path $Out
$shotInfo = ''
if ($shot) {
    $img = [System.Drawing.Image]::FromFile($Out)
    try { $shotInfo = "$($img.Width)x$($img.Height)" } finally { $img.Dispose() }
}

Write-Host ("image={0}" -f $image)
Write-Host ("files={0} scan_files={1} scan_ms={2}" -f $files.Count, $scan.Groups[1].Value, $scan.Groups[2].Value)
Write-Host ("thumb_request_batches={0} max_visible={1} max_requested={2} max_scroll={3}" -f $thumbs.Count, $maxVisible, $maxRequested, $maxScroll)
Write-Host ("thumbnail_failures={0} first_image_ms={1}" -f $failures, $firstImage)
Write-Host ("screenshot={0} size={1} total_ms={2}" -f $shot, $shotInfo, [math]::Round($sw.Elapsed.TotalMilliseconds))

$failed = 0
if (-not $scan.Success) { Write-Host 'FAIL: no folder scan log line'; $failed++ }
elseif ([int]$scan.Groups[1].Value -ne $files.Count) {
    Write-Host "FAIL: scan listed $($scan.Groups[1].Value) of $($files.Count) files"; $failed++
}
elseif ([double]$scan.Groups[2].Value -gt $MaxScanMs) {
    Write-Host "FAIL: folder scan took $($scan.Groups[2].Value)ms (limit ${MaxScanMs}ms)"; $failed++
}
if ($thumbs.Count -eq 0) { Write-Host 'FAIL: no thumbnail request log lines'; $failed++ }
if ($maxVisible -gt $MaxVisible) {
    Write-Host "FAIL: visible thumbnails $maxVisible exceeded $MaxVisible"; $failed++
}
if ($maxRequested -gt $maxVisible) {
    Write-Host "FAIL: requested $maxRequested more than visible $maxVisible"; $failed++
}
if ($failures -gt 0) { Write-Host "FAIL: $failures thumbnails failed"; $failed++ }
if (-not $shot) { Write-Host 'FAIL: screenshot missing'; $failed++ }

if ($failed -gt 0) { exit 1 }
Write-Host 'OK'
