#requires -Version 5.1
<#
.SYNOPSIS
    Build a portable or self-installing ZoreView release package.

.DESCRIPTION
    Copies the release binaries, license bundle, help/privacy/changelog and the
    generated CycloneDX SBOM into a versioned folder under dist\, adds the
    portable marker or the install scripts, records SHA-256 checksums, a release
    manifest and (optionally) Authenticode signatures, then zips the package and
    writes the zip checksum.

    The application itself performs the per-user install
    (`zoreview.exe --install`); no separate installer binary is required.
    The package carries Verify.ps1/Verify.cmd so a downloaded copy can be
    checked before installation.

.EXAMPLE
    powershell -ExecutionPolicy Bypass -File tools\package.ps1 -Mode portable

.EXAMPLE
    $env:NV_SIGN_PASSWORD = 'secret'
    powershell -ExecutionPolicy Bypass -File tools\package.ps1 -Mode installer `
        -Sign -CertificatePath C:\keys\zoreview.pfx -RequireSignature

.EXAMPLE
    # Test-only: sign with a self-signed certificate whose chain is not trusted.
    powershell -ExecutionPolicy Bypass -File tools\package.ps1 -Mode portable `
        -Sign -CertificatePath out\test-signing.pfx -CertificatePassword test `
        -AllowUntrustedSignature
#>
param(
    [ValidateSet('portable', 'installer')]
    [string]$Mode = 'portable',
    [string]$OutputDirectory = (Join-Path (Split-Path -Parent $PSScriptRoot) 'dist'),
    [string]$Configuration = 'Release',
    [string]$BuildDirectory = 'build',
    [switch]$SkipArchive,
    [switch]$Sign,
    [string]$CertificatePath = '',
    [string]$CertificatePassword = '',
    [string]$CertificateThumbprint = '',
    [string]$CertificateStore = 'My',
    [string]$TimestampUrl = 'http://timestamp.digicert.com',
    [switch]$NoTimestamp,
    [switch]$AllowUntrustedSignature,
    [switch]$RequireSignature
)

$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot
. (Join-Path $PSScriptRoot 'release_common.ps1')

$binaries = Join-Path $root "$BuildDirectory\$Configuration"
$exe = Join-Path $binaries 'zoreview.exe'
$dll = Join-Path $binaries 'ZoreViewThumb.dll'
$version = Get-ProjectVersion $root
$packageName = "ZoreView-$version-$Mode"
$packageDir = [System.IO.Path]::GetFullPath((Join-Path $OutputDirectory $packageName))

foreach ($path in @($exe, $dll, (Join-Path $root 'LICENSE'), (Join-Path $root 'NOTICE'),
        (Join-Path $root 'third_party\licenses'), (Join-Path $root 'docs\HELP.md'),
        (Join-Path $root 'docs\PRIVACY.md'), (Join-Path $root 'docs\CHANGELOG.md'),
        (Join-Path $root 'docs\RELEASE_NOTES.md'))) {
    if (-not (Test-Path -LiteralPath $path)) { throw "missing release input: $path" }
}
if ($RequireSignature -and -not $Sign) {
    throw '-RequireSignature needs -Sign (or sign the build separately and use verify_package.ps1)'
}

function Copy-ReleaseFile([string]$Source, [string]$DestinationName) {
    Copy-Item -LiteralPath $Source -Destination (Join-Path $packageDir $DestinationName) -Force
}

if (Test-Path -LiteralPath $packageDir) {
    Remove-Item -LiteralPath $packageDir -Recurse -Force
}
New-Item -ItemType Directory -Path $packageDir -Force | Out-Null
New-Item -ItemType Directory -Path (Join-Path $packageDir 'THIRD_PARTY_LICENSES') -Force | Out-Null

Copy-ReleaseFile $exe 'zoreview.exe'
Copy-ReleaseFile $dll 'ZoreViewThumb.dll'
Copy-ReleaseFile (Join-Path $root 'LICENSE') 'LICENSE'
Copy-ReleaseFile (Join-Path $root 'NOTICE') 'NOTICE.md'
Copy-ReleaseFile (Join-Path $root 'docs\HELP.md') 'README.md'
Copy-ReleaseFile (Join-Path $root 'docs\PRIVACY.md') 'PRIVACY.md'
Copy-ReleaseFile (Join-Path $root 'docs\CHANGELOG.md') 'CHANGELOG.md'
Copy-ReleaseFile (Join-Path $root 'docs\RELEASE_NOTES.md') 'RELEASE_NOTES.md'
Get-ChildItem -LiteralPath (Join-Path $root 'third_party\licenses') -File |
    ForEach-Object { Copy-ReleaseFile $_.FullName (Join-Path 'THIRD_PARTY_LICENSES' $_.Name) }
Copy-ReleaseFile (Join-Path $PSScriptRoot 'verify_package.ps1') 'Verify.ps1'

@"
ZoreView $version
Build configuration: $Configuration
Package: $Mode
Architecture: x64 (Windows 10/11)
Third-party components: FreeImage 3.18, libjpeg 9c, LibRaw 0.21.5, zlib 1.2.11
"@ | Set-Content -LiteralPath (Join-Path $packageDir 'VERSION.txt') -Encoding ASCII

@'
@echo off
cd /d "%~dp0"
powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0Verify.ps1" -PackageDirectory "%~dp0"
if errorlevel 1 (
  echo.
  echo Package verification failed. Do not install this copy.
  pause
  exit /b 1
)
echo.
echo All package files match SHA-256.txt.
pause
'@ | Set-Content -LiteralPath (Join-Path $packageDir 'Verify.cmd') -Encoding ASCII

if ($Mode -eq 'portable') {
    # The marker switches settings.ini/cache/logs to the executable directory.
    Set-Content -LiteralPath (Join-Path $packageDir 'ZoreView.portable') -Value 'portable' -Encoding ASCII
    @"
ZoreView $version portable
===========================

No installation is needed. Keep this folder together and run zoreview.exe.

Because ZoreView.portable is present, settings.ini, cache\, logs\ and
crashes\ stay inside this folder and never mix with an installed copy.
Delete ZoreView.portable to fall back to %LOCALAPPDATA%\ZoreView.

Verify the download first:
  Verify.cmd

Explorer integration is optional and per-user:
  zoreview.exe --register-shell      right-click verbs
  zoreview.exe --register-assoc      "Open with" entry
  zoreview.exe --unregister-shell --unregister-assoc   remove them

Documentation:
  README.md            formats, shortcuts, troubleshooting
  PRIVACY.md           no network, no telemetry
  CHANGELOG.md         release history
  RELEASE_NOTES.md     this release
  LICENSE / NOTICE.md  licensing and third-party components
  SBOM.cdx.json        CycloneDX software bill of materials
  THIRD_PARTY_LICENSES full license texts
"@ | Set-Content -LiteralPath (Join-Path $packageDir 'README.txt') -Encoding UTF8
} else {
    @"
ZoreView $version installer
============================

Run Install.cmd (or: zoreview.exe --install) to install for the current user
into %LOCALAPPDATA%\Programs\ZoreView. No administrator rights are required.

Verify.cmd checks every file against SHA256.txt; Install.cmd runs it first and
refuses to install when a file was modified.

Optional arguments:
  --install --machine      install for all users (requires administrator)
  --install --no-shortcut  skip the Start menu shortcut
  --install --associate-default
                           also make ZoreView the default image viewer

The installer copies zoreview.exe, ZoreViewThumb.dll, the license bundle and
the documentation, registers the thumbnail handler and the Explorer context-menu
verbs, adds an "Open with" entry for supported images, and creates an entry
under Apps & Features.

Uninstall with Uninstall.cmd, or from Settings > Apps > Installed apps >
ZoreView.
"@ | Set-Content -LiteralPath (Join-Path $packageDir 'INSTALL.txt') -Encoding UTF8

    @'
@echo off
cd /d "%~dp0"
powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0Verify.ps1" -PackageDirectory "%~dp0"
if errorlevel 1 (
  echo.
  echo Package verification failed. Installation aborted.
  pause
  exit /b 1
)
set "NVASSOC="
set /p NVASSOC=Make ZoreView the default image viewer? [y/N] 
if /i "%NVASSOC%"=="y" (
  zoreview.exe --install --associate-default
) else (
  zoreview.exe --install
)
if errorlevel 1 (
  echo.
  echo Installation failed. See %LOCALAPPDATA%\ZoreView\logs\zoreview.log
  pause
  exit /b 1
)
echo.
echo ZoreView is installed for the current user.
pause
'@ | Set-Content -LiteralPath (Join-Path $packageDir 'Install.cmd') -Encoding ASCII

    @'
@echo off
set "NV=%LOCALAPPDATA%\Programs\ZoreView\zoreview.exe"
if not exist "%NV%" (
  echo ZoreView is not installed for the current user.
  pause
  exit /b 1
)
"%NV%" --uninstall
if errorlevel 1 (
  echo.
  echo Uninstall failed. See %LOCALAPPDATA%\ZoreView\logs\zoreview.log
  pause
  exit /b 1
)
echo.
echo ZoreView was uninstalled.
pause
'@ | Set-Content -LiteralPath (Join-Path $packageDir 'Uninstall.cmd') -Encoding ASCII
}

$signatureInfo = @()
$signed = $false
if ($Sign) {
    $signArguments = @{
        Path = @((Join-Path $packageDir 'zoreview.exe'), (Join-Path $packageDir 'ZoreViewThumb.dll'))
        TimestampUrl = $TimestampUrl
        NoTimestamp = $NoTimestamp
        CertificateStore = $CertificateStore
        Verify = $true
        AllowUntrusted = $AllowUntrustedSignature
    }
    if (-not [string]::IsNullOrWhiteSpace($CertificatePath)) {
        $signArguments.CertificatePath = $CertificatePath
        $signArguments.CertificatePassword = $CertificatePassword
    } elseif (-not [string]::IsNullOrWhiteSpace($CertificateThumbprint)) {
        $signArguments.CertificateThumbprint = $CertificateThumbprint
    } else {
        throw '-Sign needs -CertificatePath or -CertificateThumbprint'
    }
    & (Join-Path $PSScriptRoot 'sign.ps1') @signArguments
    foreach ($binary in @('zoreview.exe', 'ZoreViewThumb.dll')) {
        $info = Get-SignatureInfo -Path (Join-Path $packageDir $binary)
        $signatureInfo += $info
        if ([string]::IsNullOrWhiteSpace($info.signer_thumbprint)) {
            throw "$binary was not signed"
        }
        if (-not $AllowUntrustedSignature -and $info.status -ne 'Valid') {
            throw "$binary signature is not valid: $($info.status) $($info.status_message)"
        }
    }
    $signed = $true
}

$signedValue = if ($Sign) { 'true' } else { 'false' }
$signatureLines = @("ZoreView $version Authenticode signatures", "signed=$signedValue", '')
if ($signed) {
    foreach ($info in $signatureInfo) {
        $name = Split-Path -Leaf $info.path
        $signatureLines += "$name|status=$($info.status)|signer=$($info.signer_subject)|thumbprint=$($info.signer_thumbprint)|timestamp=$($info.timestamp_subject)"
    }
} else {
    $signatureLines += 'zoreview.exe|unsigned'
    $signatureLines += 'ZoreViewThumb.dll|unsigned'
    $signatureLines += ''
    $signatureLines += 'This is an unsigned development package. Do not publish it as a release.'
}
$signatureLines | Set-Content -LiteralPath (Join-Path $packageDir 'SIGNATURES.txt') -Encoding UTF8

# SBOM hashes the signed binaries, so generate it after signing.
& (Join-Path $PSScriptRoot 'make_sbom.ps1') `
    -ArtifactDirectory $packageDir `
    -OutputPath (Join-Path $packageDir 'SBOM.cdx.json') | Out-Host
if ($LASTEXITCODE -ne 0) { throw 'SBOM generation failed' }

$manifestFiles = @()
Get-ChildItem -LiteralPath $packageDir -Recurse -File |
    Where-Object { $_.Name -notin @('release-manifest.json', 'SHA256.txt') } |
    Sort-Object FullName |
    ForEach-Object {
        $relative = $_.FullName.Substring($packageDir.Length).TrimStart('\')
        $manifestFiles += [ordered]@{
            path = $relative
            sha256 = Get-FileSha256 $_.FullName
            bytes = $_.Length
        }
    }

$manifest = [ordered]@{
    schema = 'zoreview.release-manifest/1'
    product = 'ZoreView'
    version = $version
    mode = $Mode
    architecture = 'x64'
    build_configuration = $Configuration
    created_utc = [DateTime]::UtcNow.ToString('yyyy-MM-ddTHH:mm:ssZ')
    signed = $signed
    signer = if ($signed -and $signatureInfo.Count -gt 0) {
        [ordered]@{
            subject = $signatureInfo[0].signer_subject
            thumbprint = $signatureInfo[0].signer_thumbprint
        }
    } else { $null }
    sbom = 'SBOM.cdx.json'
    files = $manifestFiles
}
$manifestJson = $manifest | ConvertTo-Json -Depth 8
$utf8 = New-Object System.Text.UTF8Encoding($false)
[System.IO.File]::WriteAllText(
    (Join-Path $packageDir 'release-manifest.json'), $manifestJson + "`r`n", $utf8)

# Per-file checksums, so a copied package can be verified without the zip.
$checksumLines = @()
Get-ChildItem -LiteralPath $packageDir -Recurse -File |
    Where-Object { $_.Name -ne 'SHA256.txt' } |
    Sort-Object FullName |
    ForEach-Object {
        $relative = $_.FullName.Substring($packageDir.Length).TrimStart('\')
        $checksumLines += "$(Get-FileSha256 $_.FullName)  $relative"
    }
$checksumLines | Set-Content -LiteralPath (Join-Path $packageDir 'SHA256.txt') -Encoding ASCII

$verify = Join-Path $PSScriptRoot 'verify_package.ps1'
& $verify -PackageDirectory $packageDir
if ($LASTEXITCODE -ne 0) { throw 'package self-check failed' }

$zipPath = $null
if (-not $SkipArchive) {
    $zipPath = "$packageDir.zip"
    if (Test-Path -LiteralPath $zipPath) { Remove-Item -LiteralPath $zipPath -Force }
    Compress-Archive -Path $packageDir -DestinationPath $zipPath -CompressionLevel Optimal
    $zipHash = Get-FileSha256 $zipPath
    "$zipHash  $packageName.zip" | Set-Content -LiteralPath "$zipPath.sha256" -Encoding ASCII
}

Write-Host "OK: $packageDir"
if ($zipPath) { Write-Host "OK: $zipPath" }
