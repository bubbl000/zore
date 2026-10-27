#requires -Version 5.1
<#
.SYNOPSIS
    Measures app memory for one image: peak working set / private bytes plus the
    app's own memory and prefetch log lines.

.DESCRIPTION
    Launches the app in --screenshot mode, polls the process every 50 ms until it
    exits, and prints:
      - peak working set / private bytes observed externally;
      - the app's "memory: first_image" line;
      - how many neighbours were prefetched vs skipped as too large.

    Used for the M1-3 8K checks; not an assertion test by itself.
#>
param(
    [Parameter(Mandatory = $true)][string]$Image,
    [string]$Exe = '',
    [string]$Out = '',
    [int]$DwellMs = 4000,
    [string]$ExtraArgs = ''
)

$ErrorActionPreference = 'Stop'
if (-not $Exe) { $Exe = Join-Path (Split-Path -Parent $PSScriptRoot) 'build\Release\zoreview.exe' }
if (-not $Out) { $Out = Join-Path $PSScriptRoot 'output\memory-check.png' }
if (-not (Test-Path -LiteralPath $Image)) { throw "image not found: $Image" }
if (-not (Test-Path $Exe)) { throw "exe not found: $Exe" }
if (Test-Path $Out) { Remove-Item $Out -Force }

$logPath = Join-Path $env:LOCALAPPDATA 'ZoreView\logs\zoreview.log'
$logOffset = 0
if (Test-Path $logPath) { $logOffset = (Get-Item $logPath).Length }

$arguments = "--screenshot `"$Out`" --shot-delay $DwellMs"
if ($ExtraArgs) { $arguments += " $ExtraArgs" }
$arguments += " `"$Image`""
$p = Start-Process -FilePath $Exe -ArgumentList $arguments -PassThru
$peakWs = 0
$peakPrivate = 0
while (-not $p.HasExited) {
    try {
        $p.Refresh()
        if ($p.WorkingSet64 -gt $peakWs) { $peakWs = $p.WorkingSet64 }
        if ($p.PrivateMemorySize64 -gt $peakPrivate) { $peakPrivate = $p.PrivateMemorySize64 }
    } catch {
        break
    }
    Start-Sleep -Milliseconds 50
}
[void]$p.WaitForExit(10000)

$text = ''
if (Test-Path $logPath) {
    $stream = [System.IO.File]::Open($logPath, 'Open', 'Read', 'ReadWrite')
    try {
        [void]$stream.Seek($logOffset, 'Begin')
        $reader = New-Object System.IO.StreamReader($stream)
        $text = $reader.ReadToEnd()
        $reader.Dispose()
    } finally {
        $stream.Dispose()
    }
}

function Show-Matches([string]$Pattern) {
    $items = [regex]::Matches($text, $Pattern)
    foreach ($m in $items) { Write-Host ("  " + $m.Value) }
}

Write-Host ("image={0}" -f $Image)
Write-Host ("peak_ws_mb={0} peak_private_mb={1}" -f [math]::Round($peakWs / 1MB, 1), [math]::Round($peakPrivate / 1MB, 1))
Write-Host 'app log:'
Show-Matches 'memory: first_image [^\r\n]+'
Show-Matches 'renderer: set_image=[0-9.]+ms'
Show-Matches 'renderer: bitmap [^\r\n]+'
Show-Matches 'prefetched: [^\r\n]+'
Show-Matches 'prefetch skipped: [^\r\n]+'
Write-Host ("screenshot={0}" -f (Test-Path $Out))
