#requires -Version 5.1
<#
.SYNOPSIS
    Performance baseline: CLI decode plus app startup phases.

.DESCRIPTION
    App startup is measured from inside the app (--bench-startup), as process
    uptime including loader time. `app_first_frame` is the first painted frame
    (window visible), `app_first_image` is the first frame that shows decoded
    picture content. WaitForInputIdle is kept only as a reference number: it
    also observes decode/thumbnail completion messages, so it is an upper bound.
#>
param(
    [string]$Cli = (Join-Path (Split-Path -Parent $PSScriptRoot) 'build\Release\zoreview_cli.exe'),
    [string]$App = (Join-Path (Split-Path -Parent $PSScriptRoot) 'build\Release\zoreview.exe'),
    [string]$Image4k = (Join-Path $PSScriptRoot 'output\4k.jpg'),
    [int]$Iterations = 5,
    [string]$Out = (Join-Path $PSScriptRoot 'output\perf.csv')
)

$ErrorActionPreference = 'Stop'
if (-not (Test-Path $Cli)) { throw "CLI not found: $Cli" }
if (-not (Test-Path $App)) { throw "App not found: $App" }
if (-not (Test-Path $Image4k)) { throw "4K image not found: $Image4k" }

function Read-BenchStartup([string]$Path) {
    $map = @{}
    if (-not (Test-Path -LiteralPath $Path)) { return $map }
    foreach ($line in (Get-Content -LiteralPath $Path -ErrorAction SilentlyContinue)) {
        $parts = $line -split ','
        if ($parts.Count -eq 2) {
            $value = 0.0
            if ([double]::TryParse($parts[1], [Globalization.NumberStyles]::Float,
                                   [Globalization.CultureInfo]::InvariantCulture, [ref]$value)) {
                $map[$parts[0]] = $value
            }
        }
    }
    return $map
}

$rows = New-Object System.Collections.Generic.List[object]
$phaseRows = New-Object System.Collections.Generic.List[object]
$tempOut = Join-Path $env:TEMP 'nv_perf.bgra'

for ($i = 1; $i -le $Iterations; $i++) {
    $sw = [System.Diagnostics.Stopwatch]::StartNew()
    $text = & $Cli --decode $Image4k --out $tempOut 2>&1
    $sw.Stop()
    $decodeMs = ($text | Select-String -Pattern '^decode_ms:\s*(\d+)' | ForEach-Object { $_.Matches[0].Groups[1].Value } | Select-Object -First 1)
    $rows.Add([pscustomobject]@{
        kind = 'cli_decode_4k'
        iteration = $i
        elapsed_ms = [math]::Round($sw.Elapsed.TotalMilliseconds, 1)
        reported_ms = if ($decodeMs) { [int]$decodeMs } else { '' }
    })
}

for ($i = 1; $i -le $Iterations; $i++) {
    $bench = Join-Path $env:TEMP "nv_startup_$PID`_$i.txt"
    Remove-Item -LiteralPath $bench -Force -ErrorAction SilentlyContinue

    $sw = [System.Diagnostics.Stopwatch]::StartNew()
    $p = Start-Process -FilePath $App `
        -ArgumentList "`"$Image4k`" --bench-startup `"$bench`"" -PassThru
    [void]$p.WaitForInputIdle(5000)
    $sw.Stop()
    $inputIdleMs = [math]::Round($sw.Elapsed.TotalMilliseconds, 1)

    # WaitForInputIdle can return before the decode completion repaint.
    $deadline = [DateTime]::UtcNow.AddSeconds(8)
    $phases = @{}
    do {
        $phases = Read-BenchStartup $bench
        if ($phases.ContainsKey('first_image')) { break }
        Start-Sleep -Milliseconds 15
    } while ([DateTime]::UtcNow -lt $deadline)

    $firstFrame = if ($phases.ContainsKey('first_frame')) { [math]::Round($phases['first_frame'], 1) } else { '' }
    $firstImage = if ($phases.ContainsKey('first_image')) { [math]::Round($phases['first_image'], 1) } else { '' }

    $rows.Add([pscustomobject]@{
        kind = 'app_first_frame'
        iteration = $i
        elapsed_ms = $firstFrame
        reported_ms = ''
    })
    $rows.Add([pscustomobject]@{
        kind = 'app_first_image'
        iteration = $i
        elapsed_ms = $firstImage
        reported_ms = ''
    })
    $rows.Add([pscustomobject]@{
        kind = 'app_input_idle'
        iteration = $i
        elapsed_ms = $inputIdleMs
        reported_ms = ''
    })

    foreach ($key in $phases.Keys) {
        $phaseRows.Add([pscustomobject]@{
            iteration = $i
            phase = $key
            ms = [math]::Round($phases[$key], 1)
        })
    }

    [void]$p.CloseMainWindow()
    Start-Sleep -Milliseconds 700
    Remove-Item -LiteralPath $bench -Force -ErrorAction SilentlyContinue
}

$outDir = Split-Path -Parent $Out
New-Item -ItemType Directory -Force -Path $outDir | Out-Null
$rows | Export-Csv -LiteralPath $Out -NoTypeInformation -Encoding UTF8

$rows | Group-Object kind | ForEach-Object {
    $values = @($_.Group | Where-Object { $_.elapsed_ms -ne '' } | ForEach-Object { [double]$_.elapsed_ms })
    if ($values.Count -eq 0) {
        Write-Host "$($_.Name): no samples"
        return
    }
    $avg = [math]::Round((($values | Measure-Object -Average).Average), 1)
    Write-Host "$($_.Name): avg=${avg}ms n=$($values.Count)"
}

if ($phaseRows.Count -gt 0) {
    Write-Host ''
    Write-Host 'startup phases (avg ms):'
    $phaseRows | Group-Object phase | Sort-Object { ($_.Group | Measure-Object ms -Average).Average } -Descending | ForEach-Object {
        $avg = [math]::Round((($_.Group | Measure-Object ms -Average).Average), 1)
        Write-Host ("  {0,-14} {1}" -f $_.Name, $avg)
    }
}
Write-Host "csv=$Out"
