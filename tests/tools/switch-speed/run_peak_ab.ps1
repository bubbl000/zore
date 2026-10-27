#requires -Version 5.1
<#
  V3.1-07 S4 peak A/B (measurement only; no product code changes).

  Runs the s4 burst scenario (60 keys, 200 ms) on 24mp25 with different rollback
  switches, interleaved round by round so slow drift (thermal, other windows,
  allocator state) hits every arm equally. One process per run, one cold
  hard-linked sample directory per run (run_switch_speed.ps1 owns both).

  Arms:
    default            no extra switches (prefetch depth 3, RAW neighbours on,
                       thumbnail prewarm on)
    prefetch-off       --prefetch=off
    prefetch-depth-1   --prefetch-depth=1
    thumb-prewarm-off  --thumb-prewarm=off
    floor              --prefetch=off --thumb-prewarm=off  (nothing scheduled)

  Output: <Out>\<arm>-r<round>\results.json plus peaks.csv / peaks.md.
  ASCII only; keep the UTF-8 BOM (the exe path contains CJK characters).

  Example:
    powershell -ExecutionPolicy Bypass -File run_peak_ab.ps1 -Rounds 2
#>
param(
  [string]$Sets = '24mp25',
  [string]$Scenario = 's4',
  [int]$Rounds = 2,
  [string]$Samples = 'E:\temp\nv-v3-07-samples',
  [string]$Out = ''
)

$ErrorActionPreference = 'Stop'
$here = $PSScriptRoot
$harness = Join-Path $here 'run_switch_speed.ps1'

if (-not $Out) {
  $Out = Join-Path (Join-Path $here '..\..\output\switch-speed') ('peak-ab-' + (Get-Date -Format 'yyyyMMdd-HHmmss'))
}
New-Item -ItemType Directory -Force $Out | Out-Null
Write-Host "out: $Out"

$arms = @(
  @{ Name = 'default';           Args = '' },
  @{ Name = 'prefetch-off';      Args = '--prefetch=off' },
  @{ Name = 'prefetch-depth-1';  Args = '--prefetch-depth=1' },
  @{ Name = 'thumb-prewarm-off'; Args = '--thumb-prewarm=off' },
  @{ Name = 'floor';             Args = '--prefetch=off --thumb-prewarm=off' }
)

$rows = @()
for ($round = 1; $round -le $Rounds; $round++) {
  foreach ($arm in $arms) {
    $dir = Join-Path $Out ($arm.Name + '-r' + $round)
    Write-Host ("=== round {0} arm {1} {2}" -f $round, $arm.Name, $arm.Args)
    $arguments = @{
      Apps = 'ours'; Sets = $Sets; Scenarios = $Scenario; Repeats = 1
      Samples = $Samples; Out = $dir
    }
    if ($arm.Args -ne '') { $arguments['AppArgs'] = $arm.Args }
    & $harness @arguments | Out-Host

    $resultPath = Join-Path $dir 'results.json'
    if (-not (Test-Path $resultPath)) { throw "missing $resultPath" }
    $run = Get-Content $resultPath -Raw | ConvertFrom-Json
    $logPath = Join-Path $dir 'runs\ours\*.txt'
    $rows += [pscustomobject]@{
      arm = $arm.Name
      round = $round
      peak_private_mb = [double]$run.peak_private_mb
      peak_ws_mb = [double]$run.peak_ws_mb
      run_ms = [double]$run.run_ms
    }
  }
}

$csv = Join-Path $Out 'peaks.csv'
$rows | Export-Csv -Path $csv -NoTypeInformation -Encoding utf8

$summary = $rows | Group-Object arm | ForEach-Object {
  $values = $_.Group.peak_private_mb | Sort-Object
  [pscustomobject]@{
    arm = $_.Name
    n = $values.Count
    min = $values[0]
    median = if ($values.Count % 2) { $values[[int]($values.Count / 2)] }
             else { 0.5 * ($values[$values.Count / 2 - 1] + $values[$values.Count / 2]) }
    max = $values[$values.Count - 1]
    samples = ($values -join ' / ')
  }
}

$md = Join-Path $Out 'peaks.md'
$lines = @('# V3.1-07 S4 peak A/B', '', "sets=$Sets scenario=$Scenario rounds=$Rounds", '',
           '| arm | n | min | median | max | values |', '| --- | --- | --- | --- | --- | --- |')
foreach ($row in $summary) {
  $lines += ('| {0} | {1} | {2:N1} | {3:N1} | {4:N1} | {5} |' -f $row.arm, $row.n, $row.min, $row.median, $row.max, $row.samples)
}
$lines += ''
$lines += 'All values are peak private MB, one process per run, cold hard-linked sample directory per run.'
$lines | Set-Content -Path $md -Encoding utf8

$summary | Format-Table -AutoSize | Out-String | Write-Host
Write-Host "peaks.csv: $csv"
Write-Host "peaks.md:  $md"
