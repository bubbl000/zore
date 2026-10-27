# V3.1-05 spike harness: libjpeg-turbo DCT-scaled JPEG decode vs the WIC path.
#
# Runs the standalone benchmark (tests/tools/jpeg-spike) over the v3.1 sample
# sets and records, per sample:
#   * WIC: full decode, e1280, e768, e750 (750 is the 1/8 DCT bucket boundary
#     for a 6000-px long edge: 6000/8 = 750)
#   * turbo: 1/1 (full), auto e1280 (=1/4), forced 1/8 e1280, auto e768 (=1/4),
#     forced 1/8 e768, and a --no-tail run for the entropy-decode floor
#   * quality: BGRA dumps + gradient energy via jpeg_spike_quality.py
#
# The go/no-go gate from 规划/v3.1/05 §7 is printed at the end:
#   "24 MP JPEG e1280: turbo <= 120 ms" and "<= 0.7 x WIC".
#
# Usage:
#   powershell -ExecutionPolicy Bypass -File run_jpeg_spike.ps1
#   powershell -ExecutionPolicy Bypass -File run_jpeg_spike.ps1 -Groups 24mp25 -Repeats 3

[CmdletBinding()]
param(
  [string]$Samples = 'E:\temp\nv-v3-07-samples',
  [string[]]$Groups = @('24mp25', '24mp12', '8k'),
  [int]$Repeats = 5,
  [string]$Out = ''
)

$ErrorActionPreference = 'Stop'
# `powershell -File script -Groups a,b,c` passes one string, so split it here.
$Groups = @($Groups | ForEach-Object { $_ -split ',' } | Where-Object { $_ -ne '' })
$root = Split-Path -Parent $MyInvocation.MyCommand.Path
$repo = (Resolve-Path (Join-Path $root '..\..\..')).Path
$exe = Join-Path $root 'build\Release\jpeg_spike.exe'
$quality = Join-Path $root 'jpeg_spike_quality.py'

if (-not (Test-Path $exe)) {
  throw "spike exe not found: $exe (configure+build tests/tools/jpeg-spike first)"
}
if ([string]::IsNullOrWhiteSpace($Out)) {
  $stamp = Get-Date -Format 'yyyyMMdd-HHmmss'
  $Out = Join-Path $repo "tests\output\v31-05-spike\$stamp"
}
New-Item -ItemType Directory -Force -Path $Out | Out-Null
$dumpDir = Join-Path $Out 'dumps'
New-Item -ItemType Directory -Force -Path $dumpDir | Out-Null

function Invoke-Spike {
  param([string]$File, [string]$Mode, [int]$Edge, [string]$Extra = '', [int]$Reps = $Repeats)
  $argv = @('--file', $File, '--edge', "$Edge", '--mode', $Mode, '--reps', "$Reps")
  if ($Extra -ne '') { $argv += $Extra.Split(' ') }
  $lines = & $exe @argv 2>&1
  $summary = $lines | Where-Object { $_ -like 'summary *' } | Select-Object -Last 1
  if (-not $summary) {
    throw "spike failed for $File $Mode edge=$Edge $Extra`n$($lines -join "`n")"
  }
  $run = $lines | Where-Object { $_ -like 'run *' } | Select-Object -First 1
  $median = [double]([regex]::Match($summary, 'median=([\d.]+)').Groups[1].Value)
  $min = [double]([regex]::Match($summary, 'min=([\d.]+)').Groups[1].Value)
  $size = [regex]::Match($run, 'out=(\d+x\d+)').Groups[1].Value
  return [pscustomobject]@{ Median = $median; Min = $min; Out = $size; Lines = $lines }
}

$rows = @()
$gate = @()
foreach ($group in $Groups) {
  $dir = Join-Path $Samples $group
  if (-not (Test-Path $dir)) { Write-Warning "missing sample dir: $dir"; continue }
  $files = Get-ChildItem -Path $dir -Filter '*.jpg' | Sort-Object Name | Select-Object -First 2
  foreach ($file in $files) {
    $label = "$group/$($file.Name)"
    Write-Host "=== $label ($([math]::Round($file.Length / 1MB, 1)) MB)"
    $log = Join-Path $Out ("spike-" + ($label -replace '[/\\]', '-') + '.log')
    "### $label bytes=$($file.Length)" | Set-Content -Path $log -Encoding utf8

    $cases = @(
      @{ Name = 'wic-full';       Mode = 'wic';   Edge = 0;    Extra = '' },
      @{ Name = 'wic-e1280';      Mode = 'wic';   Edge = 1280; Extra = '' },
      @{ Name = 'wic-e768';       Mode = 'wic';   Edge = 768;  Extra = '' },
      @{ Name = 'wic-e750';       Mode = 'wic';   Edge = 750;  Extra = '' },
      @{ Name = 'wic-e512';       Mode = 'wic';   Edge = 512;  Extra = '' },
      @{ Name = 'turbo-full';     Mode = 'turbo'; Edge = 0;    Extra = '--scale 1' },
      @{ Name = 'turbo-e1280-1/4'; Mode = 'turbo'; Edge = 1280; Extra = '--scale 4' },
      @{ Name = 'turbo-e1280-1/8'; Mode = 'turbo'; Edge = 1280; Extra = '--scale 8 --fill' },
      @{ Name = 'turbo-e768-1/4'; Mode = 'turbo'; Edge = 768;  Extra = '--scale 4' },
      @{ Name = 'turbo-e768-1/8'; Mode = 'turbo'; Edge = 768;  Extra = '--scale 8 --fill' },
      @{ Name = 'turbo-e512-1/8'; Mode = 'turbo'; Edge = 512;  Extra = '--scale 8' },
      @{ Name = 'turbo-decode-only-1/8'; Mode = 'turbo'; Edge = 0; Extra = '--scale 8 --no-tail --no-compare' }
    )

    foreach ($case in $cases) {
      $result = Invoke-Spike -File $file.FullName -Mode $case.Mode -Edge $case.Edge -Extra $case.Extra
      $rows += [pscustomobject]@{
        Sample = $label; Case = $case.Name; Median = $result.Median
        Min = $result.Min; Out = $result.Out
      }
      $result.Lines | Add-Content -Path $log
      Write-Host ("  {0,-22} median={1,7:N1} ms  out={2}" -f $case.Name, $result.Median, $result.Out)
    }

    # Quality dumps (1280) for the 1/4 (identical to WIC) and 1/8 variants.
    $base = ($label -replace '[/\\]', '-')
    $wicDump = Join-Path $dumpDir "$base-wic-e1280.bgra"
    $t4Dump = Join-Path $dumpDir "$base-turbo-1_4-e1280.bgra"
    $t8Dump = Join-Path $dumpDir "$base-turbo-1_8-e1280.bgra"
    $dims = ''
    foreach ($pair in @(
      @{ Path = $wicDump; Mode = 'wic'; Extra = '--fill' },
      @{ Path = $t4Dump; Mode = 'turbo'; Extra = '--scale 4 --fill' },
      @{ Path = $t8Dump; Mode = 'turbo'; Extra = '--scale 8 --fill' }
    )) {
      $argv = @('--file', $file.FullName, '--edge', '1280', '--mode', $pair.Mode,
                '--reps', '1', '--no-compare', '--out', $pair.Path)
      if ($pair.Extra -ne '') { $argv += $pair.Extra.Split(' ') }
      $lines = & $exe @argv 2>&1
      $lines | Add-Content -Path $log
      if ($pair.Path -eq $wicDump) {
        $dims = [regex]::Match(($lines -join "`n"), 'out=(\d+)x(\d+)')
      }
    }
    $width = [int]$dims.Groups[1].Value
    $height = [int]$dims.Groups[2].Value
    foreach ($variant in @(@{ Name = 'turbo-1/4'; Path = $t4Dump }, @{ Name = 'turbo-1/8'; Path = $t8Dump })) {
      $q = & python $quality --width $width --height $height --ref $wicDump --test $variant.Path --label "$label $($variant.Name)"
      $q | Add-Content -Path $log
      $q | ForEach-Object { Write-Host "  $_" }
    }

    $wic1280 = ($rows | Where-Object { $_.Sample -eq $label -and $_.Case -eq 'wic-e1280' }).Median
    foreach ($candidate in 'turbo-e1280-1/4', 'turbo-e1280-1/8') {
      $turbo = ($rows | Where-Object { $_.Sample -eq $label -and $_.Case -eq $candidate }).Median
      $gate += [pscustomobject]@{
        Sample = $label; Case = $candidate; Wic = $wic1280; Turbo = $turbo
        Ratio = [math]::Round($turbo / $wic1280, 3)
        Pass120 = ($turbo -le 120)
        Pass70pct = ($turbo -le 0.7 * $wic1280)
      }
    }
  }
}

$csv = Join-Path $Out 'spike-results.csv'
$rows | Export-Csv -Path $csv -NoTypeInformation -Encoding utf8

$gateCsv = Join-Path $Out 'spike-gate.csv'
$gate | Export-Csv -Path $gateCsv -NoTypeInformation -Encoding utf8

$md = Join-Path $Out 'summary.md'
$lines = @('# V3.1-05 jpeg spike', '', "out=$Out reps=$Repeats", '')
$lines += '| sample | case | median ms | min ms | out |'
$lines += '| --- | --- | --- | --- | --- |'
foreach ($row in $rows) {
  $lines += ('| {0} | {1} | {2:N1} | {3:N1} | {4} |' -f $row.Sample, $row.Case, $row.Median, $row.Min, $row.Out)
}
$lines += ''
$lines += '| sample | case | wic e1280 | turbo | ratio | <=120ms | <=0.7x wic |'
$lines += '| --- | --- | --- | --- | --- | --- | --- |'
foreach ($row in $gate) {
  $lines += ('| {0} | {1} | {2:N1} | {3:N1} | {4} | {5} | {6} |' -f $row.Sample, $row.Case, $row.Wic, $row.Turbo, $row.Ratio, $row.Pass120, $row.Pass70pct)
}
$lines | Set-Content -Path $md -Encoding utf8

Write-Host ''
Write-Host "results: $csv"
Write-Host "gate:    $gateCsv"
Write-Host "summary: $md"
Write-Host ''
$gate | Format-Table -AutoSize | Out-String | Write-Host
