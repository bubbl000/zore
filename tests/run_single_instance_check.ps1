# v2.0 S3 (plan 22/15): single-instance forwarding (path and bare activation),
# per-DataDir mutex isolation and the single_instance=0 behaviour.
#
# Two throw-away portable copies in different directories stand in for a
# portable and an installed build: their DataDir hash must put them in separate
# instance namespaces.
#
# Everything here is ASCII (pitfall 60/124).

param(
  [string]$App = ".\build\Release\zoreview.exe",
  [switch]$KeepOutput
)

$ErrorActionPreference = "Stop"
$repo = Split-Path -Parent $PSScriptRoot
Set-Location $repo

if (-not (Test-Path $App)) {
  throw "Application not found: $App (build first: tools\build.ps1)"
}
$App = (Resolve-Path $App).Path
$appDirectory = Split-Path -Parent $App

$work = Join-Path $env:TEMP ("nv-instance-" + [guid]::NewGuid().ToString("N"))
New-Item -ItemType Directory -Path $work | Out-Null

$script:checks = 0
$script:failures = 0

function Check([bool]$condition, [string]$name) {
  $script:checks++
  if ($condition) {
    Write-Host ("ok   {0}" -f $name)
  } else {
    Write-Host ("FAIL {0}" -f $name) -ForegroundColor Red
    $script:failures++
  }
}

function CheckHas([string]$text, [string]$pattern, [string]$name) {
  Check ($text -match [regex]::Escape($pattern)) ("{0} (expected '{1}')" -f $name, $pattern)
}

function MakeInstance([string]$name) {
  $dir = Join-Path $work $name
  New-Item -ItemType Directory -Path $dir | Out-Null
  $exe = Join-Path $dir "zoreview.exe"
  Copy-Item $App $exe
  Get-ChildItem $appDirectory -Filter "clang_rt.asan*.dll" -ErrorAction SilentlyContinue |
    ForEach-Object { Copy-Item $_.FullName (Join-Path $dir $_.Name) }
  return $exe
}

function WriteInstanceSettings([string]$exe, [bool]$enabled) {
  $dir = Split-Path -Parent $exe
  Set-Content -Path (Join-Path $dir "settings.ini") -Encoding ASCII -Value @(
    "[general]",
    "version=1",
    "language=en",
    "remember_window=0",
    "single_instance=$(if ($enabled) { 1 } else { 0 })",
    "")
}

function InstanceLog([string]$exe) {
  Join-Path (Split-Path -Parent $exe) "logs\zoreview.log"
}

function StartInstance([string]$exe, [string]$image) {
  $arguments = @("--portable")
  if (-not [string]::IsNullOrEmpty($image)) { $arguments += $image }
  return Start-Process -FilePath $exe -ArgumentList $arguments -PassThru
}

function StopInstance($process) {
  if ($null -eq $process) { return }
  try {
    if (-not $process.HasExited) { $process.Kill() }
    $null = $process.WaitForExit(8000)
  } catch { }
}

Add-Type -AssemblyName System.Drawing
$bitmap = New-Object System.Drawing.Bitmap(320, 240)
$graphics = [System.Drawing.Graphics]::FromImage($bitmap)
$graphics.Clear([System.Drawing.Color]::FromArgb(70, 70, 160))
$graphics.Dispose()
$sourceImage = Join-Path $work "sample.bmp"
$bitmap.Save($sourceImage, [System.Drawing.Imaging.ImageFormat]::Bmp)
$bitmap.Dispose()

$exeA = MakeInstance "a"
$exeB = MakeInstance "b"
$imageA = Join-Path (Split-Path -Parent $exeA) "first.bmp"
$imageB = Join-Path (Split-Path -Parent $exeA) "second.bmp"
Copy-Item $sourceImage $imageA
Copy-Item $sourceImage $imageB
$imageOther = Join-Path (Split-Path -Parent $exeB) "other.bmp"
Copy-Item $sourceImage $imageOther
WriteInstanceSettings $exeA $true
WriteInstanceSettings $exeB $true

$first = $null
$second = $null
$third = $null
$isolationA = $null
$isolationB = $null
$offA = $null
$offB = $null

try {
  # --- 1. The second launch forwards its path and exits 0 -----------------
  $first = StartInstance $exeA $imageA
  Start-Sleep -Seconds 3
  Check (-not $first.HasExited) "the first instance stays running"
  $logPath = InstanceLog $exeA
  $fullLog = if (Test-Path $logPath) { Get-Content $logPath -Raw } else { "" }
  CheckHas $fullLog "single_instance: receiver ready" "the first instance created the receiver before the window"

  # Headless commands must return before the single-instance decision (坑 11):
  # --print-settings while the GUI owns the mutex still has to write its dump.
  $headlessPath = Join-Path (Split-Path -Parent $exeA) "headless.txt"
  Remove-Item $headlessPath -ErrorAction SilentlyContinue
  $headless = Start-Process -FilePath $exeA -ArgumentList @("--portable", "--print-settings", $headlessPath) -PassThru
  $headlessExited = $headless.WaitForExit(8000)
  Check ($headlessExited -and (Test-Path $headlessPath)) "headless commands ignore single instance"
  if (Test-Path $headlessPath) {
    Check ((Get-Content $headlessPath -Raw) -match "settings_version=1") "the headless dump is complete"
  }

  $before = if (Test-Path $logPath) { (Get-Content $logPath).Count } else { 0 }

  $second = StartInstance $exeA $imageB
  $secondExited = $second.WaitForExit(8000)
  Check $secondExited "the second launch exits after forwarding"
  if ($secondExited) { Check ($second.ExitCode -eq 0) "the forwarding launch exits 0" }
  Start-Sleep -Milliseconds 700
  $forwardLog = if (Test-Path $logPath) { (Get-Content $logPath | Select-Object -Skip $before) -join "`n" } else { "" }
  CheckHas $forwardLog "single_instance: forwarded to existing window" "the second launch reports forwarding"
  CheckHas $forwardLog "single_instance: open=" "the forwarded path is logged"
  CheckHas $forwardLog "second.bmp" "the forwarded path is the requested file"
  CheckHas $forwardLog "single_instance: activate existing" "the first instance receives the request"
  Check (-not $first.HasExited) "the first instance survives the hand-off"

  # --- 2. A launch without a path activates the existing window -----------
  $before = if (Test-Path $logPath) { (Get-Content $logPath).Count } else { 0 }
  $third = StartInstance $exeA ""
  $thirdExited = $third.WaitForExit(8000)
  Check $thirdExited "the bare activation launch exits"
  if ($thirdExited) { Check ($third.ExitCode -eq 0) "the activation launch exits 0" }
  Start-Sleep -Milliseconds 700
  $activateLog = if (Test-Path $logPath) { (Get-Content $logPath | Select-Object -Skip $before) -join "`n" } else { "" }
  CheckHas $activateLog "single_instance: forwarded activate" "a pathless launch forwards an activation"
  CheckHas $activateLog "single_instance: activate existing" "the first instance activates its window"

  StopInstance $first
  $first = $null
  StopInstance $third
  $third = $null

  # --- 3. Different DataDirs do not share an instance ---------------------
  # A portable copy in another directory is a separate namespace; starting it
  # must not forward into (or be swallowed by) the first copy.
  $isolationA = StartInstance $exeA $imageA
  Start-Sleep -Seconds 2
  $isolationB = StartInstance $exeB $imageOther
  Start-Sleep -Seconds 3
  Check (-not $isolationA.HasExited) "the first portable copy keeps running"
  Check (-not $isolationB.HasExited) "a copy with another DataDir is not swallowed"
  StopInstance $isolationA
  StopInstance $isolationB
  $isolationA = $null
  $isolationB = $null

  # --- 4. single_instance=0 allows two windows ----------------------------
  WriteInstanceSettings $exeA $false
  $offA = StartInstance $exeA $imageA
  Start-Sleep -Seconds 1
  $offB = StartInstance $exeA $imageB
  Start-Sleep -Seconds 3
  Check (-not $offA.HasExited) "single-instance off keeps the first window"
  Check (-not $offB.HasExited) "single-instance off keeps the second window"
  StopInstance $offA
  StopInstance $offB
  $offA = $null
  $offB = $null
} finally {
  StopInstance $first
  StopInstance $second
  StopInstance $third
  StopInstance $isolationA
  StopInstance $isolationB
  StopInstance $offA
  StopInstance $offB
  if (-not $KeepOutput) {
    Remove-Item $work -Recurse -Force -ErrorAction SilentlyContinue
  } else {
    Write-Host ("Output kept in {0}" -f $work)
  }
}

Write-Host ""
if ($script:failures -eq 0) {
  Write-Host ("OK: {0} single-instance checks passed" -f $script:checks) -ForegroundColor Green
} else {
  Write-Host ("FAILED: {0} of {1} single-instance checks failed" -f $script:failures, $script:checks) -ForegroundColor Red
  exit 1
}
