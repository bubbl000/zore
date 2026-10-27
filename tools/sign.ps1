#requires -Version 5.1
<#
.SYNOPSIS
    Authenticode-sign ZoreView binaries and optionally verify them.

.DESCRIPTION
    Signs with SHA-256 and an RFC 3161 timestamp (recommended for release
    builds). Supply either a PFX file (-CertificatePath) or a certificate
    already installed in the current user's certificate store
    (-CertificateThumbprint). The PFX password is read from
    -CertificatePassword or the NV_SIGN_PASSWORD environment variable so it
    does not have to appear on the command line.

    This script does not create certificates. Production releases must use an
    OV/EV code-signing certificate from a public CA; self-signed certificates
    are for testing only (see docs/SIGNING.md).

.EXAMPLE
    $env:NV_SIGN_PASSWORD = 'secret'
    powershell -ExecutionPolicy Bypass -File tools\sign.ps1 `
        -Path build\Release\zoreview.exe, build\Release\ZoreViewThumb.dll `
        -CertificatePath C:\keys\zoreview.pfx -Verify

.EXAMPLE
    powershell -ExecutionPolicy Bypass -File tools\sign.ps1 `
        -Path dist\Zore-0.3.0-portable `
        -CertificateThumbprint 0123456789ABCDEF0123456789ABCDEF01234567 -Verify

.EXAMPLE
    # Verify an already signed package without signing anything.
    powershell -ExecutionPolicy Bypass -File tools\sign.ps1 -Path dist -VerifyOnly
#>
param(
    [Parameter(Mandatory = $true)]
    [string[]]$Path,
    [string]$CertificatePath = '',
    [string]$CertificatePassword = '',
    [string]$CertificateThumbprint = '',
    [string]$CertificateStore = 'My',
    [string]$TimestampUrl = 'http://timestamp.digicert.com',
    [switch]$NoTimestamp,
    [switch]$Verify,
    [switch]$VerifyOnly,
    [switch]$AllowUntrusted
)

$ErrorActionPreference = 'Stop'
. (Join-Path $PSScriptRoot 'release_common.ps1')

if ($NoTimestamp) { $TimestampUrl = '' }

$signTool = Find-SignTool
$files = New-Object System.Collections.Generic.List[string]
foreach ($item in $Path) {
    if (-not (Test-Path -LiteralPath $item)) { throw "missing path: $item" }
    $resolved = (Resolve-Path -LiteralPath $item).Path
    if (Test-Path -LiteralPath $resolved -PathType Container) {
        Get-ChildItem -LiteralPath $resolved -Recurse -File |
            Where-Object { $_.Extension -in @('.exe', '.dll') } |
            ForEach-Object { $files.Add($_.FullName) }
    } else {
        $files.Add($resolved)
    }
}
if ($files.Count -eq 0) { throw 'no .exe or .dll files to sign' }

if (-not $VerifyOnly) {
    if ([string]::IsNullOrWhiteSpace($CertificatePassword)) {
        $CertificatePassword = $env:NV_SIGN_PASSWORD
    }
    $arguments = New-Object System.Collections.Generic.List[string]
    $arguments.Add('sign')
    $arguments.Add('/fd'); $arguments.Add('SHA256')
    if (-not [string]::IsNullOrWhiteSpace($TimestampUrl)) {
        $arguments.Add('/tr'); $arguments.Add($TimestampUrl)
        $arguments.Add('/td'); $arguments.Add('SHA256')
    }
    if (-not [string]::IsNullOrWhiteSpace($CertificatePath)) {
        $pfx = (Resolve-Path -LiteralPath $CertificatePath).Path
        $arguments.Add('/f'); $arguments.Add($pfx)
        if (-not [string]::IsNullOrWhiteSpace($CertificatePassword)) {
            $arguments.Add('/p'); $arguments.Add($CertificatePassword)
        }
    } elseif (-not [string]::IsNullOrWhiteSpace($CertificateThumbprint)) {
        $arguments.Add('/sha1'); $arguments.Add(($CertificateThumbprint -replace '\s', ''))
        $arguments.Add('/s'); $arguments.Add($CertificateStore)
    } else {
        throw 'supply -CertificatePath or -CertificateThumbprint (or use -VerifyOnly)'
    }
    foreach ($file in $files) { $arguments.Add($file) }

    Write-Host "Signing $($files.Count) file(s) ..."
    Invoke-SignTool -SignTool $signTool -Arguments $arguments.ToArray()
}

$failed = 0
$results = @()
foreach ($file in $files) {
    if ($Verify -or $VerifyOnly) {
        $verified = $true
        try {
            Invoke-SignTool -SignTool $signTool -Arguments @('verify', '/pa', '/v', $file) | Out-Null
        } catch {
            $verified = $false
        }
        $info = Get-SignatureInfo -Path $file
        if (-not $AllowUntrusted) {
            if (-not $verified -or $info.status -ne 'Valid') { $failed++ }
        } else {
            # Test signing with a self-signed certificate: the chain is not
            # trusted, but a signer certificate and a signature must exist.
            if ([string]::IsNullOrWhiteSpace($info.signer_thumbprint)) { $failed++ }
        }
        $results += [pscustomobject]@{
            file = $file
            status = $info.status
            signer = $info.signer_subject
            thumbprint = $info.signer_thumbprint
            timestamp = $info.timestamp_subject
            trusted = $verified
        }
    } else {
        $info = Get-SignatureInfo -Path $file
        $results += [pscustomobject]@{
            file = $file
            status = $info.status
            signer = $info.signer_subject
            thumbprint = $info.signer_thumbprint
            timestamp = $info.timestamp_subject
            trusted = $null
        }
    }
}

$results | Format-Table -AutoSize
if ($failed -ne 0) {
    throw "$failed file(s) failed signature verification"
}
Write-Host "OK: signature $(if ($Verify -or $VerifyOnly) { 'verified' } else { 'applied' }) for $($files.Count) file(s)"
