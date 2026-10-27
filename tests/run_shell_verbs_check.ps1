#requires -Version 5.1
<#
.SYNOPSIS
    M3-2 shell context-menu verbs and their headless commands.

.DESCRIPTION
    Registers "用 ZoreView 打开 / 设为桌面壁纸 / 复制图像" for every supported
    image extension under HKCU, checks the command lines, runs the real
    executable's --register-shell/--unregister-shell/--copy-image paths, and
    optionally exercises --set-wallpaper (restoring the previous wallpaper).
    The script always unregisters in its finally block.

.EXAMPLE
    powershell -ExecutionPolicy Bypass -File tests\run_shell_verbs_check.ps1

.EXAMPLE
    powershell -ExecutionPolicy Bypass -File tests\run_shell_verbs_check.ps1 -Wallpaper
#>
param(
    [string]$Exe = (Join-Path (Split-Path -Parent $PSScriptRoot) 'build\Release\zoreview_shellverbtests.exe'),
    [string]$App = (Join-Path (Split-Path -Parent $PSScriptRoot) 'build\Release\zoreview.exe'),
    [string]$Images = (Join-Path (Split-Path -Parent (Split-Path -Parent $PSScriptRoot)) '测试文件夹\images'),
    [switch]$Wallpaper
)

$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot
if (-not (Test-Path -LiteralPath $Exe)) { throw "test harness not found: $Exe" }
if (-not (Test-Path -LiteralPath $App)) { throw "app not found: $App" }

$previousOutput = [Console]::OutputEncoding
[Console]::OutputEncoding = [Text.Encoding]::UTF8
$failures = 0

try {
    Push-Location $root
    try {
        $arguments = @('--exe', $App, '--images', $Images)
        if ($Wallpaper) { $arguments += '--wallpaper' }
        & $Exe @arguments
        if ($LASTEXITCODE -ne 0) { $failures++ }
    } finally {
        Pop-Location
    }

    $leftovers = @(Get-ChildItem 'HKCU:\Software\Classes\SystemFileAssociations' -ErrorAction SilentlyContinue |
        ForEach-Object { Get-ChildItem $_.PSPath -ErrorAction SilentlyContinue } |
        Where-Object { $_.PSChildName -eq 'shell' } |
        ForEach-Object { Get-ChildItem $_.PSPath -ErrorAction SilentlyContinue } |
        Where-Object { $_.PSChildName -like 'ZoreView.*' })
    if ($leftovers.Count -gt 0) {
        Write-Host "FAIL: $($leftovers.Count) ZoreView verb key(s) left behind"
        $failures++
    } else {
        Write-Host 'OK   no ZoreView verb keys remain'
    }
} finally {
    & $App --unregister-shell | Out-Null
    [Console]::OutputEncoding = $previousOutput
}

if ($failures -gt 0) {
    Write-Host "FAIL: $failures shell-verb check(s)"
    exit 1
}
Write-Host 'OK: shell context-menu verbs and headless commands'
