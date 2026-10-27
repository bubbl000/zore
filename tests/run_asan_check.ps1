#requires -Version 5.1
<#
.SYNOPSIS
    M1-4: run the AddressSanitizer build over the CLI decode paths and a GUI
    next-image soak, then fail if any ASan report was produced.

.DESCRIPTION
    Requires the ASan build produced by:
        powershell -ExecutionPolicy Bypass -File tools\build.ps1 -ASan

    ASan reports are written to tests\output\asan\ through
    ASAN_OPTIONS=log_path=... because a GUI process's stderr is not captured.

.EXAMPLE
    powershell -ExecutionPolicy Bypass -File tests\run_asan_check.ps1

.EXAMPLE
    powershell -ExecutionPolicy Bypass -File tests\run_asan_check.ps1 -Iterations 50 -SkipGui
#>
param(
    [int]$Iterations = 200,
    [int]$DelayMs = 80,
    [string]$BuildDir = 'build-asan',
    [string]$Config = 'RelWithDebInfo',
    [string]$Folder = 'tests\output\bigfolder',
    [switch]$SkipCli,
    [switch]$SkipGui
)

$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot
$bin = Join-Path $root (Join-Path $BuildDir $Config)
$gui = Join-Path $bin 'zoreview.exe'
$cli = Join-Path $bin 'zoreview_cli.exe'
$imagesRoot = Join-Path (Split-Path -Parent $root) '测试文件夹\images'
$rawRoot = Join-Path (Split-Path -Parent $root) '测试文件夹\raw'

if (-not (Test-Path -LiteralPath $gui)) {
    throw "ASan GUI exe not found: $gui (run tools\build.ps1 -ASan)"
}
if (-not (Test-Path -LiteralPath (Join-Path $bin 'clang_rt.asan_dynamic-x86_64.dll'))) {
    throw "AddressSanitizer runtime DLL is missing next to $gui"
}

$asanDir = Join-Path $root 'tests\output\asan'
if (Test-Path -LiteralPath $asanDir) {
    Remove-Item -LiteralPath $asanDir -Recurse -Force
}
New-Item -ItemType Directory -Force -Path $asanDir | Out-Null

# log_path is relative to the working directory; an absolute path would be
# split on its drive-letter colon by the ASan option parser.
$asanLog = Join-Path 'tests\output\asan' 'asan'
$env:ASAN_OPTIONS = "log_path=$asanLog"

$failures = New-Object System.Collections.Generic.List[string]
$cliRuns = 0
$guiRuns = 0

function Get-AsanReports {
    if (-not (Test-Path -LiteralPath $asanDir)) { return @() }
    Get-ChildItem -LiteralPath $asanDir -File | Where-Object {
        $_.Length -gt 0 -and $_.Name -like 'asan.*'
    } | ForEach-Object {
        $hits = Select-String -LiteralPath $_.FullName -Pattern 'AddressSanitizer|ERROR:'
        if ($hits) { $_.FullName }
    }
}

if (-not $SkipCli) {
    # Native stderr is an error record under 'Stop'; the corrupt-sample checks
    # below expect nonzero exits, so keep collecting instead of aborting.
    $previousErrorAction = $ErrorActionPreference
    $ErrorActionPreference = 'Continue'
    $extensions = @('.jpg', '.jpeg', '.png', '.bmp', '.gif', '.webp', '.tif', '.tiff', '.ico', '.psd', '.psb', '.heic', '.heif', '.avif', '.svg', '.tga', '.targa', '.pcx', '.pbm', '.pgm', '.ppm', '.pnm', '.xbm', '.xpm', '.wbmp', '.hdr', '.exr', '.pfm', '.jp2', '.j2k', '.jpc', '.jxr', '.wdp', '.mng', '.jng')
    $samples = Get-ChildItem -LiteralPath $imagesRoot -Recurse -File |
        Where-Object { $extensions -contains $_.Extension.ToLowerInvariant() } |
        Where-Object { $_.FullName -notmatch '(^|\\)corrupt(\\|$)' } |
        Sort-Object FullName
    Write-Host "ASan CLI: $($samples.Count) samples from $imagesRoot"
    $out = Join-Path $asanDir 'decode.bgra'
    foreach ($sample in $samples) {
        Push-Location $root
        try {
            & $cli --probe $sample.FullName *> $null
            if ($LASTEXITCODE -ne 0) { $failures.Add("probe $($sample.Name) exit=$LASTEXITCODE") }
            & $cli --decode $sample.FullName --out $out *> $null
            if ($LASTEXITCODE -ne 0) { $failures.Add("decode $($sample.Name) exit=$LASTEXITCODE") }
            & $cli --thumb $sample.FullName --edge 128 *> $null
            if ($LASTEXITCODE -ne 0) { $failures.Add("thumb $($sample.Name) exit=$LASTEXITCODE") }
            $cliRuns += 3
        } finally {
            Pop-Location
            if (Test-Path -LiteralPath $out) { Remove-Item -LiteralPath $out -Force }
        }
    }

    # Corrupt samples must fail cleanly (no crash, no ASan report). A nonzero
    # exit is the expected result here.
    $corruptSamples = Get-ChildItem -LiteralPath $imagesRoot -Recurse -File |
        Where-Object { $_.FullName -match '(^|\\)corrupt(\\|$)' } |
        Where-Object { $extensions -contains $_.Extension.ToLowerInvariant() } |
        Sort-Object FullName
    foreach ($sample in $corruptSamples) {
        Push-Location $root
        try {
            & $cli --decode $sample.FullName --out $out *> $null
            if ($LASTEXITCODE -eq 0) { $failures.Add("corrupt $($sample.Name) unexpectedly decoded") }
            $cliRuns++
        } finally {
            Pop-Location
            if (Test-Path -LiteralPath $out) { Remove-Item -LiteralPath $out -Force }
        }
    }
    Write-Host "ASan CLI corrupt: $($corruptSamples.Count) samples"

    # RAW: all samples probe + half-size thumbnail, one full decode. LibRaw
    # warnings go to stderr, so do not let PowerShell treat them as fatal.
    if (Test-Path -LiteralPath $rawRoot) {
        $rawExtensions = @('.cr2', '.cr3', '.nef', '.arw', '.raf', '.orf', '.rw2', '.pef', '.srw', '.dng')
        $rawSamples = Get-ChildItem -LiteralPath $rawRoot -Recurse -File |
            Where-Object { $rawExtensions -contains $_.Extension.ToLowerInvariant() } |
            Sort-Object FullName
        Write-Host "ASan CLI RAW: $($rawSamples.Count) samples from $rawRoot"
        $rawOut = Join-Path $asanDir 'raw.bgra'
        $previous = $ErrorActionPreference
        $ErrorActionPreference = 'Continue'
        try {
            $rawFull = $true
            foreach ($raw in $rawSamples) {
                Push-Location $root
                try {
                    & $cli --probe $raw.FullName *> $null
                    if ($LASTEXITCODE -ne 0) { $failures.Add("raw probe $($raw.Name) exit=$LASTEXITCODE") }
                    & $cli --thumb $raw.FullName --edge 128 *> $null
                    if ($LASTEXITCODE -ne 0) { $failures.Add("raw thumb $($raw.Name) exit=$LASTEXITCODE") }
                    $cliRuns += 2
                    if ($rawFull) {
                        & $cli --decode $raw.FullName --out $rawOut *> $null
                        if ($LASTEXITCODE -ne 0) { $failures.Add("raw decode $($raw.Name) exit=$LASTEXITCODE") }
                        $cliRuns++
                        $rawFull = $false
                    }
                } finally {
                    Pop-Location
                    if (Test-Path -LiteralPath $rawOut) { Remove-Item -LiteralPath $rawOut -Force }
                }
            }
        } finally {
            $ErrorActionPreference = $previous
        }
    }
    $ErrorActionPreference = $previousErrorAction
}

Add-Type @'
using System;
using System.Runtime.InteropServices;
public static class NvAsanInput {
  [DllImport("user32.dll")] public static extern bool PostMessage(IntPtr hWnd, uint msg, UIntPtr wParam, IntPtr lParam);
}
'@

function Start-GuiSoak([string]$image, [int]$keys, [string]$label) {
    $proc = Start-Process -FilePath $gui -ArgumentList "`"$image`"" `
        -WorkingDirectory $root -PassThru
    $script:guiRuns++
    $deadline = (Get-Date).AddSeconds(20)
    while ((Get-Date) -lt $deadline -and $proc.MainWindowHandle -eq 0) {
        Start-Sleep -Milliseconds 100
        $proc.Refresh()
    }
    if ($proc.MainWindowHandle -eq 0) {
        if (-not $proc.HasExited) { $proc.Kill(); $proc.WaitForExit(5000) }
        $failures.Add("$label no window")
        return
    }

    for ($i = 0; $i -lt $keys; $i++) {
        if ($proc.HasExited) {
            $failures.Add("$label died at iteration $i (exit=$($proc.ExitCode))")
            return
        }
        [void][NvAsanInput]::PostMessage($proc.MainWindowHandle, 0x0100,
            [UIntPtr]::new(0x27), [IntPtr]::Zero)
        Start-Sleep -Milliseconds $DelayMs
    }

    if (-not $proc.HasExited) {
        [void]$proc.CloseMainWindow()
        if (-not $proc.WaitForExit(20000)) {
            $proc.Kill()
            $proc.WaitForExit(5000)
            $failures.Add("$label did not exit after close")
        }
    }
    if ($proc.HasExited -and $proc.ExitCode -ne 0) {
        $failures.Add("$label exit=$($proc.ExitCode)")
    }
    Write-Host "ASan GUI: $label keys=$keys exit=$($proc.ExitCode)"
}

if (-not $SkipGui) {
    $soakFolder = Join-Path $root $Folder
    $first = Get-ChildItem -LiteralPath $soakFolder -File |
        Where-Object { $_.Extension -in '.jpg', '.png', '.bmp', '.gif', '.webp' } |
        Select-Object -First 1
    if ($null -eq $first) {
        throw "no image found for GUI soak in $soakFolder"
    }
    Write-Host "ASan GUI soak: $Iterations keys on $($first.Name)"
    Start-GuiSoak -image $first.FullName -keys $Iterations -label 'soak'

    $stress = Join-Path $root 'tests\output\8k.jpg'
    if (Test-Path -LiteralPath $stress) {
        Start-GuiSoak -image $stress -keys 4 -label '8k'
    }

    # Consecutive 8K frames exercise the bitmap-reuse (CopyFromMemory) path
    # added in M1-4 instead of recreating a 127 MB D2D bitmap every time.
    $stressFolder = Join-Path $root 'tests\output\8kfolder'
    if (Test-Path -LiteralPath $stressFolder) {
        $stressFirst = Get-ChildItem -LiteralPath $stressFolder -File |
            Where-Object { $_.Extension -eq '.jpg' } |
            Sort-Object Name |
            Select-Object -First 1
        if ($null -ne $stressFirst) {
            Start-GuiSoak -image $stressFirst.FullName -keys 8 -label '8k-nav'
        }
    }

    # M2: PSD alpha rendering + HEIC system-codec path under ASan.
    $psdFirst = Join-Path $imagesRoot 'psd\psd_rgba_rle.psd'
    if (Test-Path -LiteralPath $psdFirst) {
        Start-GuiSoak -image $psdFirst -keys 4 -label 'psd'
    }
    $heicFirst = Join-Path $imagesRoot 'heic\example.heic'
    if (Test-Path -LiteralPath $heicFirst) {
        Start-GuiSoak -image $heicFirst -keys 1 -label 'heic'
    }

    # Thumbnail click + screenshot path (covers OpenIndex, strip selection and
    # swapchain capture) under ASan.
    $clickShot = Join-Path $asanDir 'click.png'
    $click = Start-Process -FilePath $gui -WorkingDirectory $root -PassThru -Wait -ArgumentList `
        "--strip=pinned --screenshot `"$clickShot`" --shot-delay 3000 --click-index 10 `"$($first.FullName)`""
    $script:guiRuns++
    if ($click.ExitCode -ne 0) {
        $failures.Add("click-index exit=$($click.ExitCode)")
    }
    if (-not (Test-Path -LiteralPath $clickShot)) {
        $failures.Add('click-index screenshot missing')
    }
    Write-Host "ASan GUI: click-index exit=$($click.ExitCode)"

    if (Test-Path -LiteralPath $rawRoot) {
        $rawFirst = Get-ChildItem -LiteralPath $rawRoot -Recurse -File |
            Where-Object { $_.Extension.ToLowerInvariant() -in '.cr2', '.cr3', '.nef', '.arw', '.raf', '.orf', '.rw2', '.pef', '.srw', '.dng' } |
            Sort-Object FullName |
            Select-Object -First 1
        if ($null -ne $rawFirst) {
            $rawShot = Join-Path $asanDir 'raw.png'
            $previous = $ErrorActionPreference
            $ErrorActionPreference = 'Continue'
            try {
                $rawGui = Start-Process -FilePath $gui -WorkingDirectory $root -PassThru -Wait -ArgumentList `
                    "--screenshot `"$rawShot`" --shot-delay 7000 `"$($rawFirst.FullName)`""
                $script:guiRuns++
                if ($rawGui.ExitCode -ne 0) { $failures.Add("raw GUI exit=$($rawGui.ExitCode)") }
                if (-not (Test-Path -LiteralPath $rawShot)) { $failures.Add('raw GUI screenshot missing') }
                Write-Host "ASan GUI: raw exit=$($rawGui.ExitCode)"
            } finally {
                $ErrorActionPreference = $previous
            }
        }
    }
}

$reports = @(Get-AsanReports)
foreach ($report in $reports) {
    Write-Host "--- $report ---"
    Get-Content -LiteralPath $report | Select-Object -First 60
    $failures.Add("ASan report: $report")
}

Write-Host "cli_runs=$cliRuns gui_runs=$guiRuns asan_reports=$($reports.Count)"
if ($failures.Count -gt 0) {
    Write-Host "FAILURES:"
    $failures | ForEach-Object { Write-Host "  $_" }
    exit 1
}
Write-Host "asan check passed"
