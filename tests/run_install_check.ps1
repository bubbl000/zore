#requires -Version 5.1
<#
.SYNOPSIS
    M3-2b: portable mode, optional file associations, installer and packaging.

.DESCRIPTION
    Runs zoreview_installtests.exe (portable paths, ProgID/OpenWith/default
    takeover with backup+restore, per-user install/uninstall, machine-scope
    rejection), verifies that no per-user Shell state is left behind, then
    builds both the portable and installer packages through tools\package.ps1
    and checks their contents and checksums.

.EXAMPLE
    powershell -ExecutionPolicy Bypass -File tests\run_install_check.ps1
#>
param(
    [string]$Exe = (Join-Path (Split-Path -Parent $PSScriptRoot) 'build\Release\zoreview_installtests.exe'),
    [string]$App = (Join-Path (Split-Path -Parent $PSScriptRoot) 'build\Release\zoreview.exe'),
    [string]$Dll = (Join-Path (Split-Path -Parent $PSScriptRoot) 'build\Release\ZoreViewThumb.dll')
)

$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot
if (-not (Test-Path -LiteralPath $Exe)) { throw "test harness not found: $Exe" }
if (-not (Test-Path -LiteralPath $App)) { throw "app not found: $App" }
if (-not (Test-Path -LiteralPath $Dll)) { throw "thumbnail DLL not found: $Dll" }

$previousOutput = [Console]::OutputEncoding
[Console]::OutputEncoding = [Text.Encoding]::UTF8
$failures = 0
$dist = Join-Path $root 'tests\output\dist-check'

function Assert([bool]$Condition, [string]$What) {
    if ($Condition) {
        Write-Host "OK   $What"
    } else {
        Write-Host "FAIL $What"
        $script:failures++
    }
}

try {
    Push-Location $root
    try {
        & $Exe --exe $App --dll $Dll
        if ($LASTEXITCODE -ne 0) { $failures++ }
    } finally {
        Pop-Location
    }

    # Per-user Shell state must be back to its pre-test shape.
    Assert (-not (Test-Path 'HKCU:\Software\Classes\ZoreView.Image')) `
        'no ZoreView.Image ProgID remains'
    Assert (-not (Test-Path 'HKCU:\Software\ZoreView\Associations')) `
        'no association backup remains'
    Assert (-not (Test-Path 'HKCU:\Software\ZoreView\Install')) `
        'no install metadata remains'
    Assert (-not (Test-Path 'HKCU:\Software\Microsoft\Windows\CurrentVersion\Uninstall\ZoreView')) `
        'no Apps & Features entry remains'
    Assert (-not (Test-Path 'HKCU:\Software\Classes\CLSID\{7B2E5C1A-9F3D-4E8A-8C21-5D6F0A1B2C3D}')) `
        'no thumbnail CLSID remains'
    $shortcut = Join-Path ([Environment]::GetFolderPath('Programs')) 'ZoreView.lnk'
    Assert (-not (Test-Path -LiteralPath $shortcut)) 'no Start menu shortcut remains'

    $leftovers = @(Get-ChildItem 'HKCU:\Software\Classes\SystemFileAssociations' -ErrorAction SilentlyContinue |
        ForEach-Object { Get-ChildItem $_.PSPath -ErrorAction SilentlyContinue } |
        Where-Object { $_.PSChildName -eq 'shell' } |
        ForEach-Object { Get-ChildItem $_.PSPath -ErrorAction SilentlyContinue } |
        Where-Object { $_.PSChildName -like 'ZoreView.*' })
    Assert ($leftovers.Count -eq 0) 'no ZoreView verb keys remain'

    # Packaging: portable package + installer package.
    if (Test-Path -LiteralPath $dist) { Remove-Item -LiteralPath $dist -Recurse -Force }
    New-Item -ItemType Directory -Path $dist -Force | Out-Null
    & powershell -ExecutionPolicy Bypass -File tools\package.ps1 -Mode portable -OutputDirectory $dist | Out-Host
    if ($LASTEXITCODE -ne 0) { $failures++ }

    $portableDir = @(Get-ChildItem -LiteralPath $dist -Directory |
        Where-Object { $_.Name -like 'ZoreView-*-portable' }) | Select-Object -First 1
    Assert ($null -ne $portableDir) 'portable package directory exists'
    if ($null -ne $portableDir) {
        foreach ($name in @('zoreview.exe', 'ZoreViewThumb.dll', 'ZoreView.portable', 'NOTICE.md', 'SHA256.txt', 'README.txt')) {
            Assert (Test-Path -LiteralPath (Join-Path $portableDir.FullName $name)) "portable package contains $name"
        }
        $zip = "$($portableDir.FullName).zip"
        Assert (Test-Path -LiteralPath $zip) 'portable zip exists'
        Assert (Test-Path -LiteralPath "$zip.sha256") 'portable zip checksum exists'
        if (Test-Path -LiteralPath "$zip.sha256") {
            $expected = (Get-Content -LiteralPath "$zip.sha256" -Raw).Split(' ')[0]
            Assert ($expected -eq (Get-FileHash -LiteralPath $zip -Algorithm SHA256).Hash.ToLowerInvariant()) `
                'portable zip checksum matches'
        }

        $report = Join-Path $dist 'portable-paths.txt'
        $process = Start-Process -FilePath (Join-Path $portableDir.FullName 'zoreview.exe') `
            -ArgumentList @('--print-paths', $report) -Wait -PassThru
        Assert ($process.ExitCode -eq 0) 'packaged portable executable runs --print-paths'
        if (Test-Path -LiteralPath $report) {
            $lines = @{}
            Get-Content -LiteralPath $report -Encoding UTF8 | ForEach-Object {
                $parts = $_.Split('=', 2)
                if ($parts.Count -eq 2) { $lines[$parts[0]] = $parts[1] }
            }
            Assert ($lines['portable'] -eq '1') 'packaged copy is portable'
            Assert ($lines['data'] -eq $portableDir.FullName) 'packaged copy keeps data next to the executable'
        }
    }

    & powershell -ExecutionPolicy Bypass -File tools\package.ps1 -Mode installer -OutputDirectory $dist | Out-Host
    if ($LASTEXITCODE -ne 0) { $failures++ }
    $installerDir = @(Get-ChildItem -LiteralPath $dist -Directory |
        Where-Object { $_.Name -like 'ZoreView-*-installer' }) | Select-Object -First 1
    Assert ($null -ne $installerDir) 'installer package directory exists'
    if ($null -ne $installerDir) {
        foreach ($name in @('zoreview.exe', 'ZoreViewThumb.dll', 'NOTICE.md', 'INSTALL.txt', 'Install.cmd', 'Uninstall.cmd', 'SHA256.txt')) {
            Assert (Test-Path -LiteralPath (Join-Path $installerDir.FullName $name)) "installer package contains $name"
        }
    }
} finally {
    # Safety net: undo anything the harness could not clean up.
    foreach ($arguments in @('--unregister-shell', '--unregister-assoc')) {
        try {
            Start-Process -FilePath $App -ArgumentList $arguments -Wait -WindowStyle Hidden | Out-Null
        } catch {
            Write-Host "cleanup failed: $arguments"
        }
    }
    [Console]::OutputEncoding = $previousOutput
}

if ($failures -gt 0) {
    Write-Host "FAIL: $failures install/association check(s)"
    exit 1
}
Write-Host 'OK: portable mode, associations, installer and packaging'
