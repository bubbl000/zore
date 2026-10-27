#requires -Version 5.1
<#
.SYNOPSIS
    Generates M1-3 stress samples: an 8K JPEG and a large directory of images.

.DESCRIPTION
    The 8K JPEG is made by upscaling the existing 4K sample with high-quality
    bicubic resampling, so it is a real 7680x4320 baseline JPEG. The large
    directory contains numbered copies of a small JPEG so folder scanning,
    sorting and thumbnail virtualization can be exercised without shipping
    hundreds of unique photos.

    Everything is written under tests/output/ (git-ignored).
#>
param(
    [string]$Source4k = (Join-Path $PSScriptRoot 'output\4k.jpg'),
    [string]$Source1600 = (Join-Path (Split-Path -Parent (Split-Path -Parent $PSScriptRoot)) '测试文件夹\images\jpeg\photo_1600.jpg'),
    [string]$SourceSmall = (Join-Path (Split-Path -Parent (Split-Path -Parent $PSScriptRoot)) '测试文件夹\images\jpeg\photo_420.jpg'),
    [string]$OutDir = (Join-Path $PSScriptRoot 'output'),
    [int]$FolderCount = 1200,
    [int]$EightKFolderCount = 4,
    [switch]$Skip8k,
    [switch]$SkipFolder,
    [switch]$Skip8kFolder
)

$ErrorActionPreference = 'Stop'
Add-Type -AssemblyName System.Drawing
New-Item -ItemType Directory -Force -Path $OutDir | Out-Null

function Save-Jpeg([System.Drawing.Bitmap]$Bitmap, [string]$Path, [long]$Quality) {
    $codec = [System.Drawing.Imaging.ImageCodecInfo]::GetImageEncoders() |
        Where-Object { $_.MimeType -eq 'image/jpeg' } | Select-Object -First 1
    $parameters = New-Object System.Drawing.Imaging.EncoderParameters(1)
    $parameters.Param[0] = New-Object System.Drawing.Imaging.EncoderParameter(
        [System.Drawing.Imaging.Encoder]::Quality, $Quality)
    try {
        $Bitmap.Save($Path, $codec, $parameters)
    } finally {
        $parameters.Dispose()
    }
}

# The 4K fixture used by run_perf.ps1 / run_startup_samples.ps1 lives in the
# git-ignored output directory, so build it from the checked-in 1600px sample
# when it is missing. That keeps a fresh checkout bootstrappable.
if (-not (Test-Path -LiteralPath $Source4k)) {
    if (-not (Test-Path -LiteralPath $Source1600)) {
        throw "no 4K fixture and no 1600px source to build one: $Source1600"
    }
    $sw = [System.Diagnostics.Stopwatch]::StartNew()
    $source = [System.Drawing.Image]::FromFile($Source1600)
    $bitmap = New-Object System.Drawing.Bitmap(3840, 2160)
    try {
        $graphics = [System.Drawing.Graphics]::FromImage($bitmap)
        try {
            $graphics.InterpolationMode = [System.Drawing.Drawing2D.InterpolationMode]::HighQualityBicubic
            $graphics.DrawImage($source, 0, 0, 3840, 2160)
        } finally {
            $graphics.Dispose()
        }
        Save-Jpeg $bitmap $Source4k 90
    } finally {
        $bitmap.Dispose()
        $source.Dispose()
    }
    $sw.Stop()
    Write-Host ("4k: $Source4k ({0} ms)" -f [math]::Round($sw.Elapsed.TotalMilliseconds))
}

if (-not $Skip8k) {
    $out8k = Join-Path $OutDir '8k.jpg'
    if (Test-Path -LiteralPath $Out8k) {
        Write-Host "8k: exists $out8k"
    } else {
        if (-not (Test-Path -LiteralPath $Source4k)) { throw "4K source not found: $Source4k" }
        $sw = [System.Diagnostics.Stopwatch]::StartNew()
        $source = [System.Drawing.Image]::FromFile($Source4k)
        $bitmap = New-Object System.Drawing.Bitmap(7680, 4320)
        try {
            $graphics = [System.Drawing.Graphics]::FromImage($bitmap)
            try {
                $graphics.InterpolationMode = [System.Drawing.Drawing2D.InterpolationMode]::HighQualityBicubic
                $graphics.PixelOffsetMode = [System.Drawing.Drawing2D.PixelOffsetMode]::HighQuality
                $graphics.DrawImage($source, 0, 0, 7680, 4320)
            } finally {
                $graphics.Dispose()
            }
            Save-Jpeg $bitmap $out8k 90
        } finally {
            $bitmap.Dispose()
            $source.Dispose()
        }
        $sw.Stop()
        $mb = [math]::Round((Get-Item -LiteralPath $out8k).Length / 1MB, 2)
        Write-Host ("8k: $out8k ({0} MB, {1} ms)" -f $mb, [math]::Round($sw.Elapsed.TotalMilliseconds))
    }
}

if (-not $SkipFolder) {
    $folder = Join-Path $OutDir 'bigfolder'
    if (-not (Test-Path -LiteralPath $SourceSmall)) { throw "Small source not found: $SourceSmall" }
    New-Item -ItemType Directory -Force -Path $folder | Out-Null
    $existing = (Get-ChildItem -LiteralPath $folder -File -Filter 'img_*.jpg' -ErrorAction SilentlyContinue | Measure-Object).Count
    if ($existing -ge $FolderCount) {
        Write-Host "folder: exists $folder ($existing files)"
    } else {
        $sw = [System.Diagnostics.Stopwatch]::StartNew()
        for ($i = 1; $i -le $FolderCount; $i++) {
            $name = 'img_{0:D4}.jpg' -f $i
            $target = Join-Path $folder $name
            if (-not (Test-Path -LiteralPath $target)) {
                Copy-Item -LiteralPath $SourceSmall -Destination $target
            }
        }
        $sw.Stop()
        Write-Host ("folder: $folder ($FolderCount files, {0} ms)" -f [math]::Round($sw.Elapsed.TotalMilliseconds))
    }
}

if (-not $Skip8kFolder) {
    $folder8k = Join-Path $OutDir '8kfolder'
    $out8k = Join-Path $OutDir '8k.jpg'
    if (Test-Path -LiteralPath $out8k) {
        New-Item -ItemType Directory -Force -Path $folder8k | Out-Null
        for ($i = 1; $i -le $EightKFolderCount; $i++) {
            $target = Join-Path $folder8k ('8k_{0}.jpg' -f $i)
            if (-not (Test-Path -LiteralPath $target)) {
                Copy-Item -LiteralPath $out8k -Destination $target
            }
        }
        Write-Host "8kfolder: $folder8k ($EightKFolderCount files)"
    }
}
