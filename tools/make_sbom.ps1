#requires -Version 5.1
<#
.SYNOPSIS
    Generate a CycloneDX 1.5 SBOM for a ZoreView build or release package.

.DESCRIPTION
    Reads the shared version macros and the vendored third-party libraries,
    hashes the shipped binaries, and writes a deterministic CycloneDX JSON file
    (serial number derived from version + artifact hashes). The output covers
    the CISA SBOM minimum elements: supplier, component name, version, unique
    identifiers (purl/bom-ref), dependency relationships, author and timestamp.

.EXAMPLE
    powershell -ExecutionPolicy Bypass -File tools\make_sbom.ps1
    # writes build\Release\SBOM.cdx.json

.EXAMPLE
    powershell -ExecutionPolicy Bypass -File tools\make_sbom.ps1 `
        -ArtifactDirectory dist\ZoreView-0.3.0-portable -OutputPath dist\ZoreView-0.3.0-portable\SBOM.cdx.json
#>
param(
    [string]$ArtifactDirectory = '',
    [string]$OutputPath = '',
    [string]$Version = '',
    [string]$Timestamp = ''
)

$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot
if ([string]::IsNullOrWhiteSpace($ArtifactDirectory)) {
    $ArtifactDirectory = Join-Path $root 'build\Release'
}
if ([string]::IsNullOrWhiteSpace($Version)) {
    $versionText = Get-Content -LiteralPath (Join-Path $root 'src\core\version_numbers.h') -Raw
    $match = [regex]::Match($versionText, '#define\s+NV_VERSION_STRING\s+"([^"]+)"')
    if (-not $match.Success) {
        throw 'cannot read NV_VERSION_STRING from src\core\version_numbers.h'
    }
    $Version = $match.Groups[1].Value
}
if ([string]::IsNullOrWhiteSpace($OutputPath)) {
    $OutputPath = Join-Path $ArtifactDirectory 'SBOM.cdx.json'
}
if ([string]::IsNullOrWhiteSpace($Timestamp)) {
    $Timestamp = [DateTime]::UtcNow.ToString('yyyy-MM-ddTHH:mm:ssZ')
}

function Get-Sha256([string]$Path) {
    if (-not (Test-Path -LiteralPath $Path)) {
        throw "missing file for SBOM: $Path"
    }
    return (Get-FileHash -LiteralPath $Path -Algorithm SHA256).Hash.ToLowerInvariant()
}

$exe = Join-Path $ArtifactDirectory 'zoreview.exe'
$dll = Join-Path $ArtifactDirectory 'ZoreViewThumb.dll'
$exeHash = Get-Sha256 $exe
$dllHash = Get-Sha256 $dll

$freeImageLib = Join-Path $root 'third_party\lib\freeimage.lib'
$libRawLib = Join-Path $root 'third_party\lib\libraw.lib'
$jpegLib = Join-Path $root 'third_party\lib\zoreview_jpeg.lib'
$freeImageHash = Get-Sha256 $freeImageLib
$libRawHash = Get-Sha256 $libRawLib
$jpegHash = Get-Sha256 $jpegLib

$appRef = "pkg:generic/zoreview@${Version}?platform=windows&arch=x64"
$freeImageRef = 'pkg:generic/freeimage@3.18.0'
$libRawRef = 'pkg:generic/libraw@0.21.5'
$jpegRef = 'pkg:generic/libjpeg@9c'
$zlibRef = 'pkg:generic/zlib@1.2.11'

# Deterministic UUID (version 5 style bits) from the build identity so two
# SBOMs for the same binaries are identical apart from the timestamp.
$seed = "$Version|$exeHash|$dllHash|$freeImageHash|$libRawHash|$jpegHash"
$sha = [System.Security.Cryptography.SHA256]::Create()
try {
    $bytes = $sha.ComputeHash([System.Text.Encoding]::UTF8.GetBytes($seed))
} finally {
    $sha.Dispose()
}
$uuidBytes = New-Object 'byte[]' 16
[Array]::Copy($bytes, $uuidBytes, 16)
$uuidBytes[6] = [byte](($uuidBytes[6] -band 0x0F) -bor 0x50)
$uuidBytes[8] = [byte](($uuidBytes[8] -band 0x3F) -bor 0x80)
$serial = 'urn:uuid:' + (New-Object Guid (,$uuidBytes)).ToString('D')

$properties = @(
    [ordered]@{ name = 'zoreview:artifact.zoreview.exe'; value = $exeHash },
    [ordered]@{ name = 'zoreview:artifact.ZoreViewThumb.dll'; value = $dllHash },
    [ordered]@{ name = 'zoreview:version-source'; value = 'src/core/version_numbers.h' },
    [ordered]@{ name = 'zoreview:runtime'; value = 'Windows 10/11 x64; WIC, D3D11, Direct2D, DXGI, Shell APIs' },
    [ordered]@{ name = 'zoreview:network'; value = 'none; no telemetry' }
)

$components = @(
    [ordered]@{
        type = 'library'
        'bom-ref' = $freeImageRef
        name = 'FreeImage'
        version = '3.18.0'
        supplier = [ordered]@{ name = 'FreeImage project' }
        purl = $freeImageRef
        licenses = @([ordered]@{ license = [ordered]@{ id = 'FIPL-1.0' } })
        hashes = @([ordered]@{ alg = 'SHA-256'; content = $freeImageHash })
        properties = @(
            [ordered]@{ name = 'zoreview:path'; value = 'third_party/lib/freeimage.lib' },
            [ordered]@{ name = 'zoreview:usage'; value = 'fallback decoding (ICO/CUR, TGA, PCX, PNM, XBM, XPM)' }
        )
    },
    [ordered]@{
        type = 'library'
        'bom-ref' = $jpegRef
        name = 'libjpeg'
        version = '9c'
        supplier = [ordered]@{ name = 'Independent JPEG Group' }
        purl = $jpegRef
        licenses = @([ordered]@{ license = [ordered]@{ id = 'IJG' } })
        hashes = @([ordered]@{ alg = 'SHA-256'; content = $jpegHash })
        properties = @(
            [ordered]@{ name = 'zoreview:path'; value = 'third_party/lib/zoreview_jpeg.lib' },
            [ordered]@{ name = 'zoreview:usage'; value = 'JPEG codec inside the FreeImage archive' }
        )
    },
    [ordered]@{
        type = 'library'
        'bom-ref' = $libRawRef
        name = 'LibRaw'
        version = '0.21.5'
        supplier = [ordered]@{ name = 'LibRaw LLC' }
        purl = $libRawRef
        licenses = @([ordered]@{ expression = 'LGPL-2.1-only OR CDDL-1.0' })
        hashes = @([ordered]@{ alg = 'SHA-256'; content = $libRawHash })
        properties = @(
            [ordered]@{ name = 'zoreview:path'; value = 'third_party/lib/libraw.lib' },
            [ordered]@{ name = 'zoreview:usage'; value = 'camera RAW decoding (CR2/CR3/NEF/ARW/RAF/ORF/RW2/PEF/SRW/DNG)' },
            [ordered]@{ name = 'zoreview:linkage'; value = 'static; LGPL relinking review required before release' }
        )
    },
    [ordered]@{
        type = 'library'
        'bom-ref' = $zlibRef
        name = 'zlib'
        version = '1.2.11'
        supplier = [ordered]@{ name = 'zlib project' }
        purl = $zlibRef
        licenses = @([ordered]@{ license = [ordered]@{ id = 'Zlib' } })
        properties = @(
            [ordered]@{ name = 'zoreview:path'; value = 'third_party/zlib' },
            [ordered]@{ name = 'zoreview:usage'; value = 'zlib inflate for PSD ZIP / ZIP+prediction' }
        )
    }
)

$dependencies = @(
    [ordered]@{
        ref = $appRef
        dependsOn = @($freeImageRef, $jpegRef, $libRawRef, $zlibRef)
    }
)
foreach ($component in $components) {
    $dependencies += [ordered]@{ ref = $component['bom-ref']; dependsOn = @() }
}

$document = [ordered]@{
    bomFormat = 'CycloneDX'
    specVersion = '1.5'
    serialNumber = $serial
    version = 1
    metadata = [ordered]@{
        timestamp = $Timestamp
        authors = @([ordered]@{ name = 'ZoreView contributors' })
        tools = @([ordered]@{ vendor = 'ZoreView'; name = 'make_sbom.ps1'; version = '1.0' })
        component = [ordered]@{
            type = 'application'
            'bom-ref' = $appRef
            name = 'ZoreView'
            version = $Version
            supplier = [ordered]@{ name = 'ZoreView contributors' }
            licenses = @([ordered]@{ license = [ordered]@{ id = 'GPL-3.0-only' } })
            purl = $appRef
            hashes = @([ordered]@{ alg = 'SHA-256'; content = $exeHash })
            properties = $properties
        }
    }
    components = $components
    dependencies = $dependencies
}

$json = $document | ConvertTo-Json -Depth 12
$outputDirectory = Split-Path -Parent $OutputPath
if ($outputDirectory -and -not (Test-Path -LiteralPath $outputDirectory)) {
    New-Item -ItemType Directory -Path $outputDirectory -Force | Out-Null
}
$utf8 = New-Object System.Text.UTF8Encoding($false)
[System.IO.File]::WriteAllText($OutputPath, $json + "`r`n", $utf8)
Write-Host "OK: $OutputPath"
Write-Host "    version=$Version serial=$serial components=$($components.Count)"
exit 0
