#requires -Version 5.1
# Explore how 2345PicViewer behaves: fullscreen vs windowed, which keys change it.
param([string]$Samples = (Join-Path $env:TEMP 'nv-v3-07-samples'))
$ErrorActionPreference = 'Stop'
Add-Type -Path (Join-Path $PSScriptRoot 'SwitchHarness.cs') -ReferencedAssemblies System.Windows.Forms, System.Drawing
[NvSwitch]::MakeDpiAware()
$out = Join-Path $PSScriptRoot '..\..\output\switch-speed\probe'
New-Item -ItemType Directory -Force $out | Out-Null
$image = Join-Path $Samples '24mp12\img00.jpg'
$exe = 'E:\01_软件中枢\办公协作\2345Pic\2345PicViewer.exe'

Get-Process 2345PicViewer, 2345Pic -ErrorAction SilentlyContinue | Stop-Process -Force
Start-Sleep -Milliseconds 500
$p = Start-Process $exe -ArgumentList ('"' + $image + '"') -PassThru `
  -RedirectStandardError (Join-Path $out 'p2345-stderr.log') -RedirectStandardOutput (Join-Path $out 'p2345-stdout.log')
Start-Sleep -Seconds 4
$h = [NvSwitch]::FindWindowByTitlePart('2345', 200 * 120)
if ($h -eq [IntPtr]::Zero) { $h = [NvSwitch]::FindLargestWindowOfPid($p.Id, 200 * 120) }
"hwnd=0x{0:X} pid={1} class={2} title={3}" -f $h.ToInt64(), [NvSwitch]::PidOf($h), [NvSwitch]::ClassOf($h), [NvSwitch]::Title($h)
"rect0=" + ([NvSwitch]::RectOf($h) -join ',')
[NvSwitch]::SaveScreenshot(0, 0, 2560, 1440, (Join-Path $out '2345-step0.png'))

"restore + move"
[NvSwitch]::ShowWindow($h, 9)
Start-Sleep -Milliseconds 300
[void][NvSwitch]::MoveWindow($h, 120, 120, 1280, 800, $true)
Start-Sleep -Milliseconds 800
"rect1=" + ([NvSwitch]::RectOf($h) -join ',')
[NvSwitch]::SaveScreenshot(0, 0, 2560, 1440, (Join-Path $out '2345-step1-move.png'))

"send ESC"
[void][NvSwitch]::ForceForeground($h)
Add-Type @"
using System;using System.Runtime.InteropServices;
public static class K { [DllImport("user32.dll")] public static extern void keybd_event(byte vk, byte scan, uint flags, IntPtr extra); }
"@
[K]::keybd_event(0x1B, 0, 0, [IntPtr]::Zero); Start-Sleep -Milliseconds 60
[K]::keybd_event(0x1B, 0, 2, [IntPtr]::Zero)
Start-Sleep -Milliseconds 900
"rect2=" + ([NvSwitch]::RectOf($h) -join ',')
[NvSwitch]::SaveScreenshot(0, 0, 2560, 1440, (Join-Path $out '2345-step2-esc.png'))

"double click center"
Add-Type @"
using System;using System.Runtime.InteropServices;
public static class M {
  [DllImport("user32.dll")] public static extern bool SetCursorPos(int x, int y);
  [DllImport("user32.dll")] public static extern void mouse_event(uint f, uint dx, uint dy, uint d, IntPtr e);
}
"@
[void][M]::SetCursorPos(700, 500)
Start-Sleep -Milliseconds 100
[M]::mouse_event(0x0002,0,0,0,[IntPtr]::Zero); [M]::mouse_event(0x0004,0,0,0,[IntPtr]::Zero)
Start-Sleep -Milliseconds 80
[M]::mouse_event(0x0002,0,0,0,[IntPtr]::Zero); [M]::mouse_event(0x0004,0,0,0,[IntPtr]::Zero)
Start-Sleep -Milliseconds 1200
"rect3=" + ([NvSwitch]::RectOf($h) -join ',')
[NvSwitch]::SaveScreenshot(0, 0, 2560, 1440, (Join-Path $out '2345-step3-dblclick.png'))
[NvSwitch]::TopWindows() | Where-Object { $_ -match '2345|RCImage' }
Stop-Process -Id ([NvSwitch]::PidOf($h)) -Force -ErrorAction SilentlyContinue
Get-Process 2345PicViewer, 2345Pic -ErrorAction SilentlyContinue | Stop-Process -Force
