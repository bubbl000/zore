<#
.SYNOPSIS
    Build the 零式 / Zore installer (single Zore-<version>-setup.exe).

.DESCRIPTION
    Wraps Inno Setup 6. The script is compiled with Zore.iss in the repository
    root and the payload is taken from build\Release, so build the app first
    (tools\build.ps1 -Config Release).

    The wizard shows, in order: language picker (中文 / English) -> license
    (must be accepted) -> install folder -> options (associate image formats and
    become the default viewer, both checked) -> install -> finish.

.EXAMPLE
    powershell -ExecutionPolicy Bypass -File tools\make_installer.ps1
#>
param(
    [string]$Iscc = '',
    [string]$Configuration = 'Release'
)

$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot
Set-Location $root

function Find-Iscc {
    param([string]$Explicit)
    if ($Explicit -and (Test-Path $Explicit)) { return (Resolve-Path $Explicit).Path }
    if ($env:INNO_ISCC -and (Test-Path $env:INNO_ISCC)) { return $env:INNO_ISCC }
    $candidates = @(
        (Join-Path $env:LOCALAPPDATA 'Programs\Inno Setup 6\ISCC.exe'),
        'C:\Program Files (x86)\Inno Setup 6\ISCC.exe',
        'C:\Program Files\Inno Setup 6\ISCC.exe'
    )
    foreach ($candidate in $candidates) {
        if (Test-Path $candidate) { return $candidate }
    }
    $onPath = Get-Command ISCC.exe -ErrorAction SilentlyContinue
    if ($onPath) { return $onPath.Source }
    throw 'ISCC.exe (Inno Setup 6) not found. Install it or pass -Iscc <path>.'
}

$exe = Join-Path $root "build\$Configuration\zoreview.exe"
if (-not (Test-Path $exe)) {
    throw "Build the application first: $exe is missing (tools\build.ps1 -Config $Configuration)."
}

$isccPath = Find-Iscc -Explicit $Iscc
Write-Host "ISCC           : $isccPath"
Write-Host "application    : $exe"

& $isccPath 'Zore.iss'
if ($LASTEXITCODE -ne 0) {
    throw "ISCC failed with exit code $LASTEXITCODE"
}

$setup = Get-ChildItem (Join-Path $root 'dist') -Filter 'Zore-*-setup.exe' |
    Sort-Object LastWriteTime -Descending | Select-Object -First 1
if ($null -eq $setup) { throw 'No setup executable was produced.' }

$hash = (Get-FileHash $setup.FullName -Algorithm SHA256).Hash
Write-Host ''
Write-Host ("installer      : {0}" -f $setup.FullName)
Write-Host ("size           : {0:N1} MB" -f ($setup.Length / 1MB))
Write-Host ("sha256         : {0}" -f $hash)
