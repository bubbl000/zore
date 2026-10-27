#requires -Version 5.1
<#
  V3.1-07 S2 startup-gap probe (measurement only; no product code changes).

  Quantifies the one gap S2 left: the *startup* load never consults any cache
  (HANDOFF pitfall 340), so a second launch pays a full decode even when the disk
  already holds that file's display tier. The only way to get a cache hit across
  a restart is a non-startup load (navigate away and back).

  Runs against one copied exe in one --portable directory (shared disk cache,
  stable sample paths, so the tier keys match across processes):

    1 populate         start img00, Right, Right   -> writes tiers for img01/img02
    2 startup-on-tier  start img01 (it HAS a tier) -> expect no cache hit
    3 revisit-disk     start img00, Right with prefetch/prewarm OFF
                       -> img01 must be served from the disk tier
    4 revisit-memory   start img00, Right, Left     -> in-process memory hits

  Output: <Out>\*.log plus summary.md/csv. ASCII only; keep the UTF-8 BOM.

  Example:
    powershell -ExecutionPolicy Bypass -File run_startup_cache_probe.ps1
#>
param(
  [string]$Samples = 'E:\temp\nv-v3-07-samples\24mp25',
  [string]$Out = ''
)

$ErrorActionPreference = 'Stop'
$here = $PSScriptRoot
$newRoot = (Resolve-Path (Join-Path $here '..\..\..')).Path
$exeSource = Join-Path $newRoot 'build\Release\zoreview.exe'

if (-not $Out) {
  $Out = Join-Path (Join-Path $here '..\..\output\startup-cache') (Get-Date -Format 'yyyyMMdd-HHmmss')
}
New-Item -ItemType Directory -Force $Out | Out-Null
$work = Join-Path $Out 'work'
$images = Join-Path $work 'images'
New-Item -ItemType Directory -Force $work, $images | Out-Null
Copy-Item $exeSource (Join-Path $work 'zoreview.exe') -Force

$files = Get-ChildItem -Path $Samples -Filter '*.jpg' | Sort-Object Name | Select-Object -First 6
foreach ($file in $files) {
  $target = Join-Path $images $file.Name
  if (-not (Test-Path $target)) { New-Item -ItemType HardLink -Path $target -Value $file.FullName | Out-Null }
}
if ($files.Count -lt 3) { throw "need at least 3 jpg samples in $Samples" }
Write-Host ("samples: " + (($files | ForEach-Object { $_.Name }) -join ', '))

$img0 = (Join-Path $images $files[0].Name)
$img1 = (Join-Path $images $files[1].Name)

function Invoke-Run([string]$name, [string]$image, [string[]]$sendKeys, [string[]]$switches, [int]$quitAt) {
  $exe = Join-Path $work 'zoreview.exe'
  $log = Join-Path $work 'logs\zoreview.log'
  Remove-Item $log -Force -ErrorAction SilentlyContinue
  $arguments = @('--portable', '--language', 'en', '--quit-at', "$quitAt")
  $arguments += $switches
  if ($sendKeys.Count -gt 0) {
    $arguments += @('--send-keys', ($sendKeys -join ','), '--send-keys-interval', '1200')
  }
  $arguments += $image
  $process = Start-Process $exe -ArgumentList $arguments -PassThru
  if (-not $process.WaitForExit(60000)) {
    $process.Kill()
    throw "${name}: timeout"
  }
  Start-Sleep -Milliseconds 300
  $copy = Join-Path $Out ($name + '.log')
  Copy-Item $log $copy -Force
  return $copy
}

function Get-FirstLoadMs([string]$logPath) {
  $line = Get-Content $logPath | Select-String -Pattern 'load: done ms=' | Select-Object -First 1
  if (-not $line) { return $null }
  $match = [regex]::Match($line.Line, 'load: done ms=(\d+)')
  if ($match.Success) { return [int]$match.Groups[1].Value }
  return $null
}

function Get-Lines([string]$logPath, [string]$pattern) {
  return (Get-Content $logPath | Select-String -Pattern $pattern)
}

$results = @()

Write-Host '=== 1 populate (img00 -> Right -> Right)'
$log = Invoke-Run 'populate' $img0 @('Right', 'Right') @() 18000
$results += [pscustomobject]@{ Run = 'populate'; FirstLoadMs = (Get-FirstLoadMs $log); Hits = 0; DiskTierReads = (Get-Lines $log 'tier disk hit').Count }

Write-Host '=== 2 startup on img01, which has a disk tier'
$log = Invoke-Run 'startup-on-tier' $img1 @() @() 11000
$hits = Get-Lines $log 'display cache hit|memory cache hit'
$tierReads = Get-Lines $log 'tier disk hit|disk cache hit'
$results += [pscustomobject]@{ Run = 'startup-on-tier'; FirstLoadMs = (Get-FirstLoadMs $log); Hits = $hits.Count; DiskTierReads = $tierReads.Count }
if ($hits.Count -gt 0) { $hits | ForEach-Object { Write-Host ('  unexpected hit: ' + $_.Line) } }

Write-Host '=== 3 revisit across restart with prefetch/prewarm OFF (img00 -> Right)'
$log = Invoke-Run 'revisit-disk' $img0 @('Right') @('--prefetch=off', '--thumb-prewarm=off') 14000
$hits = Get-Lines $log 'display cache hit'
$results += [pscustomobject]@{ Run = 'revisit-disk'; FirstLoadMs = (Get-FirstLoadMs $log); Hits = $hits.Count; DiskTierReads = (Get-Lines $log 'tier disk hit').Count }
$hits | ForEach-Object { Write-Host ('  ' + $_.Line) }

Write-Host '=== 4 revisit in one process (img00 -> Right -> Left)'
$log = Invoke-Run 'revisit-memory' $img0 @('Right', 'Left') @() 14000
$hits = Get-Lines $log 'display cache hit|memory cache hit'
$results += [pscustomobject]@{ Run = 'revisit-memory'; FirstLoadMs = (Get-FirstLoadMs $log); Hits = $hits.Count; DiskTierReads = (Get-Lines $log 'tier disk hit').Count }
$hits | ForEach-Object { Write-Host ('  ' + $_.Line) }

$csv = Join-Path $Out 'startup-cache.csv'
$results | Export-Csv -Path $csv -NoTypeInformation -Encoding utf8

$md = Join-Path $Out 'summary.md'
$lines = @('# V3.1-07 S2 startup-cache probe', '', "samples=$Samples", '',
           '| run | first load ms | display/memory hits | disk tier reads |', '| --- | --- | --- | --- |')
foreach ($row in $results) {
  $lines += ('| {0} | {1} | {2} | {3} |' -f $row.Run, $row.FirstLoadMs, $row.Hits, $row.DiskTierReads)
}
$lines += ''
$lines += 'Run 1 populates the disk tiers for img01/img02 (its own hits are not counted).'
$lines += 'Run 2 proves the startup load ignores an existing tier (pitfall 340).'
$lines += 'Run 3 proves the same tier is served once the load is not the startup one.'
$lines | Set-Content -Path $md -Encoding utf8

$results | Format-Table -AutoSize | Out-String | Write-Host
Write-Host "csv: $csv"
Write-Host "md:  $md"
