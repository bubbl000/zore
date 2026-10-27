# v2.0 S1 (plan 22/11): error cards, danger confirmation, toasts and the
# loading indicator.
#
# Everything is deterministic: the app exposes --force-error/--force-toast/
# --force-confirm/--force-loading so each presentation can be checked without
# racing a decoder. The script is ASCII only (HANDOFF pitfall 60/124) and runs a
# throw-away portable copy of the executable (plan 22/00 rule 10).

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

$work = Join-Path $env:TEMP ("nv-errors-" + [guid]::NewGuid().ToString("N"))
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

function CheckNotHas([string]$text, [string]$pattern, [string]$name) {
  Check (-not ($text -match [regex]::Escape($pattern))) ("{0} (unexpected '{1}')" -f $name, $pattern)
}

function Log() { Join-Path $work "logs\zoreview.log" }

# --- Sample images ---------------------------------------------------------
Add-Type -AssemblyName System.Drawing
$samples = @(
  @{ Name = "a_wide.bmp";   W = 800; H = 600 },
  @{ Name = "b_tall.bmp";   W = 200; H = 1600 },
  @{ Name = "c_square.bmp"; W = 400; H = 400 }
)
foreach ($sample in $samples) {
  $bitmap = New-Object System.Drawing.Bitmap($sample.W, $sample.H)
  $graphics = [System.Drawing.Graphics]::FromImage($bitmap)
  $graphics.Clear([System.Drawing.Color]::FromArgb(40, 90, 160))
  $graphics.Dispose()
  $path = Join-Path $work $sample.Name
  $bitmap.Save($path, [System.Drawing.Imaging.ImageFormat]::Bmp)
  $bitmap.Dispose()
}
$firstImage = Join-Path $work "a_wide.bmp"

$dumpPath = Join-Path $work "layout.txt"

function RunApp([string]$label, [string[]]$arguments) {
  $logPath = Log
  Remove-Item $logPath -ErrorAction SilentlyContinue
  $all = @("--portable", "--language", "en") + $arguments
  $process = Start-Process -FilePath $appExe -ArgumentList $all -PassThru
  if (-not $process.WaitForExit(25000)) {
    $process.Kill()
    throw "zoreview.exe did not exit in time: $label"
  }
  Start-Sleep -Milliseconds 100
  if (Test-Path $logPath) { return (Get-Content $logPath -Raw) }
  return ""
}

function Dump([string]$label, [string[]]$arguments) {
  Remove-Item $dumpPath -ErrorAction SilentlyContinue
  $all = @("--layout-dump", $dumpPath) + $arguments
  $log = RunApp $label $all
  if (-not (Test-Path $dumpPath)) { throw "no layout dump for $label" }
  $values = @{}
  foreach ($line in Get-Content $dumpPath) {
    $parts = $line.Split("=", 2)
    if ($parts.Count -eq 2) { $values[$parts[0]] = $parts[1] }
  }
  return @{ Values = $values; Log = $log }
}

function Rect([hashtable]$values, [string]$key) {
  if (-not $values.ContainsKey($key)) { return $null }
  $numbers = $values[$key].Split(",") | ForEach-Object { [double]$_ }
  if ($numbers.Count -ne 4) { return $null }
  return @{ L = $numbers[0]; T = $numbers[1]; R = $numbers[2]; B = $numbers[3];
            W = $numbers[2] - $numbers[0]; H = $numbers[3] - $numbers[1] }
}

function Inside([hashtable]$inner, [hashtable]$outer) {
  if ($null -eq $inner -or $null -eq $outer) { return $false }
  return ($inner.L -ge $outer.L - 0.5) -and ($inner.T -ge $outer.T - 0.5) -and
         ($inner.R -le $outer.R + 0.5) -and ($inner.B -le $outer.B + 0.5)
}

function ClientRect([hashtable]$values) {
  $parts = $values["client"].Split("x")
  $w = [double]$parts[0]
  $h = [double]$parts[1]
  return @{ L = 0; T = 0; R = $w; B = $h; W = $w; H = $h }
}

try {
  # --- 1. Every error kind presents a card with the documented actions -----
  # Expected action counts follow plan 22/11 section 3.1; the close button is
  # reported separately (error_close) and is not part of the action count.
  $cases = @(
    @{ Kind = "missing";     Name = "file_missing";  Buttons = 2; Default = 0 },
    @{ Kind = "access";      Name = "access_denied"; Buttons = 2; Default = 0 },
    @{ Kind = "unsupported"; Name = "unsupported";   Buttons = 2; Default = 0 },
    @{ Kind = "corrupt";     Name = "corrupt";       Buttons = 3; Default = 0 },
    @{ Kind = "codec";       Name = "codec_missing"; Buttons = 2; Default = 0 },
    @{ Kind = "oom";         Name = "out_of_memory"; Buttons = 1; Default = 0 },
    @{ Kind = "save";        Name = "save_failed";   Buttons = 2; Default = 0 }
  )
  foreach ($case in $cases) {
    $result = Dump ("error-" + $case.Kind) @(
      "--force-error", $case.Kind, $firstImage
    )
    $v = $result.Values
    Check ($v["error_open"] -eq "1") ("{0}: the error card is open" -f $case.Kind)
    Check ($v["error_kind"] -eq $case.Name) ("{0}: the kind is reported" -f $case.Kind)
    Check ([int]$v["error_buttons"] -eq $case.Buttons) ("{0}: {1} action buttons" -f $case.Kind, $case.Buttons)
    Check ([int]$v["error_default"] -eq $case.Default) ("{0}: Enter targets the default action" -f $case.Kind)
    CheckHas $result.Log ("error: kind=" + $case.Name) ("{0}: the error is logged with its kind" -f $case.Kind)
    $card = Rect $v "error_card"
    $content = Rect $v "content"
    Check (Inside $card $content) ("{0}: the card is inside the image area" -f $case.Kind)
    Check ($card.W -le 480 * [double]$v["dpi"] + 1) ("{0}: the card respects its 480 DIP width" -f $case.Kind)
    $close = Rect $v "error_close"
    Check (Inside $close $card) ("{0}: Close is inside the card" -f $case.Kind)
    $allButtons = $true
    for ($i = 0; $i -lt $case.Buttons; $i++) {
      $button = Rect $v ("error_button_{0}" -f $i)
      if (-not (Inside $button $card)) { $allButtons = $false }
      if (-not ($button.T -eq (Rect $v "error_button_0").T)) { $allButtons = $false }
    }
    Check $allButtons ("{0}: every action button is inside the card on one row" -f $case.Kind)
  }

  # --- 2. Retry / copy / close / Escape / Enter ----------------------------
  $retry = Dump "error-retry" @(
    "--force-error", "corrupt", "--click-hit", "error.0", $firstImage
  )
  CheckHas $retry.Log "cmd: error=retry" "the retry button runs the retry action"
  Check ($retry.Values["error_open"] -eq "0") "retry closes the card"
  Check ($retry.Values["has_image"] -eq "1") "retry reloads the image"

  Set-Clipboard -Value "nv-sentinel"
  $copy = Dump "error-copy" @(
    "--force-error", "missing", "--click-hit", "error.0", $firstImage
  )
  CheckHas $copy.Log "cmd: error=copy_path" "the copy button copies the path"
  CheckHas $copy.Log "ui: toast=" "copying shows a toast"
  Start-Sleep -Milliseconds 120
  Check ((Get-Clipboard) -like "*a_wide.bmp") "the copied text is the full path"
  Check ($copy.Values["error_open"] -eq "1") "copying leaves the card open"

  $closed = Dump "error-close" @(
    "--force-error", "corrupt", "--click-hit", "error.close", $firstImage
  )
  Check ($closed.Values["error_open"] -eq "0") "the Close button dismisses the card"
  CheckHas $closed.Log "error: closed" "closing the card is logged"

  $escape = Dump "error-escape" @(
    "--force-error", "corrupt", "--send-keys", "Esc", $firstImage
  )
  Check ($escape.Values["error_open"] -eq "0") "Escape dismisses the error card"
  CheckHas $escape.Log "cmd: escape=error" "Escape reports the error layer"

  $enter = Dump "error-enter" @(
    "--force-error", "corrupt", "--send-keys", "Enter", $firstImage
  )
  CheckHas $enter.Log "cmd: error=retry" "Enter runs the default (retry) action"
  Check ($enter.Values["error_open"] -eq "0") "Enter closes the card"

  # --- 3. The card is not modal: navigation still works --------------------
  # A new image that actually loads dismisses the old failure (plan 22/11
  # pitfall 1); the important part is that Right is not blocked.
  $navigate = Dump "error-navigate" @(
    "--force-error", "corrupt", "--send-keys", "Right", $firstImage
  )
  Check ([int]$navigate.Values["folder_index"] -eq 1) "Right navigates while the error card is open"
  Check ($navigate.Values["error_open"] -eq "0") "a successful new image dismisses the old error"
  CheckHas $navigate.Log "error: cleared=new_image" "the dismissal is logged"

  # Rapid navigation cancels decodes; a cancel must never be reported as an
  # error (plan 22/11 pitfall 2).
  $cancel = Dump "error-cancel" @(
    "--send-keys", "Right,Right,Right,Right", $firstImage
  )
  CheckNotHas $cancel.Log "error: kind=" "rapid navigation reports no error"
  Check ($cancel.Values["error_open"] -eq "0") "rapid navigation opens no error card"

  # --- 4. Danger confirmation card ----------------------------------------
  $confirm = Dump "confirm-open" @("--force-confirm", $firstImage)
  Check ($confirm.Values["confirm_open"] -eq "1") "the delete confirmation opens"
  CheckHas $confirm.Log "confirm: open=delete" "opening the confirmation is logged"
  $confirmCard = Rect $confirm.Values "confirm_card"
  $client = ClientRect $confirm.Values
  $primary = Rect $confirm.Values "confirm_primary"
  $secondary = Rect $confirm.Values "confirm_secondary"
  Check (Inside $confirmCard $client) "the confirmation card is inside the client"
  Check (Inside $primary $confirmCard) "the primary button is inside the card"
  Check (Inside $secondary $confirmCard) "the secondary button is inside the card"
  Check ($primary.L -gt $secondary.R) "the destructive button is on the right"

  $cancelClick = Dump "confirm-cancel" @(
    "--force-confirm", "--click-hit", "confirm.secondary", $firstImage
  )
  Check ($cancelClick.Values["confirm_open"] -eq "0") "Cancel closes the confirmation"
  CheckHas $cancelClick.Log "confirm: action=cancel" "Cancel is logged"
  Check (Test-Path $firstImage) "Cancel keeps the file"

  $escapeConfirm = Dump "confirm-escape" @(
    "--force-confirm", "--send-keys", "Esc", $firstImage
  )
  Check ($escapeConfirm.Values["confirm_open"] -eq "0") "Escape cancels the confirmation"
  Check (Test-Path $firstImage) "Escape keeps the file"

  # The real Delete key opens the same card.
  $deleteKey = Dump "confirm-key" @(
    "--send-keys", "Delete", $firstImage
  )
  Check ($deleteKey.Values["confirm_open"] -eq "1") "the Delete key opens the confirmation"
  Check (Test-Path $firstImage) "the Delete key alone does not delete"

  # Primary deletes: run on a dedicated copy so the later checks keep their
  # three-image folder intact.
  $victim = Join-Path $work "zz_victim.bmp"
  Copy-Item $firstImage $victim
  $remove = Dump "confirm-delete" @(
    "--force-confirm", "--click-hit", "confirm.primary", $victim
  )
  CheckHas $remove.Log "confirm: action=primary" "the primary action is logged"
  CheckHas $remove.Log "deleted image:" "the primary action deletes the file"
  Check (-not (Test-Path $victim)) "the file is gone after confirming"

  # --- 5. Toasts: queue bound, merge, geometry -----------------------------
  $toasts = Dump "toasts" @(
    "--force-toast", "one", "--force-toast", "two",
    "--force-toast", "three", "--force-toast", "four", $firstImage
  )
  Check ([int]$toasts.Values["toast_count"] -eq 3) "at most three toasts are kept"
  CheckHas $toasts.Log "ui: toast=four" "the newest toast is queued"
  $toastClient = ClientRect $toasts.Values
  $toastBottom = Rect $toasts.Values "bottom"
  for ($i = 0; $i -lt 3; $i++) {
    $toast = Rect $toasts.Values ("toast_{0}" -f $i)
    Check (Inside $toast $toastClient) ("toast {0} stays inside the client" -f $i)
    Check ($toast.B -le $toastBottom.T + 0.5) ("toast {0} stays above the bottom bar" -f $i)
  }
  $toast0 = Rect $toasts.Values "toast_0"
  $toast1 = Rect $toasts.Values "toast_1"
  Check ($toast1.B -le $toast0.T) "toasts stack upwards"

  $merged = Dump "toast-merge" @(
    "--force-toast", "same", "--force-toast", "same", $firstImage
  )
  Check ([int]$merged.Values["toast_count"] -eq 1) "a repeated toast is merged"

  # 2.5 s lifetime: --send-keys owns the dump and the key sequence pushes it
  # past the expiry, so the toast must be gone without any click.
  $expired = Dump "toast-expiry" @(
    "--force-toast", "Fades", "--send-keys", "Right,Right,Right", $firstImage
  )
  Check ([int]$expired.Values["toast_count"] -eq 0) "a toast expires after 2.5 s"

  # --- 6. Loading indicator and its 400 ms threshold -----------------------
  $loading = Dump "loading-on" @("--force-loading", $firstImage)
  Check ($loading.Values["loading_shown"] -eq "1") "a slow load shows the indicator"
  $bar = Rect $loading.Values "loading_bar"
  $label = Rect $loading.Values "loading_label"
  $loadingContent = Rect $loading.Values "content"
  Check (Inside $bar $loadingContent) "the loading bar is inside the image area"
  Check ($label.T -ge $bar.B) "the loading label sits under the bar"
  Check (Inside $label $loadingContent) "the loading label stays in the image area"
  $match = [regex]::Match($loading.Log, "load: shown .*after_ms=(\d+)")
  Check $match.Success "the loading indicator logs when it appears"
  if ($match.Success) {
    Check ([int]$match.Groups[1].Value -ge 380) ("it appears only after the 400 ms delay (got {0})" -f $match.Groups[1].Value)
  }

  $fast = Dump "loading-fast" @("--force-loading", "300", $firstImage)
  Check ($fast.Values["loading_shown"] -eq "0") "a load shorter than 400 ms shows nothing"
  CheckHas $fast.Log "load: done ms=300 shown=0" "the fast load is logged without an indicator"
  CheckNotHas $fast.Log "load: shown" "the fast load never shows the indicator"

  # --- 7. DPI / fullscreen -------------------------------------------------
  foreach ($scale in @(100, 150, 200)) {
    $dpiFile = Join-Path $work ("error-dpi-{0}.txt" -f $scale)
    Remove-Item $dpiFile -ErrorAction SilentlyContinue
    $log = RunApp ("error-dpi-{0}" -f $scale) @(
      "--force-dpi-scale", "$scale", "--force-error", "corrupt",
      "--layout-dump", $dpiFile, $firstImage
    )
    $values = @{}
    if (Test-Path $dpiFile) {
      foreach ($line in Get-Content $dpiFile) {
        $parts = $line.Split("=", 2)
        if ($parts.Count -eq 2) { $values[$parts[0]] = $parts[1] }
      }
    }
    $card = Rect $values "error_card"
    $content = Rect $values "content"
    Check (Inside $card $content) ("forced {0}%: the error card stays in the content" -f $scale)
    $button = Rect $values "error_button_0"
    Check (Inside $button $card) ("forced {0}%: the action button stays in the card" -f $scale)
    Check ([math]::Abs([double]$values["dpi"] - ($scale / 100.0)) -lt 0.001) ("forced {0}%: the scale matches" -f $scale)
  }

  $full = Dump "error-fullscreen" @(
    "--fullscreen", "--force-error", "corrupt", $firstImage
  )
  Check ($full.Values["fullscreen"] -eq "1") "the fullscreen error run enters fullscreen"
  Check ($full.Values["error_open"] -eq "1") "the card is visible in fullscreen"
  $fullCard = Rect $full.Values "error_card"
  $fullContent = Rect $full.Values "content"
  Check (Inside $fullCard $fullContent) "the card is centred in the fullscreen content"

  # --- 8. No system message boxes remain ----------------------------------
  $mainCpp = Join-Path $repo "src\app\main.cpp"
  $mainText = Get-Content $mainCpp -Raw
  CheckNotHas $mainText "MessageBoxW(" "main.cpp no longer calls MessageBoxW"
  CheckNotHas $mainText "MessageBoxA(" "main.cpp no longer calls MessageBoxA"
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
  Write-Host ("OK: {0} error UI checks passed" -f $script:checks) -ForegroundColor Green
  exit 0
}
Write-Host ("FAILED: {0} of {1} error UI checks failed" -f $script:failures, $script:checks) -ForegroundColor Red
exit 1
