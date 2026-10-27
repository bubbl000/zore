#requires -Version 5.1
<#
.SYNOPSIS
    Cycle through a folder with Right-arrow and record process memory/handles.

.EXAMPLE
    powershell -ExecutionPolicy Bypass -File tests\run_soak.ps1 -Iterations 200
#>
param(
    [string]$Exe = (Join-Path (Split-Path -Parent $PSScriptRoot) 'build\Release\zoreview.exe'),
    [string]$Image = (Join-Path (Split-Path -Parent (Split-Path -Parent $PSScriptRoot)) '测试文件夹\images\jpeg\photo_1600.jpg'),
    [int]$Iterations = 200,
    [int]$DelayMs = 150,
    [string]$Out = (Join-Path $PSScriptRoot 'output\soak.csv')
)

$ErrorActionPreference = 'Stop'

if (-not (Test-Path -LiteralPath $Exe)) { throw "exe not found: $Exe" }
if (-not (Test-Path -LiteralPath $Image)) { throw "image not found: $Image" }

Add-Type @'
using System;
using System.Runtime.InteropServices;
public static class NvSoakInput {
  [DllImport("user32.dll")] public static extern bool PostMessage(IntPtr hWnd, uint msg, UIntPtr wParam, IntPtr lParam);
}
'@

$p = Start-Process -FilePath $Exe -ArgumentList "`"$Image`"" -PassThru
Start-Sleep -Seconds 2

$rows = New-Object System.Collections.Generic.List[object]
function Add-Sample([int]$iteration) {
    $proc = Get-Process -Id $p.Id -ErrorAction SilentlyContinue
    if ($null -eq $proc) { return $false }
    $rows.Add([pscustomobject]@{
        iteration = $iteration
        handles = $proc.Handles
        working_set_mb = [math]::Round($proc.WorkingSet64 / 1MB, 1)
        private_mb = [math]::Round($proc.PrivateMemorySize64 / 1MB, 1)
    })
    return $true
}

[void](Add-Sample 0)
$alive = $true
for ($i = 1; $i -le $Iterations; $i++) {
    $proc = Get-Process -Id $p.Id -ErrorAction SilentlyContinue
    if ($null -eq $proc) { $alive = $false; break }

    [void][NvSoakInput]::PostMessage($proc.MainWindowHandle, 0x0100, [UIntPtr]::new(0x27), [IntPtr]::Zero)
    Start-Sleep -Milliseconds $DelayMs

    if ($i % 10 -eq 0 -or $i -eq $Iterations) {
        if (-not (Add-Sample $i)) { $alive = $false; break }
    }
}

$proc = Get-Process -Id $p.Id -ErrorAction SilentlyContinue
if ($null -ne $proc) {
    [void](Add-Sample ($Iterations + 1))
    [void]$p.CloseMainWindow()
}

$outDir = Split-Path -Parent $Out
New-Item -ItemType Directory -Force -Path $outDir | Out-Null
$rows | Export-Csv -LiteralPath $Out -NoTypeInformation -Encoding UTF8

$first = $rows | Select-Object -First 1
$last = $rows | Select-Object -Last 1
Write-Host "iterations=$Iterations alive=$alive samples=$($rows.Count)"
Write-Host "handles: $($first.handles) -> $($last.handles)"
Write-Host "working_set_mb: $($first.working_set_mb) -> $($last.working_set_mb)"
Write-Host "private_mb: $($first.private_mb) -> $($last.private_mb)"
Write-Host "csv=$Out"
if (-not $alive) { exit 1 }
