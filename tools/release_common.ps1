#requires -Version 5.1
<#
    Shared helpers for the release scripts (package.ps1, sign.ps1,
    verify_package.ps1). ASCII only: Windows PowerShell 5.1 reads BOM-less
    files as ANSI, so keeping these scripts ASCII avoids encoding surprises.
#>

function Get-ProjectVersion {
    param([string]$Root)
    $versionFile = Join-Path $Root 'src\core\version_numbers.h'
    if (-not (Test-Path -LiteralPath $versionFile)) {
        throw "missing version header: $versionFile"
    }
    $text = Get-Content -LiteralPath $versionFile -Raw
    $match = [regex]::Match($text, '#define\s+NV_VERSION_STRING\s+"([^"]+)"')
    if (-not $match.Success) {
        throw "cannot read NV_VERSION_STRING from $versionFile"
    }
    return $match.Groups[1].Value
}

function Get-FileSha256 {
    param([string]$Path)
    if (-not (Test-Path -LiteralPath $Path)) {
        throw "missing file: $Path"
    }
    return (Get-FileHash -LiteralPath $Path -Algorithm SHA256).Hash.ToLowerInvariant()
}

function Find-SignTool {
    if (-not [string]::IsNullOrWhiteSpace($env:NV_SIGNTOOL)) {
        if (Test-Path -LiteralPath $env:NV_SIGNTOOL) { return $env:NV_SIGNTOOL }
        throw "NV_SIGNTOOL points to a missing file: $($env:NV_SIGNTOOL)"
    }
    $command = Get-Command signtool.exe -ErrorAction SilentlyContinue
    if ($null -ne $command) { return $command.Source }

    $kitsRoot = Join-Path ${env:ProgramFiles(x86)} 'Windows Kits\10\bin'
    if (Test-Path -LiteralPath $kitsRoot) {
        $candidates = Get-ChildItem -LiteralPath $kitsRoot -Directory -ErrorAction SilentlyContinue |
            Where-Object { $_.Name -match '^\d+\.\d+\.\d+\.\d+$' } |
            Sort-Object { [version]$_.Name } -Descending
        foreach ($candidate in $candidates) {
            $tool = Join-Path $candidate.FullName 'x64\signtool.exe'
            if (Test-Path -LiteralPath $tool) { return $tool }
        }
    }
    throw 'signtool.exe not found. Install the Windows SDK or set NV_SIGNTOOL.'
}

function Invoke-SignTool {
    param(
        [string]$SignTool,
        [string[]]$Arguments
    )
    & $SignTool @Arguments
    if ($LASTEXITCODE -ne 0) {
        throw "signtool failed with exit code $LASTEXITCODE"
    }
}

function Get-SignatureInfo {
    param([string]$Path)
    $signature = Get-AuthenticodeSignature -LiteralPath $Path
    $result = [ordered]@{
        path = $Path
        status = [string]$signature.Status
        status_message = [string]$signature.StatusMessage
        signer_subject = ''
        signer_thumbprint = ''
        timestamp_subject = ''
    }
    if ($null -ne $signature.SignerCertificate) {
        $result.signer_subject = $signature.SignerCertificate.Subject
        $result.signer_thumbprint = $signature.SignerCertificate.Thumbprint
    }
    if ($null -ne $signature.TimeStamperCertificate) {
        $result.timestamp_subject = $signature.TimeStamperCertificate.Subject
    }
    return $result
}
