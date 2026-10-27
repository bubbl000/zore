# v2.0 S1 (plan 22/03): startup empty state, format-line derivation, the
# all-formats card and the OLE drop paths.
#
# The script runs a throw-away copy of the executable in portable mode, so it
# never touches the developer's %LOCALAPPDATA% settings or caches (plan 22/00
# rule 10). All paths and strings here are ASCII on purpose (HANDOFF 60/124).
#
# Everything is driven through --layout-dump, --simulate-drop and
# --simulate-ole-drop (the latter calls the real IDropTarget with a real
# CF_HDROP data object); the actual mouse gesture cannot be synthesized without
# shell automation.

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

$work = Join-Path $env:TEMP ("nv-empty-" + [guid]::NewGuid().ToString("N"))
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

function Log() {
  Join-Path $work "logs\zoreview.log"
}

# --- Samples ---------------------------------------------------------------
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
$emptyFolder = Join-Path $work "empty"
New-Item -ItemType Directory -Path $emptyFolder | Out-Null
$textFile = Join-Path $work "notes.txt"
Set-Content -LiteralPath $textFile -Value "not an image" -Encoding ASCII

# --- Helpers ---------------------------------------------------------------
function RunApp([string[]]$arguments) {
  $logPath = Log
  Remove-Item $logPath -ErrorAction SilentlyContinue
  $all = @("--portable", "--language", "en") + $arguments
  $process = Start-Process -FilePath $appExe -ArgumentList $all -PassThru
  if (-not $process.WaitForExit(30000)) {
    $process.Kill()
    throw "zoreview.exe did not exit in time"
  }
  Start-Sleep -Milliseconds 120
  if (Test-Path $logPath) { return (Get-Content $logPath -Raw) }
  return ""
}

$dumpPath = Join-Path $work "layout.txt"
function Dump([string]$label, [string[]]$arguments) {
  Remove-Item $dumpPath -ErrorAction SilentlyContinue
  $all = @("--layout-dump", $dumpPath) + $arguments
  $log = RunApp $all
  if (-not (Test-Path $dumpPath)) { throw "no layout dump for $label" }
  $values = @{}
  foreach ($line in Get-Content $dumpPath) {
    $parts = $line.Split("=", 2)
    if ($parts.Count -eq 2) { $values[$parts[0]] = $parts[1] }
  }
  return @{ Values = $values; Log = $log }
}

function Shot([string]$name, [string[]]$arguments) {
  $path = Join-Path $outDirectory ("{0}.png" -f $name)
  Remove-Item $path -ErrorAction SilentlyContinue
  $all = @("--screenshot", $path) + $arguments
  $log = RunApp $all
  return @{ Path = $path; Log = $log }
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

# V3.1-06 (§06a): copies the BGRA bytes of a PNG once; GetPixel per sample is far
# too slow for a 1280x800 comparison (same helper as run_minimap_check.ps1).
function Read-Pixels([string]$path) {
  $bitmap = New-Object System.Drawing.Bitmap($path)
  $width = $bitmap.Width
  $height = $bitmap.Height
  $rect = New-Object System.Drawing.Rectangle(0, 0, $width, $height)
  $data = $bitmap.LockBits($rect,
    [System.Drawing.Imaging.ImageLockMode]::ReadOnly,
    [System.Drawing.Imaging.PixelFormat]::Format32bppArgb)
  $stride = $data.Stride
  $bytes = New-Object byte[] ($stride * $height)
  [System.Runtime.InteropServices.Marshal]::Copy($data.Scan0, $bytes, 0,
    $bytes.Length)
  $bitmap.UnlockBits($data)
  $bitmap.Dispose()
  return @{ Bytes = $bytes; Stride = $stride; W = $width; H = $height }
}

function ClientRect([hashtable]$values) {
  if (-not $values.ContainsKey("client")) { return $null }
  $parts = $values["client"].Split("x")
  if ($parts.Count -ne 2) { return $null }
  $w = [double]$parts[0]
  $h = [double]$parts[1]
  return @{ L = 0; T = 0; R = $w; B = $h; W = $w; H = $h }
}

$outDirectory = Join-Path $repo "tests\output\empty-state"
New-Item -ItemType Directory -Force -Path $outDirectory | Out-Null

try {
  # --- 1. Default empty state ---------------------------------------------
  $empty = Dump "empty-default" @()
  $v = $empty.Values
  Check ($v["empty"] -eq "1") "the empty card is visible without an image"
  Check ($v["empty_kind"] -eq "empty") "the default state is the empty state"
  Check ([int]$v["empty_lines"] -ge 1) ("the format line has at least one line (got {0})" -f $v["empty_lines"])
  Check ([int]$v["empty_lines"] -le 2) "the format line never exceeds two lines"
  Check ($v["empty_compact"] -eq "0") "a default window keeps the decorative icon"
  Check ($v["has_image"] -eq "0") "no image is reported"
  Check ($v["loading"] -eq "0") "nothing is loading"
  Check ([int]$v["format_count"] -ge 15) ("the derived format count is plausible (got {0})" -f $v["format_count"])
  Check ($v["format_highlights"] -match "JPG") "the format line contains JPG"
  Check ($v["format_highlights"] -match "PNG") "the format line contains PNG"
  Check ($v["format_highlights"] -match "PSD") "the format line contains PSD"
  if ($empty.Log -match "libraw=1") {
    Check ($v["format_highlights"] -match "RAW") "the format line contains RAW"
  }
  if ($empty.Log -match "freeimage=1") {
    Check ($v["format_highlights"] -match "HDR") "the format line contains HDR"
  }
  CheckNotHas $empty.Log "Error" "the empty startup logs no error"
  CheckHas $empty.Log "format: freeimage=" "the format capabilities are probed once"
  CheckHas $empty.Log "drop: register ok" "the OLE drop target registers"

  $card = Rect $v "empty_card"
  $content = Rect $v "content"
  $client = ClientRect $v
  $primary = Rect $v "empty_primary"
  $openFolder = Rect $v "empty_open_folder"
  $allFormats = Rect $v "empty_all_formats"
  $dpi = [double]$v["dpi"]
  Check (Inside $card $content) "the empty card stays inside the content rectangle"
  Check (Inside $primary $card) "the primary button stays inside the card"
  Check (Inside $openFolder $card) "open folder stays inside the card"
  Check (Inside $allFormats $card) "all formats stays inside the card"
  Check ($allFormats.L -gt $openFolder.R) "the two footer buttons do not overlap"
  Check ([math]::Abs((($card.L + $card.R) / 2) - (($content.L + $content.R) / 2)) -lt 1.0) "the card is horizontally centred"
  Check ([math]::Abs((($card.T + $card.B) / 2) - (($content.T + $content.B) / 2)) -lt 1.0) "the card is vertically centred"
  Check ([math]::Abs($primary.H - (36 * $dpi)) -lt 1.0) ("the primary button is 36 DIP tall ({0} px)" -f $primary.H)
  Check ([math]::Abs($primary.W - (132 * $dpi)) -lt 1.5) ("the primary button is 132 DIP wide ({0} px)" -f $primary.W)
  Check ($card.W -le (520 * $dpi) + 1.0) ("the card is at most 520 DIP wide ({0} px)" -f $card.W)

  $emptyShot = Shot "empty-en" @()
  Check (Test-Path $emptyShot.Path) "the empty-state screenshot is written"
  Check ($emptyShot.Log -match "screenshot saved") "the screenshot is confirmed in the log"

  # --- 2. Drag-over highlight ---------------------------------------------
  $drag = Dump "drag-over" @("--simulate-drag-over")
  Check ($drag.Values["drag_over"] -eq "1") "the drag-over state is reported"
  CheckHas $drag.Log "drop: enter (simulated)" "the drag overlay is logged"
  $hint = Rect $drag.Values "drag_hint"
  Check (Inside $hint (Rect $drag.Values "content")) "the release hint stays inside the content"
  Check ($hint.W -gt 0 -and $hint.H -gt 0) "the release hint has a size"
  $dragShot = Shot "empty-drag" @("--simulate-drag-over")
  Check (Test-Path $dragShot.Path) "the drag-over screenshot is written"

  # V3.1-06 (§06a): the hint is centred in the content and the window outside the
  # canvas must not change a single pixel (the old whole-client border did).
  $content = Rect $drag.Values "content"
  Check ([math]::Abs((($hint.L + $hint.R) / 2) - (($content.L + $content.R) / 2)) -lt 1.0) `
    "the release hint is horizontally centred in the content"
  Check ([math]::Abs((($hint.T + $hint.B) / 2) - (($content.T + $content.B) / 2)) -lt 1.0) `
    "the release hint is vertically centred in the content"
  $emptyPixels = Read-Pixels $emptyShot.Path
  $dragPixels = Read-Pixels $dragShot.Path
  $outside = 0
  $inside = 0
  $step = 4
  for ($y = 0; $y -lt $emptyPixels.H; $y += $step) {
    for ($x = 0; $x -lt $emptyPixels.W; $x += $step) {
      $index = $y * $emptyPixels.Stride + $x * 4
      $different = ($emptyPixels.Bytes[$index] -ne $dragPixels.Bytes[$index]) -or
                   ($emptyPixels.Bytes[$index + 1] -ne $dragPixels.Bytes[$index + 1]) -or
                   ($emptyPixels.Bytes[$index + 2] -ne $dragPixels.Bytes[$index + 2])
      $inCanvas = ($x -ge $content.L) -and ($x -le $content.R) -and
                  ($y -ge $content.T) -and ($y -le $content.B)
      if ($inCanvas) {
        if ($different) { $inside++ }
      } elseif ($different) {
        $outside++
      }
    }
  }
  Check ($outside -eq 0) ("a drag-over changes nothing outside the canvas (diff={0})" -f $outside)
  Check ($inside -gt 100) ("the canvas itself is highlighted (diff={0})" -f $inside)

  # --- 2b. The OLE layer itself -------------------------------------------
  # --simulate-ole-drop builds a real CF_HDROP data object and calls the
  # registered IDropTarget, so ReadPaths/effects/hover are exercised too.
  $oleFile = Dump "ole-file" @("--simulate-ole-drop", $firstImage)
  Check ($oleFile.Values["has_image"] -eq "1") "an OLE file drop opens the image"
  CheckHas $oleFile.Log "drop: enter" "the OLE drag enters the hover state"
  CheckHas $oleFile.Log "drop: leave" "the OLE drop leaves the hover state"
  CheckHas $oleFile.Log "test: ole DragEnter hr=0x00000000 effect=1" "DragEnter negotiates COPY"
  CheckHas $oleFile.Log "test: ole DragOver effect=1" "DragOver keeps COPY"
  CheckHas $oleFile.Log "test: ole Drop hr=0x00000000 effect=1" "Drop reports COPY"
  $oleFolder = Dump "ole-folder" @("--simulate-ole-drop", $work)
  Check ($oleFolder.Values["has_image"] -eq "1") "an OLE folder drop opens its first image"
  Check ([int]$oleFolder.Values["folder_files"] -eq 3) "the OLE folder drop lists three images"
  CheckHas $oleFolder.Log "dir=1" "the OLE folder drop is dispatched as a directory"
  $oleBad = Dump "ole-unsupported" @("--simulate-ole-drop", $textFile)
  Check ($oleBad.Values["empty_kind"] -eq "unsupported") "an OLE unsupported drop reports the note"

  # --- 3. Drop a file ------------------------------------------------------
  $file = Dump "drop-file" @("--simulate-drop", $firstImage)
  Check ($file.Values["has_image"] -eq "1") "dropping an image opens it"
  Check ($file.Values["empty"] -eq "0") "the empty card disappears after a drop"
  Check ([int]$file.Values["folder_files"] -eq 3) "the dropped file's folder lists three images"
  Check ([int]$file.Values["folder_index"] -eq 0) "the first file is selected"
  CheckHas $file.Log "drop: path=" "the drop path is logged"
  CheckHas $file.Log "dir=0" "the file drop is logged as a file"
  # A decode, or a cache hit for the same file earlier in this script run.
  Check (($file.Log -match "open request:") -or ($file.Log -match "disk cache hit:")) "the drop starts a load"

  # --- 4. Drop a folder ----------------------------------------------------
  $folder = Dump "drop-folder" @("--simulate-drop", $work)
  Check ($folder.Values["has_image"] -eq "1") "dropping a folder opens its first image"
  Check ([int]$folder.Values["folder_files"] -eq 3) "the folder lists its three images"
  Check ([int]$folder.Values["folder_index"] -eq 0) "the first image of the folder is selected"
  Check ($folder.Values["empty"] -eq "0") "the empty card is gone after a folder drop"
  CheckHas $folder.Log "folder: open " "the folder open is logged"
  CheckHas $folder.Log "dir=1" "the directory drop is logged as a directory"

  # --- 5. Drop an empty folder --------------------------------------------
  $emptyDrop = Dump "drop-empty-folder" @("--simulate-drop", $emptyFolder)
  Check ($emptyDrop.Values["empty"] -eq "1") "an empty folder shows the empty card"
  Check ($emptyDrop.Values["empty_kind"] -eq "folder") "the empty-folder state is reported"
  Check ([int]$emptyDrop.Values["folder_files"] -eq 0) "the empty folder lists no images"
  CheckHas $emptyDrop.Log "empty: state=folder" "the empty-folder state is logged"
  CheckNotHas $emptyDrop.Log "open request:" "an empty folder never starts a decode"
  $folderShot = Shot "empty-folder" @("--simulate-drop", $emptyFolder, "--shot-delay", "1800")
  Check (Test-Path $folderShot.Path) "the empty-folder screenshot is written"

  # --- 6. Drop an unsupported file without an image -----------------------
  $bad = Dump "drop-unsupported" @("--simulate-drop", $textFile)
  Check ($bad.Values["empty"] -eq "1") "an unsupported drop keeps the empty card"
  Check ($bad.Values["empty_kind"] -eq "unsupported") "the unsupported state is reported"
  $note = Rect $bad.Values "empty_note"
  Check ($note.H -gt 0) "the unsupported note has a rectangle"
  CheckHas $bad.Log "empty: state=unsupported" "the unsupported drop is logged"
  CheckHas $bad.Log "Unsupported format: .txt" "the note names the rejected extension"
  CheckNotHas $bad.Log "open request:" "an unsupported file never starts a decode"
  $badShot = Shot "empty-unsupported" @("--simulate-drop", $textFile, "--shot-delay", "1800")
  Check (Test-Path $badShot.Path) "the unsupported screenshot is written"

  # A missing path is rejected the same way instead of being decoded.
  $missing = Dump "drop-missing" @("--simulate-drop", (Join-Path $work "nope.png"))
  Check ($missing.Values["empty_kind"] -eq "unsupported") "a missing path reports unsupported"
  CheckHas $missing.Log "nope.png" "the missing file is named in the note"

  # --- 7. Unsupported drop while an image is open -------------------------
  $keep = Dump "drop-keep-image" @("--simulate-drop", $firstImage, "--simulate-drop", $textFile)
  Check ($keep.Values["has_image"] -eq "1") "an unsupported drop keeps the open image"
  Check ($keep.Values["empty"] -eq "0") "the empty card does not replace the image"
  CheckHas $keep.Log "ui: toast=" "the unsupported drop shows a toast"
  CheckHas $keep.Log "Unsupported format: .txt" "the toast names the rejected extension"

  # --- 8. All-formats card -------------------------------------------------
  $formats = Dump "formats-open" @("--click-hit", "empty.all_formats")
  $fv = $formats.Values
  Check ($fv["formats_open"] -eq "1") "All formats opens the modal card"
  Check ([int]$fv["formats_rows"] -ge 10) ("the card lists the families (got {0})" -f $fv["formats_rows"])
  Check ([int]$fv["format_families"] -ge 20) ("the card knows every family (got {0})" -f $fv["format_families"])
  $panel = Rect $fv "formats_panel"
  $formatsClient = ClientRect $fv
  $close = Rect $fv "formats_close"
  Check (Inside $panel $formatsClient) "the all-formats card stays inside the client"
  Check (Inside $close $panel) "the close button stays inside the card"
  Check ($close.W -gt 0 -and $close.H -gt 0) "the close button has a size"
  for ($i = 0; $i -lt [int]$fv["formats_rows"]; $i++) {
    $row = Rect $fv ("formats_row_{0}" -f $i)
    Check (Inside $row $panel) ("format row {0} stays inside the card" -f $i)
  }
  CheckHas $formats.Log "empty: activate=formats" "opening the card is logged"
  $formatsShot = Shot "empty-formats" @("--click-hit", "empty.all_formats", "--shot-delay", "2400")
  Check (Test-Path $formatsShot.Path) "the all-formats screenshot is written"

  $closed = Dump "formats-close" @("--click-hit", "empty.all_formats", "--click-hit", "formats.close")
  Check ($closed.Values["formats_open"] -eq "0") "the close button closes the card"
  CheckHas $closed.Log "cmd: formats=closed" "closing the card is logged"
  Check ($closed.Values["empty"] -eq "1") "the empty card is still behind the modal"

  # --- 9. DPI: the card scales without leaving the content ----------------
  foreach ($scale in @(100, 125, 150, 200)) {
    $dpiFile = Join-Path $work ("layout-{0}.txt" -f $scale)
    Remove-Item $dpiFile -ErrorAction SilentlyContinue
    $log = RunApp @("--force-dpi-scale", "$scale", "--layout-dump", $dpiFile)
    $values = @{}
    if (Test-Path $dpiFile) {
      foreach ($line in Get-Content $dpiFile) {
        $parts = $line.Split("=", 2)
        if ($parts.Count -eq 2) { $values[$parts[0]] = $parts[1] }
      }
    }
    $scaledCard = Rect $values "empty_card"
    $scaledContent = Rect $values "content"
    $scaledPrimary = Rect $values "empty_primary"
    $scaleFactor = [double]$values["dpi"]
    Check (Inside $scaledCard $scaledContent) ("forced {0}%: the card stays inside the content" -f $scale)
    Check ([math]::Abs($scaledPrimary.H - (36 * $scaleFactor)) -lt 1.0) ("forced {0}%: the primary button scales" -f $scale)
    Check ([math]::Abs($scaleFactor - ($scale / 100.0)) -lt 0.001) ("forced {0}%: the reported scale matches" -f $scale)
    Check ([math]::Abs((($scaledCard.L + $scaledCard.R) / 2) - (($scaledContent.L + $scaledContent.R) / 2)) -lt 1.0) ("forced {0}%: the card is centred" -f $scale)
  }

  # --- 10. Chinese --------------------------------------------------------
  $zhPath = Join-Path $work "layout-zh.txt"
  Remove-Item $zhPath -ErrorAction SilentlyContinue
  $zhLog = RunApp @("--language", "zh", "--layout-dump", $zhPath)
  $zh = @{}
  if (Test-Path $zhPath) {
    foreach ($line in Get-Content $zhPath) {
      $parts = $line.Split("=", 2)
      if ($parts.Count -eq 2) { $zh[$parts[0]] = $parts[1] }
    }
  }
  Check ($zh["empty"] -eq "1") "the empty card is visible in Chinese"
  Check (Inside (Rect $zh "empty_card") (Rect $zh "content")) "the Chinese card stays inside the content"
  $zhShot = Shot "empty-zh" @("--language", "zh")
  Check (Test-Path $zhShot.Path) "the Chinese screenshot is written"

  # --- 11. Tall window keeps the icon / small compact card ----------------
  $tall = Dump "empty-tall" @("--maximized")
  Check ($tall.Values["empty"] -eq "1") "the maximized empty window still shows the card"
  Check (Inside (Rect $tall.Values "empty_card") (Rect $tall.Values "content")) "the maximized card stays inside the content"
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
  Write-Host ("OK: {0} empty-state checks passed" -f $script:checks) -ForegroundColor Green
  exit 0
}
Write-Host ("FAILED: {0} of {1} empty-state checks failed" -f $script:failures, $script:checks) -ForegroundColor Red
exit 1
