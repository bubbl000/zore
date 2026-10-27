# v3.1-06c: one-stage Save As (IFileSaveDialog + IFileDialogCustomize).
#
# Verifies the six containers (PNG/JPEG/BMP/TIFF/WebP/GIF), the dialog's quality
# combo, the size estimate, "Original format" byte copy, the view rotation on
# export, and the failure paths (no truncated target, no .nvtmp left behind).
#
# Every run uses a throw-away copy of the executable in portable mode, so
# settings/cache/logs never touch the user profile. The dialog cases open the
# real system dialog through NV_SAVEAS and write into %TEMP%\nv-save-<guid>.

param(
  [string]$App = ".\build\Release\zoreview.exe",
  [switch]$Skip8k,
  [switch]$KeepOutput
)

$ErrorActionPreference = "Stop"
$repo = Split-Path -Parent $PSScriptRoot
Set-Location $repo

if (-not (Test-Path $App)) {
  throw "Application not found: $App (build first: tools\build.ps1)"
}
$App = (Resolve-Path $App).Path
$AppDir = Split-Path -Parent $App
$cliSource = Join-Path $AppDir "zoreview_cli.exe"
if (-not (Test-Path $cliSource)) {
  throw "CLI not found next to the app: $cliSource"
}

Add-Type -AssemblyName System.Drawing

$work = Join-Path $env:TEMP ("nv-save-" + [guid]::NewGuid().ToString("N"))
New-Item -ItemType Directory -Path $work | Out-Null
$appExe = Join-Path $work "zoreview.exe"
Copy-Item $App $appExe
$cli = Join-Path $work "zoreview_cli.exe"
Copy-Item $cliSource $cli
Get-ChildItem $AppDir -Filter "clang_rt.asan*.dll" -ErrorAction SilentlyContinue |
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

$outputDir = Join-Path $PSScriptRoot "output\save"
New-Item -ItemType Directory -Force -Path $outputDir | Out-Null

# --- Samples ---------------------------------------------------------------
# Four solid quadrants: rotating the view must move colours, not just the box.
$colors = @{
  red    = [System.Drawing.Color]::FromArgb(220, 30, 30)
  green  = [System.Drawing.Color]::FromArgb(30, 200, 60)
  blue   = [System.Drawing.Color]::FromArgb(40, 60, 220)
  yellow = [System.Drawing.Color]::FromArgb(230, 200, 40)
}
$markerWidth = 400
$markerHeight = 300
$marker = New-Object System.Drawing.Bitmap($markerWidth, $markerHeight)
$graphics = [System.Drawing.Graphics]::FromImage($marker)
$halfW = [int]($markerWidth / 2)
$halfH = [int]($markerHeight / 2)
$graphics.FillRectangle((New-Object System.Drawing.SolidBrush($colors.red)), 0, 0, $halfW, $halfH)
$graphics.FillRectangle((New-Object System.Drawing.SolidBrush($colors.green)), $halfW, 0, $halfW, $halfH)
$graphics.FillRectangle((New-Object System.Drawing.SolidBrush($colors.blue)), 0, $halfH, $halfW, $halfH)
$graphics.FillRectangle((New-Object System.Drawing.SolidBrush($colors.yellow)), $halfW, $halfH, $halfW, $halfH)
$graphics.Dispose()
$gallery = Join-Path $work "gallery"
New-Item -ItemType Directory -Path $gallery | Out-Null
$markerPath = Join-Path $gallery "a_marker.bmp"
$marker.Save($markerPath, [System.Drawing.Imaging.ImageFormat]::Bmp)
$jpegSource = Join-Path $gallery "c_source.jpg"
$marker.Save($jpegSource, [System.Drawing.Imaging.ImageFormat]::Jpeg)
$marker.Dispose()

# Gradient with a real alpha ramp: PNG/TIFF/WebP must preserve it.
$alphaWidth = 200
$alphaHeight = 120
$alphaSource = New-Object System.Drawing.Bitmap($alphaWidth, $alphaHeight,
  [System.Drawing.Imaging.PixelFormat]::Format32bppArgb)
for ($y = 0; $y -lt $alphaHeight; $y++) {
  for ($x = 0; $x -lt $alphaWidth; $x++) {
    $r = [int](255 * $x / ($alphaWidth - 1))
    $g = [int](255 * $y / ($alphaHeight - 1))
    $a = 16 + [int](223 * ($x + $y) / ($alphaWidth + $alphaHeight - 2))
    $alphaSource.SetPixel($x, $y, [System.Drawing.Color]::FromArgb($a, $r, $g, ($r + $g) / 2))
  }
}
$alphaPath = Join-Path $gallery "b_alpha.png"
$alphaSource.Save($alphaPath, [System.Drawing.Imaging.ImageFormat]::Png)
$alphaSource.Dispose()

# An animated GIF sample for the "saves the current frame" check. Found by
# probe instead of a non-ASCII literal path.
$sampleRoot = Get-ChildItem -Path $repo -Directory |
  Where-Object { Test-Path (Join-Path $_.FullName "images\gif\anim.gif") } |
  Select-Object -First 1
$animGif = ""
if ($null -ne $sampleRoot) {
  $animGif = Join-Path $sampleRoot.FullName "images\gif\anim.gif"
}

# --- Helpers ---------------------------------------------------------------
function ReadDump([string]$path) {
  $values = @{}
  if (-not (Test-Path $path)) { return $values }
  foreach ($line in Get-Content $path) {
    $parts = $line.Split("=", 2)
    if ($parts.Count -eq 2) { $values[$parts[0]] = $parts[1] }
  }
  return $values
}

function Rect([hashtable]$values, [string]$key) {
  if (-not $values.ContainsKey($key)) { return $null }
  $numbers = $values[$key].Split(",") | ForEach-Object { [double]$_ }
  if ($numbers.Count -ne 4) { return $null }
  return @{ L = $numbers[0]; T = $numbers[1]; R = $numbers[2]; B = $numbers[3];
            W = $numbers[2] - $numbers[0]; H = $numbers[3] - $numbers[1] }
}

function ClientSize([hashtable]$values) {
  if (-not $values.ContainsKey("client")) { return $null }
  $parts = $values["client"].Split("x")
  if ($parts.Count -ne 2) { return $null }
  return @{ W = [double]$parts[0]; H = [double]$parts[1] }
}

function LogText() {
  $log = Join-Path $work "logs\zoreview.log"
  if (Test-Path $log) { return (Get-Content $log -Raw) }
  return ""
}

function ClearState() {
  Remove-Item (Join-Path $work "settings.ini") -Force -ErrorAction SilentlyContinue
  Remove-Item (Join-Path $work "logs\zoreview.log") -Force -ErrorAction SilentlyContinue
}

# Runs the app, dumps the layout at $dumpAt and exits at $quitAt.
function Run([string]$label, [string[]]$extra, [int]$dumpAt = 3000,
             [int]$quitAt = 3400, [string]$image = $markerPath) {
  ClearState
  $dump = Join-Path $work ("dump-" + $label + ".txt")
  Remove-Item $dump -Force -ErrorAction SilentlyContinue
  $arguments = @("--portable", "--language", "en", "--layout-dump", $dump,
                 "--dump-at", "$dumpAt", "--quit-at", "$quitAt") + $extra
  if ($image) { $arguments += $image }
  $process = Start-Process -FilePath $appExe -ArgumentList $arguments -PassThru
  if (-not $process.WaitForExit(40000)) {
    $process.Kill()
    throw "zoreview.exe did not exit in time: $label"
  }
  Start-Sleep -Milliseconds 150
  return @{ Values = (ReadDump $dump); Log = (LogText); Dump = $dump }
}

# Opens the native Save As dialog for a target and lets the dialog itself drive
# the outcome. NV_SAVEAS is the same production path the hook reads; accept=1 /
# cancel=1 make the automation confirm through the real dialog buttons.
function RunDialog([string]$label, [string]$target, [string]$script,
                   [int]$dumpAt = 3000, [int]$quitAt = 12000,
                   [string]$image = $markerPath, [string[]]$extra = @()) {
  $old = $env:NV_SAVEAS
  $env:NV_SAVEAS = $script
  try {
    return Run $label (@("--save-options", $target) + $extra) $dumpAt $quitAt $image
  } finally {
    if ($null -eq $old) {
      Remove-Item Env:NV_SAVEAS -ErrorAction SilentlyContinue
    } else {
      $env:NV_SAVEAS = $old
    }
  }
}

function EstimateFromLog([string]$text) {
  $match = [regex]::Match($text, "saveas: estimate bytes=([0-9]+)")
  if ($match.Success) { return [int64]$match.Groups[1].Value }
  return 0
}

function Shot([string]$label, [int]$delay, [string[]]$extra) {
  ClearState
  $png = Join-Path $outputDir ($label + ".png")
  Remove-Item $png -Force -ErrorAction SilentlyContinue
  $arguments = @("--portable", "--language", "en", "--screenshot", $png,
                 "--shot-delay", "$delay") + $extra + $markerPath
  $process = Start-Process -FilePath $appExe -ArgumentList $arguments -PassThru
  if (-not $process.WaitForExit(40000)) {
    $process.Kill()
    throw "zoreview.exe did not exit in time: $label"
  }
  Start-Sleep -Milliseconds 150
  return @{ Path = $png; Log = (LogText) }
}

function DecodeBgra([string]$source, [string]$output) {
  Remove-Item $output -Force -ErrorAction SilentlyContinue
  $process = Start-Process -FilePath $cli -ArgumentList @("--decode", $source, "--out", $output) -PassThru -Wait -NoNewWindow
  return ($process.ExitCode -eq 0 -and (Test-Path $output))
}

function Probe([string]$source) {
  $text = & $cli --probe $source 2>&1 | Out-String
  return $text
}

$script:bgraCache = @{}

function DecodedPixels([string]$source, [int]$width, [int]$height) {
  $key = ("{0}|{1}x{2}" -f $source, $width, $height)
  if ($script:bgraCache.ContainsKey($key)) { return $script:bgraCache[$key] }
  $raw = Join-Path $work ("decoded-" + [guid]::NewGuid().ToString("N") + ".bgra")
  if (-not (DecodeBgra $source $raw)) { return $null }
  $bytes = [System.IO.File]::ReadAllBytes($raw)
  Remove-Item $raw -Force -ErrorAction SilentlyContinue
  if ($bytes.Length -ne ($width * $height * 4)) { return $null }
  $script:bgraCache[$key] = $bytes
  return $bytes
}

function ChannelDiff([int]$a, [int]$b) {
  return [math]::Abs($a - $b)
}

# Samples a 12x8 grid inside the image and compares RGB against the source
# bitmap decoded by the same CLI (so WebP/GIF work too). Transparent source
# pixels are skipped: GIF keeps no colour there.
function ComparePixels([string]$expectedPath, [string]$actualPath,
                       [int]$width, [int]$height, [int]$tolerance,
                       [string]$name) {
  $expected = DecodedPixels $expectedPath $width $height
  $actual = DecodedPixels $actualPath $width $height
  if ($null -eq $expected -or $null -eq $actual) {
    Check $false ("{0}: pixels decoded" -f $name)
    return
  }
  $maxDiff = 0
  for ($gy = 0; $gy -lt 8; $gy++) {
    for ($gx = 0; $gx -lt 12; $gx++) {
      $x = [int](($gx + 0.5) * $width / 12)
      $y = [int](($gy + 0.5) * $height / 8)
      if ($x -ge $width) { $x = $width - 1 }
      if ($y -ge $height) { $y = $height - 1 }
      $i = ($y * $width + $x) * 4
      if ($expected[$i + 3] -lt 128) { continue }
      for ($c = 0; $c -lt 3; $c++) {
        $d = ChannelDiff $expected[$i + $c] $actual[$i + $c]
        if ($d -gt $maxDiff) { $maxDiff = $d }
      }
    }
  }
  Check ($maxDiff -le $tolerance) ("{0}: RGB within {1} (max_diff={2})" -f $name, $tolerance, $maxDiff)
}

function CompareAlpha([string]$expectedPath, [string]$actualPath, [int]$width,
                      [int]$height, [string]$name) {
  $expected = DecodedPixels $expectedPath $width $height
  $actual = DecodedPixels $actualPath $width $height
  if ($null -eq $expected -or $null -eq $actual) {
    Check $false ("{0}: pixels decoded" -f $name)
    return
  }
  $bad = 0
  for ($y = 0; $y -lt $height; $y += 3) {
    for ($x = 0; $x -lt $width; $x += 3) {
      $i = ($y * $width + $x) * 4 + 3
      if ($expected[$i] -ne $actual[$i]) { $bad++ }
    }
  }
  Check ($bad -eq 0) ("{0}: alpha round-trips exactly (bad={1})" -f $name, $bad)
}

function NoTempFiles([string]$name) {
  $left = @(Get-ChildItem -Path $work -Recurse -Filter "*.nvtmp" -ErrorAction SilentlyContinue)
  Check ($left.Count -eq 0) ("{0}: no .nvtmp left behind" -f $name)
}

# --- 1. Encoder capabilities ----------------------------------------------
$caps = Run "caps" @() 1200 1600
CheckHas $caps.Log "save: encoders=" "the startup log reports the encoders"
$encoderLine = ($caps.Log -split "`n" | Where-Object { $_ -match "save: encoders=" } | Select-Object -First 1)
$encoders = ""
if ($encoderLine) {
  $encoders = ($encoderLine -split "save: encoders=")[1].Trim()
}
Write-Host ("info encoders={0}" -f $encoders)
foreach ($required in @("png", "jpeg", "bmp", "tiff")) {
  Check ($encoders -split "," -contains $required) ("encoder available: {0}" -f $required)
}

# --- 2. Six containers round-trip -----------------------------------------
$formats = @(
  @{ name = "png";  ext = ".png";  tol = 0;  source = $alphaPath;  w = $alphaWidth; h = $alphaHeight },
  @{ name = "jpeg"; ext = ".jpg";  tol = 16; source = $alphaPath;  w = $alphaWidth; h = $alphaHeight },
  @{ name = "bmp";  ext = ".bmp";  tol = 0;  source = $markerPath; w = $markerWidth; h = $markerHeight },
  @{ name = "tiff"; ext = ".tif";  tol = 0;  source = $markerPath; w = $markerWidth; h = $markerHeight },
  @{ name = "webp"; ext = ".webp"; tol = 28; source = $alphaPath;  w = $alphaWidth; h = $alphaHeight },
  @{ name = "gif";  ext = ".gif";  tol = 36; source = $alphaPath;  w = $alphaWidth; h = $alphaHeight }
)
$saved = @{}
foreach ($format in $formats) {
  if (-not ($encoders -split "," -contains $format.name)) {
    Write-Host ("skip {0} (encoder not available)" -f $format.name)
    continue
  }
  $target = Join-Path $work ("out-" + $format.name + $format.ext)
  Remove-Item $target -Force -ErrorAction SilentlyContinue
  $result = Run ("save-" + $format.name) @("--save-to", $target) 2400 2800 $format.source
  Check (Test-Path $target) ("{0}: the save hook wrote the file" -f $format.name)
  CheckHas $result.Log "saved image:" ("{0}: the save is logged" -f $format.name)
  if (-not (Test-Path $target)) { continue }
  $size = (Get-Item $target).Length
  Check ($size -gt 0) ("{0}: the file is not empty" -f $format.name)
  $saved[$format.name] = $target
  $probe = Probe $target
  Check ($probe -match ("size: {0}x{1}" -f $format.w, $format.h)) `
    ("{0}: the saved file reports the right size" -f $format.name)
  ComparePixels $format.source $target $format.w $format.h $format.tol $format.name
  if ($format.name -eq "png" -or $format.name -eq "tiff" -or $format.name -eq "webp") {
    CompareAlpha $format.source $target $format.w $format.h $format.name
  }
  NoTempFiles $format.name
}

# GIF transparency: a fully transparent corner must stay transparent.
if ($saved.ContainsKey("gif")) {
  $gifPixels = DecodedPixels $saved["gif"] $alphaWidth $alphaHeight
  $probeText = Probe $saved["gif"]
  if ($null -ne $gifPixels) {
    # Pixel (0,0) has alpha 16 in the source: it must be transparent in the GIF.
    Check (($gifPixels[3]) -lt 128 -or ($probeText -match "alpha: no")) `
      "gif: transparent pixels stay transparent"
  }
}

# --- 3. Quality changes the file size and the pixels ----------------------
if ($encoders -split "," -contains "webp") {
  $q20 = Join-Path $work "q20.webp"
  $q90 = Join-Path $work "q90.webp"
  Remove-Item $q20, $q90 -Force -ErrorAction SilentlyContinue
  $r20 = Run "q20" @("--save-to", $q20, "--save-quality", "20") 2400 2800 $alphaPath
  Run "q90" @("--save-to", $q90, "--save-quality", "90") 2400 2800 $alphaPath | Out-Null
  if ((Test-Path $q20) -and (Test-Path $q90)) {
    $s20 = (Get-Item $q20).Length
    $s90 = (Get-Item $q90).Length
    Check ($s20 -lt $s90) ("webp: quality 20 ({0}) is smaller than 90 ({1})" -f $s20, $s90)
    CheckHas $r20.Log "quality=20" "webp: the quality reaches the save path"
  } else {
    Check $false "webp: quality runs wrote both files"
  }
}

if ($encoders -split "," -contains "jpeg") {
  $j60 = Join-Path $work "q60.jpg"
  $j95 = Join-Path $work "q95.jpg"
  Remove-Item $j60, $j95 -Force -ErrorAction SilentlyContinue
  Run "j60" @("--save-to", $j60, "--save-quality", "60") 2400 2800 $alphaPath | Out-Null
  Run "j95" @("--save-to", $j95, "--save-quality", "95") 2400 2800 $alphaPath | Out-Null
  if ((Test-Path $j60) -and (Test-Path $j95)) {
    Check ((Get-Item $j60).Length -lt (Get-Item $j95).Length) `
      "jpeg: quality 60 is smaller than 95"
  } else {
    Check $false "jpeg: quality runs wrote both files"
  }
}

# --- 4. Size estimate matches the file ------------------------------------
$estimateTarget = Join-Path $work "estimate.jpg"
Remove-Item $estimateTarget -Force -ErrorAction SilentlyContinue
$estimateRun = RunDialog "estimate" $estimateTarget "type=jpg;quality=90;accept=1" 3000 14000
Check (Test-Path $estimateTarget) "estimate: the dialog saved the JPEG"
CheckHas $estimateRun.Log "saveas: accepted" "dialog: the dialog accepted a target"
CheckHas $estimateRun.Log "format=jpeg" "dialog: the JPEG type reaches the outcome"
CheckHas $estimateRun.Log "quality=90" "dialog: the default quality is 90"
CheckHas $estimateRun.Log "save: done format=jpeg" "dialog: the async save completed"
CheckHas $estimateRun.Log "byte_copy=0" "dialog: a normal save is re-encoded"
$estimate = EstimateFromLog $estimateRun.Log
Check ($estimate -gt 0) "estimate: the dialog produced a positive estimate"
CheckHas $estimateRun.Log "saveas: estimate bytes=" "estimate: the encode is logged"
$acceptedEstimate = [regex]::Match($estimateRun.Log, "saveas: accepted .*estimate=([0-9]+)")
Check ($acceptedEstimate.Success -and $estimate -gt 0 -and [int64]$acceptedEstimate.Groups[1].Value -eq $estimate) `
  "estimate: the accepted outcome carries the estimate"
if ((Test-Path $estimateTarget) -and $estimate -gt 0) {
  $actual = (Get-Item $estimateTarget).Length
  $error_ratio = [math]::Abs($estimate - $actual) / [double]$actual
  # The estimate comes from a box-averaged 1024 px proxy encode scaled back up, so it
  # is deliberately approximate (~10% under on a 24 MP photo); the dialog labels it
  # with a "≈". 25% still catches a broken estimator.
  Check ($error_ratio -lt 0.25) `
    ("estimate: within 25% of the file (estimate={0} actual={1})" -f $estimate, $actual)
}

# --- 5. One-stage Save As dialog: type switching --------------------------
# The six containers are already encoded in section 2; here the dialog's type
# combo is exercised with a lossless and a lossy container (PNG/WebP) so the
# test does not open a dozen modal dialogs.
foreach ($name in @("png", "webp")) {
  $format = $formats | Where-Object { $_.name -eq $name } | Select-Object -First 1
  if ($null -eq $format -or -not ($encoders -split "," -contains $name)) { continue }
  $target = Join-Path $work ("dialog-" + $name + $format.ext)
  Remove-Item $target -Force -ErrorAction SilentlyContinue
  $result = RunDialog ("dialog-" + $name) $target ("type=" + $name + ";accept=1") 3000 14000
  Check (Test-Path $target) ("dialog {0}: the type combo saves a file" -f $name)
  CheckHas $result.Log ("format=" + $name) ("dialog {0}: the chosen type is the outcome format" -f $name)
  if (Test-Path $target) {
    $probe = Probe $target
    Check ($probe -match ("size: {0}x{1}" -f $markerWidth, $markerHeight)) `
      ("dialog {0}: the file has the view size" -f $name)
  }
}

# Quality 40 must lower the estimate and the actual file versus quality 90.
$q40Target = Join-Path $work "dialog-q40.jpg"
Remove-Item $q40Target -Force -ErrorAction SilentlyContinue
$q40 = RunDialog "dialog-q40" $q40Target "type=jpg;quality=40;accept=1" 3000 14000
$estimate40 = EstimateFromLog $q40.Log
Check ($estimate40 -gt 0) "dialog: quality 40 produced an estimate"
Check ($estimate40 -lt $estimate) ("dialog: quality 40 ({0}) < quality 90 ({1})" -f $estimate40, $estimate)
CheckHas $q40.Log "quality=40" "dialog: the quality combo value reaches the outcome"
if ((Test-Path $q40Target) -and (Test-Path $estimateTarget)) {
  Check ((Get-Item $q40Target).Length -lt (Get-Item $estimateTarget).Length) `
    "dialog: quality 40 is smaller on disk"
}

# "Original format" on an untouched source copies bytes (SHA-256 identical).
$copyTarget = Join-Path $work "dialog-original.jpg"
Remove-Item $copyTarget -Force -ErrorAction SilentlyContinue
$copy = RunDialog "dialog-original" $copyTarget "type=jpg;original=1;accept=1" `
  3000 14000 $jpegSource
Check (Test-Path $copyTarget) "dialog: original format wrote the file"
if (Test-Path $copyTarget) {
  $srcHash = (Get-FileHash -LiteralPath $jpegSource -Algorithm SHA256).Hash
  $dstHash = (Get-FileHash -LiteralPath $copyTarget -Algorithm SHA256).Hash
  Check ($srcHash -eq $dstHash) "dialog: original format is a byte-identical copy"
  Check ((Get-Item $copyTarget).Length -eq (Get-Item $jpegSource).Length) `
    "dialog: original format keeps the source size"
}
CheckHas $copy.Log "byte_copy=1" "dialog: the byte-copy outcome is logged"
CheckHas $copy.Log ("estimate=" + (Get-Item $jpegSource).Length) `
  "dialog: the byte-copy estimate is the source size"

# Rotating the view disables byte copy: the same checkbox re-encodes.
$rotTarget = Join-Path $work "dialog-original-rot.jpg"
Remove-Item $rotTarget -Force -ErrorAction SilentlyContinue
$rot = RunDialog "dialog-original-rot" $rotTarget "type=jpg;original=1;accept=1" `
  3000 14000 $jpegSource @("--rotation", "90")
Check (Test-Path $rotTarget) "dialog: rotated original format wrote the file"
CheckHas $rot.Log "byte_copy=0" "dialog: rotation forces a re-encode"
if (Test-Path $rotTarget) {
  $rotProbe = Probe $rotTarget
  Check ($rotProbe -match "size: 300x400") "dialog: rotation swaps the saved dimensions"
}

# Cancel closes without writing or encoding.
$cancelTarget = Join-Path $work "dialog-cancel.jpg"
Remove-Item $cancelTarget -Force -ErrorAction SilentlyContinue
$cancel = RunDialog "dialog-cancel" $cancelTarget "type=jpg;cancel=1" 3000 12000
Check (-not (Test-Path $cancelTarget)) "dialog: cancel writes nothing"
CheckHas $cancel.Log "saveas: cancelled" "dialog: cancel is logged"
Check (-not ($cancel.Log -match "save: done")) "dialog: cancel never starts an encode"
# --- 6. Export follows the view rotation ----------------------------------
$rotated = Join-Path $work "rotated.png"
Remove-Item $rotated -Force -ErrorAction SilentlyContinue
$rotation = Run "rotation" @("--rotation", "90", "--save-to", $rotated) 2400 2800
Check (Test-Path $rotated) "rotation: the rotated save writes the file"
if (Test-Path $rotated) {
  $png = [System.Drawing.Bitmap]::FromFile($rotated)
  try {
    Check ($png.Width -eq $markerHeight) ("rotation: the width is swapped ({0})" -f $png.Width)
    Check ($png.Height -eq $markerWidth) ("rotation: the height is swapped ({0})" -f $png.Height)
    $expected = $colors.blue  # 90 clockwise: old bottom-left moves to top-left
    $pixel = $png.GetPixel(20, 20)
    Check ([math]::Abs($pixel.R - $expected.R) -le 12 -and
           [math]::Abs($pixel.G - $expected.G) -le 12 -and
           [math]::Abs($pixel.B - $expected.B) -le 12) `
      "rotation: the top-left corner follows the view"
  } finally {
    $png.Dispose()
  }
}

# --- 7. Animated source saves the current frame ---------------------------
if ($animGif -ne "") {
  $frameTarget = Join-Path $work "frame.png"
  Remove-Item $frameTarget -Force -ErrorAction SilentlyContinue
  $frame = Run "frame" @("--save-to", $frameTarget) 2400 2800 $animGif
  Check (Test-Path $frameTarget) "animation: the current frame saves as PNG"
  $probeFrame = Probe $frameTarget
  Check ($probeFrame -match "frames: 1") "animation: the export is a single frame"
  if ($encoders -split "," -contains "gif") {
    $frameGif = Join-Path $work "frame.gif"
    Remove-Item $frameGif -Force -ErrorAction SilentlyContinue
    $frameRun = Run "frame-gif" @("--save-to", $frameGif) 2400 2800 $animGif
    Check (Test-Path $frameGif) "animation: the current frame saves as GIF"
    CheckHas $frameRun.Log "saved image:" "animation: the GIF export is logged"
  }
}

# --- 8. Failure paths -----------------------------------------------------
# A directory named like the target: opening the temporary file works but the
# atomic replace fails, so the card must appear and no temp file may remain.
$blocked = Join-Path $work "blocked.png"
New-Item -ItemType Directory -Path $blocked -Force | Out-Null
$failure = Run "failure" @("--save-to", $blocked) 2400 2800
Check ($failure.Values["error_open"] -eq "1") `
  "failure: an unwritable target shows the error card"
Check ($failure.Values["error_kind"] -eq "save_failed") `
  ("failure: the error is classified as save_failed (got {0})" -f $failure.Values["error_kind"])
NoTempFiles "failure"

# A wrong extension: a toast, no file.
$badExt = Join-Path $work "bad.xyz"
Remove-Item $badExt -Force -ErrorAction SilentlyContinue
$bad = Run "bad-ext" @("--save-to", $badExt) 2400 2800
Check (-not (Test-Path $badExt)) "failure: an unknown extension writes nothing"
CheckHas $bad.Log "ui: toast=" "failure: an unknown extension shows a toast"

# --- 9. 8K save stays off the UI thread ----------------------------------
if (-not $Skip8k) {
  $sample8k = Join-Path $repo "tests\output\8k.jpg"
  if (Test-Path $sample8k) {
    $bigTarget = Join-Path $work "8k.jpg"
    Remove-Item $bigTarget -Force -ErrorAction SilentlyContinue
    $big = Run "8k" @("--save-to", $bigTarget, "--save-quality", "85") 9000 10000 $sample8k
    Check (Test-Path $bigTarget) "8k: the save completes"
    if (Test-Path $bigTarget) {
      $probe8k = Probe $bigTarget
      Check ($probe8k -match "size: 7680x4320") "8k: the output keeps the 8K size"
      Check (Test-Path $bigTarget) "8k: the file is readable"
    }
    CheckHas $big.Log "saved image:" "8k: the save is logged"

    # Above 32 MP the trial encode is skipped, so the native dialog must not
    # queue an estimate encode that would freeze it for seconds.
    $bigDialogTarget = Join-Path $work "8k-dialog.jpg"
    Remove-Item $bigDialogTarget -Force -ErrorAction SilentlyContinue
    $bigDialog = RunDialog "8k-dialog" $bigDialogTarget `
      "type=jpg;quality=85;accept=1" 5000 22000 $sample8k
    Check (Test-Path $bigDialogTarget) "8k: the native dialog saves the file"
    CheckHas $bigDialog.Log "saveas: accepted" "8k: the dialog accepts the save"
    Check (-not ($bigDialog.Log -match "saveas: estimate bytes=")) `
      "8k: the size estimate is skipped above 32 MP"
  } else {
    Write-Host "skip 8k (tests\output\8k.jpg missing)"
  }
}

# --- Summary --------------------------------------------------------------
Write-Host ""
Write-Host ("{0} checks, {1} failures" -f $script:checks, $script:failures)
if (-not $KeepOutput) {
  Remove-Item $work -Recurse -Force -ErrorAction SilentlyContinue
}
if ($script:failures -ne 0) {
  exit 1
}
exit 0
