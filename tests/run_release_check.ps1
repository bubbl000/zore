#requires -Version 5.1
<#
.SYNOPSIS
    M3-4 release-engineering check: version resources, package contents,
    checksums, manifest, SBOM, license bundle and the signing toolchain.

.DESCRIPTION
    Builds a portable and an installer package into tests\output\release-check,
    verifies them with the packaged Verify.ps1, and (unless -SkipSigningTest)
    signs a separate copy with a temporary self-signed certificate to exercise
    tools\sign.ps1 and package.ps1 -Sign end to end. The self-signed certificate
    is removed afterwards; it must never be used for a public release.

.EXAMPLE
    powershell -ExecutionPolicy Bypass -File tests\run_release_check.ps1

.EXAMPLE
    powershell -ExecutionPolicy Bypass -File tests\run_release_check.ps1 -SkipSigningTest

.EXAMPLE
    # Also install the installer package into a temporary directory and check
    # that the license bundle/docs are copied, then uninstall.
    powershell -ExecutionPolicy Bypass -File tests\run_release_check.ps1 -TestInstallerDocs
#>
param(
    [switch]$SkipSigningTest,
    [switch]$TestInstallerDocs,
    [switch]$KeepPackages
)

$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot
$release = Join-Path $root 'tests\output\release-check'
$failures = 0
$checks = 0

function Assert([bool]$Condition, [string]$What) {
    $script:checks++
    if ($Condition) {
        Write-Host "  ok   $What"
    } else {
        Write-Host "  FAIL $What"
        $script:failures++
    }
}

function Read-Version {
    $text = Get-Content -LiteralPath (Join-Path $root 'src\core\version_numbers.h') -Raw
    $match = [regex]::Match($text, '#define\s+NV_VERSION_STRING\s+"([^"]+)"')
    if (-not $match.Success) { throw 'cannot read NV_VERSION_STRING' }
    return $match.Groups[1].Value
}

$version = Read-Version
Write-Host "release check: version $version"

# The numeric VERSIONINFO fields and the textual version must agree.
$versionHeader = Get-Content -LiteralPath (Join-Path $root 'src\core\version_numbers.h') -Raw
$major = [regex]::Match($versionHeader, '#define\s+NV_VERSION_MAJOR\s+(\d+)').Groups[1].Value
$minor = [regex]::Match($versionHeader, '#define\s+NV_VERSION_MINOR\s+(\d+)').Groups[1].Value
$patch = [regex]::Match($versionHeader, '#define\s+NV_VERSION_PATCH\s+(\d+)').Groups[1].Value
Assert (("{0}.{1}.{2}" -f $major, $minor, $patch) -eq $version) 'numeric version macros match NV_VERSION_STRING'

# --- source inputs --------------------------------------------------------
foreach ($name in @('LICENSE', 'NOTICE', 'README.md', 'docs\HELP.md', 'docs\PRIVACY.md',
        'docs\CHANGELOG.md', 'docs\RELEASE_NOTES.md', 'docs\SIGNING.md',
        'docs\RELEASE_CHECKLIST.md', 'third_party\licenses\FreeImage-FIPL-1.0.txt',
        'third_party\licenses\LibJPEG-IJG.txt', 'third_party\licenses\LibRaw-LGPL-2.1.txt',
        'third_party\licenses\LibRaw-CDDL-1.0.txt', 'third_party\licenses\zlib-1.2.11.txt')) {
    Assert (Test-Path -LiteralPath (Join-Path $root $name)) "source has $name"
}

$license = Get-Content -LiteralPath (Join-Path $root 'LICENSE') -Raw
Assert ($license -match 'Apache License') 'LICENSE is Apache-2.0'
$notice = Get-Content -LiteralPath (Join-Path $root 'NOTICE') -Raw
foreach ($component in @('FreeImage', 'libjpeg', 'LibRaw', 'zlib')) {
    Assert ($notice -match $component) "NOTICE mentions $component"
}

# --- version resources ----------------------------------------------------
$exe = Join-Path $root 'build\Release\zoreview.exe'
$dll = Join-Path $root 'build\Release\ZoreViewThumb.dll'
$cli = Join-Path $root 'build\Release\zoreview_cli.exe'
foreach ($binary in @($exe, $dll, $cli)) {
    Assert (Test-Path -LiteralPath $binary) "built $(Split-Path -Leaf $binary)"
}
if ((Test-Path -LiteralPath $exe) -and (Test-Path -LiteralPath $dll)) {
    $exeInfo = (Get-Item -LiteralPath $exe).VersionInfo
    $dllInfo = (Get-Item -LiteralPath $dll).VersionInfo
    $cliInfo = (Get-Item -LiteralPath $cli).VersionInfo
    Assert ($exeInfo.FileVersion -eq $version) "zoreview.exe FileVersion is $version"
    Assert ($dllInfo.FileVersion -eq $version) "ZoreViewThumb.dll FileVersion is $version"
    Assert ($cliInfo.FileVersion -eq $version) "zoreview_cli.exe FileVersion is $version"
    Assert ($exeInfo.ProductName -eq 'ZoreView') 'zoreview.exe ProductName'
    Assert ($exeInfo.LegalCopyright -match 'ZoreView') 'zoreview.exe copyright'
    Assert ($dllInfo.FileDescription -match 'thumbnail') 'DLL FileDescription'
}

# --- package -----------------------------------------------------------------
if (Test-Path -LiteralPath $release) {
    Get-ChildItem -LiteralPath $release -Directory -Filter 'ZoreView-*' |
        Remove-Item -Recurse -Force
    Get-ChildItem -LiteralPath $release -File -Filter 'ZoreView-*.zip*' |
        Remove-Item -Force
}
New-Item -ItemType Directory -Path $release -Force | Out-Null

$packageScript = Join-Path $root 'tools\package.ps1'
foreach ($mode in @('portable', 'installer')) {
    & powershell -ExecutionPolicy Bypass -File $packageScript -Mode $mode -OutputDirectory $release
    Assert ($LASTEXITCODE -eq 0) "$mode package builds"
}

$portable = Join-Path $release "ZoreView-$version-portable"
$installer = Join-Path $release "ZoreView-$version-installer"
Assert (Test-Path -LiteralPath $portable) 'portable package directory'
Assert (Test-Path -LiteralPath $installer) 'installer package directory'

foreach ($package in @($portable, $installer)) {
    $label = Split-Path -Leaf $package
    if (-not (Test-Path -LiteralPath $package)) { continue }
    Write-Host "package $label"

    $required = @('zoreview.exe', 'ZoreViewThumb.dll', 'LICENSE', 'NOTICE.md', 'README.md',
        'PRIVACY.md', 'CHANGELOG.md', 'RELEASE_NOTES.md', 'SBOM.cdx.json', 'VERSION.txt',
        'SHA256.txt', 'SIGNATURES.txt', 'release-manifest.json', 'Verify.ps1', 'Verify.cmd',
        'THIRD_PARTY_LICENSES\FreeImage-FIPL-1.0.txt', 'THIRD_PARTY_LICENSES\LibJPEG-IJG.txt',
        'THIRD_PARTY_LICENSES\LibRaw-LGPL-2.1.txt', 'THIRD_PARTY_LICENSES\LibRaw-CDDL-1.0.txt',
        'THIRD_PARTY_LICENSES\zlib-1.2.11.txt')
    foreach ($name in $required) {
        Assert (Test-Path -LiteralPath (Join-Path $package $name)) "$label contains $name"
    }
    Assert (-not (Test-Path -LiteralPath (Join-Path $package 'zoreview_cli.exe'))) "$label excludes CLI"
    Assert (-not (Test-Path -LiteralPath (Join-Path $package 'zoreview_tests.exe'))) "$label excludes test binaries"
    $pdb = @(Get-ChildItem -LiteralPath $package -Recurse -File -Filter '*.pdb')
    Assert ($pdb.Count -eq 0) "$label excludes PDB files"

    # Run the packaged verifier, not tools\verify_package.ps1.
    & powershell -ExecutionPolicy Bypass -File (Join-Path $package 'Verify.ps1') -PackageDirectory $package
    Assert ($LASTEXITCODE -eq 0) "$label verifies with packaged Verify.ps1"

    $versionText = Get-Content -LiteralPath (Join-Path $package 'VERSION.txt') -Raw
    Assert ($versionText -match [regex]::Escape($version)) "$label VERSION.txt matches"

    $manifest = Get-Content -LiteralPath (Join-Path $package 'release-manifest.json') -Raw | ConvertFrom-Json
    Assert ($manifest.schema -eq 'zoreview.release-manifest/1') "$label manifest schema"
    Assert ($manifest.version -eq $version) "$label manifest version"
    Assert ($manifest.files.Count -ge 10) "$label manifest lists files"

    $sbom = Get-Content -LiteralPath (Join-Path $package 'SBOM.cdx.json') -Raw | ConvertFrom-Json
    Assert ($sbom.bomFormat -eq 'CycloneDX' -and $sbom.specVersion -eq '1.5') "$label SBOM is CycloneDX 1.5"
    Assert (@($sbom.components).Count -eq 4) "$label SBOM has 4 components"
    $freeImage = @($sbom.components | Where-Object { $_.name -eq 'FreeImage' })[0]
    $freeImageHash = (Get-FileHash -LiteralPath (Join-Path $root 'third_party\lib\freeimage.lib') -Algorithm SHA256).Hash.ToLowerInvariant()
    Assert ($freeImage.hashes[0].content -eq $freeImageHash) "$label SBOM FreeImage hash matches the vendored library"

    $sigText = Get-Content -LiteralPath (Join-Path $package 'SIGNATURES.txt') -Raw
    Assert ($sigText -match 'signed=false') "$label unsigned marker"
    Assert ($manifest.signed -eq $false) "$label manifest records unsigned"

    $zip = "$package.zip"
    Assert (Test-Path -LiteralPath $zip) "$label zip exists"
    Assert (Test-Path -LiteralPath "$zip.sha256") "$label zip checksum exists"
    if (Test-Path -LiteralPath "$zip.sha256") {
        $expected = (Get-Content -LiteralPath "$zip.sha256" -Raw).Split(' ')[0]
        $actual = (Get-FileHash -LiteralPath $zip -Algorithm SHA256).Hash.ToLowerInvariant()
        Assert ($expected -eq $actual) "$label zip checksum matches"
    }
}

# Portable path behaviour inside the package.
if (Test-Path -LiteralPath (Join-Path $portable 'zoreview.exe')) {
    $pathsFile = Join-Path $release 'portable-paths.txt'
    $process = Start-Process -FilePath (Join-Path $portable 'zoreview.exe') `
        -ArgumentList @('--print-paths', $pathsFile) -Wait -PassThru
    Assert ($process.ExitCode -eq 0) 'packaged portable runs --print-paths'
    if (Test-Path -LiteralPath $pathsFile) {
        $lines = @{}
        Get-Content -LiteralPath $pathsFile -Encoding UTF8 | ForEach-Object {
            $index = $_.IndexOf('=')
            if ($index -gt 0) { $lines[$_.Substring(0, $index)] = $_.Substring($index + 1) }
        }
        Assert ($lines['portable'] -eq '1') 'packaged portable reports portable=1'
    }
    # Running the portable copy creates logs\ inside the package (expected for
    # portable mode). Remove it so the package still matches SHA256.txt.
    $logs = Join-Path $portable 'logs'
    if (Test-Path -LiteralPath $logs) { Remove-Item -LiteralPath $logs -Recurse -Force }
}

# Optional: install from the installer package and confirm the license bundle
# and documentation land in the install directory, then uninstall.
if ($TestInstallerDocs -and (Test-Path -LiteralPath (Join-Path $installer 'zoreview.exe'))) {
    Write-Host 'installer documentation test'
    $target = Join-Path $release 'install-docs'
    if (Test-Path -LiteralPath $target) { Remove-Item -LiteralPath $target -Recurse -Force }
    try {
        $process = Start-Process -FilePath (Join-Path $installer 'zoreview.exe') `
            -ArgumentList @('--install', '--no-shortcut', '--install-dir', $target) -Wait -PassThru
        Assert ($process.ExitCode -eq 0) 'installer package installs to a custom directory'
        foreach ($name in @('LICENSE', 'NOTICE.md', 'README.md', 'PRIVACY.md',
                'SBOM.cdx.json', 'VERSION.txt', 'THIRD_PARTY_LICENSES\zlib-1.2.11.txt')) {
            Assert (Test-Path -LiteralPath (Join-Path $target $name)) "installed copy has $name"
        }
        $uninstall = Start-Process -FilePath (Join-Path $target 'zoreview.exe') `
            -ArgumentList @('--uninstall', '--install-dir', $target) -Wait -PassThru
        Assert ($uninstall.ExitCode -eq 0) 'installed copy uninstalls'
        $removed = $false
        for ($i = 0; $i -lt 40; $i++) {
            if (-not (Test-Path -LiteralPath $target)) { $removed = $true; break }
            Start-Sleep -Milliseconds 500
        }
        Assert $removed 'install directory is removed after uninstall'
    } finally {
        if (Test-Path -LiteralPath $target) {
            Remove-Item -LiteralPath $target -Recurse -Force -ErrorAction SilentlyContinue
        }
        $logs = Join-Path $installer 'logs'
        if (Test-Path -LiteralPath $logs) { Remove-Item -LiteralPath $logs -Recurse -Force }
    }
}

# --- signing toolchain ----------------------------------------------------
if (-not $SkipSigningTest) {
    Write-Host 'signing test (temporary self-signed certificate)'
    $signingDir = Join-Path $release 'signed'
    if (Test-Path -LiteralPath $signingDir) { Remove-Item -LiteralPath $signingDir -Recurse -Force }
    New-Item -ItemType Directory -Path $signingDir -Force | Out-Null
    $pfx = Join-Path $signingDir 'test-signing.pfx'
    $cert = $null
    try {
        $cert = New-SelfSignedCertificate -Type CodeSigningCert `
            -Subject 'CN=ZoreView Test Signing' -CertStoreLocation Cert:\CurrentUser\My `
            -KeyUsage DigitalSignature -NotAfter (Get-Date).AddDays(2)
        $password = ConvertTo-SecureString -String 'test' -Force -AsPlainText
        Export-PfxCertificate -Cert $cert -FilePath $pfx -Password $password | Out-Null
        Assert (Test-Path -LiteralPath $pfx) 'temporary signing certificate exported'

        & powershell -ExecutionPolicy Bypass -File $packageScript -Mode portable `
            -OutputDirectory $signingDir -Sign -CertificatePath $pfx `
            -CertificatePassword 'test' -NoTimestamp -AllowUntrustedSignature
        Assert ($LASTEXITCODE -eq 0) 'signed package builds'

        $signedPackage = Join-Path $signingDir "ZoreView-$version-portable"
        $signedManifest = Get-Content -LiteralPath (Join-Path $signedPackage 'release-manifest.json') -Raw | ConvertFrom-Json
        Assert ($signedManifest.signed -eq $true) 'signed manifest records signed=true'
        $sigText = Get-Content -LiteralPath (Join-Path $signedPackage 'SIGNATURES.txt') -Raw
        Assert ($sigText -match 'signed=true') 'SIGNATURES.txt records signed=true'
        Assert ($sigText -match [regex]::Escape($cert.Subject)) 'SIGNATURES.txt names the signer'

        $signature = Get-AuthenticodeSignature -LiteralPath (Join-Path $signedPackage 'zoreview.exe')
        Assert ($null -ne $signature.SignerCertificate) 'zoreview.exe carries a signer certificate'

        & powershell -ExecutionPolicy Bypass -File (Join-Path $signedPackage 'Verify.ps1') `
            -PackageDirectory $signedPackage -VerifySignature -RequireSignature -AllowUntrustedSigner
        Assert ($LASTEXITCODE -eq 0) 'signed package passes Verify.ps1 with signature checks'

        $signedSbom = Get-Content -LiteralPath (Join-Path $signedPackage 'SBOM.cdx.json') -Raw | ConvertFrom-Json
        $signedExeHash = (Get-FileHash -LiteralPath (Join-Path $signedPackage 'zoreview.exe') -Algorithm SHA256).Hash.ToLowerInvariant()
        Assert ($signedSbom.metadata.component.hashes[0].content -eq $signedExeHash) 'SBOM hashes the signed executable'
        Assert ($signedSbom.serialNumber -ne $sbom.serialNumber) 'signed SBOM has a distinct serial number'

        $signedZip = "$signedPackage.zip"
        $signedZipHash = (Get-FileHash -LiteralPath $signedZip -Algorithm SHA256).Hash.ToLowerInvariant()
        $expected = (Get-Content -LiteralPath "$signedZip.sha256" -Raw).Split(' ')[0]
        Assert ($expected -eq $signedZipHash) 'signed zip checksum matches'

        # Unsigned package must fail the production signature gate.
        & powershell -ExecutionPolicy Bypass -File (Join-Path $portable 'Verify.ps1') `
            -PackageDirectory $portable -VerifySignature -RequireSignature
        Assert ($LASTEXITCODE -ne 0) 'unsigned package fails -RequireSignature'
    } finally {
        if ($null -ne $cert) {
            Remove-Item -LiteralPath ("Cert:\CurrentUser\My\" + $cert.Thumbprint) -Force -ErrorAction SilentlyContinue
        }
        if (Test-Path -LiteralPath $pfx) { Remove-Item -LiteralPath $pfx -Force }
    }
}

if (-not $KeepPackages) {
    # Keep the built directories for inspection but drop the throwaway signed copy.
    $signing = Join-Path $release 'signed'
    if (Test-Path -LiteralPath $signing) { Remove-Item -LiteralPath $signing -Recurse -Force }
}

Write-Host ''
if ($failures -ne 0) {
    Write-Host "FAILED: $failures of $checks checks failed"
    exit 1
}
Write-Host "OK: $checks release checks passed"
exit 0
