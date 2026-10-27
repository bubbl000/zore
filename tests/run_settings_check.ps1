# v2.0 S2 (plan 22/07): settings window, settings model persistence, migration,
# recovery and the file-association page.
#
# The script runs a throw-away copy of the executable in portable mode, so the
# developer's %LOCALAPPDATA% settings, caches and associations are not touched
# (except the two extensions the association subtest registers and removes
# again). Everything here is ASCII so no UTF-8 BOM is required (pitfall 60/124).
#
# The settings window is driven through --open-settings / --settings-click /
# --settings-dump, which call the same activation path as a mouse click.

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

$work = Join-Path $env:TEMP ("nv-settings-" + [guid]::NewGuid().ToString("N"))
New-Item -ItemType Directory -Path $work | Out-Null
$appExe = Join-Path $work "zoreview.exe"
Copy-Item $App $appExe
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
  Check ($text -match [regex]::Escape($pattern)) ("{0} (expected '{1}')" -f $name, $pattern)
}

function LogPath() {
  Join-Path $work "logs\zoreview.log"
}

function RunApp([string]$label, [string[]]$arguments, [int]$timeout = 30000) {
  $logPath = LogPath
  Remove-Item $logPath -ErrorAction SilentlyContinue
  $all = @("--portable") + $arguments
  $process = Start-Process -FilePath $appExe -ArgumentList $all -PassThru
  if (-not $process.WaitForExit($timeout)) {
    $process.Kill()
    throw "zoreview.exe did not exit in time: $label"
  }
  Start-Sleep -Milliseconds 100
  if (Test-Path $logPath) { return (Get-Content $logPath -Raw) }
  return ""
}

function ReadValues([string]$path) {
  $values = @{}
  if (-not (Test-Path $path)) { return $values }
  foreach ($line in Get-Content $path) {
    $parts = $line.Split("=", 2)
    if ($parts.Count -eq 2) { $values[$parts[0]] = $parts[1] }
  }
  return $values
}

$dumpPath = Join-Path $work "settings.txt"
function Settings([string]$label, [string[]]$arguments) {
  Remove-Item $dumpPath -ErrorAction SilentlyContinue
  $all = @("--open-settings") + $arguments + @("--settings-dump", $dumpPath, $firstImage)
  $log = RunApp $label $all
  if (-not (Test-Path $dumpPath)) { throw "no settings dump for $label" }
  return @{ Values = (ReadValues $dumpPath); Log = $log }
}

$printPath = Join-Path $work "print.txt"
function Printed([string]$label) {
  Remove-Item $printPath -ErrorAction SilentlyContinue
  $log = RunApp $label @("--print-settings", $printPath)
  return @{ Values = (ReadValues $printPath); Log = $log }
}

$layoutPath = Join-Path $work "layout.txt"
function Layout([string]$label, [string]$image = $firstImage, [string[]]$extra = @()) {
  Remove-Item $layoutPath -ErrorAction SilentlyContinue
  $log = RunApp $label (@("--layout-dump", $layoutPath) + $extra + @($image))
  if (-not (Test-Path $layoutPath)) { throw "no layout dump for $label" }
  return @{ Values = (ReadValues $layoutPath); Log = $log }
}

function Rect([hashtable]$values, [string]$key) {
  if (-not $values.ContainsKey($key)) { return $null }
  $n = $values[$key].Split(",") | ForEach-Object { [double]$_ }
  if ($n.Count -ne 4) { return $null }
  return [pscustomobject]@{ L = $n[0]; T = $n[1]; R = $n[2]; B = $n[3]; W = $n[2] - $n[0]; H = $n[3] - $n[1] }
}

function Inside([object]$inner, [object]$outer) {
  if ($null -eq $inner -or $null -eq $outer) { return $false }
  return ($inner.L -ge $outer.L - 0.5 -and $inner.T -ge $outer.T - 0.5 -and
          $inner.R -le $outer.R + 0.5 -and $inner.B -le $outer.B + 0.5)
}

# --- Samples ----------------------------------------------------------------
Add-Type -AssemblyName System.Drawing
$bitmap = New-Object System.Drawing.Bitmap(800, 600)
$graphics = [System.Drawing.Graphics]::FromImage($bitmap)
$graphics.Clear([System.Drawing.Color]::FromArgb(40, 90, 160))
$graphics.Dispose()
$firstImage = Join-Path $work "sample.bmp"
$bitmap.Save($firstImage, [System.Drawing.Imaging.ImageFormat]::Bmp)
$bitmap.Dispose()
$settingsIni = Join-Path $work "settings.ini"
$output = Join-Path $repo "tests\output\settings"
New-Item -ItemType Directory -Force -Path $output | Out-Null

# Two images in their own folder are used by the wrap-policy checks.
$pairDir = Join-Path $work "pair"
New-Item -ItemType Directory -Force -Path $pairDir | Out-Null
$pairFirst = Join-Path $pairDir "a.bmp"
$pairSecond = Join-Path $pairDir "b.bmp"
Copy-Item $firstImage $pairFirst
Copy-Item $firstImage $pairSecond

try {
  # --- 1. Window opens on the general page ---------------------------------
  $open = Settings "open" @()
  Check ($open.Values["open"] -eq "1") "the settings window opens"
  Check ($open.Values["page"] -eq "general") "it starts on the general page"
  Check ([int]$open.Values["nav_rows"] -eq 3) "three page entries are laid out"
  Check ([int]$open.Values["rows"] -eq 13) "the general page has 13 rows"
  CheckHas $open.Log "settings: opened" "opening is logged"
  Check ($open.Values["language"] -eq "system" -or $open.Values["language"] -eq "zh" -or $open.Values["language"] -eq "en") "the language value is valid"
  $titleRect = Rect $open.Values "title_rect"
  $closeRect = Rect $open.Values "close_rect"
  Check ($null -ne $titleRect -and $titleRect.H -gt 0) "the title rectangle is reported"
  Check ($null -ne $closeRect -and $closeRect.R -le 950.5) "the close button is inside the window"

  # --- 1b. #13: the strip radio starts on "auto" in a clean portable dir ----
  # 規劃/26 §2.11: the default was always auto (settings.cpp:132/164); this
  # pins the settings page to it so a regression in the radio order or in the
  # model mapping is caught here instead of at a user sighting.
  Check ($open.Values["strip_mode"] -eq "auto") "a clean portable dir starts the strip radio on auto (#13)"
  $stripRound = Settings "strip-roundtrip" @(
    "--settings-click", "row.strip.2",
    "--settings-click", "row.strip.0")
  Check ($stripRound.Values["strip_mode"] -eq "auto") "off -> auto lands back on auto (option order)"

  # --- 2. Page switching ---------------------------------------------------
  $view = Settings "page-view" @("--settings-page", "view")
  Check ($view.Values["page"] -eq "view") "the view page opens directly"
  Check ([int]$view.Values["rows"] -eq 8) "the view page has 8 rows"
  $assoc = Settings "page-assoc" @("--settings-page", "association")
  Check ($assoc.Values["page"] -eq "association") "the association page opens directly"
  Check ([int]$assoc.Values["assoc_count"] -eq 50) "50 document extensions are listed"
  Check ([int]$assoc.Values["rows"] -eq 0) "the association page has no setting rows"

  $switchBack = Settings "switch-back" @("--settings-click", "page.view", "--settings-click", "page.general")
  Check ($switchBack.Values["page"] -eq "general") "clicking the nav switches pages"
  Check ([int]$switchBack.Values["rows"] -eq 13) "the general rows are rebuilt after switching"

  # --- 3. General page values persist -------------------------------------
  $changed = Settings "general-change" @(
    "--settings-click", "row.theme.1",
    "--settings-click", "row.checker_opacity.25",
    "--settings-click", "row.checker",
    "--settings-click", "row.strip.2",
    "--settings-click", "row.birdseye",
    "--settings-click", "row.remember",
    "--settings-click", "row.single",
    "--settings-click", "row.topmost")
  Check ($changed.Values["theme"] -eq "light") "the theme radio changes the model"
  Check ($changed.Values["dark_theme"] -eq "0") "light resolves dark_theme=0"
  Check ([int]$changed.Values["checker_opacity"] -eq 25) "the opacity slider sets 25"
  Check ($changed.Values["checker"] -eq "0") "the checkerboard checkbox toggles"
  Check ($changed.Values["strip_mode"] -eq "off") "the strip radio selects off"
  Check ($changed.Values["birdseye"] -eq "1") "the birdseye checkbox toggles"
  Check ($changed.Values["remember_window"] -eq "0") "remember-window toggles"
  Check ($changed.Values["single_instance"] -eq "1") "single-instance toggles"
  Check ($changed.Values["topmost"] -eq "1") "always-on-top toggles"
  CheckHas $changed.Log "settings: change=theme" "a change is logged"
  CheckHas $changed.Log "settings: applied=theme" "the app applies the change"
  Check (-not (Test-Path ($settingsIni + ".tmp"))) "the atomic save leaves no .tmp"

  $printed = Printed "print-general"
  Check ($printed.Values["theme"] -eq "light") "the theme survives a restart"
  Check ([int]$printed.Values["checker_opacity"] -eq 25) "the opacity survives a restart"
  Check ($printed.Values["thumbstrip_mode"] -eq "off") "the strip mode survives a restart"
  Check ($printed.Values["remember_window"] -eq "0") "remember-window survives a restart"
  Check ($printed.Values["single_instance"] -eq "1") "single-instance survives a restart"

  $applied = Layout "apply-to-main"
  Check ($applied.Values["strip_mode"] -eq "off") "the main window receives the strip mode"
  CheckHas $applied.Log "ui: high_contrast=0" "the theme is applied to the main window"

  # --- 4. View page values persist ----------------------------------------
  $viewChange = Settings "view-change" @(
    "--settings-page", "view",
    "--settings-click", "row.wheel.1",
    "--settings-click", "row.zoom_step.3",
    "--settings-click", "row.pixel",
    "--settings-click", "row.keep_rotation",
    "--settings-click", "row.fullscreen_bg.1",
    "--settings-click", "row.folder_wrap.1")
  Check ($viewChange.Values["page"] -eq "view") "the view page stays open"
  Check ($viewChange.Values["wheel"] -eq "page") "the wheel mode becomes page"
  Check ($viewChange.Values["zoom_step"] -eq "1.25") "the zoom step becomes 1.25"
  Check ($viewChange.Values["pixel_mode"] -eq "1") "pixel mode turns on"
  Check ($viewChange.Values["keep_rotation"] -eq "0") "keep-rotation turns off"
  Check ($viewChange.Values["fullscreen_bg"] -eq "black") "the fullscreen background becomes black"
  Check ($viewChange.Values["folder_wrap"] -eq "next") "the wrap policy becomes stop"
  $viewPrinted = Printed "print-view"
  Check ($viewPrinted.Values["wheel"] -eq "page") "the wheel mode survives a restart"
  Check ($viewPrinted.Values["zoom_step"] -eq "1.25") "the zoom step survives a restart"
  Check ($viewPrinted.Values["fullscreen_bg"] -eq "black") "the fullscreen background survives"
  Check ($viewPrinted.Values["folder_wrap"] -eq "next") "the wrap policy survives"

  # --- 4b. The wrap policy changes navigation at the folder edges ---------
  # "next" peeks at the sibling folders; this fixture has none, so the policy
  # falls back to the in-folder loop instead of stopping (22/10 §4.2).
  $wrapNext = Layout "wrap-next" $pairFirst @("--send-keys", "End,Right")
  Check ([int]$wrapNext.Values["folder_index"] -eq 0) "wrap=next falls back to loop without a sibling image folder"
  CheckHas $wrapNext.Log "nav: peek dir=1 probed=0 found=0" "the failed peek is logged"

  $null = Settings "wrap-ask" @("--settings-page", "view", "--settings-click", "row.folder_wrap.2")
  $askOpen = Layout "wrap-ask-open" $pairFirst @("--send-keys", "End,Right")
  Check ($askOpen.Values["confirm_open"] -eq "1") "wrap=ask opens the prompt"
  CheckHas $askOpen.Log "confirm: open=wrap next" "the prompt is logged"
  $askAccept = Layout "wrap-ask-accept" $pairFirst @("--send-keys", "End,Right,Enter")
  Check ($askAccept.Values["confirm_open"] -eq "0") "Enter closes the prompt"
  Check ([int]$askAccept.Values["folder_index"] -eq 0) "Enter continues from the first image"
  CheckHas $askAccept.Log "nav: wrap=first" "the wrap is logged"
  $null = Settings "wrap-loop" @("--settings-page", "view", "--settings-click", "row.folder_wrap.0")

  # --- 4c. Single instance forwards a second launch -----------------------
  # general-change above already turned single_instance on and persisted it.
  $firstProcess = Start-Process -FilePath $appExe -ArgumentList @("--portable", $firstImage) -PassThru
  Start-Sleep -Seconds 3
  $logPath = LogPath
  $before = if (Test-Path $logPath) { (Get-Content $logPath).Count } else { 0 }
  $secondProcess = Start-Process -FilePath $appExe -ArgumentList @("--portable", $pairSecond) -PassThru
  $secondExited = $secondProcess.WaitForExit(8000)
  Check $secondExited "the second instance exits after forwarding"
  if (-not $secondExited) { $secondProcess.Kill() }
  Start-Sleep -Milliseconds 500
  $firstProcess.Kill()
  $null = $firstProcess.WaitForExit(5000)
  $forwardLog = if (Test-Path $logPath) { (Get-Content $logPath | Select-Object -Skip $before) -join "`n" } else { "" }
  CheckHas $forwardLog "single_instance: forwarded to existing window" "the second instance is forwarded"
  CheckHas $forwardLog "single_instance: open=" "the forwarded path is logged"

  # --- 4d. Remember-window saves a placement ------------------------------
  # general-change turned remember-window off; turn it back on for this check.
  $null = Settings "placement-on" @("--settings-click", "row.remember")
  $placementProcess = Start-Process -FilePath $appExe -ArgumentList @("--portable", $firstImage) -PassThru
  Start-Sleep -Seconds 3
  $null = $placementProcess.CloseMainWindow()
  $null = $placementProcess.WaitForExit(8000)
  $placement = Printed "placement"
  Check ($placement.Values["window_saved"] -eq "1") "closing the window stores a placement"
  $placementParts = $placement.Values["window"].Split(",")
  Check ($placementParts.Count -eq 3 -and [int]($placementParts[2].Split("x")[0]) -ge 640) "the stored placement has a sane size"

  # --- 5. Restore defaults keeps language and window placement ------------
  $restoreFirst = Settings "restore-first" @("--settings-click", "footer.reset")
  Check ($restoreFirst.Values["theme"] -eq "light") "the first restore click only asks"
  $restoreSecond = Settings "restore-second" @(
    "--settings-click", "footer.reset", "--settings-click", "footer.reset")
  Check ($restoreSecond.Values["theme"] -eq "dark") "restore resets the theme"
  Check ([int]$restoreSecond.Values["checker_opacity"] -eq 60) "restore resets the opacity"
  Check ($restoreSecond.Values["strip_mode"] -eq "auto") "restore resets the strip to auto"
  Check ($restoreSecond.Values["zoom_step"] -eq "1.15") "restore resets the zoom step"
  Check ($restoreSecond.Values["folder_wrap"] -eq "loop") "restore resets the wrap policy"
  Check ($restoreSecond.Values["remember_window"] -eq "1") "restore turns remember-window back on"
  Check (-not [string]::IsNullOrEmpty($restoreSecond.Values["language"])) "restore keeps a language value"

  # --- 6. Migration from the pre-S2 [ui] section --------------------------
  Set-Content -Path $settingsIni -Encoding ASCII -Value @(
    "[ui]",
    "dark_theme=0",
    "checkerboard=0",
    "language=en",
    "thumbstrip_mode=auto",
    "sort_mode=size",
    "birdseye=1",
    "topmost=1",
    "keep_rotation=0",
    "zoom_step=1.10",
    "")
  $migrated = Printed "migrated"
  Check ($migrated.Values["theme"] -eq "light") "migration maps dark_theme=0 to light"
  Check ($migrated.Values["checkerboard"] -eq "0") "migration carries the checkerboard"
  Check ($migrated.Values["language"] -eq "en") "migration carries the language"
  Check ($migrated.Values["thumbstrip_mode"] -eq "auto") "migration carries the strip mode"
  # v3.0 removed the sort UI, but the legacy value is kept on disk (no consumer);
  # this check is what guarantees "旧值不用丢" during migration.
  Check ($migrated.Values["sort_mode"] -eq "size") "migration preserves the legacy sort value"
  Check ($migrated.Values["zoom_step"] -eq "1.10") "migration carries the zoom step"
  Check ((Get-Content $settingsIni -Raw) -notmatch "\[ui\]") "migration removes the legacy section"
  Check ((Get-Content $settingsIni -Raw) -match "version=1") "migration writes schema version 1"

  # --- 7. Recovery from an unreadable file --------------------------------
  [IO.File]::WriteAllBytes($settingsIni, [byte[]](0x5B, 0x75, 0x69, 0x5D, 0x00, 0x41))
  $recovered = Printed "recovered"
  Check ($recovered.Values["theme"] -eq "dark") "recovery falls back to the default theme"
  Check ([int]$recovered.Values["checker_opacity"] -eq 60) "recovery falls back to 60 percent"
  $backups = Get-ChildItem $work -Filter "settings.ini.bak-*"
  Check ($backups.Count -ge 1) "the unreadable file is preserved as a backup"
  CheckHas $recovered.Log "settings: unreadable file backed up" "recovery is logged"

  # --- 8. Association page ------------------------------------------------
  $noneSelected = Settings "assoc-none" @(
    "--settings-page", "association",
    "--settings-click", "assoc.select_all",
    "--settings-click", "assoc.cell.0",
    "--settings-click", "assoc.cell.1",
    "--settings-click", "assoc.register")
  Check ($noneSelected.Values["page"] -eq "association") "association actions keep the page"
  Check ([int]$noneSelected.Values["assoc_registered"] -ge 2) "registering two extensions updates the count"
  Check ([int]$noneSelected.Values["assoc_selected"] -eq 2) "the selection is kept"
  CheckHas $noneSelected.Log "settings: assoc action=1" "the register action is logged"

  $extensionKey = "Software\Classes\.jpg\OpenWithProgids"
  $hasOpenWith = {
    $key = [Microsoft.Win32.Registry]::CurrentUser.OpenSubKey($extensionKey)
    if ($null -eq $key) { return $false }
    try { return ($key.GetValueNames() -contains "ZoreView.Image") }
    finally { $key.Close() }
  }
  Check (& $hasOpenWith) "the .jpg OpenWith entry exists after registering"

  $removed = Settings "assoc-remove" @(
    "--settings-page", "association",
    "--settings-click", "assoc.select_all",
    "--settings-click", "assoc.cell.0",
    "--settings-click", "assoc.cell.1",
    "--settings-click", "assoc.unregister")
  Check ([int]$removed.Values["assoc_registered"] -eq 0) "unregister removes both extensions again"
  Check (-not (& $hasOpenWith)) "the .jpg OpenWith entry is gone"

  $confirm = Settings "assoc-default-confirm" @(
    "--settings-page", "association",
    "--settings-click", "assoc.default")
  CheckHas $confirm.Log "settings: assoc action=3" "the default action is reached"
  Check ($confirm.Values["assoc_defaults"] -eq "0") "the first default click does not take over"

  # --- 9. DPI scaling ------------------------------------------------------
  foreach ($scale in @(100, 125, 150, 200)) {
    $shot = Join-Path $output ("general-dpi{0}.png" -f $scale)
    Remove-Item $shot -ErrorAction SilentlyContinue
    $dpi = Settings ("dpi-{0}" -f $scale) @("--force-dpi-scale", "$scale", "--settings-screenshot", "`"$shot`"")
    $title = Rect $dpi.Values "title_rect"
    $close = Rect $dpi.Values "close_rect"
    Check ($null -ne $title -and [math]::Abs($title.H - (32 * $scale / 100.0)) -lt 1.0) ("forced {0}%: title height scales" -f $scale)
    Check ($null -ne $close -and $close.L -ge 0) ("forced {0}%: close button is laid out" -f $scale)
    Check (Test-Path $shot) ("forced {0}%: screenshot written" -f $scale)
  }

  # --- 10. Screenshots: light, high contrast, English, association --------
  $lightShot = Join-Path $output "general-light.png"
  $null = Settings "shot-light" @("--settings-click", "row.theme.1", "--settings-screenshot", "`"$lightShot`"")
  Check (Test-Path $lightShot) "light-theme screenshot written"

  $hcShot = Join-Path $output "general-hc.png"
  $null = Settings "shot-hc" @("--force-high-contrast", "--settings-screenshot", "`"$hcShot`"")
  Check (Test-Path $hcShot) "high-contrast screenshot written"

  $enShot = Join-Path $output "general-en.png"
  $null = Settings "shot-en" @("--language", "en", "--settings-screenshot", "`"$enShot`"")
  Check (Test-Path $enShot) "English screenshot written"

  $assocShot = Join-Path $output "association.png"
  $null = Settings "shot-assoc" @("--settings-page", "association", "--settings-screenshot", "`"$assocShot`"")
  Check (Test-Path $assocShot) "association-page screenshot written"

  # Restore the defaults so the next manual run starts from a clean model.
  $null = Settings "final-reset" @("--settings-click", "footer.reset", "--settings-click", "footer.reset")
}
finally {
  if (-not $KeepOutput) {
    Remove-Item $work -Recurse -Force -ErrorAction SilentlyContinue
  } else {
    Write-Host ("Output kept in {0}" -f $work)
  }
}

Write-Host ""
if ($script:failures -eq 0) {
  Write-Host ("OK: {0} settings checks passed" -f $script:checks) -ForegroundColor Green
} else {
  Write-Host ("FAILED: {0} of {1} settings checks failed" -f $script:failures, $script:checks) -ForegroundColor Red
  exit 1
}
