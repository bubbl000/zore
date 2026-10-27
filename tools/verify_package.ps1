#requires -Version 5.1
<#
.SYNOPSIS
    Verify a ZoreView release package: checksums, manifest, SBOM, licenses and
    (optionally) Authenticode signatures.

.DESCRIPTION
    Self-contained: this file is copied into every release package as
    Verify.ps1, so it must not depend on the source tree or other tools scripts.
    Exit code 0 means every check passed.

.EXAMPLE
    powershell -ExecutionPolicy Bypass -File tools\verify_package.ps1 `
        -PackageDirectory dist\Zore-0.3.0-portable

.EXAMPLE
    # Production gate: signatures must be present and trusted.
    powershell -ExecutionPolicy Bypass -File tools\verify_package.ps1 `
        -PackageDirectory dist\Zore-0.3.0-portable `
        -VerifySignature -RequireSignature
#>
param(
    [string]$PackageDirectory = '.',
    [switch]$VerifySignature,
    [switch]$RequireSignature,
    [switch]$AllowUntrustedSigner
)

$ErrorActionPreference = 'Stop'
$package = (Resolve-Path -LiteralPath $PackageDirectory).Path
$failures = New-Object System.Collections.Generic.List[string]

function Add-Failure([string]$Message) {
    $script:failures.Add($Message)
    Write-Host "FAIL: $Message"
}

function Get-Sha256Lower([string]$Path) {
    return (Get-FileHash -LiteralPath $Path -Algorithm SHA256).Hash.ToLowerInvariant()
}

$required = @(
    'zoreview.exe',
    'ZoreViewThumb.dll',
    'LICENSE',
    'NOTICE.md',
    'README.md',
    'PRIVACY.md',
    'CHANGELOG.md',
    'RELEASE_NOTES.md',
    'SBOM.cdx.json',
    'VERSION.txt',
    'SHA256.txt',
    'SIGNATURES.txt',
    'release-manifest.json',
    'THIRD_PARTY_LICENSES\FreeImage-FIPL-1.0.txt',
    'THIRD_PARTY_LICENSES\LibJPEG-IJG.txt',
    'THIRD_PARTY_LICENSES\LibRaw-LGPL-2.1.txt',
    'THIRD_PARTY_LICENSES\LibRaw-CDDL-1.0.txt',
    'THIRD_PARTY_LICENSES\zlib-1.2.11.txt'
)
foreach ($name in $required) {
    if (-not (Test-Path -LiteralPath (Join-Path $package $name))) {
        Add-Failure "missing required file: $name"
    }
}

$versionText = Get-Content -LiteralPath (Join-Path $package 'VERSION.txt') -Raw -ErrorAction SilentlyContinue
$version = ''
if ($versionText -match 'ZoreView\s+([0-9][0-9A-Za-z.\-+]*)') {
    $version = $Matches[1]
} else {
    Add-Failure 'VERSION.txt does not contain a ZoreView version'
}

# --- SHA256.txt -----------------------------------------------------------
$checksumPath = Join-Path $package 'SHA256.txt'
$listed = @{}
if (Test-Path -LiteralPath $checksumPath) {
    $lines = Get-Content -LiteralPath $checksumPath -Encoding ASCII
    foreach ($line in $lines) {
        if ([string]::IsNullOrWhiteSpace($line)) { continue }
        $match = [regex]::Match($line, '^([0-9a-f]{64})  (.+)$')
        if (-not $match.Success) {
            Add-Failure "malformed SHA256.txt line: $line"
            continue
        }
        $expected = $match.Groups[1].Value
        $relative = $match.Groups[2].Value
        $listed[$relative] = $expected
        $file = Join-Path $package $relative
        if (-not (Test-Path -LiteralPath $file)) {
            Add-Failure "checksum lists a missing file: $relative"
            continue
        }
        $actual = Get-Sha256Lower $file
        if ($actual -ne $expected) {
            Add-Failure "checksum mismatch: $relative"
        }
    }

    # Every file except SHA256.txt itself must be covered.
    $files = Get-ChildItem -LiteralPath $package -Recurse -File
    foreach ($file in $files) {
        $relative = $file.FullName.Substring($package.Length).TrimStart('\')
        if ($relative -eq 'SHA256.txt') { continue }
        if (-not $listed.ContainsKey($relative)) {
            Add-Failure "file not covered by SHA256.txt: $relative"
        }
    }
} else {
    Add-Failure 'missing SHA256.txt'
}

# --- release manifest -----------------------------------------------------
$manifestPath = Join-Path $package 'release-manifest.json'
$manifest = $null
if (Test-Path -LiteralPath $manifestPath) {
    try {
        $manifest = Get-Content -LiteralPath $manifestPath -Raw | ConvertFrom-Json
    } catch {
        Add-Failure "release-manifest.json is not valid JSON: $($_.Exception.Message)"
    }
    if ($null -ne $manifest) {
        if ($manifest.schema -ne 'zoreview.release-manifest/1') {
            Add-Failure 'release-manifest.json has an unexpected schema'
        }
        if ($manifest.version -ne $version) {
            Add-Failure "release-manifest version ($($manifest.version)) does not match VERSION.txt ($version)"
        }
        foreach ($entry in @($manifest.files)) {
            $file = Join-Path $package $entry.path
            if (-not (Test-Path -LiteralPath $file)) {
                Add-Failure "manifest lists a missing file: $($entry.path)"
                continue
            }
            if ((Get-Sha256Lower $file) -ne $entry.sha256) {
                Add-Failure "manifest hash mismatch: $($entry.path)"
            }
        }
    }
}

# --- SBOM -----------------------------------------------------------------
$sbomPath = Join-Path $package 'SBOM.cdx.json'
if (Test-Path -LiteralPath $sbomPath) {
    try {
        $sbom = Get-Content -LiteralPath $sbomPath -Raw | ConvertFrom-Json
        if ($sbom.bomFormat -ne 'CycloneDX') { Add-Failure 'SBOM bomFormat is not CycloneDX' }
        if ($sbom.specVersion -ne '1.5') { Add-Failure "SBOM specVersion is $($sbom.specVersion), expected 1.5" }
        if ($sbom.metadata.component.version -ne $version) {
            Add-Failure "SBOM component version ($($sbom.metadata.component.version)) does not match VERSION.txt ($version)"
        }
        $components = @($sbom.components)
        if ($components.Count -lt 4) { Add-Failure 'SBOM lists fewer than four third-party components' }
        $refs = @{}
        foreach ($component in $components) {
            if ([string]::IsNullOrWhiteSpace($component.'bom-ref')) {
                Add-Failure "SBOM component without bom-ref: $($component.name)"
            } else {
                $refs[$component.'bom-ref'] = $true
            }
            if ($null -eq $component.licenses) {
                Add-Failure "SBOM component without a license: $($component.name)"
            }
        }
        foreach ($dependency in @($sbom.dependencies)) {
            if (-not $refs.ContainsKey($dependency.ref) -and $dependency.ref -ne $sbom.metadata.component.'bom-ref') {
                Add-Failure "SBOM dependency references an unknown component: $($dependency.ref)"
            }
            foreach ($target in @($dependency.dependsOn)) {
                if (-not $refs.ContainsKey($target) -and $target -ne $sbom.metadata.component.'bom-ref') {
                    Add-Failure "SBOM dependsOn references an unknown component: $target"
                }
            }
        }
        $exeEntry = @($components | Where-Object { $_.name -eq 'FreeImage' })
        if ($exeEntry.Count -eq 0) { Add-Failure 'SBOM is missing FreeImage' }
    } catch {
        Add-Failure "SBOM is not valid JSON: $($_.Exception.Message)"
    }
}

# --- Authenticode ---------------------------------------------------------
$signed = $false
$signatureSummary = 'unsigned'
foreach ($binary in @('zoreview.exe', 'ZoreViewThumb.dll')) {
    $binaryPath = Join-Path $package $binary
    if (-not (Test-Path -LiteralPath $binaryPath)) { continue }
    $signature = Get-AuthenticodeSignature -LiteralPath $binaryPath
    $hasSigner = $null -ne $signature.SignerCertificate
    if ($hasSigner) {
        $signed = $true
        if (-not ($VerifySignature -or $RequireSignature)) {
            $signatureSummary = "present ($($signature.SignerCertificate.Subject))"
        }
    }
    if ($VerifySignature -or $RequireSignature) {
        if (-not $hasSigner) {
            Add-Failure "$binary is not signed"
            continue
        }
        if ($AllowUntrustedSigner) {
            $signatureSummary = "$binary signed by $($signature.SignerCertificate.Subject) (untrusted chain allowed)"
        } elseif ($signature.Status -ne 'Valid') {
            Add-Failure "$binary signature status is $($signature.Status): $($signature.StatusMessage)"
        } else {
            $signatureSummary = "$binary signed by $($signature.SignerCertificate.Subject)"
        }
    }
}
if ($RequireSignature -and -not $signed) {
    Add-Failure 'package must be Authenticode-signed'
}
if (-not $signed) {
    Write-Host 'WARN: package is unsigned (development build)'
}

if ($failures.Count -ne 0) {
    Write-Host "FAILED: $($failures.Count) problem(s) in $package"
    exit 1
}

$checked = if ($listed.Count -gt 0) { $listed.Count } else { 0 }
Write-Host "OK: package $version verified (files=$checked signature=$signatureSummary)"
exit 0
