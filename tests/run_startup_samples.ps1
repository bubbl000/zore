#requires -Version 5.1
<#
.SYNOPSIS
    Steady-state startup sampler: launches the app N times and reports per-phase
    minimum/median from its own startup log lines.

.DESCRIPTION
    The first launch after a rebuild is often slowed by antivirus scanning, so
    this script reports min/median rather than mean. Phases come from
    "startup: <name>=<ms>ms" and "renderer: <name>=<ms>ms" log lines.
#>
param(
    [string]$App = (Join-Path (Split-Path -Parent $PSScriptRoot) 'build\Release\zoreview.exe'),
    [string]$Image = (Join-Path $PSScriptRoot 'output\4k.jpg'),
    [int]$Iterations = 8,
    [int]$DwellMs = 2400
)

$ErrorActionPreference = 'Stop'
if (-not (Test-Path $App)) { throw "App not found: $App" }
if (-not (Test-Path $Image)) { throw "Image not found: $Image" }

$log = Join-Path $env:LOCALAPPDATA 'ZoreView\logs\zoreview.log'
$samples = New-Object System.Collections.Generic.List[object]

for ($i = 1; $i -le $Iterations; $i++) {
    $start = 0
    if (Test-Path $log) { $start = (Get-Item $log).Length }

    $p = Start-Process -FilePath $App -ArgumentList "`"$Image`"" -PassThru
    Start-Sleep -Milliseconds $DwellMs
    [void]$p.CloseMainWindow()
    if (-not $p.WaitForExit(4000)) { $p.Kill() | Out-Null }
    Start-Sleep -Milliseconds 250

    if (-not (Test-Path $log)) { continue }
    $fs = [System.IO.File]::Open($log, 'Open', 'Read', 'ReadWrite')
    try {
        [void]$fs.Seek($start, 'Begin')
        $reader = New-Object System.IO.StreamReader($fs, [System.Text.Encoding]::UTF8)
        $text = $reader.ReadToEnd()
        $reader.Dispose()
    } finally {
        $fs.Dispose()
    }

    foreach ($line in ($text -split "`n")) {
        if ($line -match 'startup:\s*([A-Za-z_]+)=([0-9.]+)ms') {
            $samples.Add([pscustomobject]@{ iteration = $i; phase = 'startup:' + $Matches[1]; ms = [double]$Matches[2] })
        } elseif ($line -match 'renderer:\s*([A-Za-z_]+)=([0-9.]+)ms') {
            $samples.Add([pscustomobject]@{ iteration = $i; phase = 'renderer:' + $Matches[1]; ms = [double]$Matches[2] })
        }
    }
}

if ($samples.Count -eq 0) { throw 'No startup phase log lines were found.' }

$summary = foreach ($group in ($samples | Group-Object phase)) {
    $values = @($group.Group | ForEach-Object { $_.ms } | Sort-Object)
    [pscustomobject]@{
        phase = $group.Name
        n = $values.Count
        min = [math]::Round($values[0], 1)
        median = [math]::Round($values[[int][math]::Floor($values.Count / 2)], 1)
        max = [math]::Round($values[$values.Count - 1], 1)
    }
}

$summary | Sort-Object { $_.median } -Descending | Format-Table -AutoSize
