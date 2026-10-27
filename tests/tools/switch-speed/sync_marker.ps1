#requires -Version 5.1
# V3-07 sync marker: a 24x24 topmost no-activate window that flashes white
# for 40 ms every time the harness sets the named event. ASCII only.
param([Parameter(Mandatory = $true)][string]$EventName)
$ErrorActionPreference = 'Stop'
Add-Type -Path (Join-Path $PSScriptRoot 'SwitchHarness.cs') `
  -ReferencedAssemblies System.Windows.Forms, System.Drawing
[NvSwitch]::RunMarker($EventName)
