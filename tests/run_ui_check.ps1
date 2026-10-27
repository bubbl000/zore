#requires -Version 5.1
<#
.SYNOPSIS
    M3-3: DPI scaling, Chinese/English UI, high contrast and UI Automation name.

.DESCRIPTION
    Runs the real application at forced 100/125/150/200% scales, checks the
    device-independent layout values in the log, exercises --language zh/en and
    --force-high-contrast, reads the window name through UI Automation (the same
    path a screen reader uses), and verifies that the screenshot still renders.

.EXAMPLE
    powershell -ExecutionPolicy Bypass -File tests\run_ui_check.ps1
#>
param(
    [string]$App = (Join-Path (Split-Path -Parent $PSScriptRoot) 'build\Release\zoreview.exe'),
    [string]$Image = (Join-Path (Split-Path -Parent (Split-Path -Parent $PSScriptRoot)) '测试文件夹\images\jpeg\photo_1600.jpg'),
    [string]$OutputDirectory = (Join-Path (Split-Path -Parent $PSScriptRoot) 'tests\output\ui')
)

$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot
if (-not (Test-Path -LiteralPath $App)) { throw "app not found: $App" }
if (-not (Test-Path -LiteralPath $Image)) { throw "image not found: $Image" }
if (-not (Test-Path -LiteralPath $OutputDirectory)) {
    New-Item -ItemType Directory -Path $OutputDirectory -Force | Out-Null
}

Add-Type -AssemblyName System.Drawing
Add-Type -AssemblyName UIAutomationClient
Add-Type -AssemblyName UIAutomationTypes

$previousOutput = [Console]::OutputEncoding
[Console]::OutputEncoding = [Text.Encoding]::UTF8
$failures = 0
$logPath = Join-Path $env:LOCALAPPDATA 'ZoreView\logs\zoreview.log'

function Assert([bool]$Condition, [string]$What) {
    if ($Condition) {
        Write-Host "OK   $What"
    } else {
        Write-Host "FAIL $What"
        $script:failures++
    }
}

function Read-NewLogLines([int]$Skip) {
    if (-not (Test-Path -LiteralPath $logPath)) { return @() }
    $all = @(Get-Content -LiteralPath $logPath -Encoding UTF8)
    if ($all.Count -le $Skip) { return @() }
    return @($all[$Skip..($all.Count - 1)])
}

function Run-App([string[]]$Arguments, [string]$Screenshot) {
    $skip = 0
    if (Test-Path -LiteralPath $logPath) {
        $skip = @(Get-Content -LiteralPath $logPath -Encoding UTF8).Count
    }
    $full = @()
    if ($Screenshot) {
        $full += @('--screenshot', $Screenshot, '--shot-delay', '800')
    }
    $full += $Arguments
    $full += $Image
    $process = Start-Process -FilePath $App -ArgumentList $full -Wait -PassThru `
        -WorkingDirectory $root
    Start-Sleep -Milliseconds 150
    return [pscustomobject]@{
        ExitCode = $process.ExitCode
        Lines = @(Read-NewLogLines $skip)
    }
}

function Find-UiName([int]$ProcessId) {
    $root = [Windows.Automation.AutomationElement]::RootElement
    $condition = New-Object Windows.Automation.PropertyCondition(
        [Windows.Automation.AutomationElement]::ProcessIdProperty, $ProcessId)
    $element = $root.FindFirst(
        [Windows.Automation.TreeScope]::Children, $condition)
    if ($null -eq $element) { return $null }
    return $element.Current.Name
}

# 100/125/150/200%: the strip height is a DIP value scaled to physical pixels.
foreach ($percent in 100, 125, 150, 200) {
    $scale = $percent / 100.0
    $screenshot = Join-Path $OutputDirectory ("dpi-{0}.png" -f $percent)
    $result = Run-App @('--force-dpi-scale', "$percent") $screenshot
    $expected = 76.0 * $scale
    $scaleLine = $result.Lines | Where-Object { $_ -match 'ui: dpi_scale=([0-9.]+)' } |
        Select-Object -Last 1
    if ($null -eq $scaleLine) { $scaleLine = '' }
    $stripLine = $result.Lines | Where-Object { $_ -match 'ui: dpi_scale=.*strip_height=([0-9]+)' } |
        Select-Object -Last 1
    if ($null -eq $stripLine) { $stripLine = '' }
    Assert ($result.ExitCode -eq 0) "screenshot run at $percent% exits cleanly"
    if ($scaleLine -match 'dpi_scale=([0-9.]+)') {
        Assert ([math]::Abs([double]$Matches[1] - $scale) -lt 0.001) `
            "log reports dpi_scale at $percent%"
    } else {
        Assert $false "log reports dpi_scale at $percent%"
    }
    if ($stripLine -match 'strip_height=([0-9]+)') {
        Assert ([math]::Abs([double]$Matches[1] - $expected) -le 1.0) `
            "strip height at $percent% is $expected px"
    } else {
        Assert $false "strip height is logged at $percent%"
    }
    Assert (Test-Path -LiteralPath $screenshot) "screenshot exists at $percent%"
    if (Test-Path -LiteralPath $screenshot) {
        $bitmap = [System.Drawing.Image]::FromFile($screenshot)
        Assert ($bitmap.Width -gt 100 -and $bitmap.Height -gt 100) `
            "screenshot has a usable size at $percent%"
        $bitmap.Dispose()
    }
}

# Languages: the UI logs the active language; the English run must not alter the
# persisted setting (only the context-menu switch saves it).
foreach ($language in 'zh', 'en') {
    $screenshot = Join-Path $OutputDirectory "language-$language.png"
    $result = Run-App @('--language', $language) $screenshot
    Assert ($result.ExitCode -eq 0) "run with language=$language exits cleanly"
    Assert (@($result.Lines | Where-Object { $_ -match "ui: language=$language" }).Count -gt 0) `
        "log reports ui language=$language"
}

# High contrast: background must be the system COLOR_WINDOW, checkerboard off.
$windowColor = [System.Drawing.SystemColors]::Window
$expectedBackground = "$($windowColor.R),$($windowColor.G),$($windowColor.B)"
$result = Run-App @('--force-high-contrast') (Join-Path $OutputDirectory 'high-contrast.png')
Assert (@($result.Lines | Where-Object { $_ -match 'ui: high_contrast=1' }).Count -gt 0) `
    'log reports high contrast on'
$backgroundLine = $result.Lines | Where-Object { $_ -match 'ui: high_contrast=.*background=([0-9,]+)' } |
    Select-Object -Last 1
if ($null -eq $backgroundLine) { $backgroundLine = '' }
if ($backgroundLine -match 'background=([0-9,]+)') {
    Assert ($Matches[1] -eq $expectedBackground) `
        "high-contrast background is the system window color ($expectedBackground)"
} else {
    Assert $false 'high-contrast background is logged'
}

# UI Automation: Explorer/screen readers see the window title as the Name.
$process = Start-Process -FilePath $App -ArgumentList @($Image) -PassThru `
    -WorkingDirectory $root
try {
    $deadline = (Get-Date).AddSeconds(15)
    while (-not $process.HasExited -and $process.MainWindowHandle -eq 0 -and
           (Get-Date) -lt $deadline) {
        Start-Sleep -Milliseconds 100
        $process.Refresh()
    }
    Assert ($process.MainWindowHandle -ne 0) 'main window became visible'
    Start-Sleep -Milliseconds 1200
    $name = Find-UiName $process.Id
    Assert ($null -ne $name -and $name -ne '') 'UI Automation exposes a window name'
    if ($name) {
        Write-Host "     UIA name: $name"
        Assert ($name -like 'ZoreView*') 'UIA name starts with the application name'
        Assert ($name -like '*photo_1600.jpg*') 'UIA name contains the current file'
    }
} finally {
    if (-not $process.HasExited) {
        $null = $process.CloseMainWindow()
        if (-not $process.WaitForExit(5000)) { $process.Kill() }
    }
}

# The custom rename dialog: localized title/buttons and accessible names.
if (-not ('UiNative' -as [type])) {
    Add-Type @"
using System;
using System.Text;
using System.Runtime.InteropServices;
public static class UiNative {
  public delegate bool EnumProc(IntPtr hWnd, IntPtr lParam);
  [DllImport("user32.dll")] public static extern bool EnumWindows(
      EnumProc callback, IntPtr lParam);
  [DllImport("user32.dll")] public static extern uint GetWindowThreadProcessId(
      IntPtr hWnd, out uint processId);
  [DllImport("user32.dll", CharSet=CharSet.Unicode)] public static extern int
      GetClassName(IntPtr hWnd, StringBuilder text, int count);
  [DllImport("user32.dll", CharSet=CharSet.Unicode)] public static extern int
      GetWindowText(IntPtr hWnd, StringBuilder text, int count);
  [DllImport("user32.dll")] public static extern bool PostMessage(
      IntPtr hWnd, uint message, IntPtr wParam, IntPtr lParam);
}
"@
}

function Find-Dialog([int]$ProcessId, [string]$ExpectedName, [int]$TimeoutMs) {
    $deadline = (Get-Date).AddMilliseconds($TimeoutMs)
    while ((Get-Date) -lt $deadline) {
        $script:dialogHandle = [IntPtr]::Zero
        [UiNative]::EnumWindows({
            param($hWnd, $lParam)
            $owner = 0
            [void][UiNative]::GetWindowThreadProcessId($hWnd, [ref]$owner)
            if ($owner -eq $ProcessId) {
                $class = New-Object Text.StringBuilder 256
                [void][UiNative]::GetClassName($hWnd, $class, 256)
                if ($class.ToString() -eq 'ZoreView.TextInput') {
                    $title = New-Object Text.StringBuilder 256
                    [void][UiNative]::GetWindowText($hWnd, $title, 256)
                    if ($title.ToString() -eq $ExpectedName) {
                        $script:dialogHandle = $hWnd
                    }
                }
            }
            return $true
        }, [IntPtr]::Zero) | Out-Null
        if ($script:dialogHandle -ne [IntPtr]::Zero) {
            return [Windows.Automation.AutomationElement]::FromHandle(
                $script:dialogHandle)
        }
        Start-Sleep -Milliseconds 150
    }
    return $null
}

function Get-Dialog-Names($Dialog) {
    return @($Dialog.FindAll(
        [Windows.Automation.TreeScope]::Children,
        [Windows.Automation.Condition]::TrueCondition) |
        ForEach-Object { $_.Current.Name })
}

foreach ($case in @(
    @{ Language = 'en'; Title = 'Rename'; Ok = 'OK'; Cancel = 'Cancel' },
    @{ Language = 'zh'; Title = '重命名'; Ok = '确定'; Cancel = '取消' })) {
    $process = Start-Process -FilePath $App `
        -ArgumentList @('--language', $case.Language, '--open-rename-dialog', $Image) `
        -PassThru -WorkingDirectory $root
    try {
        $dialog = Find-Dialog $process.Id $case.Title 15000
        Assert ($null -ne $dialog) "rename dialog title is '$($case.Title)'"
        if ($null -ne $dialog) {
            $names = Get-Dialog-Names $dialog
            Write-Host "     dialog children: $($names -join ' | ')"
            Assert ($names -contains $case.Ok) `
                "rename dialog has the '$($case.Ok)' button"
            Assert ($names -contains $case.Cancel) `
                "rename dialog has the '$($case.Cancel)' button"
            # Close with Cancel (WM_COMMAND / IDCANCEL) so no file is renamed.
            $handle = [IntPtr]$dialog.Current.NativeWindowHandle
            $null = [UiNative]::PostMessage($handle, 0x0111, [IntPtr]2, [IntPtr]0)
            Start-Sleep -Milliseconds 300
        }
    } finally {
        if (-not $process.HasExited) {
            $null = $process.CloseMainWindow()
            if (-not $process.WaitForExit(5000)) { $process.Kill() }
        }
    }
}

# Persisted language switch (headless --set-language writes settings.ini).
$settingsPath = Join-Path $env:LOCALAPPDATA 'ZoreView\settings.ini'
$settingsBackup = $null
if (Test-Path -LiteralPath $settingsPath) {
    $settingsBackup = [IO.File]::ReadAllBytes($settingsPath)
}
try {
    foreach ($language in 'en', 'zh') {
        $process = Start-Process -FilePath $App `
            -ArgumentList @('--set-language', $language) -Wait -PassThru `
            -WorkingDirectory $root
        Assert ($process.ExitCode -eq 0) "--set-language $language exits cleanly"
        Assert ((Get-Content -LiteralPath $settingsPath -Raw -ErrorAction SilentlyContinue) -match "language=$language") `
            "settings.ini stores language=$language"
    }
    $process = Start-Process -FilePath $App `
        -ArgumentList @('--set-language', 'fr') -Wait -PassThru `
        -WorkingDirectory $root
    Assert ($process.ExitCode -ne 0) '--set-language rejects unknown codes'
} finally {
    if ($null -ne $settingsBackup) {
        [IO.File]::WriteAllBytes($settingsPath, $settingsBackup)
    } elseif (Test-Path -LiteralPath $settingsPath) {
        Remove-Item -LiteralPath $settingsPath -Force
    }
}

[Console]::OutputEncoding = $previousOutput
if ($failures -gt 0) {
    Write-Host "FAIL: $failures UI check(s)"
    exit 1
}
Write-Host 'OK: DPI, localization, high contrast and UI Automation'
