#requires -Version 5.1
<#
.SYNOPSIS
    Configure and build ZoreView with the Visual Studio generator.

.DESCRIPTION
    Uses CMake + MSBuild from the installed Visual Studio. The Visual Studio
    generator is used (instead of Ninja) because it tracks header dependencies
    reliably on localized MSVC installs.

.EXAMPLE
    powershell -ExecutionPolicy Bypass -File tools\build.ps1

.EXAMPLE
    powershell -ExecutionPolicy Bypass -File tools\build.ps1 -Clean

.EXAMPLE
    # M1-4: AddressSanitizer build in build-asan (keeps Release untouched).
    # RelWithDebInfo uses the Release CRT so the prebuilt FreeImage links.
    powershell -ExecutionPolicy Bypass -File tools\build.ps1 -ASan

.EXAMPLE
    # M1-4: Debug CRT leak checking in build-debug (FreeImage is Release-only).
    powershell -ExecutionPolicy Bypass -File tools\build.ps1 -NoFreeImage
#>
param(
    [ValidateSet('Debug', 'Release', 'RelWithDebInfo')]
    [string]$Config = 'Release',
    [string]$Generator = '',
    [string]$BuildDir = '',
    [switch]$ASan,
    [switch]$NoFreeImage,
    [switch]$Clean
)

$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot
$configExplicit = $PSBoundParameters.ContainsKey('Config')
if ($ASan) {
    if ($configExplicit -and $Config -eq 'Release') {
        throw "-ASan cannot use -Config Release (use the default RelWithDebInfo)."
    }
    if (-not $configExplicit) { $Config = 'RelWithDebInfo' }
    if ($Config -eq 'Debug' -and -not $NoFreeImage) {
        throw "-ASan -Config Debug needs -NoFreeImage (prebuilt libraries use the Release CRT)."
    }
    if ([string]::IsNullOrWhiteSpace($BuildDir)) { $BuildDir = 'build-asan' }
} elseif ($NoFreeImage) {
    if (-not $configExplicit) { $Config = 'Debug' }
    if ([string]::IsNullOrWhiteSpace($BuildDir)) { $BuildDir = 'build-debug' }
} elseif ([string]::IsNullOrWhiteSpace($BuildDir)) {
    $BuildDir = 'build'
}
$build = Join-Path $root $BuildDir

$vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
if (-not (Test-Path -LiteralPath $vswhere)) {
    throw "vswhere not found: $vswhere"
}

$vsPath = & $vswhere -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
if ([string]::IsNullOrWhiteSpace($vsPath)) {
    throw "Visual Studio with C++ tools not found."
}

if ([string]::IsNullOrWhiteSpace($Generator)) {
    $vsVersion = & $vswhere -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationVersion
    $major = 0
    [void][int]::TryParse(($vsVersion -split '\.')[0], [ref]$major)
    switch ($major) {
        18 { $Generator = 'Visual Studio 18 2026' }
        17 { $Generator = 'Visual Studio 17 2022' }
        default { throw "Unsupported Visual Studio version: $vsVersion" }
    }
}

if ($Clean -and (Test-Path -LiteralPath $build)) {
    Remove-Item -LiteralPath $build -Recurse -Force
}

$cmake = (Get-Command cmake -ErrorAction Stop).Source

Write-Host "Configuring $Generator / $Config$(if ($ASan) { ' / ASan' })$(if ($NoFreeImage) { ' / no FreeImage' }) ..."
$configureArgs = @('-S', $root, '-B', $build, '-G', $Generator, '-A', 'x64')
if ($ASan) { $configureArgs += '-DZOREVIEW_ASAN=ON' }
if ($NoFreeImage) { $configureArgs += '-DZOREVIEW_USE_FREEIMAGE=OFF' }
& $cmake @configureArgs
if ($LASTEXITCODE -ne 0) {
    throw "CMake configure failed with exit code $LASTEXITCODE"
}

Write-Host "Building $Config ..."
& $cmake --build $build --config $Config
if ($LASTEXITCODE -ne 0) {
    throw "Build failed with exit code $LASTEXITCODE"
}

$exe = Join-Path $build "$Config\zoreview.exe"
if (-not (Test-Path -LiteralPath $exe)) {
    throw "Build succeeded but $exe was not produced."
}

Write-Host "OK: $exe"
