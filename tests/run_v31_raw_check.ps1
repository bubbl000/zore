#requires -Version 5.1
<#
  V3.1-04a RAW switch measurement (規劃/v3.1/01 §V3.1-04).

  Drives a folder of camera RAW files with real right-arrow keys at a 1.2 s
  cadence (the same cadence as the switch-speed S1 scenario) and reads the
  evidence the design is stated in:

    * "load: done ms="      request -> final frame on screen per switch;
    * "renderer: bitmap ... bgra=NN MB"   texture upload per switch;
    * "raw: preview as final edge="       the embedded JPEG accepted as final;
    * peak private bytes sampled from the process.

  Then it runs the same folder once with --raw-preview=off (the v3.0 behaviour:
  fit always full-decodes) so the two numbers are directly comparable.

  V3.1-07 S2 adds a second group (規劃/v3.1/07 §3 S2, put-from-preview): one
  process in one portable directory that opens the same CR3 twice (startup,
  Right to the next file, Left back). The first open promotes the embedded
  preview and must store it as the fit display tier
  ("raw: preview cached as tier edge=", made by MakeDisplayTier);
  the second open must be a tier hit ("display cache hit: ... source=mem")
  instead of another ~80 ms LibRaw preview decode, and must report
  "load: done ms=" <= 20. The same sequence runs with --preview-cache=off as
  the rollback A/B, and once more with a final "1" key so the 1:1 upgrade still
  pulls the real pixels ("upgrade: full frame applied", dump raw_mode=full).

  NOTE: the memory cache is per process, so "open the same file twice" has to
  happen inside one run here. A second *process* in the same portable directory
  can only hit the disk tier, and its startup load skips the cache lookup
  entirely (the renderer does not exist yet — the documented v3.0 limitation).

  ASCII only (Windows PowerShell 5.1 reads BOM-less scripts as ANSI).
#>
param(
  [string]$App = '.\build\Release\zoreview.exe',
  [string]$Samples = '',   # 必填：指向自备 RAW 样本目录（如 D:\raw-samples）
  [int]$Count = 4,
  [string]$Out = '',
  [double]$MaxSwitchMs = 150.0,
  [double]$MaxUploadMb = 10.0,
  [double]$MaxPeakPrivateMb = 380.0
)

$ErrorActionPreference = 'Stop'
if (-not (Test-Path -LiteralPath $App)) { throw "exe not found: $App" }
$App = (Resolve-Path -LiteralPath $App).Path
if (-not (Test-Path -LiteralPath $Samples)) { throw "RAW samples not found: $Samples" }

if (-not $Out) {
  $Out = Join-Path (Join-Path $PSScriptRoot 'output') ('v31-raw-' + (Get-Date -Format 'yyyyMMdd-HHmmss'))
}
New-Item -ItemType Directory -Force $Out | Out-Null

$failures = 0
function Check([bool]$condition, [string]$name) {
  Write-Host ("{0} {1}" -f $(if ($condition) { 'ok  ' } else { 'FAIL' }), $name)
  if (-not $condition) { $script:failures++ }
}

function Run-Case([string]$label, [string[]]$extra) {
  $dir = Join-Path $Out $label
  New-Item -ItemType Directory -Force $dir | Out-Null
  $exe = Join-Path $dir 'zoreview.exe'
  Copy-Item -LiteralPath $App -Destination $exe -Force
  $shots = Join-Path $dir 'shots'
  New-Item -ItemType Directory -Force $shots | Out-Null
  $files = Get-ChildItem -LiteralPath $Samples -Filter '*.CR3' | Sort-Object Name |
    Select-Object -First ($Count + 1)
  if ($files.Count -lt 2) { throw "not enough CR3 files under $Samples" }
  $first = $null
  foreach ($file in $files) {
    $link = Join-Path $shots $file.Name
    if (-not $first) { $first = $link }
    New-Item -ItemType HardLink -Path $link -Value $file.FullName | Out-Null
  }
  $keys = (@('Right') * $Count) -join ','
  $arguments = @('--portable', '--language', 'en', '--send-keys', $keys,
                 '--send-keys-at', '2600', '--send-keys-interval', '1200',
                 '--quit-at', '20000', $first) + $extra
  $process = Start-Process -FilePath $exe -ArgumentList $arguments -PassThru
  $peak = 0
  while (-not $process.HasExited) {
    try {
      $process.Refresh()
      if ($process.PrivateMemorySize64 -gt $peak) { $peak = $process.PrivateMemorySize64 }
    } catch {}
    Start-Sleep -Milliseconds 40
  }
  $log = Join-Path $dir 'logs\zoreview.log'
  if (-not (Test-Path -LiteralPath $log)) { throw "no log for $label" }
  Copy-Item -LiteralPath $log (Join-Path $dir 'app.log') -Force
  $text = Get-Content -LiteralPath $log -Raw
  $done = @()
  foreach ($match in [regex]::Matches($text, 'load: done ms=([0-9.]+)')) {
    $done += [double]$match.Groups[1].Value
  }
  $uploads = @()
  foreach ($match in [regex]::Matches($text, 'renderer: bitmap \d+x\d+ bgra=([0-9.]+)MB')) {
    $uploads += [double]$match.Groups[1].Value
  }
  $previewFinal = ([regex]::Matches($text, 'raw: preview as final')).Count
  # V3.1-11: the camera JPEG is a placeholder now, never the final frame.
  $placeholder = ([regex]::Matches($text, 'raw: preview as placeholder')).Count
  $halfRequests = ([regex]::Matches($text, 'raw: half-size request')).Count
  $deferred = ([regex]::Matches($text, 'raw: deferred full decode')).Count
  $tierHits = ([regex]::Matches($text, 'display cache hit:')).Count
  return [ordered]@{
    label = $label
    switches = $done.Count
    done_ms = $done
    first_ms = $(if ($done.Count -gt 0) { $done[0] } else { $null })
    uploads_mb = $uploads
    preview_final = $previewFinal
    placeholder = $placeholder
    half_requests = $halfRequests
    deferred = $deferred
    tier_hits = $tierHits
    peak_private_mb = [math]::Round($peak / 1MB, 1)
    log = $log
  }
}

# V3.1-07 S2: one process, one portable directory, the same CR3 opened twice.
# `$keys` is explicit because the sequence matters: Right leaves the startup
# file, Left comes back to it (second open), an optional final "1" is the 1:1
# check. Returns the S2 evidence plus the same peak/log fields as Run-Case.
function Run-RevisitCase([string]$label, [string]$keys, [string[]]$extra = @(),
                         [int]$dumpAt = 0) {
  $dir = Join-Path $Out $label
  New-Item -ItemType Directory -Force $dir | Out-Null
  $exe = Join-Path $dir 'zoreview.exe'
  Copy-Item -LiteralPath $App -Destination $exe -Force
  $shots = Join-Path $dir 'shots'
  New-Item -ItemType Directory -Force $shots | Out-Null
  $files = Get-ChildItem -LiteralPath $Samples -Filter '*.CR3' | Sort-Object Name |
    Select-Object -First 3
  if ($files.Count -lt 3) { throw "not enough CR3 files under $Samples" }
  $first = $null
  foreach ($file in $files) {
    $link = Join-Path $shots $file.Name
    if (-not $first) { $first = $link }
    New-Item -ItemType HardLink -Path $link -Value $file.FullName | Out-Null
  }
  $dump = Join-Path $dir 'dump.txt'
  $arguments = @('--portable', '--language', 'en', '--send-keys', $keys,
                 '--send-keys-at', '2600', '--send-keys-interval', '1200')
  if ($dumpAt -gt 0) {
    $arguments += @('--layout-dump', $dump, '--dump-at', [string]$dumpAt,
                    '--quit-at', [string]($dumpAt + 1500))
  } else {
    $arguments += @('--quit-at', '9000')
  }
  $arguments += $extra
  $arguments += $first
  $process = Start-Process -FilePath $exe -ArgumentList $arguments -PassThru
  $peak = 0
  while (-not $process.HasExited) {
    try {
      $process.Refresh()
      if ($process.PrivateMemorySize64 -gt $peak) { $peak = $process.PrivateMemorySize64 }
    } catch {}
    Start-Sleep -Milliseconds 40
  }
  $log = Join-Path $dir 'logs\zoreview.log'
  if (-not (Test-Path -LiteralPath $log)) { throw "no log for $label" }
  Copy-Item -LiteralPath $log (Join-Path $dir 'app.log') -Force
  $text = Get-Content -LiteralPath $log -Raw
  $done = @()
  foreach ($match in [regex]::Matches($text, 'load: done ms=([0-9.]+)')) {
    $done += [double]$match.Groups[1].Value
  }
  $tierPuts = @()
  foreach ($match in [regex]::Matches($text, 'raw: preview cached as tier edge=(\d+)')) {
    $tierPuts += [int]$match.Groups[1].Value
  }
  $tierMade = @()
  foreach ($match in [regex]::Matches($text, 'display tier: \d+x\d+ -> \d+x\d+ edge=(\d+)')) {
    $tierMade += [int]$match.Groups[1].Value
  }
  $hits = @()
  foreach ($match in [regex]::Matches($text,
      'display cache hit: (.*?) edge=(\d+) source=(\w+)')) {
    $hits += [ordered]@{
      file = [System.IO.Path]::GetFileName($match.Groups[1].Value)
      edge = [int]$match.Groups[2].Value
      source = $match.Groups[3].Value
    }
  }
  # The S2 revisit is the only RAW cache hit that reports a `load: done`; take
  # the first one that follows a memory hit so the number belongs to it.
  $revisitMs = $null
  $lines = Get-Content -LiteralPath $log
  for ($i = 0; $i -lt $lines.Count; $i++) {
    if ($lines[$i] -match 'display cache hit: .* source=mem') {
      for ($j = $i + 1; $j -lt [Math]::Min($i + 5, $lines.Count); $j++) {
        if ($lines[$j] -match 'load: done ms=([0-9.]+)') {
          $revisitMs = [double]$Matches[1]
          break
        }
      }
      if ($null -ne $revisitMs) { break }
    }
  }
  $values = @{}
  if (Test-Path -LiteralPath $dump) {
    foreach ($line in Get-Content -LiteralPath $dump) {
      $eq = $line.IndexOf('=')
      if ($eq -gt 0) { $values[$line.Substring(0, $eq)] = $line.Substring($eq + 1) }
    }
  }
  return [ordered]@{
    label = $label
    done_ms = $done
    last_ms = $(if ($done.Count -gt 0) { $done[$done.Count - 1] } else { $null })
    revisit_ms = $revisitMs
    tier_puts = $tierPuts
    tier_made = $tierMade
    hits = $hits
    mem_hits = @($hits | Where-Object { $_.source -eq 'mem' })
    preview_final = ([regex]::Matches($text, 'raw: preview as final')).Count
    first_file = [System.IO.Path]::GetFileName($first)
    values = $values
    peak_private_mb = [math]::Round($peak / 1MB, 1)
    text = $text
    log = $log
  }
}

$on = Run-Case 'preview-on' @()
$off = Run-Case 'preview-off' @('--raw-preview=off')

# --- V3.1-07 S2: put-from-preview and the revisit it buys --------------------
# The rollback case also disables the S4 RAW neighbour prefetch: both routes fill
# the same fit-tier entry, so without `--prefetch-raw=off` the second open would
# still be a cache hit and the A/B would prove nothing about the put.
$revisit = Run-RevisitCase 'revisit' 'Right,Left'
$revisitOff = Run-RevisitCase 'revisit-preview-cache-off' 'Right,Left' `
  @('--preview-cache=off', '--prefetch-raw=off')
$upgrade = Run-RevisitCase 'revisit-then-1to1' 'Right,Left,1' @() -dumpAt 7500

# The first load of each run decodes full (the renderer does not exist yet, a
# documented v3.0 limitation), so the switch metrics start at the second load.
$onSwitches = @($on.done_ms | Select-Object -Skip 1)
$onUploads = @($on.uploads_mb | Select-Object -Skip 1)
$offSwitches = @($off.done_ms | Select-Object -Skip 1)

Write-Host ""
Write-Host ("case            loads  switch ms (2nd..)            uploads MB (2nd..)          preview-as-final  peak private")
Write-Host ("preview-on      {0,5}  {1,-28} {2,-26} {3,16}  {4} MB" -f `
  $on.switches, (($onSwitches | ForEach-Object { $_ }) -join ", "), `
  (($onUploads | ForEach-Object { $_ }) -join ", "), $on.preview_final, $on.peak_private_mb)
Write-Host ("preview-off     {0,5}  {1,-28} {2,-26} {3,16}  {4} MB" -f `
  $off.switches, (($offSwitches | ForEach-Object { $_ }) -join ", "), `
  (($off.uploads_mb | Select-Object -Skip 1 | ForEach-Object { $_ }) -join ", "), `
  $off.preview_final, $off.peak_private_mb)

$maxSwitch = ($onSwitches | Measure-Object -Maximum).Maximum
$maxUpload = ($onUploads | Measure-Object -Maximum).Maximum
Check ($on.deferred -ge 1) "the second load defers its full decode"
# V3.1-11 (option C): the camera JPEG is a placeholder, followed by a LibRaw
# half-size decode that becomes the fit frame. The old "preview is final"
# assertion is inverted on purpose.
Check ($on.preview_final -eq 0) "the embedded preview is never the final frame"
Check ($on.placeholder -ge 1) "the embedded preview is shown as a placeholder"
Check ($on.half_requests -ge 1) "the placeholder is followed by a half-size decode"
Check ($onSwitches.Count -ge 1) "there is at least one measured switch"
Check ($maxSwitch -le $MaxSwitchMs) ("every switch is <= {0} ms (worst {1})" -f $MaxSwitchMs, $maxSwitch)
Check ($maxUpload -le $MaxUploadMb) ("every upload is <= {0} MB (worst {1})" -f $MaxUploadMb, $maxUpload)
Check ($on.peak_private_mb -le $MaxPeakPrivateMb) `
  ("peak private is <= {0} MB (actual {1})" -f $MaxPeakPrivateMb, $on.peak_private_mb)
# V3.1-07 S4: the neighbour prefetch now fills the *fit tier* of the next RAW, so
# every later switch is a 2-7 ms cache hit in both cases and "off is slower" (the
# 04a A/B) no longer measures the preview path. What still separates them is the
# first frame, which is exactly what 04a changed: embedded preview at fit
# (~330 ms) vs a full 24 MP decode (~1000 ms). Later switches are compared to the
# same 150 ms budget instead.
Check ($off.first_ms -gt $on.first_ms) `
  ("--raw-preview=off needs longer for the first frame (off {0} ms vs on {1} ms)" -f $off.first_ms, $on.first_ms)
if ($offSwitches.Count -ge 1) {
  $offMax = ($offSwitches | Measure-Object -Maximum).Maximum
  Check ($offMax -le $MaxSwitchMs) `
    ("every --raw-preview=off switch is <= {0} ms (worst {1})" -f $MaxSwitchMs, $offMax)
}
Check ($on.tier_hits -ge 1) `
  ("the RAW run produces display-tier hits (actual {0})" -f $on.tier_hits)

# --- V3.1-11 (option C): the fit tier now comes from the half-size decode -----
# S2's put-from-preview (a MakeDisplayTier downscale of the camera JPEG) is
# retired: caching that JPEG would bring the colour drift back on the next open.
# The half frame caches *itself* at the same edge, so the revisit contract
# (memory/disk hit, <= 20 ms) is unchanged while the pixels stay LibRaw-rendered.
# `--preview-cache` therefore has nothing left to skip and is a parsed no-op.
$fit = $(if ($revisit.mem_hits.Count -gt 0) { $revisit.mem_hits[0].edge } else { 0 })
Write-Host ""
Write-Host ("revisit                tier edge={0}  mem hits={1}  revisit ms={2}  placeholder={3}  half={4}  peak={5} MB" -f `
  $fit, $revisit.mem_hits.Count, $revisit.revisit_ms, $revisit.placeholder, $revisit.half_requests, $revisit.peak_private_mb)
Write-Host ("revisit --preview-cache=off  mem hits={0}  last ms={1}  (flag is a no-op now)" -f `
  $revisitOff.mem_hits.Count, $revisitOff.last_ms)
Write-Host ("1:1 on tier frame      raw_mode={0}  display_edge={1}  zoom={2}%" -f `
  $upgrade.values['raw_mode'], $upgrade.values['display_edge'], $upgrade.values['zoom_percent'])

Check ($revisit.half_requests -ge 1) `
  "the first open decodes a LibRaw half-size fit frame"
Check (($revisit.mem_hits | Where-Object { $_.file -eq $revisit.first_file -and $_.edge -eq $fit }).Count -ge 1) `
  ("the second open of the same CR3 is a memory tier hit at edge={0}" -f $fit)
Check ($null -ne $revisit.revisit_ms -and $revisit.revisit_ms -le 20) `
  ("the second open finishes in <= 20 ms (actual {0} ms)" -f $revisit.revisit_ms)
# The placeholder may be decoded twice at most (startup + one revisit); a third
# one would mean the revisit re-decoded the camera JPEG, which the <= 20 ms check
# would already catch.
Check ($revisit.placeholder -le 2) `
  ("the second open does not decode the embedded preview again (placeholders {0})" -f $revisit.placeholder)
Check ($revisit.preview_final -eq 0) `
  "the camera JPEG never becomes the final frame (V3.1-11)"
Check ($revisit.peak_private_mb -le $MaxPeakPrivateMb) `
  ("the revisit peak private is <= {0} MB (actual {1})" -f $MaxPeakPrivateMb, $revisit.peak_private_mb)
$diskPattern = 'disk cache save: .*edge=' + $fit
Check ($revisit.text -match $diskPattern) `
  ("the tier is also written to the disk cache at edge={0}" -f $fit)

# The half frame is cached regardless of the retired flag: both runs must hit the
# same tier entry on the second open.
Check ($revisitOff.mem_hits.Count -ge 1) `
  "--preview-cache=off no longer suppresses the fit tier (it is the half frame)"
Check ($null -ne $revisitOff.last_ms -and $revisitOff.last_ms -le 20) `
  ("the --preview-cache=off second open is also a cache hit (actual {0} ms)" -f $revisitOff.last_ms)

# The §7.6 invariant is untouched: 1:1 on a frame that came from the tier
# still asks for the file's pixels and keeps the full frame in full_frames.
Check ($upgrade.text -match 'upgrade: full frame applied') `
  "a 1:1 view on the tier frame applies the full decode"
Check ($upgrade.values['raw_mode'] -eq 'full') "the 1:1 view ends on raw_mode=full"
Check ($upgrade.values['display_edge'] -eq '0') "the full frame is no longer a display tier"
Check ($upgrade.values['zoom_percent'] -eq '100') "the 1:1 view is 100% of the file"
Check ($upgrade.preview_final -eq 0) "the 1:1 upgrade never promotes a preview"

Write-Host ""
if ($failures -eq 0) {
  Write-Host ("OK: V3.1-04a/S2 RAW checks passed (artifacts: {0})" -f $Out)
  exit 0
}
Write-Host ("FAIL: {0} check(s) (artifacts: {1})" -f $failures, $Out)
exit 1
