#requires -Version 5.1
<#
  V3.1 switch check (規劃/v3.1/01 §V3.1-01, 00 §4).

  Wraps the V3-07 harness for the only scenario V3.1 changes: a navigation burst
  (200 ms per key). Runs ours only, one cold hard-linked sample directory per
  repeat, records the window, then checks distinct frames / cancels / T3 / peak
  private against the V3.1-01 targets.

  ASCII only (Windows PowerShell 5.1 reads BOM-less scripts as ANSI).

  Example:
    powershell -ExecutionPolicy Bypass -File tests\tools\switch-speed\run_v31_switch_check.ps1
    powershell -ExecutionPolicy Bypass -File ...\run_v31_switch_check.ps1 -Sets 24mp12 -Repeats 1

  Exit code 0 = every target met; 1 = at least one failed (details printed).
#>
param(
  [string]$Sets = '24mp25',
  [string]$Scenarios = 's3,s4',
  [int]$Repeats = 3,
  [string]$Samples = 'E:\temp\nv-v3-07-samples',
  [string]$Out = '',
  [switch]$SkipSlow
)

$ErrorActionPreference = 'Stop'
$here = $PSScriptRoot
$python = (Get-Command python -ErrorAction Stop).Source

if (-not $Out) {
  $outputRoot = (Resolve-Path (Join-Path $here '..\..\output\switch-speed')).Path
  $Out = Join-Path $outputRoot ('v31-' + (Get-Date -Format 'yyyyMMdd-HHmmss'))
}
New-Item -ItemType Directory -Force $Out | Out-Null
Write-Host "out: $Out"

# 1. Recorded burst runs (the harness owns the cold cache, key injection, video
#    capture, window placement and the peak-memory sampler).
& (Join-Path $here 'run_switch_speed.ps1') -Apps 'ours' -Sets $Sets `
  -Scenarios $Scenarios -Repeats $Repeats -Samples $Samples -Out $Out

# 2. Per-key T3/T4 and frames.csv from the videos (slow runs, when present,
#    seed the ROI; a burst-only run derives it from the stable frames).
& $python (Join-Path $here 'analyze_all.py') --out $Out
if ($LASTEXITCODE -ne 0) { throw "analyze_all.py failed ($LASTEXITCODE)" }

# 3. V3.1 targets.
& $python (Join-Path $here 'v31_metrics.py') --out $Out --set $Sets `
  --scenarios $Scenarios --markdown (Join-Path $Out 'v31-metrics.md')
$metricsExit = $LASTEXITCODE

if (-not $SkipSlow) {
  # S1/S2 non-regression: slow paging must not enter the burst and must stay a
  # cache hit (T2 ~ 0). Reuses the same harness with one slow run per repeat.
  $slowOut = Join-Path $Out 'slow'
  & (Join-Path $here 'run_switch_speed.ps1') -Apps 'ours' -Sets $Sets `
    -Scenarios 'slow' -Repeats $Repeats -Samples $Samples -Out $slowOut
  & $python (Join-Path $here 'analyze_all.py') --out $slowOut
  Write-Host "slow-check tables: $(Join-Path $slowOut 'aggregate.csv')"
}

exit $metricsExit
