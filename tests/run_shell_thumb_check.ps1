#requires -Version 5.1
<#
.SYNOPSIS
    M3-1 shell thumbnail checks: COM provider + Shell (IShellItemImageFactory).

.DESCRIPTION
    The harness registers ZoreViewThumb.dll under HKCU, exercises the
    IThumbnailProvider directly for every supported format family, then asks
    the Shell thumbnail cache for thumbnails at the four Explorer sizes.
    It unregisters the handler at the end (and in the finally block).

    Per-user registration sets DisableProcessIsolation=1 because the isolated
    thumbnail host does not resolve HKCU in-proc CLSIDs on Windows 11; the
    machine-wide installer will register under HKLM instead (isolation kept).

.EXAMPLE
    powershell -ExecutionPolicy Bypass -File tests\run_shell_thumb_check.ps1

.EXAMPLE
    powershell -ExecutionPolicy Bypass -File tests\run_shell_thumb_check.ps1 -KeepRegistered
#>
param(
    [string]$Exe = (Join-Path (Split-Path -Parent $PSScriptRoot) 'build\Release\zoreview_shelltests.exe'),
    [string]$Probe = (Join-Path (Split-Path -Parent $PSScriptRoot) 'build\Release\zoreview_shellprobe.exe'),
    [string]$Images = (Join-Path (Split-Path -Parent (Split-Path -Parent $PSScriptRoot)) '测试文件夹\images'),
    [switch]$KeepRegistered
)

$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot
if (-not (Test-Path -LiteralPath $Exe)) { throw "test harness not found: $Exe" }
$dll = Join-Path (Split-Path -Parent $Exe) 'ZoreViewThumb.dll'
if (-not (Test-Path -LiteralPath $dll)) { throw "thumbnail DLL not found: $dll" }

# The harness writes UTF-8; PowerShell 5.1 otherwise decodes native output with
# the OEM code page and mangles Chinese paths.
$previousOutput = [Console]::OutputEncoding
[Console]::OutputEncoding = [Text.Encoding]::UTF8

$clsid = '{7B2E5C1A-9F3D-4E8A-8C21-5D6F0A1B2C3D}'
$handler = '{E357FCCD-A995-4576-B01F-234630154E96}'
$failures = 0

function Unregister-Handler {
    if (Test-Path -LiteralPath $Exe) {
        & $Exe --unregister-only | Out-Null
    }
}

try {
    Push-Location $root
    try {
        & $Exe --images $Images
        if ($LASTEXITCODE -ne 0) { $failures++ }
    } finally {
        Pop-Location
    }

    # Fresh-process probe: this executable does not link ZoreViewThumb.dll,
    # so Shell has to discover and load the handler from the registry exactly
    # like Explorer. A new sample path avoids the Shell thumbnail cache.
    if (Test-Path -LiteralPath $Probe) {
        & $Exe --register-only
        $sampleSource = Join-Path $Images 'tga\photo.tga'
        $sample = Join-Path $env:TEMP ("nv-shell-probe-{0}.tga" -f ([guid]::NewGuid().ToString('N')))
        Copy-Item -LiteralPath $sampleSource -Destination $sample -Force
        try {
            Start-Sleep -Milliseconds 2500   # HKCR merged view refresh
            $probeOutput = & $Probe $sample 256 2>&1
            if ($LASTEXITCODE -ne 0) {
                Write-Host "FAIL: fresh-process Shell probe: $probeOutput"
                $failures++
            } else {
                Write-Host "OK   fresh-process Shell probe: $probeOutput"
            }
        } finally {
            Remove-Item -LiteralPath $sample -Force -ErrorAction SilentlyContinue
            if (-not $KeepRegistered) { Unregister-Handler }
        }
    } else {
        Write-Host "SKIP fresh-process Shell probe (missing $Probe)"
    }

    # Registration must be fully reversible.
    if (-not $KeepRegistered) {
        $clsidKey = "HKCU:\Software\Classes\CLSID\$clsid"
        if (Test-Path -LiteralPath $clsidKey) {
            Write-Host "FAIL: CLSID key still present after unregister"
            $failures++
        }
        $tgaKey = "HKCU:\Software\Classes\.tga\ShellEx\$handler"
        if (Test-Path -LiteralPath $tgaKey) {
            Write-Host "FAIL: .tga ShellEx value still present after unregister"
            $failures++
        }
        if (-not (Test-Path -LiteralPath 'HKCU:\Software\Classes\.tga')) {
            Write-Host 'OK   .tga key removed with the registration'
        }
    }
} finally {
    if (-not $KeepRegistered) { Unregister-Handler }
    [Console]::OutputEncoding = $previousOutput
}

if ($failures -gt 0) {
    Write-Host "FAIL: $failures shell-thumbnail check(s)"
    exit 1
}
Write-Host "OK: shell thumbnail provider and Explorer integration"
