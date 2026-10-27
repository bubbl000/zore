#requires -Version 5.1
# V3-07 probe: launch each app once, force the 1280x800 window, dump window
# facts and a screenshot. ASCII only.
param(
  [string]$Samples = (Join-Path $env:TEMP 'nv-v3-07-samples'),
  [string]$Out = (Join-Path $PSScriptRoot '..\..\output\switch-speed\probe')
)

$ErrorActionPreference = 'Stop'
Add-Type -Path (Join-Path $PSScriptRoot 'SwitchHarness.cs') `
  -ReferencedAssemblies System.Windows.Forms, System.Drawing
[NvSwitch]::MakeDpiAware()

$NewRoot = (Resolve-Path (Join-Path $PSScriptRoot '..\..\..')).Path
$OursExe = Join-Path $NewRoot 'build\Release\zoreview.exe'
$ReplicaExe = 'E:\06-xiangmu\new\看图软件复刻\build\zoreview.exe'
$PicExe = 'E:\01_软件中枢\办公协作\2345Pic\2345PicViewer.exe'

New-Item -ItemType Directory -Force $Out | Out-Null
$image = Join-Path $Samples '24mp12\img00.jpg'
if (-not (Test-Path $image)) { throw "sample missing: $image" }

function Kill-App([string]$name) {
  Get-Process -Name $name -ErrorAction SilentlyContinue | Stop-Process -Force -ErrorAction SilentlyContinue
}

function Wait-Window([int]$processId, [int]$timeoutMs) {
  $deadline = [DateTime]::UtcNow.AddMilliseconds($timeoutMs)
  while ([DateTime]::UtcNow -lt $deadline) {
    $h = [NvSwitch]::FindLargestWindowOfPid($processId, 200 * 120)
    if ($h -ne [IntPtr]::Zero) { return $h }
    Start-Sleep -Milliseconds 100
  }
  return [IntPtr]::Zero
}

Kill-App 'zoreview'
Kill-App 'zoreview'
Kill-App '2345PicViewer'
Kill-App '2345Pic'
Start-Sleep -Milliseconds 500

$apps = @(
  @{ name = 'ours'; exe = $OursExe; work = (Join-Path $Out 'work-ours') },
  @{ name = 'replica'; exe = $ReplicaExe; work = (Join-Path $Out 'work-replica') },
  @{ name = '2345'; exe = $PicExe; work = (Join-Path $Out 'work-2345') }
)

foreach ($app in $apps) {
  Write-Host ("probe {0}" -f $app.name)
  $work = $app.work
  New-Item -ItemType Directory -Force $work | Out-Null
  if ($app.name -eq 'ours') {
    Copy-Item $app.exe (Join-Path $work 'zoreview.exe') -Force
    $p = Start-Process (Join-Path $work 'zoreview.exe') `
      -ArgumentList @('--portable', '--language', 'en', '--quit-at', '120000', $image) -PassThru
  } elseif ($app.name -eq 'replica') {
    $lad = Join-Path $work 'lad'
    $temp = Join-Path $work 'temp'
    New-Item -ItemType Directory -Force $lad, $temp | Out-Null
    $oldLad = $env:LOCALAPPDATA; $oldTemp = $env:TEMP; $oldTmp = $env:TMP; $oldTrace = $env:PICVIEW_TRACE
    $env:LOCALAPPDATA = $lad
    $env:TEMP = $temp
    $env:TMP = $temp
    $env:PICVIEW_TRACE = '1'
    $p = Start-Process $app.exe -ArgumentList ('"' + $image + '"') -PassThru `
      -RedirectStandardError (Join-Path $work 'stderr.log') `
      -RedirectStandardOutput (Join-Path $work 'stdout.log')
    $env:LOCALAPPDATA = $oldLad; $env:TEMP = $oldTemp; $env:TMP = $oldTmp; $env:PICVIEW_TRACE = $oldTrace
  } else {
    $p = Start-Process $app.exe -ArgumentList @('"' + $image + '"') -PassThru `
      -RedirectStandardError (Join-Path $work 'stderr.log') `
      -RedirectStandardOutput (Join-Path $work 'stdout.log')
  }

  Start-Sleep -Seconds 4
  $h = [IntPtr]::Zero
  if ($app.name -eq '2345') { $h = [NvSwitch]::FindWindowByClassPrefix('RCImageViewerFrame', 200 * 120) }
  if ($h -eq [IntPtr]::Zero) { $h = Wait-Window $p.Id 8000 }
  if ($h -eq [IntPtr]::Zero) {
    $h = [NvSwitch]::FindWindowByTitlePart('2345', 200 * 120)
  }
  if ($h -eq [IntPtr]::Zero) {
    Write-Host "  NO WINDOW (pid $($p.Id) exit=$($p.HasExited))"
    [NvSwitch]::TopWindows() | ForEach-Object { Write-Host ("    " + $_) }
    continue
  }
  [void][NvSwitch]::RestoreIfMaximized($h)
  [void][NvSwitch]::PlaceWindow($h, 120, 120, 1280, 800, 3)
  [void][NvSwitch]::ForceForeground($h)
  Start-Sleep -Milliseconds 1500
  $rect = [NvSwitch]::RectOf($h)
  $client = [NvSwitch]::ClientRectOf($h)
  $pid2 = [NvSwitch]::PidOf($h)
  $info = [ordered]@{
    app = $app.name
    hwnd = ('0x{0:X}' -f $h.ToInt64())
    pid = $pid2
    class = [NvSwitch]::ClassOf($h)
    title = [NvSwitch]::Title($h)
    rect = $rect
    client = $client
    launched_pid = $p.Id
  }
  $info | ConvertTo-Json | Set-Content -Encoding UTF8 (Join-Path $Out ($app.name + '.json'))
  Write-Host ("  hwnd={0} pid={1} rect={2} client={3} class={4}" -f `
    $info.hwnd, $pid2, ($rect -join ','), ($client -join ','), $info.class)
  [NvSwitch]::SaveScreenshot(0, 0, 2560, 1440, (Join-Path $Out ($app.name + '-desktop.png')))
  [NvSwitch]::SaveScreenshot($rect[0], $rect[1], $rect[2], $rect[3], (Join-Path $Out ($app.name + '-window.png')))

  if ($app.name -eq 'ours') { $p.CloseMainWindow() | Out-Null }
  elseif ($app.name -eq 'replica') { $p.CloseMainWindow() | Out-Null }
  else { $p.CloseMainWindow() | Out-Null }
  Start-Sleep -Milliseconds 800
  Kill-App 'zoreview'
  Kill-App 'zoreview'
  Kill-App '2345PicViewer'
  Kill-App '2345Pic'
  Start-Sleep -Milliseconds 500
}
Write-Host "done"
