# v2.0 S1 (plan 22/05): view rotation.
#
# Display-only quarter turns: Ctrl+L / Ctrl+R, the bottom-bar rotate pair,
# keep_rotation across navigation, and exports (copy/save) that follow the view.
# The more panel used to carry a "Reset rotation" row; v3.0 removed it, so the
# script now returns to upright with a fourth turn. Screenshots are sampled with
# a four-colour marker image, so a wrong transform fails on pixel values rather
# than on a log line alone.
#
# The script runs a throw-away copy of the executable in portable mode and forces
# 125% DPI, so the layout numbers are reproducible on any host. All strings are
# ASCII; the file needs no UTF-8 BOM (HANDOFF pitfalls 60/124).

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

Add-Type -AssemblyName System.Drawing

$work = Join-Path $env:TEMP ("nv-rotate-" + [guid]::NewGuid().ToString("N"))
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

# --- Marker samples --------------------------------------------------------
# Four solid quadrants: TL red, TR green, BL blue, BR yellow. Rotating the view
# clockwise must move the colours, not just the bounding box.
$markerWidth = 400
$markerHeight = 300
$colors = @{
  red    = [System.Drawing.Color]::FromArgb(220, 30, 30)
  green  = [System.Drawing.Color]::FromArgb(30, 200, 60)
  blue   = [System.Drawing.Color]::FromArgb(40, 60, 220)
  yellow = [System.Drawing.Color]::FromArgb(230, 200, 40)
}
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
$marker.Dispose()

# A second image so navigation can prove keep_rotation.
$second = New-Object System.Drawing.Bitmap(320, 240)
$secondGraphics = [System.Drawing.Graphics]::FromImage($second)
$secondGraphics.Clear([System.Drawing.Color]::FromArgb(30, 80, 140))
$secondGraphics.Dispose()
$second.Save((Join-Path $gallery "b_second.bmp"), [System.Drawing.Imaging.ImageFormat]::Bmp)
$second.Dispose()

$outputDir = Join-Path $PSScriptRoot "output\rotation"
New-Item -ItemType Directory -Force -Path $outputDir | Out-Null

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

function LogText() {
  $log = Join-Path $work "logs\zoreview.log"
  if (Test-Path $log) { return (Get-Content $log -Raw) }
  return ""
}

function ClearState([bool]$keepSettings) {
  if (-not $keepSettings) {
    Remove-Item (Join-Path $work "settings.ini") -Force -ErrorAction SilentlyContinue
  }
  Remove-Item (Join-Path $work "logs\zoreview.log") -Force -ErrorAction SilentlyContinue
}

# Runs one process with a layout dump. Each run starts from default settings
# unless -KeepSettings is passed (keep_rotation check). `image` defaults to the
# gallery; pass $null for the no-image case.
function Dump([string]$label, [string[]]$extra, [bool]$keepSettings = $false,
              [string]$image = $markerPath) {
  ClearState $keepSettings
  $dump = Join-Path $work ("dump-" + $label + ".txt")
  Remove-Item $dump -Force -ErrorAction SilentlyContinue
  $arguments = @("--portable", "--language", "en", "--force-dpi-scale", "125",
                 "--layout-dump", $dump) + $extra
  if ($image) { $arguments += $image }
  $process = Start-Process -FilePath $appExe -ArgumentList $arguments -PassThru
  if (-not $process.WaitForExit(30000)) {
    $process.Kill()
    throw "zoreview.exe did not exit in time: $label"
  }
  Start-Sleep -Milliseconds 120
  return @{ Values = (ReadDump $dump); Log = (LogText); Dump = $dump }
}

# Runs one process that writes a screenshot of the final view.
function Shot([string]$label, [int]$delay, [string[]]$extra) {
  ClearState $false
  $png = Join-Path $outputDir ($label + ".png")
  Remove-Item $png -Force -ErrorAction SilentlyContinue
  $arguments = @("--portable", "--language", "en", "--force-dpi-scale", "125",
                 "--screenshot", $png, "--shot-delay", "$delay") + $extra + $markerPath
  $process = Start-Process -FilePath $appExe -ArgumentList $arguments -PassThru
  if (-not $process.WaitForExit(30000)) {
    $process.Kill()
    throw "zoreview.exe did not exit in time: $label"
  }
  Start-Sleep -Milliseconds 120
  return @{ Path = $png; Log = (LogText) }
}

function ColorsOf([System.Drawing.Bitmap]$bitmap, [double]$x, [double]$y) {
  if ($x -lt 0) { $x = 0 }
  if ($y -lt 0) { $y = 0 }
  if ($x -ge $bitmap.Width) { $x = $bitmap.Width - 1 }
  if ($y -ge $bitmap.Height) { $y = $bitmap.Height - 1 }
  return $bitmap.GetPixel([int]$x, [int]$y)
}

function SameColor($actual, $expected, [int]$tolerance = 12) {
  if ($null -eq $actual) { return $false }
  return ([math]::Abs($actual.R - $expected.R) -le $tolerance) -and
         ([math]::Abs($actual.G - $expected.G) -le $tolerance) -and
         ([math]::Abs($actual.B - $expected.B) -le $tolerance)
}

# Expected displayed image rectangle for a fitted view: the same
# min(content/effective size) formula as Renderer::ComputeFitScale.
function DisplayRect([hashtable]$values, [int]$steps) {
  $content = Rect $values "content"
  $swap = ($steps % 2) -ne 0
  $dw = if ($swap) { $markerHeight } else { $markerWidth }
  $dh = if ($swap) { $markerWidth } else { $markerHeight }
  $scale = [math]::Min($content.W / $dw, $content.H / $dh)
  $w = $dw * $scale
  $h = $dh * $scale
  $cx = ($content.L + $content.R) / 2
  $cy = ($content.T + $content.B) / 2
  return @{ L = $cx - $w / 2; T = $cy - $h / 2; R = $cx + $w / 2; B = $cy + $h / 2;
            W = $w; H = $h }
}

# Corner colours a clockwise rotation produces. The source quadrants are
# TL=red TR=green BL=blue BR=yellow, so a 90-degree clockwise turn puts the old
# bottom-left in the new top-left.
$expectedCorners = @(
  @{ TL = "red";    TR = "green";  BL = "blue";   BR = "yellow" },
  @{ TL = "blue";   TR = "red";    BL = "yellow"; BR = "green"  },
  @{ TL = "yellow"; TR = "blue";   BL = "green";  BR = "red"    },
  @{ TL = "green";  TR = "yellow"; BL = "red";    BR = "blue"   }
)

# Screenshots use the --rotation startup hook: the transform is identical no
# matter how the state was entered, and the key/click paths are asserted
# separately from the dump.
function CheckCorners([string]$label, [int]$steps) {
  $shot = Shot $label 2600 @("--rotation", "$($steps * 90)")
  Check (Test-Path $shot.Path) ("{0}: screenshot exists" -f $label)
  if (-not (Test-Path $shot.Path)) { return }
  $geometry = Dump ("geom-" + $label) @("--rotation", "$($steps * 90)")
  $bitmap = [System.Drawing.Bitmap]::FromFile($shot.Path)
  try {
    $rect = DisplayRect $geometry.Values $steps
    $content = Rect $geometry.Values "content"
    $expected = $expectedCorners[$steps]
    # Sample inside each quadrant (22% of the displayed image, away from the
    # edges, the arrows and the title/bottom bars).
    $xLeft = $rect.L + $rect.W * 0.22
    $xRight = $rect.R - $rect.W * 0.22
    $yTop = $rect.T + $rect.H * 0.22
    $yBottom = $rect.B - $rect.H * 0.22
    $tl = ColorsOf $bitmap $xLeft $yTop
    $tr = ColorsOf $bitmap $xRight $yTop
    $bl = ColorsOf $bitmap $xLeft $yBottom
    $br = ColorsOf $bitmap $xRight $yBottom
    Check (SameColor $tl $colors[$expected.TL]) ("{0}: top-left is {1}" -f $label, $expected.TL)
    Check (SameColor $tr $colors[$expected.TR]) ("{0}: top-right is {1}" -f $label, $expected.TR)
    Check (SameColor $bl $colors[$expected.BL]) ("{0}: bottom-left is {1}" -f $label, $expected.BL)
    Check (SameColor $br $colors[$expected.BR]) ("{0}: bottom-right is {1}" -f $label, $expected.BR)
    Check ($rect.L -ge $content.L - 0.5 -and $rect.R -le $content.R + 0.5 -and
           $rect.T -ge $content.T - 0.5 -and $rect.B -le $content.B + 0.5) `
      ("{0}: the rotated image stays inside the content" -f $label)
  } finally {
    $bitmap.Dispose()
  }
}

# Byte comparison of two screenshots. The hash shortcut keeps the common
# identical case fast; a mismatch falls back to a max-difference scan.
function Compare-Shots([string]$left, [string]$right) {
  $a = [System.Drawing.Bitmap]::FromFile($left)
  $b = [System.Drawing.Bitmap]::FromFile($right)
  try {
    if ($a.Width -ne $b.Width -or $a.Height -ne $b.Height) {
      return @{ Max = 255; Different = -1 }
    }
    $rect = New-Object System.Drawing.Rectangle(0, 0, $a.Width, $a.Height)
    $format = [System.Drawing.Imaging.PixelFormat]::Format32bppArgb
    $dataA = $a.LockBits($rect, [System.Drawing.Imaging.ImageLockMode]::ReadOnly, $format)
    $dataB = $b.LockBits($rect, [System.Drawing.Imaging.ImageLockMode]::ReadOnly, $format)
    try {
      $bytesA = New-Object byte[] ($dataA.Stride * $a.Height)
      $bytesB = New-Object byte[] ($dataB.Stride * $b.Height)
      [System.Runtime.InteropServices.Marshal]::Copy($dataA.Scan0, $bytesA, 0, $bytesA.Length)
      [System.Runtime.InteropServices.Marshal]::Copy($dataB.Scan0, $bytesB, 0, $bytesB.Length)
      $sha = [System.Security.Cryptography.SHA256]::Create()
      try {
        $hashA = $sha.ComputeHash($bytesA)
        $hashB = $sha.ComputeHash($bytesB)
      } finally {
        $sha.Dispose()
      }
      if ([System.Convert]::ToBase64String($hashA) -eq
          [System.Convert]::ToBase64String($hashB)) {
        return @{ Max = 0; Different = 0 }
      }
      $max = 0
      for ($i = 0; $i -lt $bytesA.Length; $i += 4) {
        $delta = [math]::Abs($bytesA[$i] - $bytesB[$i])
        $g = [math]::Abs($bytesA[$i + 1] - $bytesB[$i + 1])
        $b2 = [math]::Abs($bytesA[$i + 2] - $bytesB[$i + 2])
        if ($g -gt $delta) { $delta = $g }
        if ($b2 -gt $delta) { $delta = $b2 }
        if ($delta -gt $max) { $max = $delta }
      }
      return @{ Max = $max; Different = -1 }
    } finally {
      $a.UnlockBits($dataA)
      $b.UnlockBits($dataB)
    }
  } finally {
    $a.Dispose()
    $b.Dispose()
  }
}

try {
  # --- 1. Upright baseline -------------------------------------------------
  $base = Dump "base" @()
  $v = $base.Values
  Check ($v["has_image"] -eq "1") "the marker image opens"
  Check ($v["rotation"] -eq "0") "startup is upright"
  Check ($v["rotation_degrees"] -eq "0") "the reported angle is 0"
  Check ($v["display_width"] -eq "$markerWidth") "upright display width is the source width"
  Check ($v["display_height"] -eq "$markerHeight") "upright display height is the source height"
  Check ($v["fit"] -eq "1") "startup uses fit mode"
  Check ($v["zoom_min"] -eq "0.1") "the zoom floor still starts at 10%"
  CheckCorners "angle-0" 0

  # --- 2. Keyboard rotation ------------------------------------------------
  $case90 = Dump "rotate-right" @("--send-keys", "Ctrl+R")
  Check ($case90.Values["rotation"] -eq "1") "Ctrl+R rotates 90 degrees clockwise"
  Check ($case90.Values["rotation_degrees"] -eq "90") "the dump reports 90 degrees"
  Check ($case90.Values["display_width"] -eq "$markerHeight") "90 degrees swaps the display width"
  Check ($case90.Values["display_height"] -eq "$markerWidth") "90 degrees swaps the display height"
  CheckHas $case90.Log "cmd: view=rotate right rotation=1 angle=90 display=300x400" `
    "Ctrl+R logs the rotation and the effective size"
  CheckCorners "angle-90" 1

  $caseLeft = Dump "rotate-left" @("--send-keys", "Ctrl+L")
  Check ($caseLeft.Values["rotation"] -eq "3") "Ctrl+L rotates 90 degrees counter-clockwise"
  Check ($caseLeft.Values["rotation_degrees"] -eq "270") "the dump reports 270 degrees"
  CheckHas $caseLeft.Log "cmd: view=rotate left rotation=3 angle=270" "Ctrl+L logs the rotation"

  $case180 = Dump "rotate-180" @("--send-keys", "Ctrl+R,Ctrl+R")
  Check ($case180.Values["rotation"] -eq "2") "two turns give 180 degrees"
  Check ($case180.Values["display_width"] -eq "$markerWidth") "180 degrees keeps the width"
  Check ($case180.Values["display_height"] -eq "$markerHeight") "180 degrees keeps the height"
  CheckCorners "angle-180" 2

  $case270 = Dump "rotate-270" @("--send-keys", "Ctrl+R,Ctrl+R,Ctrl+R")
  Check ($case270.Values["rotation"] -eq "3") "three turns give 270 degrees"
  CheckCorners "angle-270" 3

  # --- 3. Four quarter turns return to the original pixels -----------------
  $baseShot = Shot "angle-0-shot" 2600 @()
  $roundTrip = Shot "angle-360" 3600 @("--send-keys", "Ctrl+R,Ctrl+R,Ctrl+R,Ctrl+R")
  $diff = Compare-Shots $baseShot.Path $roundTrip.Path
  Check ($diff.Different -eq 0) ("four Ctrl+R reproduce the original frame exactly (different={0}, max={1})" -f $diff.Different, $diff.Max)

  # --- 4. Bottom-bar buttons ------------------------------------------------
  $clicked = Dump "click-right" @("--click-hit", "bottom.rotate_right")
  Check ($clicked.Values["rotation"] -eq "1") "the bottom-bar rotate-right button works"
  CheckHas $clicked.Log "cmd: id=bottom.rotate_right" "the button reports its hit id"
  CheckHas $clicked.Log "cmd: view=rotate right" "the button dispatches the same command"

  $clicked = Dump "click-left" @("--click-hit", "bottom.rotate_left", "--click-hit", "bottom.rotate_left")
  Check ($clicked.Values["rotation"] -eq "2") "two rotate-left clicks give 180 degrees"
  CheckHas $clicked.Log "cmd: view=rotate left" "the left button dispatches rotate=left"

  # --- 5. Back to upright without a dedicated reset row --------------------
  # v3.0 (plan 26 section 2.5) removed the "Reset rotation" row from the more
  # panel and there is no Ctrl+0 binding; the view rotation is a mod-4 turn, so
  # one more right turn from 270 degrees is the documented way back to upright.
  $reset = Dump "menu-reset" @("--rotation", "270", "--click-hit",
                               "bottom.rotate_right")
  Check ($reset.Values["rotation"] -eq "0") "a right turn from 270 degrees returns to upright"
  CheckHas $reset.Log "cmd: view=rotate right" "the turn dispatches the normal rotate command"

  # --- 6. keep_rotation across navigation ----------------------------------
  $nav = Dump "nav-keep" @("--send-keys", "Ctrl+R,Right")
  Check ($nav.Values["rotation"] -eq "1") "the default keeps the rotation on the next image"
  Check ($nav.Values["folder_index"] -eq "1") "navigation still moves to the next file"
  Check ($nav.Values["display_width"] -eq "240") "the kept rotation applies to the next image"
  Check ($nav.Values["display_height"] -eq "320") "the next image keeps the swapped height"

  Set-Content -Path (Join-Path $work "settings.ini") -Encoding ASCII -Value @(
    "[ui]",
    "keep_rotation=0"
  )
  $navReset = Dump "nav-reset" @("--send-keys", "Ctrl+R,Right") $true
  Check ($navReset.Values["rotation"] -eq "0") "keep_rotation=0 resets the view on the next image"
  Check ($navReset.Values["folder_index"] -eq "1") "the reset run still navigates"
  CheckHas $navReset.Log "cmd: view=rotate reset keep_rotation=0" `
    "the reset is logged when keep_rotation is off"

  # --- 7. Copy image follows the view --------------------------------------
  ClearState $false
  $copyShot = Join-Path $outputDir "copy-exit.png"
  $copyArguments = @("--portable", "--language", "en", "--force-dpi-scale", "125",
                     "--screenshot", $copyShot, "--shot-delay", "3200",
                     "--send-keys", "Ctrl+R,Ctrl+C") + $markerPath
  $process = Start-Process -FilePath $appExe -ArgumentList $copyArguments -PassThru
  if (-not $process.WaitForExit(30000)) {
    $process.Kill()
    throw "copy run did not exit"
  }
  Start-Sleep -Milliseconds 250
  $clip = Get-Clipboard -Format Image
  Check ($null -ne $clip) "Ctrl+C puts an image on the clipboard"
  if ($null -ne $clip) {
    Check ($clip.Width -eq $markerHeight) ("copied bitmap is 90-degree rotated (width={0})" -f $clip.Width)
    Check ($clip.Height -eq $markerWidth) ("copied bitmap is 90-degree rotated (height={0})" -f $clip.Height)
    $expected = $expectedCorners[1]
    Check (SameColor $clip.GetPixel(20, 20) $colors[$expected.TL]) "copied top-left follows the view"
    Check (SameColor $clip.GetPixel($clip.Width - 21, 20) $colors[$expected.TR]) "copied top-right follows the view"
    $clip.Dispose()
  }

  # --- 8. Save as follows the view -----------------------------------------
  $savedPath = Join-Path $work "rotated.png"
  Remove-Item $savedPath -Force -ErrorAction SilentlyContinue
  ClearState $false
  $saveShot = Join-Path $outputDir "save-exit.png"
  $saveArguments = @("--portable", "--language", "en", "--force-dpi-scale", "125",
                     "--screenshot", $saveShot, "--shot-delay", "3800",
                     "--send-keys", "Ctrl+R", "--save-as", $savedPath) + $markerPath
  $process = Start-Process -FilePath $appExe -ArgumentList $saveArguments -PassThru
  if (-not $process.WaitForExit(30000)) {
    $process.Kill()
    throw "save run did not exit"
  }
  Start-Sleep -Milliseconds 250
  $saveLog = LogText
  Check (Test-Path $savedPath) "the save hook wrote the file"
  if (Test-Path $savedPath) {
    $png = [System.Drawing.Bitmap]::FromFile($savedPath)
    try {
      Check ($png.Width -eq $markerHeight) ("saved PNG swapped the width ({0})" -f $png.Width)
      Check ($png.Height -eq $markerWidth) ("saved PNG swapped the height ({0})" -f $png.Height)
      $expected = $expectedCorners[1]
      Check (SameColor $png.GetPixel(20, 20) $colors[$expected.TL]) "saved top-left follows the view"
      Check (SameColor $png.GetPixel($png.Width - 21, 20) $colors[$expected.TR]) "saved top-right follows the view"
    } finally {
      $png.Dispose()
    }
    CheckHas $saveLog "saved image:" "the save logs the rotation"
  }

  # --- 9. Info panel shows the view angle ---------------------------------
  $info = Dump "info" @("--send-keys", "Ctrl+R,I")
  Check ($info.Values["info_open"] -eq "1") "the info panel opens while rotated"
  Check ($info.Values["info_dimension"] -match "view 90") `
    ("the info panel appends the view angle (got '{0}')" -f $info.Values["info_dimension"])

  # --- 10. Fullscreen and the no-image state -------------------------------
  $fs = Dump "fullscreen" @("--send-keys", "F11,Ctrl+R")
  Check ($fs.Values["fullscreen"] -eq "1") "fullscreen stays on while rotating"
  Check ($fs.Values["rotation"] -eq "1") "fullscreen does not reset the rotation"
  Check ($fs.Values["display_width"] -eq "$markerHeight") "the fullscreen fit uses the swapped size"

  $none = Dump "no-image" @("--send-keys", "Ctrl+R") $false $null
  Check ($none.Values["rotation"] -eq "0") "the empty state stays upright"
  CheckHas $none.Log "cmd: blocked=rotate.right" "Ctrl+R is blocked without a picture"

  # --- 11. 8K: pure transform, no second frame -----------------------------
  if (-not $Skip8k) {
    $sample8k = Join-Path $repo "tests\output\8k.jpg"
    if (Test-Path $sample8k) {
      $mem = @{}
      foreach ($steps in @(0, 1)) {
        ClearState $false
        $png = Join-Path $outputDir ("8k-" + $steps + ".png")
        $arguments = @("--portable", "--language", "en", "--rotation", "$($steps * 90)",
                       "--screenshot", $png, "--shot-delay", "3000", $sample8k)
        $process = Start-Process -FilePath $appExe -ArgumentList $arguments -PassThru
        if (-not $process.WaitForExit(30000)) {
          $process.Kill()
          throw "8K run did not exit"
        }
        Start-Sleep -Milliseconds 120
        $log = LogText
        if ($log -match "memory: first_image ws_mb=([\d.]+) private_mb=([\d.]+)") {
          $mem[$steps] = [double]$matches[2]
        }
      }
      if ($mem.ContainsKey(0) -and $mem.ContainsKey(1)) {
        Check ([math]::Abs($mem[0] - $mem[1]) -le 15.0) `
          ("8K rotation adds no full frame (upright={0:0.0}MB rotated={1:0.0}MB)" -f $mem[0], $mem[1])
      } else {
        Check $false "8K runs report their peak memory"
      }
    } else {
      Write-Host "skip 8K rotation check (tests\output\8k.jpg not generated)"
    }
  }

  # --- Summary -------------------------------------------------------------
  Write-Host ""
  Write-Host ("{0} checks, {1} failure(s)" -f $script:checks, $script:failures)
  Write-Host "Screenshots: $outputDir"
  if ($script:failures -ne 0) {
    Write-Host "Artifacts kept at $work" -ForegroundColor Yellow
    $KeepOutput = $true
    exit 1
  }
  Write-Host "OK: rotation checks passed"
} finally {
  if (-not $KeepOutput) {
    Remove-Item $work -Recurse -Force -ErrorAction SilentlyContinue
  }
}
