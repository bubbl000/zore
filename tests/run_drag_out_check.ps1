# v2.0 S3 (plan 22/19): drag files out to other programs.
#
# Automated coverage:
#   * zoreview_dragtests.exe: FileDataObject payloads / FileDropSource / 4 DIP
#     trigger policy
#   * --simulate-drag-out and --simulate-strip-drag: the same start path as a
#     real drag (selection snapshot, copy-only logging, no navigation) with
#     DoDragDrop dry-run, because a real drop needs another application.
#
# Manual checklist (Explorer / Word / 微信, Esc cancel, Ctrl release) lives in
# docs/DRAG-OUT.md; it is not scripted on purpose.
#
# The script runs a throw-away copy of the executable in portable mode. All
# strings are ASCII, so the file needs no UTF-8 BOM (HANDOFF pitfalls 60/124).

param(
  [string]$App = ".\build\Release\zoreview.exe",
  [string]$Tests = ".\build\Release\zoreview_dragtests.exe",
  [switch]$KeepOutput
)

$ErrorActionPreference = "Stop"
$repo = Split-Path -Parent $PSScriptRoot
Set-Location $repo

if (-not (Test-Path $App)) {
  throw "Application not found: $App (build first: tools\build.ps1)"
}
if (-not (Test-Path $Tests)) {
  throw "Test harness not found: $Tests (build first: tools\build.ps1)"
}
$App = (Resolve-Path $App).Path
$Tests = (Resolve-Path $Tests).Path

Add-Type -AssemblyName System.Drawing

$work = Join-Path $env:TEMP ("nv-dragout-" + [guid]::NewGuid().ToString("N"))
New-Item -ItemType Directory -Path $work | Out-Null
$appExe = Join-Path $work "zoreview.exe"
Copy-Item $App $appExe
# AddressSanitizer builds need their runtime next to the copied executable.
$appDirectory = Split-Path -Parent $App
Get-ChildItem $appDirectory -Filter "clang_rt.asan*.dll" -ErrorAction SilentlyContinue |
  ForEach-Object { Copy-Item $_.FullName (Join-Path $work $_.Name) }

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
  Check ($text -match [regex]::Escape($pattern)) `
    ("{0} (expected '{1}')" -f $name, $pattern)
}

function ReadDump([string]$path) {
  $values = @{}
  if (-not (Test-Path $path)) { return $values }
  foreach ($line in Get-Content $path) {
    $parts = $line.Split("=", 2)
    if ($parts.Count -eq 2) { $values[$parts[0]] = $parts[1] }
  }
  return $values
}

function LogText() {
  $log = Join-Path $work "logs\zoreview.log"
  if (Test-Path $log) { return (Get-Content $log -Raw) }
  return ""
}

function Run([string]$label, [string[]]$extra, [string]$image) {
  Remove-Item (Join-Path $work "settings.ini") -Force -ErrorAction SilentlyContinue
  Remove-Item (Join-Path $work "logs\zoreview.log") -Force -ErrorAction SilentlyContinue
  $dump = Join-Path $work ("dump-" + $label + ".txt")
  Remove-Item $dump -Force -ErrorAction SilentlyContinue
  $arguments = @("--portable", "--language", "en", "--force-dpi-scale", "125",
                 "--strip=pinned", "--layout-dump", $dump) + $extra
  if ($image) { $arguments += $image }
  $process = Start-Process -FilePath $appExe -ArgumentList $arguments -PassThru
  if (-not $process.WaitForExit(40000)) {
    $process.Kill()
    throw "zoreview.exe did not exit in time: $label"
  }
  Start-Sleep -Milliseconds 150
  return @{ Values = (ReadDump $dump); Log = (LogText); Dump = $dump }
}

# --- gallery ---------------------------------------------------------------
$gallery = Join-Path $work "gallery"
New-Item -ItemType Directory -Path $gallery | Out-Null
foreach ($name in @("a", "b", "c")) {
  $bitmap = New-Object System.Drawing.Bitmap(64, 64)
  $graphics = [System.Drawing.Graphics]::FromImage($bitmap)
  $graphics.Clear([System.Drawing.Color]::FromArgb(30, 90, 160))
  $graphics.Dispose()
  $bitmap.Save((Join-Path $gallery ($name + ".png")),
               [System.Drawing.Imaging.ImageFormat]::Png)
  $bitmap.Dispose()
}
$fileA = Join-Path $gallery "a.png"
$fileB = Join-Path $gallery "b.png"
$fileC = Join-Path $gallery "c.png"

try {
  # --- 1. data object / drop source / threshold harness --------------------
  $harnessOutput = & $Tests 2>&1
  $harnessText = ($harnessOutput -join "`n")
  $harnessOk = ($harnessOutput | Select-String -Pattern "^ok").Count
  Check ($LASTEXITCODE -eq 0) "the drag-out harness exits successfully"
  CheckHas $harnessText "all drag-out checks passed" "the harness reports success"
  Check ($harnessOk -ge 45) ("the harness ran its full set (actual={0})" -f $harnessOk)
  Check (-not ($harnessText -match "FAIL")) "the harness has no failures"

  # --- 2. image drag-out (current file) -----------------------------------
  # V3.1-06 (§06b): the plain gesture is "leave the window by 16 DIP". The hook
  # moves the pointer 20 DIP up-left from the content centre and the app runs the
  # same press/move/decision chain a real mouse would.
  $image = Run "image" @("--simulate-drag-out") $fileB
  CheckHas $image.Log ("drag: out path=" + $fileB + " source=image effect=1 pan=center") `
    "the current file is exported as a COPY and the pan is recentred"
  Check (-not ($image.Log -match "effect=2")) "the drag never reports MOVE (2)"
  Check ($image.Values["drag_source"] -eq "image") "the dump reports the image source"
  Check ($image.Values["pan_after"] -eq "center") "the dump reports the recentred pan"
  Check ($image.Values["drag_press"] -eq "0") "the drag candidate is cleared"
  Check ($image.Values["file_name"] -eq "b.png") `
    "starting a drag does not change the current image"

  # 200 DIP to the right stays inside the window: a pan, not a drag.
  $inside = Run "image-inside" @("--simulate-drag-out", "200,0") $fileB
  Check (-not ($inside.Log -match "drag: out")) `
    "a large move inside the window starts no file drag"
  CheckHas $inside.Log "drag: dry_run inside_window" `
    "the dry run reports that it stayed inside"
  Check ($inside.Values["drag_source"] -eq "") "no drag source is recorded"

  # Ctrl keeps the old immediate gesture, 10 DIP, no recentre (there was no pan).
  $ctrl = Run "image-ctrl" @("--simulate-drag-out-ctrl") $fileB
  CheckHas $ctrl.Log ("drag: out path=" + $fileB + " source=image effect=1") `
    "Ctrl + a 10 DIP move still drags out immediately"
  Check ($ctrl.Values["drag_source"] -eq "image") "the Ctrl drag reports image"
  Check ($ctrl.Values["pan_after"] -eq "keep") `
    "the Ctrl gesture does not recentre (it never panned)"

  # --- 3. the rail is no longer a drag-out source --------------------------
  $strip = Run "strip2" @("--simulate-strip-drag", "2") $fileB
  Check (-not ($strip.Log -match "drag: out")) `
    "a rail drag starts no file drag (V3.1-06)"
  CheckHas $strip.Log "strip: drag_out blocked" `
    "the rail press is reported as blocked"
  Check ($strip.Values["file_name"] -eq "c.png") `
    "releasing the blocked rail press opens the pressed slot"

  $stripFirst = Run "strip0" @("--simulate-strip-drag", "0") $fileB
  CheckHas $stripFirst.Log "strip: drag_out blocked" `
    "the first rail slot is blocked the same way"
  Check ($stripFirst.Values["file_name"] -eq "a.png") `
    "releasing it opens the first file"

  # --- 4. out-of-range / no-image presses are safe ------------------------
  $outOfRange = Run "strip9" @("--simulate-strip-drag", "9") $fileB
  Check (-not ($outOfRange.Log -match "drag: out")) `
    "an out-of-range rail index starts no drag"
  Check ($outOfRange.Values["file_name"] -eq "b.png") `
    "an out-of-range rail index changes nothing"

  $noImage = Run "no-image" @("--simulate-drag-out") $null
  CheckHas $noImage.Log "drag: out skipped=no_path" `
    "dragging with no image is skipped without a crash"
  Check ($noImage.Values["has_image"] -eq "0") "the empty state is preserved"

  # --- 5. dropping a file in still works (the out flag must not leak) ------
  $dropIn = Run "drop-in" @("--simulate-ole-drop", $fileC) $fileB
  CheckHas $dropIn.Log "drop: path=" "a normal OLE drop from Explorer still works"
  Check (-not ($dropIn.Log -match "self-drop ignored")) `
    "a drop from outside is not mistaken for our own drag"
  Check ($dropIn.Values["file_name"] -eq "c.png") `
    "the dropped file becomes current"

  # --- Summary -------------------------------------------------------------
  Write-Host ""
  Write-Host ("{0} checks, {1} failure(s)" -f $script:checks, $script:failures)
  if ($script:failures -ne 0) {
    Write-Host "Artifacts kept at $work" -ForegroundColor Yellow
    $KeepOutput = $true
    exit 1
  }
  Write-Host "OK: drag-out checks passed"
  Write-Host "Manual checklist: docs\DRAG-OUT.md"
} finally {
  if (-not $KeepOutput) {
    Remove-Item $work -Recurse -Force -ErrorAction SilentlyContinue
  }
}
