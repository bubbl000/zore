#requires -Version 5.1
<#
.SYNOPSIS
    UI check for the thumbnail strip: run the app in screenshot mode and inspect
    the PNG plus the "thumbnail ready" log lines.

.DESCRIPTION
    The app's --screenshot mode renders one frame straight from the swap chain,
    so the capture is independent of DPI virtualization, window focus and other
    windows on the desktop.

.EXAMPLE
    powershell -ExecutionPolicy Bypass -File tests\run_thumbnail_check.ps1
#>
param(
    [string]$Exe = (Join-Path (Split-Path -Parent $PSScriptRoot) 'build\Release\zoreview.exe'),
    [string]$Image = (Join-Path (Split-Path -Parent (Split-Path -Parent $PSScriptRoot)) '测试文件夹\images\jpeg\photo_1600.jpg'),
    [int]$DelayMs = 2500,
    [int]$MinThumbnails = 3,
    [int]$ClickIndex = -1,
    [int]$ScrollNotches = 0,
    [string]$Out = (Join-Path $PSScriptRoot 'output\thumb-strip.png')
)

$ErrorActionPreference = 'Stop'
if (-not (Test-Path -LiteralPath $Exe)) { throw "exe not found: $Exe" }
if (-not (Test-Path -LiteralPath $Image)) { throw "image not found: $Image" }

Add-Type -AssemblyName System.Drawing

$logPath = Join-Path $env:LOCALAPPDATA 'ZoreView\logs\zoreview.log'
$logOffset = 0
if (Test-Path -LiteralPath $logPath) {
    $logOffset = (Get-Item -LiteralPath $logPath).Length
}

$outDir = Split-Path -Parent $Out
New-Item -ItemType Directory -Force -Path $outDir | Out-Null
if (Test-Path -LiteralPath $Out) { Remove-Item -LiteralPath $Out -Force }

$extra = ''
if ($ClickIndex -ge 0) { $extra += " --click-index $ClickIndex" }
if ($ScrollNotches -ne 0) { $extra += " --scroll-notches $ScrollNotches" }
# --strip=pinned: the rail is auto (fade-in) by default since 22/08, but this
# check needs the previews visible without a hover.
$arguments = "--strip=pinned --screenshot `"$Out`" --shot-delay $DelayMs$extra `"$Image`""
$p = Start-Process -FilePath $Exe -ArgumentList $arguments -PassThru
if (-not $p.WaitForExit(20000)) {
    $p.Kill()
    throw "app did not exit within 20s"
}
Write-Host "exit_code=$($p.ExitCode)"

# Read only what this run appended to the log.
$text = ''
if (Test-Path -LiteralPath $logPath) {
    $stream = [System.IO.File]::Open($logPath, 'Open', 'Read', 'ReadWrite')
    try {
        [void]$stream.Seek($logOffset, 'Begin')
        $reader = New-Object System.IO.StreamReader($stream)
        $text = $reader.ReadToEnd()
    } finally {
        $stream.Dispose()
    }
}

$ready = ([regex]::Matches($text, 'thumbnail ready:')).Count
$failed = ([regex]::Matches($text, 'thumbnail failed:')).Count

$shot = $false
$width = 0
$height = 0
if (Test-Path -LiteralPath $Out) {
    $imageInfo = [System.Drawing.Image]::FromFile($Out)
    try {
        $width = $imageInfo.Width
        $height = $imageInfo.Height
    } finally {
        $imageInfo.Dispose()
    }
    $shot = $true
}

Write-Host "screenshot: ${width}x${height} saved=$shot"
Write-Host "thumbnails_ready=$ready thumbnails_failed=$failed"
Write-Host "png=$Out"
Write-Host "log=$logPath"

if (-not $shot) {
    Write-Host 'FAIL: screenshot was not written'
    exit 1
}
if ($ready -lt $MinThumbnails) {
    Write-Host "FAIL: expected at least $MinThumbnails thumbnails"
    exit 1
}
if ($failed -gt 0) {
    Write-Host "FAIL: $failed thumbnails failed"
    exit 1
}
Write-Host 'OK'
