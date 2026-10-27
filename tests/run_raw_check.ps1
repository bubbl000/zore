#requires -Version 5.1
<#
.SYNOPSIS
    M2 RAW check: LibRaw probe + thumbnail decode for every sample, one full
    decode, and a GUI screenshot of the first RAW.

.DESCRIPTION
    - CLI --probe: decoder must be "libraw", dimensions/orientation valid.
    - CLI --thumb --edge 128: half-size decode + resize; "full:" must match the
      probe dimensions.
    - CLI --decode (first file by default, -FullAll for every file): the BGRA
      byte count must equal width*height*4.
    - App --screenshot: renderer bitmap dimensions must match the RAW.

    Samples live under 测试文件夹\raw (10 camera files).

.EXAMPLE
    powershell -ExecutionPolicy Bypass -File tests\run_raw_check.ps1
    powershell -ExecutionPolicy Bypass -File tests\run_raw_check.ps1 -FullAll
#>
param(
    [string]$Exe = (Join-Path (Split-Path -Parent $PSScriptRoot) 'build\Release\zoreview.exe'),
    [string]$Cli = (Join-Path (Split-Path -Parent $PSScriptRoot) 'build\Release\zoreview_cli.exe'),
    [string]$Root = (Join-Path (Split-Path -Parent (Split-Path -Parent $PSScriptRoot)) '测试文件夹\raw'),
    [string]$Out = (Join-Path $PSScriptRoot 'output\raw-screen.png'),
    [switch]$FullAll
)

$ErrorActionPreference = 'Continue'
if (-not (Test-Path -LiteralPath $Exe)) { throw "exe not found: $Exe" }
if (-not (Test-Path -LiteralPath $Cli)) { throw "CLI not found: $Cli" }
if (-not (Test-Path -LiteralPath $Root)) { throw "RAW root not found: $Root" }

$extensions = @('.cr2', '.cr3', '.nef', '.arw', '.raf', '.orf', '.rw2', '.pef', '.srw', '.dng')
$files = Get-ChildItem -LiteralPath $Root -Recurse -File |
    Where-Object { $extensions -contains $_.Extension.ToLowerInvariant() } |
    Sort-Object FullName
if ($files.Count -eq 0) { throw "no RAW samples under $Root" }

$failures = 0
function Fail([string]$message) { Write-Host "FAIL: $message"; $script:failures++ }

function Parse-Output([string[]]$lines) {
    $map = @{}
    foreach ($line in $lines) {
        $s = [string]$line
        $i = $s.IndexOf(':')
        if ($i -gt 0) { $map[$s.Substring(0, $i).Trim()] = $s.Substring($i + 1).Trim() }
    }
    return $map
}

$tmp = Join-Path $env:TEMP 'nv-raw-check'
New-Item -ItemType Directory -Force -Path $tmp | Out-Null
$decoded = 0
$first = $null

foreach ($file in $files) {
    $probe = Parse-Output (& $Cli --probe $file.FullName 2>$null)
    if ($probe['decoder'] -ne 'libraw') {
        Fail "$($file.Name) probe decoder=$($probe['decoder'])"
        continue
    }
    $size = $probe['size']
    if ($size -match '^(\d+)x(\d+)$') {
        $width = [int]$Matches[1]
        $height = [int]$Matches[2]
    } else {
        Fail "$($file.Name) bad size '$size'"
        continue
    }
    $orientation = [int]$probe['orientation']
    if ($width -le 0 -or $height -le 0) { Fail "$($file.Name) empty dimensions" }
    if ($orientation -lt 1 -or $orientation -gt 8) { Fail "$($file.Name) orientation=$orientation" }
    if ($null -eq $first) { $first = @{ File = $file; Width = $width; Height = $height } }

    $thumb = Parse-Output (& $Cli --thumb $file.FullName --edge 128 2>$null)
    if ($LASTEXITCODE -ne 0) { Fail "$($file.Name) thumb exit=$LASTEXITCODE"; continue }
    if ($thumb['decoder'] -ne 'libraw_half') { Fail "$($file.Name) thumb decoder=$($thumb['decoder'])" }
    if ($thumb['full'] -ne $size) { Fail "$($file.Name) thumb full=$($thumb['full']) expected $size" }
    if ($thumb['size'] -match '^(\d+)x(\d+)$') {
        $tw = [int]$Matches[1]
        $th = [int]$Matches[2]
        if ([math]::Max($tw, $th) -ne 128) { Fail "$($file.Name) thumb edge=$tw`x$th" }
    } else {
        Fail "$($file.Name) thumb size '$($thumb['size'])'"
    }

    if ($FullAll -or $decoded -eq 0) {
        $raw = Join-Path $tmp 'raw.bgra'
        $decode = Parse-Output (& $Cli --decode $file.FullName --out $raw 2>$null)
        if ($LASTEXITCODE -ne 0) {
            Fail "$($file.Name) decode exit=$LASTEXITCODE"
        } else {
            $expected = [int64]$width * $height * 4
            if ([int64]$decode['bytes'] -ne $expected) {
                Fail "$($file.Name) decoded bytes=$($decode['bytes']) expected $expected"
            }
            if ((Get-Item -LiteralPath $raw).Length -ne $expected) {
                Fail "$($file.Name) output file size mismatch"
            }
        }
        Remove-Item -LiteralPath $raw -Force -ErrorAction SilentlyContinue
        $decoded++
    }
}

# GUI path: the renderer must receive the upright/sensor-sized bitmap.
if ($first -ne $null) {
    $logPath = Join-Path $env:LOCALAPPDATA 'ZoreView\logs\zoreview.log'
    $logOffset = 0
    if (Test-Path -LiteralPath $logPath) { $logOffset = (Get-Item -LiteralPath $logPath).Length }
    $outDir = Split-Path -Parent $Out
    New-Item -ItemType Directory -Force -Path $outDir | Out-Null
    if (Test-Path -LiteralPath $Out) { Remove-Item -LiteralPath $Out -Force }
    $arguments = "--screenshot `"$Out`" --shot-delay 5000 `"$($first.File.FullName)`""
    $process = Start-Process -FilePath $Exe -ArgumentList $arguments -PassThru
    if (-not $process.WaitForExit(40000)) { $process.Kill(); Fail 'GUI did not exit' }
    elseif ($process.ExitCode -ne 0) { Fail "GUI exit=$($process.ExitCode)" }

    $text = ''
    if (Test-Path -LiteralPath $logPath) {
        $stream = [System.IO.File]::Open($logPath, 'Open', 'Read', 'ReadWrite')
        try {
            [void]$stream.Seek($logOffset, 'Begin')
            $reader = New-Object System.IO.StreamReader($stream)
            $text = $reader.ReadToEnd()
            $reader.Dispose()
        } finally { $stream.Dispose() }
    }
    # V3.1-04a/V3.1-11: at fit no RAW may upload the full sensor frame. The frame
    # is either the camera-embedded JPEG shown as a *placeholder*
    # ("raw: preview as placeholder") followed by a LibRaw half-size decode, or
    # (H.265-only CR3s) straight to the half-size request. Both must show a
    # picture and complete the load.
    $full = [regex]::Escape("renderer: bitmap $($first.Width)x$($first.Height)")
    $placeholder = $text -match 'raw: preview as placeholder edge='
    $halfSize = $text -match 'raw: half-size request|raw: preview too small'
    if ($text -match $full) {
        Fail "the fit view uploaded the full frame ($($first.Width)x$($first.Height))"
    }
    if ($text -notmatch 'preview: shown|renderer: bitmap') {
        Fail 'no frame reached the screen'
    }
    if ($text -notmatch 'load: done ms=') { Fail 'the load never completed' }
    if ($placeholder) {
        if ($text -notmatch 'preview: shown source=embedded') {
            Fail 'GUI log missing the embedded RAW preview frame'
        }
        if (-not $halfSize) {
            Fail 'the placeholder was never followed by a LibRaw half-size decode'
        }
    } elseif (-not $halfSize) {
        Fail 'neither the preview-placeholder nor the half-size path ran'
    }
    if (-not (Test-Path -LiteralPath $Out)) { Fail 'GUI screenshot missing' }
}

Write-Host "raw_files=$($files.Count) full_decodes=$decoded"
if ($failures -gt 0) { Write-Host "FAIL: $failures check(s)"; exit 1 }
Write-Host 'OK'
