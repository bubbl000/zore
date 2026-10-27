#requires -Version 5.1
<#
.SYNOPSIS
    M1-5 orientation check: EXIF 1..8 is baked into the decoded pixels.

.DESCRIPTION
    - CLI --probe: upright dimensions, orientation tag and applied flag.
    - CLI --decode: compares sampled pixels of orient_2..8 with the CPU
      reference mapping of orient_1 (the files differ only in their text glyph,
      which is excluded from sampling).
    - CLI --thumb: 90-degree thumbnails swap width/height.
    - App: opens orient_6, writes a screenshot and logs the upright 1000x1600
      bitmap plus the orientation decision.

.EXAMPLE
    powershell -ExecutionPolicy Bypass -File tests\run_orientation_check.ps1
#>
param(
    [string]$Cli = (Join-Path (Split-Path -Parent $PSScriptRoot) 'build\Release\zoreview_cli.exe'),
    [string]$Exe = (Join-Path (Split-Path -Parent $PSScriptRoot) 'build\Release\zoreview.exe'),
    [string]$ImageDir = (Join-Path (Split-Path -Parent (Split-Path -Parent $PSScriptRoot)) '测试文件夹\images\jpeg'),
    [string]$Out = (Join-Path $PSScriptRoot 'output\orientation-screen.png')
)

$ErrorActionPreference = 'Stop'
if (-not (Test-Path -LiteralPath $Cli)) { throw "CLI not found: $Cli" }
if (-not (Test-Path -LiteralPath $Exe)) { throw "exe not found: $Exe" }

$tmp = Join-Path $env:TEMP 'nv-orientation-check'
New-Item -ItemType Directory -Force -Path $tmp | Out-Null
$failures = 0
function Fail([string]$message) { Write-Host "FAIL: $message"; $script:failures++ }

function Parse-Probe([string[]]$lines) {
    $map = @{}
    foreach ($line in $lines) {
        $s = [string]$line
        $i = $s.IndexOf(':')
        if ($i -gt 0) { $map[$s.Substring(0, $i).Trim()] = $s.Substring($i + 1).Trim() }
    }
    return $map
}

function Get-Pixel([byte[]]$bytes, [int]$width, [int]$x, [int]$y) {
    $i = (($y * $width) + $x) * 4
    return @([int]$bytes[$i], [int]$bytes[$i + 1], [int]$bytes[$i + 2], [int]$bytes[$i + 3])
}

# Destination -> source mapping, matching src/core/image_transform.cpp.
function Map-Source([int]$orientation, [int]$x, [int]$y, [int]$w, [int]$h) {
    switch ($orientation) {
        2 { return @(($w - 1 - $x), $y) }
        3 { return @(($w - 1 - $x), ($h - 1 - $y)) }
        4 { return @($x, ($h - 1 - $y)) }
        5 { return @($y, $x) }
        6 { return @($y, ($h - 1 - $x)) }
        7 { return @(($w - 1 - $y), ($h - 1 - $x)) }
        8 { return @(($w - 1 - $y), $x) }
        default { return @($x, $y) }
    }
}

$width = 1600
$height = 1000
$reference = $null
$decoded = @{}

foreach ($orientation in 1..8) {
    $file = Join-Path $ImageDir "orient_$orientation.jpg"
    if (-not (Test-Path -LiteralPath $file)) { throw "sample missing: $file" }

    $probe = Parse-Probe (& $Cli --probe $file 2>&1)
    $swaps = $orientation -ge 5
    $expected = if ($swaps) { "$height`x$width" } else { "$width`x$height" }
    if ($probe['size'] -ne $expected) {
        Fail "probe orient_$orientation size=$($probe['size']) expected $expected"
    }
    if ($probe['orientation'] -ne "$orientation") {
        Fail "probe orient_$orientation tag=$($probe['orientation'])"
    }
    $applied = if ($orientation -eq 1) { 'no' } else { 'yes' }
    if ($probe['orientation_applied'] -ne $applied) {
        Fail "probe orient_$orientation applied=$($probe['orientation_applied']) expected $applied"
    }

    $raw = Join-Path $tmp "orient_$orientation.bgra"
    $decode = & $Cli --decode $file --out $raw 2>&1
    if ($LASTEXITCODE -ne 0) { Fail "decode orient_$orientation"; continue }
    $decoded[$orientation] = [System.IO.File]::ReadAllBytes($raw)
    if ($orientation -eq 1) { $reference = $decoded[1] }

    $decodeMap = Parse-Probe $decode
    if ($decodeMap['icc'] -ne 'none') {
        Fail "orient_$orientation unexpectedly reports ICC $($decodeMap['icc'])"
    }
}

# Pixel mapping: sample a grid in destination space and compare against the
# reference orientation-1 pixels. The generated files draw a different
# "ORIENT N" glyph, so the source text box is skipped.
foreach ($orientation in 2..8) {
    $bytes = $decoded[$orientation]
    $swaps = $orientation -ge 5
    $dw = if ($swaps) { $height } else { $width }
    $dh = if ($swaps) { $width } else { $height }
    $checked = 0
    for ($gy = 0; $gy -lt 11; $gy++) {
        for ($gx = 0; $gx -lt 13; $gx++) {
            $x = [int]($gx * ($dw - 1) / 12)
            $y = [int]($gy * ($dh - 1) / 10)
            $source = Map-Source $orientation $x $y $width $height
            $sx = $source[0]
            $sy = $source[1]
            if ($sx -ge 55 -and $sx -le 130 -and $sy -ge 5 -and $sy -le 60) { continue }
            $actual = Get-Pixel $bytes $dw $x $y
            $expectedPixel = Get-Pixel $reference $width $sx $sy
            for ($c = 0; $c -lt 4; $c++) {
                if ($actual[$c] -ne $expectedPixel[$c]) {
                    Fail "orient_$orientation pixel ($x,$y) channel $c = $($actual[$c]) expected $($expectedPixel[$c])"
                    break
                }
            }
            $checked++
        }
    }
    if ($checked -lt 100) { Fail "orient_$orientation sampled only $checked pixels" }
}

# Thumbnails: 90-degree output must swap the 128-pixel long edge.
$thumb1 = Parse-Probe (& $Cli --thumb (Join-Path $ImageDir 'orient_1.jpg') --edge 128 2>&1)
if ($thumb1['size'] -ne '128x80') { Fail "thumb orient_1 size=$($thumb1['size'])" }
$thumb6 = Parse-Probe (& $Cli --thumb (Join-Path $ImageDir 'orient_6.jpg') --edge 128 2>&1)
if ($thumb6['size'] -ne '80x128') { Fail "thumb orient_6 size=$($thumb6['size'])" }
if ($thumb6['orientation_applied'] -ne 'yes') { Fail 'thumb orient_6 did not apply orientation' }

# App path: the renderer must receive the upright 1000x1600 frame.
$logPath = Join-Path $env:LOCALAPPDATA 'ZoreView\logs\zoreview.log'
$logOffset = 0
if (Test-Path -LiteralPath $logPath) { $logOffset = (Get-Item -LiteralPath $logPath).Length }
$outDir = Split-Path -Parent $Out
New-Item -ItemType Directory -Force -Path $outDir | Out-Null
if (Test-Path -LiteralPath $Out) { Remove-Item -LiteralPath $Out -Force }
$app = Start-Process -FilePath $Exe -ArgumentList "--screenshot `"$Out`" --shot-delay 1500 `"$(Join-Path $ImageDir 'orient_6.jpg')`"" -PassThru
if (-not $app.WaitForExit(20000)) { $app.Kill(); throw 'app did not exit' }
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
if ($text -notmatch 'renderer: bitmap 1000x1600') { Fail 'renderer did not create a 1000x1600 bitmap' }
if ($text -notmatch 'wic: orientation=6 applied=1' -and $text -notmatch 'disk cache hit: .*orient_6') {
    Fail 'decode log missing orientation=6 applied=1'
}
$screenOk = $false
if (Test-Path -LiteralPath $Out) {
    Add-Type -AssemblyName System.Drawing
    $info = [System.Drawing.Image]::FromFile($Out)
    try { $screenOk = $info.Width -gt 0 -and $info.Height -gt 0 } finally { $info.Dispose() }
}
if (-not $screenOk) { Fail 'screenshot missing' }

Write-Host "orientation_samples=8 thumbnail_swap=ok gui_orientation=6 screenshot=$Out"
if ($failures -gt 0) { Write-Host "FAIL: $failures check(s)"; exit 1 }
Write-Host 'OK'
