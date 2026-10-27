#requires -Version 5.1
<#
.SYNOPSIS
    M1-6 check: run the file-op harness plus the GUI rename/delete hooks.

.DESCRIPTION
    - zoreview_filetests.exe: save/overwrite/rename/recycle-bin/clipboard.
    - GUI: --rename-to and --delete-current verify that the folder list is
      rebuilt and the next image is opened after a mutation.
    - -Wallpaper also runs the optional wallpaper set/restore check.

    The file-op harness temporarily replaces the clipboard and sends a temp
    file to the recycle bin.

.EXAMPLE
    powershell -ExecutionPolicy Bypass -File tests\run_file_ops_check.ps1
#>
param(
    [string]$Exe = (Join-Path (Split-Path -Parent $PSScriptRoot) 'build\Release\zoreview.exe'),
    [string]$FileTests = (Join-Path (Split-Path -Parent $PSScriptRoot) 'build\Release\zoreview_filetests.exe'),
    [string]$Image = (Join-Path (Split-Path -Parent (Split-Path -Parent $PSScriptRoot)) '测试文件夹\images\jpeg\photo_1600.jpg'),
    [switch]$Wallpaper
)

$ErrorActionPreference = 'Stop'
if (-not (Test-Path -LiteralPath $Exe)) { throw "exe not found: $Exe" }
if (-not (Test-Path -LiteralPath $FileTests)) { throw "file tests not found: $FileTests" }
if (-not (Test-Path -LiteralPath $Image)) { throw "image not found: $Image" }

$failures = 0
function Check([bool]$condition, [string]$name) {
    Write-Host ("{0} {1}" -f $(if ($condition) { 'ok  ' } else { 'FAIL' }), $name)
    if (-not $condition) { $script:failures++ }
}

& $FileTests | Select-Object -Last 2
Check ($LASTEXITCODE -eq 0) 'file-op harness'
if ($Wallpaper) {
    & $FileTests --wallpaper | Select-Object -Last 2
    Check ($LASTEXITCODE -eq 0) 'wallpaper harness'
}

$dir = Join-Path $PSScriptRoot 'output\fileops-ui'
if (Test-Path -LiteralPath $dir) { Remove-Item -LiteralPath $dir -Recurse -Force }
New-Item -ItemType Directory -Force -Path $dir | Out-Null

function Run-App([string[]]$arguments) {
    $process = Start-Process -FilePath $Exe -ArgumentList $arguments -PassThru
    if (-not $process.WaitForExit(20000)) {
        $process.Kill()
        return -1
    }
    return $process.ExitCode
}

# Rename: lists must refresh and the renamed file must become current.
$rename_src = Join-Path $dir 'rename-ui.jpg'
$rename_dst = Join-Path $dir 'renamed-ui.jpg'
Copy-Item -LiteralPath $Image -Destination $rename_src -Force
$rename_exit = Run-App @('--screenshot', (Join-Path $dir 'rename.png'),
    '--shot-delay', '2800', '--rename-to', 'renamed-ui.jpg', $rename_src)
Check ($rename_exit -eq 0) 'GUI rename exit'
Check (-not (Test-Path -LiteralPath $rename_src)) 'GUI rename removed old name'
Check (Test-Path -LiteralPath $rename_dst) 'GUI rename created new name'

# --- V3.1-06 (§06e): the in-window rename overlay ---------------------------
# The overlay replaces the Win32 input dialog: F2/menu opens the card, the text
# lives in an EDIT placed over the card's input box, Enter commits, Escape
# cancels, and a rejected name stays in the card with an error line.
function Dump-Text([string]$path) {
    $values = @{}
    if (Test-Path -LiteralPath $path) {
        foreach ($line in Get-Content -LiteralPath $path) {
            $parts = $line.Split('=', 2)
            if ($parts.Count -eq 2) { $values[$parts[0]] = $parts[1] }
        }
    }
    return $values
}

$overlay_src = Join-Path $dir 'overlay-src.jpg'
Copy-Item -LiteralPath $Image -Destination $overlay_src -Force
$overlay_dump = Join-Path $dir 'overlay-open.txt'
$overlay_exit = Run-App @('--portable', '--language', 'en',
    '--open-rename-dialog', '--rename-input', 'overlay-name',
    '--layout-dump', $overlay_dump, '--dump-at', '2600',
    '--quit-at', '3600', $overlay_src)
Check ($overlay_exit -eq 0) 'the rename overlay run exits cleanly'
$overlay = Dump-Text $overlay_dump
Check ($overlay['rename_open'] -eq '1') 'the overlay is open and modal'
Check ($overlay['rename_text'] -eq 'overlay-name') 'the EDIT starts with the requested text'
$card = $overlay['rename_card'].Split(',') | ForEach-Object { [double]$_ }
$input = $overlay['rename_input'].Split(',') | ForEach-Object { [double]$_ }
$okButton = $overlay['rename_ok'].Split(',') | ForEach-Object { [double]$_ }
$cancelButton = $overlay['rename_cancel'].Split(',') | ForEach-Object { [double]$_ }
$dpi = [double]$overlay['dpi']
Check ([math]::Abs(($card[2] - $card[0]) - (440 * $dpi)) -lt 1.5) `
    ("the card uses the confirmation card width (got {0})" -f ($card[2] - $card[0]))
Check ($input[0] -gt $card[0] -and $input[2] -lt $card[2]) 'the input box is inside the card'
Check ($okButton[0] -gt $cancelButton[2]) 'OK sits to the right of Cancel'
Check ($okButton[1] -gt $input[3]) 'the buttons sit below the input box'

# Rejected name: the card stays open with an error line and the file is untouched.
$bad_dump = Join-Path $dir 'overlay-bad.txt'
$bad_exit = Run-App @('--portable', '--language', 'en',
    '--open-rename-dialog', '--rename-input', 'bad/name',
    '--click-hit', 'rename.ok', '--layout-dump', $bad_dump,
    '--dump-at', '3600', '--quit-at', '4600', $overlay_src)
Check ($bad_exit -eq 0) 'the rejected rename exits cleanly'
$bad = Dump-Text $bad_dump
Check ($bad['rename_open'] -eq '1') 'an invalid name keeps the overlay open'
Check ($bad['rename_error'] -ne '') 'the card shows an error line'
Check (Test-Path -LiteralPath $overlay_src) 'an invalid name leaves the file alone'

# Commit through the overlay: the renamed file becomes current and the overlay closes.
$overlay_new = Join-Path $dir 'overlay-done.jpg'
$commit_dump = Join-Path $dir 'overlay-commit.txt'
$commit_exit = Run-App @('--portable', '--language', 'en',
    '--open-rename-dialog', '--rename-input', 'overlay-done',
    '--click-hit', 'rename.ok', '--layout-dump', $commit_dump,
    '--dump-at', '5000', '--quit-at', '6000', $overlay_src)
Check ($commit_exit -eq 0) 'the overlay commit exits cleanly'
$commit = Dump-Text $commit_dump
Check ($commit['rename_open'] -eq '0') 'the overlay closes after a successful rename'
Check ($commit['file_name'] -eq 'overlay-done.jpg') 'the renamed file becomes current'
Check (Test-Path -LiteralPath $overlay_new) 'the new name exists on disk'
Check (-not (Test-Path -LiteralPath $overlay_src)) 'the old name is gone'

# Cancel: Escape closes the card and changes nothing.
$cancel_dump = Join-Path $dir 'overlay-cancel.txt'
$cancel_exit = Run-App @('--portable', '--language', 'en',
    '--open-rename-dialog', '--rename-input', 'should-not-apply',
    '--click-hit', 'rename.cancel', '--layout-dump', $cancel_dump,
    '--dump-at', '3600', '--quit-at', '4600', $overlay_new)
Check ($cancel_exit -eq 0) 'the cancelled rename exits cleanly'
$cancel = Dump-Text $cancel_dump
Check ($cancel['rename_open'] -eq '0') 'Cancel closes the overlay'
Check ($cancel['file_name'] -eq 'overlay-done.jpg') 'Cancel keeps the current name'
Check (Test-Path -LiteralPath $overlay_new) 'Cancel leaves the file on disk'

# Delete: confirmation is bypassed by the hook; the next file is opened.
$delete_a = Join-Path $dir 'delete-a.jpg'
$delete_b = Join-Path $dir 'delete-b.jpg'
Copy-Item -LiteralPath $Image -Destination $delete_a -Force
Copy-Item -LiteralPath $Image -Destination $delete_b -Force
$delete_exit = Run-App @('--screenshot', (Join-Path $dir 'delete.png'),
    '--shot-delay', '2800', '--delete-current', $delete_a)
Check ($delete_exit -eq 0) 'GUI delete exit'
Check (-not (Test-Path -LiteralPath $delete_a)) 'GUI delete sent file to recycle bin'
Check (Test-Path -LiteralPath $delete_b) 'GUI delete kept the other file'

Remove-Item -LiteralPath $dir -Recurse -Force -ErrorAction SilentlyContinue

if ($failures -gt 0) {
    Write-Host "FAIL: $failures check(s)"
    exit 1
}
Write-Host 'OK'
