#requires -Version 5.1
<#
.SYNOPSIS
    Probe every test image with zoreview_cli and write a CSV matrix.

.EXAMPLE
    powershell -ExecutionPolicy Bypass -File tests\run_format_matrix.ps1

.EXAMPLE
    powershell -ExecutionPolicy Bypass -File tests\run_format_matrix.ps1 -SkipRaw
#>
param(
    [string]$Cli = (Join-Path (Split-Path -Parent $PSScriptRoot) 'build\Release\zoreview_cli.exe'),
    [string]$Root = (Join-Path (Split-Path -Parent (Split-Path -Parent $PSScriptRoot)) '测试文件夹'),
    [string]$Out = (Join-Path $PSScriptRoot 'output\format-matrix.csv'),
    [switch]$SkipRaw
)

$ErrorActionPreference = 'Continue'

if (-not (Test-Path -LiteralPath $Cli)) {
    throw "CLI not found: $Cli (build first: tools\build.ps1)"
}
if (-not (Test-Path -LiteralPath $Root)) {
    throw "Test root not found: $Root"
}

$extensions = @('.jpg', '.jpeg', '.png', '.bmp', '.gif', '.webp', '.tif', '.tiff', '.ico', '.cur', '.psd', '.psb', '.heic', '.heif', '.hif', '.avif', '.avifs', '.svg', '.tga', '.targa', '.pcx', '.pbm', '.pgm', '.ppm', '.pnm', '.xbm', '.xpm', '.wbmp', '.hdr', '.exr', '.pfm', '.jp2', '.j2k', '.jpc', '.jxr', '.wdp', '.mng', '.jng')
if (-not $SkipRaw) {
    $extensions += @('.cr2', '.cr3', '.nef', '.arw', '.raf', '.orf', '.rw2', '.pef', '.srw', '.dng')
}

$files = Get-ChildItem -LiteralPath $Root -Recurse -File |
    Where-Object { $extensions -contains $_.Extension.ToLowerInvariant() } |
    Sort-Object FullName

if ($files.Count -eq 0) {
    throw "No test images found under $Root"
}

$rows = New-Object System.Collections.Generic.List[object]
$ok = 0
$failed = 0
$expectedFailed = 0

foreach ($file in $files) {
    $relative = $file.FullName.Substring($Root.Length).TrimStart('\')
    # Corrupt samples have valid-looking headers, so probe is allowed to
    # succeed; only the pixel payload must be rejected by --decode.
    $isCorrupt = $relative -match '(^|\\)corrupt(\\)'
    $arguments = if ($isCorrupt) { @('--decode', $file.FullName) } else { @('--probe', $file.FullName) }
    $text = & $Cli @arguments 2>&1
    $exit = $LASTEXITCODE
    $map = @{}
    foreach ($line in $text) {
        $s = [string]$line
        $i = $s.IndexOf(':')
        if ($i -gt 0) {
            $key = $s.Substring(0, $i).Trim()
            $value = $s.Substring($i + 1).Trim()
            $map[$key] = $value
        }
    }

    $errorText = if ($exit -ne 0) { ($text -join ' ').Trim() } else { '' }
    $row = [pscustomobject]@{
        file = $relative
        format = $(if ($map.ContainsKey('format')) { $map['format'] } else { '' })
        width = $(if ($map.ContainsKey('size')) { ($map['size'] -split 'x')[0] } else { '' })
        height = $(if ($map.ContainsKey('size')) { ($map['size'] -split 'x')[1] } else { '' })
        alpha = $(if ($map.ContainsKey('alpha')) { $map['alpha'] } else { '' })
        decoder = $(if ($map.ContainsKey('decoder')) { $map['decoder'] } else { '' })
        probe_ms = $(if ($map.ContainsKey('probe_ms')) { $map['probe_ms'] } else { '' })
        expected = $(if ($isCorrupt) { 'fail' } else { 'ok' })
        exit = $exit
        error = $errorText
    }
    $rows.Add($row)

    if ($isCorrupt) {
        if ($exit -ne 0) { $ok++; $expectedFailed++ } else { $failed++ }
    } elseif ($exit -eq 0) {
        $ok++
    } else {
        $failed++
    }
}

$outDir = Split-Path -Parent $Out
New-Item -ItemType Directory -Force -Path $outDir | Out-Null
$rows | Export-Csv -LiteralPath $Out -NoTypeInformation -Encoding UTF8

Write-Host "files=$($files.Count) ok=$ok failed=$failed expected_failed=$expectedFailed"
Write-Host "matrix=$Out"
if ($failed -gt 0) { exit 1 }
