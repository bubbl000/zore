# v2.0 S2 (plan 22/10): folder watching and folder wrap.
#
# The watch cases launch the real application, wait until the first image is
# decoded (the watcher baseline), mutate the folder from PowerShell, and then
# read a --dump-at layout dump. The app runs in a throw-away portable directory,
# so the developer's settings/logs are never touched.
#
# ASCII only, like the other UI scripts (HANDOFF pitfall 60/124).

param(
  [string]$App = ".\build\Release\zoreview.exe"
)

$ErrorActionPreference = "Stop"
$repo = Split-Path -Parent $PSScriptRoot
Set-Location $repo

if (-not (Test-Path $App)) {
  throw "Application not found: $App (build first: tools\build.ps1)"
}
$App = (Resolve-Path $App).Path

$work = Join-Path $env:TEMP ("nv-watch-" + [guid]::NewGuid().ToString("N"))
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

function Inside([hashtable]$inner, [hashtable]$outer) {
  if ($null -eq $inner -or $null -eq $outer) { return $false }
  return ($inner.L -ge $outer.L - 0.5) -and ($inner.T -ge $outer.T - 0.5) -and
         ($inner.R -le $outer.R + 0.5) -and ($inner.B -le $outer.B + 0.5)
}

Add-Type -AssemblyName System.Drawing

function New-Bmp([string]$path, [int]$r, [int]$g, [int]$b) {
  $bitmap = New-Object System.Drawing.Bitmap(320, 200)
  $graphics = [System.Drawing.Graphics]::FromImage($bitmap)
  $graphics.Clear([System.Drawing.Color]::FromArgb($r, $g, $b))
  $graphics.Dispose()
  $bitmap.Save($path, [System.Drawing.Imaging.ImageFormat]::Bmp)
  $bitmap.Dispose()
}

function New-ImageFolder([string]$path, [string[]]$names, [int]$r = 40,
                         [int]$g = 110, [int]$b = 170) {
  New-Item -ItemType Directory -Path $path -Force | Out-Null
  $index = 0
  foreach ($name in $names) {
    New-Bmp (Join-Path $path $name) ($r + 30 * $index) $g $b
    $index++
  }
}

function Remove-Settings {
  Remove-Item (Join-Path $work "settings.ini") -Force -ErrorAction SilentlyContinue
}

function Write-Wrap([string]$wrap) {
  Remove-Settings
  if ($wrap) {
    $text = "[general]`nversion=1`n`n[view]`nfolder_wrap=$wrap`n"
    Set-Content -Path (Join-Path $work "settings.ini") -Value $text -Encoding ascii
  }
}

function Wait-Log([string]$log, [string]$pattern, [int]$timeoutMs = 12000) {
  $deadline = [DateTime]::UtcNow.AddMilliseconds($timeoutMs)
  while ([DateTime]::UtcNow -lt $deadline) {
    if (Test-Path $log) {
      $text = Get-Content $log -Raw
      if ($text -match $pattern) { return $true }
    }
    Start-Sleep -Milliseconds 100
  }
  return $false
}

# One watch scenario: launch, wait for the first decoded image, run the mutation,
# then read the dump written by --dump-at without exiting first.
function RunWatch([string]$label, [string]$image, [scriptblock]$mutate,
                  [string[]]$extra = @(), [string]$readyPattern = "decoded:",
                  [int]$dumpAt = 5200, [int]$quitAt = 5800) {
  Remove-Settings
  $dump = Join-Path $work "$label.txt"
  Remove-Item $dump -Force -ErrorAction SilentlyContinue
  $log = Join-Path $work "logs\zoreview.log"
  Remove-Item $log -Force -ErrorAction SilentlyContinue

  $arguments = @("--portable", "--language", "en", "--layout-dump", $dump,
                 "--dump-at", "$dumpAt", "--quit-at", "$quitAt")
  $arguments += $extra
  $arguments += $image
  $process = Start-Process -FilePath $appExe -ArgumentList $arguments -PassThru
  if (-not (Wait-Log $log $readyPattern)) {
    $process.Kill()
    throw "application did not reach '$readyPattern' for $label"
  }
  & $mutate
  if (-not $process.WaitForExit(20000)) {
    $process.Kill()
    throw "zoreview.exe did not exit in time: $label"
  }
  Start-Sleep -Milliseconds 120
  if (-not (Test-Path $dump)) { throw "no layout dump for $label" }
  $text = ""
  if (Test-Path $log) { $text = Get-Content $log -Raw }
  return @{ Values = (ReadDump $dump); Log = $text }
}

function RunWrap([string]$label, [string]$wrap, [string]$keys, [string]$image) {
  Write-Wrap $wrap
  $dump = Join-Path $work "$label.txt"
  Remove-Item $dump -Force -ErrorAction SilentlyContinue
  $log = Join-Path $work "logs\zoreview.log"
  Remove-Item $log -Force -ErrorAction SilentlyContinue
  $arguments = @("--portable", "--language", "en", "--layout-dump", $dump,
                 "--send-keys", $keys, $image)
  $process = Start-Process -FilePath $appExe -ArgumentList $arguments -PassThru
  if (-not $process.WaitForExit(25000)) {
    $process.Kill()
    throw "zoreview.exe did not exit in time: $label"
  }
  Start-Sleep -Milliseconds 120
  if (-not (Test-Path $dump)) { throw "no layout dump for $label" }
  $text = ""
  if (Test-Path $log) { $text = Get-Content $log -Raw }
  return @{ Values = (ReadDump $dump); Log = $text }
}

# V3-05 (plan 25 section 8): local-disk notifications. The app runs with the
# default source (ReadDirectoryChangesW), the mutation runs right after the
# first decode, and the latency is measured from the mutation to the watch
# line in the log. A 2 s poll cannot beat 500 ms; the notify merge window is the
# only intended delay.
function LogText([string]$path) {
  if (Test-Path $path) { return (Get-Content $path -Raw) }
  return ""
}

function Count-Matches([string]$text, [string]$pattern) {
  if (-not $text) { return 0 }
  return ([regex]::Matches($text, $pattern)).Count
}

function RunNotifyLatency([string]$label, [string]$image, [scriptblock]$mutate,
                          [string[]]$extra = @(), [int]$dumpAt = 5200,
                          [int]$quitAt = 5800) {
  Remove-Settings
  $dump = Join-Path $work "$label.txt"
  Remove-Item $dump -Force -ErrorAction SilentlyContinue
  $log = Join-Path $work "logs\zoreview.log"
  Remove-Item $log -Force -ErrorAction SilentlyContinue
  $arguments = @("--portable", "--language", "en", "--layout-dump", $dump,
                 "--dump-at", "$dumpAt", "--quit-at", "$quitAt")
  $arguments += $extra
  $arguments += $image
  $process = Start-Process -FilePath $appExe -ArgumentList $arguments -PassThru
  if (-not (Wait-Log $log "decoded:")) {
    $process.Kill()
    throw "application did not decode for $label"
  }
  # The decode line is logged before the watcher baseline; give the baseline and
  # the notify thread a moment so the mutation is a genuine external change.
  Start-Sleep -Milliseconds 150
  $seen = Count-Matches (LogText $log) "watch: rescan|watch: current=removed"
  $stopwatch = [System.Diagnostics.Stopwatch]::StartNew()
  & $mutate
  $latency = -1
  while ($stopwatch.ElapsedMilliseconds -lt 3000) {
    if ((Count-Matches (LogText $log) "watch: rescan|watch: current=removed") -gt $seen) {
      $latency = $stopwatch.ElapsedMilliseconds
      break
    }
    Start-Sleep -Milliseconds 10
  }
  if (-not $process.WaitForExit(20000)) {
    $process.Kill()
    throw "zoreview.exe did not exit in time: $label"
  }
  Start-Sleep -Milliseconds 120
  if (-not (Test-Path $dump)) { throw "no layout dump for $label" }
  return @{ Values = (ReadDump $dump); Log = (LogText $log); Latency = $latency }
}

try {
  # --- 1. External add: list grows, current image stays ---------------------
  $addFolder = Join-Path $work "add"
  New-ImageFolder $addFolder @("a.bmp", "b.bmp")
  $add = RunWatch "add" (Join-Path $addFolder "a.bmp") {
    New-Bmp (Join-Path $addFolder "c.bmp") 40 40 200
  }
  $v = $add.Values
  Check ($v["watch_interval"] -eq "2000") "the fallback poll interval is still 2 s"
  Check ($v["folder_files"] -eq "3") "an external add is reflected in the list"
  Check ($v["folder_index"] -eq "0") "an external add keeps the current index"
  Check ($v["file_name"] -eq "a.bmp") "an external add keeps the current image"
  Check ($v["has_image"] -eq "1") "the picture stays on screen after a rescan"
  Check ($add.Log -match "watch: rescan files=3 index=0") "the rescan is logged"

  # --- 2. External delete of the current file: advance + toast --------------
  $delFolder = Join-Path $work "del"
  New-ImageFolder $delFolder @("a.bmp", "b.bmp")
  $del = RunWatch "delete" (Join-Path $delFolder "a.bmp") {
    Remove-Item (Join-Path $delFolder "a.bmp")
  }
  $v = $del.Values
  Check ($v["folder_files"] -eq "1") "deleting the current file shrinks the list"
  Check ($v["file_name"] -eq "b.bmp") "deleting the current file advances to the next image"
  Check ($v["has_image"] -eq "1") "the next image is displayed"
  Check ($del.Log -match "watch: current=removed") "the removal branch is logged"
  Check ($del.Log -match "The current file no longer exists") "a removal toast is shown"
  Check ($del.Log -match "watch: advance index=0 files=1") "the advance is logged"

  # --- 3. Deleting the last image falls back to the empty-folder state ------
  $emptyFolder = Join-Path $work "empty"
  New-ImageFolder $emptyFolder @("only.bmp")
  $empty = RunWatch "empty" (Join-Path $emptyFolder "only.bmp") {
    Remove-Item (Join-Path $emptyFolder "only.bmp")
  }
  $v = $empty.Values
  Check ($v["folder_files"] -eq "0") "the empty folder has no files"
  Check ($v["empty"] -eq "1") "deleting the last image shows the empty state"
  Check ($v["empty_kind"] -eq "folder") "the empty state is the folder variant"
  Check ($v["has_image"] -eq "0") "no image remains after the last delete"

  # --- 3b. An image appearing in an empty folder leaves the empty state ----
  $appearFolder = Join-Path $work "appear"
  New-Item -ItemType Directory -Path $appearFolder -Force | Out-Null
  $appear = RunWatch "appear" (Join-Path $addFolder "a.bmp") {
    New-Bmp (Join-Path $appearFolder "new.bmp") 40 60 200
  } @("--simulate-drop", $appearFolder) "empty: state=folder"
  $v = $appear.Values
  Check ($v["folder_files"] -eq "1") "a new image in an empty folder is listed"
  Check ($v["file_name"] -eq "new.bmp") "a new image in an empty folder is opened"
  Check ($v["has_image"] -eq "1") "the empty state is left without a manual refresh"
  Check ($appear.Log -match "watch: folder=appeared files=1") "the appearance is logged"

  # --- 4. External rename of the current file: no guessing, advance ---------
  $renameFolder = Join-Path $work "rename"
  New-ImageFolder $renameFolder @("a.bmp", "b.bmp")
  $renamed = RunWatch "rename" (Join-Path $renameFolder "a.bmp") {
    Rename-Item (Join-Path $renameFolder "a.bmp") "z.bmp"
  }
  $v = $renamed.Values
  Check ($v["folder_files"] -eq "2") "an external rename keeps the file count"
  Check ($v["file_name"] -eq "b.bmp") "a renamed current file is treated as removed"
  Check ($renamed.Log -match "watch: current=removed") "the rename removal is logged"

  # --- 5. External content edit: toast, no automatic reload ----------------
  $modFolder = Join-Path $work "modify"
  New-ImageFolder $modFolder @("a.bmp")
  # The dump must land while the toast is still up: with notifications the
  # "modified" toast appears ~700 ms after launch and expires after 2500 ms, so
  # 2600 ms is late enough for the edit and early enough for the toast.
  $modified = RunWatch "modify" (Join-Path $modFolder "a.bmp") {
    Start-Sleep -Milliseconds 20
    New-Bmp (Join-Path $modFolder "a.bmp") 200 40 40
  } -dumpAt 2600 -quitAt 3200
  $v = $modified.Values
  Check ($v["file_name"] -eq "a.bmp") "a content edit keeps the same file open"
  Check ($v["has_image"] -eq "1") "a content edit does not clear the picture"
  Check ($v["toast_count"] -ge "1") "a content edit shows the reload toast"
  Check ($modified.Log -match "watch: current=modified") "the modification is logged"
  Check ($modified.Log -match "press F5 to reload") "the toast tells the user to press F5"
  $decodedCount = ([regex]::Matches($modified.Log, "decoded:")).Count
  Check ($decodedCount -eq 1) "a content edit does not reload the image automatically"

  # --- 6. Our own delete: no second rescan ---------------------------------
  $ownFolder = Join-Path $work "own"
  New-ImageFolder $ownFolder @("a.bmp", "b.bmp")
  $own = RunWatch "own-delete" (Join-Path $ownFolder "a.bmp") {
    # --delete-current runs on its own timer; nothing to mutate here.
    Start-Sleep -Milliseconds 100
  } @("--delete-current")
  $v = $own.Values
  Check ($v["folder_files"] -eq "1") "the app's own delete updates the list once"
  Check ($v["file_name"] -eq "b.bmp") "the app's own delete opens the next image"
  Check (-not ($own.Log -match "watch: rescan")) "the app's own delete does not trigger a watch rescan"
  Check (-not ($own.Log -match "current=removed")) "the app's own delete is not reported as external"

  # --- 7. Our own rename: no second rescan ---------------------------------
  $ownRenameFolder = Join-Path $work "own-rename"
  New-ImageFolder $ownRenameFolder @("a.bmp", "b.bmp")
  $ownRename = RunWatch "own-rename" (Join-Path $ownRenameFolder "a.bmp") {
    Start-Sleep -Milliseconds 100
  } @("--rename-to", "z.bmp")
  $v = $ownRename.Values
  Check ($v["folder_files"] -eq "2") "the app's own rename keeps the file count"
  Check ($v["file_name"] -eq "z.bmp") "the renamed file stays open"
  Check (-not ($ownRename.Log -match "watch: rescan")) "the app's own rename does not trigger a watch rescan"

  # --- 8. Minimized windows pause the watcher ------------------------------
  $minFolder = Join-Path $work "min"
  New-ImageFolder $minFolder @("a.bmp")
  $minimized = RunWatch "minimized" (Join-Path $minFolder "a.bmp") {
    New-Bmp (Join-Path $minFolder "c.bmp") 40 40 200
  } @("--minimize-at", "700") "test: minimized"
  $v = $minimized.Values
  Check ($v["watch_mode"] -eq "notify") "a local folder is watched with notifications"
  Check ($v["folder_files"] -eq "1") "a minimized window does not rescan the folder"
  Check ($minimized.Log -match "watch: paused") "the watcher logs the paused state"
  Check (-not ($minimized.Log -match "watch: rescan")) "no rescan runs while minimized"

  # --- 8b. V3-05: local notifications replace the 2 s poll -----------------
  $notifyFolder = Join-Path $work "notify"
  New-ImageFolder $notifyFolder @("a.bmp")
  $notifyAdd = RunNotifyLatency "notify-add" (Join-Path $notifyFolder "a.bmp") {
    [System.IO.File]::WriteAllText((Join-Path $notifyFolder "c.bmp"), "x")
  }
  $v = $notifyAdd.Values
  Check ($v["watch_mode"] -eq "notify") "a local drive uses ReadDirectoryChangesW"
  Check ($v["watch_merge_ms"] -eq "250") "the notification merge window is 250 ms"
  Check ($notifyAdd.Log -match "watch: mode=notify") "the notify source is logged"
  Check ($v["folder_files"] -eq "2") "the notified add is reflected in the list"
  Check ($notifyAdd.Latency -ge 0 -and $notifyAdd.Latency -le 500) (
    "an external add is reflected within 500 ms (measured {0} ms)" -f $notifyAdd.Latency)

  $notifyDelFolder = Join-Path $work "notify-delete"
  New-ImageFolder $notifyDelFolder @("a.bmp", "b.bmp")
  $notifyDelete = RunNotifyLatency "notify-delete" (Join-Path $notifyDelFolder "a.bmp") {
    [System.IO.File]::Delete((Join-Path $notifyDelFolder "a.bmp"))
  }
  $v = $notifyDelete.Values
  Check ($notifyDelete.Latency -ge 0 -and $notifyDelete.Latency -le 500) (
    "an external delete is reflected within 500 ms (measured {0} ms)" -f $notifyDelete.Latency)
  Check ($v["folder_files"] -eq "1") "the notified delete shrinks the list"
  Check ($v["file_name"] -eq "b.bmp") "the notified delete advances to the next image"
  Check ($notifyDelete.Log -match "watch: current=removed") "the removal branch runs on the notify path"

  $notifyRenameFolder = Join-Path $work "notify-rename"
  New-ImageFolder $notifyRenameFolder @("a.bmp", "b.bmp")
  $notifyRename = RunNotifyLatency "notify-rename" (Join-Path $notifyRenameFolder "a.bmp") {
    Rename-Item (Join-Path $notifyRenameFolder "a.bmp") "z.bmp"
  }
  $v = $notifyRename.Values
  Check ($notifyRename.Latency -ge 0 -and $notifyRename.Latency -le 500) (
    "an external rename is reflected within 500 ms (measured {0} ms)" -f $notifyRename.Latency)
  Check ($v["file_name"] -eq "b.bmp") "a notified rename is treated as removed"
  Check ($notifyRename.Log -match "watch: current=removed") "the rename removal is logged"

  # 100 rapid writes must merge into at most two notifications (and therefore at
  # most two rescans) instead of one per file.
  $notifyBurstFolder = Join-Path $work "notify-burst"
  New-ImageFolder $notifyBurstFolder @("a.bmp")
  $notifyBurst = RunNotifyLatency "notify-burst" (Join-Path $notifyBurstFolder "a.bmp") {
    for ($i = 0; $i -lt 100; $i++) {
      [System.IO.File]::WriteAllText((Join-Path $notifyBurstFolder ("burst$i.bmp")), "x")
    }
  }
  $notifyCount = Count-Matches $notifyBurst.Log "watch: notify"
  $rescanCount = Count-Matches $notifyBurst.Log "watch: rescan"
  Check ($notifyCount -ge 1 -and $notifyCount -le 2) (
    "100 rapid writes merge into at most two notifications (saw {0})" -f $notifyCount)
  Check ($rescanCount -le 2) (
    "the burst triggers at most two rescans (saw {0})" -f $rescanCount)
  Check ($notifyBurst.Values["folder_files"] -eq "101") "the burst is fully listed"

  # --watch-poll: the fallback branch (remote path / failed Start) still polls.
  $pollFolder = Join-Path $work "poll"
  New-ImageFolder $pollFolder @("a.bmp")
  $poll = RunNotifyLatency "poll-fallback" (Join-Path $pollFolder "a.bmp") {
    [System.IO.File]::WriteAllText((Join-Path $pollFolder "c.bmp"), "x")
  } @("--watch-poll")
  $v = $poll.Values
  Check ($v["watch_mode"] -eq "poll") "the fallback keeps the 2 s poll"
  Check ($v["watch_merge_ms"] -eq "0") "the poll has no merge window"
  Check ($v["watch_interval"] -eq "2000") "the fallback interval is unchanged"
  Check ($poll.Log -match "watch: mode=poll interval=2000 reason=forced") "the poll fallback is logged"
  Check (-not ($poll.Log -match "watch: mode=notify")) "no notifier starts in poll mode"
  Check ($v["folder_files"] -eq "2") "the poll fallback still notices the change"

  # A second change inside the 2 s minimum-rescan window must be retried, not
  # lost: the notification path has no next poll to fall back on.
  $twiceFolder = Join-Path $work "notify-twice"
  New-ImageFolder $twiceFolder @("a.bmp")
  $twice = RunWatch "notify-twice" (Join-Path $twiceFolder "a.bmp") {
    [System.IO.File]::WriteAllText((Join-Path $twiceFolder "c.bmp"), "x")
    Start-Sleep -Milliseconds 300
    [System.IO.File]::WriteAllText((Join-Path $twiceFolder "d.bmp"), "x")
  }
  Check ($twice.Values["folder_files"] -eq "3") "a second change inside the rescan window is retried"

  # A change that lands while minimised must be applied after restore (the
  # notifier keeps it pending and retries every 500 ms) instead of being lost.
  $resumeFolder = Join-Path $work "notify-resume"
  New-ImageFolder $resumeFolder @("a.bmp")
  $resume = RunWatch "notify-resume" (Join-Path $resumeFolder "a.bmp") {
    [System.IO.File]::WriteAllText((Join-Path $resumeFolder "c.bmp"), "x")
  } @("--minimize-at", "700", "--restore-at", "2600") "test: minimized"
  $v = $resume.Values
  Check ($v["watch_mode"] -eq "notify") "the local folder keeps using notifications"
  Check ($v["folder_files"] -eq "2") "a change during a pause is applied after restore"
  Check ($resume.Log -match "watch: paused") "the pause is logged"
  Check ($resume.Log -match "watch: resumed") "the resume is logged"

  # --- 9. Folder wrap: loop / next / ask -----------------------------------
  $tree = Join-Path $work "tree"
  foreach ($name in @("1", "2empty", "3", "10")) {
    New-Item -ItemType Directory -Path (Join-Path $tree $name) -Force | Out-Null
  }
  New-Bmp (Join-Path $tree "1\1-a.bmp") 90 40 40
  New-Bmp (Join-Path $tree "3\3-a.bmp") 40 90 40
  New-Bmp (Join-Path $tree "3\3-b.bmp") 40 40 90
  New-Bmp (Join-Path $tree "10\10-a.bmp") 90 90 40
  $start = Join-Path $tree "3\3-a.bmp"

  $loop = RunWrap "wrap-loop" "" "End,Right" $start
  Check ($loop.Values["file_name"] -eq "3-a.bmp") "loop wraps to the first image of the same folder"
  Check ((Rect $loop.Values "confirm_card").W -eq 0) "loop never opens the boundary prompt"

  $next = RunWrap "wrap-next" "next" "End,Right" $start
  Check ($next.Values["file_name"] -eq "10-a.bmp") "next enters the following image folder"
  Check ((Split-Path -Leaf $next.Values["folder_path"]) -eq "10") "next skips the empty sibling folder"
  Check ([double]$next.Values["folder_index"] -eq 0) "next opens the first image of the sibling"

  $previous = RunWrap "wrap-prev" "next" "Home,Left" $start
  Check ($previous.Values["file_name"] -eq "1-a.bmp") "prev enters the previous image folder"
  Check ((Split-Path -Leaf $previous.Values["folder_path"]) -eq "1") "prev also skips the empty folder"

  $ask = RunWrap "wrap-ask" "ask" "End,Right" $start
  $v = $ask.Values
  Check ($v["confirm_open"] -eq "1") "ask opens the boundary prompt"
  Check ($v["confirm_kind"] -eq "wrap_next") "the prompt remembers its direction"
  Check ($v["confirm_target"] -eq "10") "the prompt names the next folder"
  Check ($v["file_name"] -eq "3-b.bmp") "the prompt does not navigate before a choice"
  Check ($v["folder_wrap"] -eq "ask") "the policy is unchanged while the prompt is open"
  $card = Rect $v "confirm_card"
  $row = Rect $v "confirm_remember_row"
  $box = Rect $v "confirm_remember_box"
  Check ($card.W -gt 0) "the boundary prompt has a card"
  Check ($row.W -gt 0 -and $box.W -gt 0) "the prompt shows the remember row"
  Check (Inside $row $card) "the remember row stays inside the card"
  Check ((Rect $v "confirm_primary").L -gt (Rect $v "confirm_secondary").R) "the primary action is on the right"
  Check ($ask.Log -match "confirm: open=wrap next target=10") "the prompt opening is logged"

  $askEscape = RunWrap "wrap-ask-esc" "ask" "End,Right,Esc" $start
  Check ($askEscape.Values["file_name"] -eq "3-b.bmp") "Escape stays on the current image"
  Check ($askEscape.Values["confirm_open"] -eq "0") "Escape closes the prompt"
  Check ($askEscape.Values["folder_wrap"] -eq "ask") "Escape leaves the policy unchanged"
  Check ($askEscape.Log -match "confirm: action=cancel") "the cancel is logged"

  $askEnter = RunWrap "wrap-ask-enter" "ask" "End,Right,Return" $start
  Check ($askEnter.Values["file_name"] -eq "10-a.bmp") "Enter enters the offered folder"
  Check ($askEnter.Values["folder_wrap"] -eq "ask") "Enter without remember keeps the policy"

  $askRemember = RunWrap "wrap-ask-remember" "ask" "End,Right,Space,Return" $start
  Check ($askRemember.Values["file_name"] -eq "10-a.bmp") "Space + Enter enters the offered folder"
  Check ($askRemember.Values["folder_wrap"] -eq "next") "remembering the enter choice saves next"
  Check ($askRemember.Log -match "confirm: remember=1") "the checked state is logged"

  $askRememberStay = RunWrap "wrap-ask-stay" "ask" "End,Right,Space,Esc" $start
  Check ($askRememberStay.Values["file_name"] -eq "3-b.bmp") "Space + Escape stays on the image"
  Check ($askRememberStay.Values["folder_wrap"] -eq "loop") "remembering the stay choice saves loop"

  # A folder with no image neighbour still wraps inside itself under "next".
  $lonely = Join-Path $work "lonely"
  New-ImageFolder $lonely @("a.bmp", "b.bmp")
  $single = RunWrap "wrap-single" "next" "End,Right" (Join-Path $lonely "a.bmp")
  Check ($single.Values["file_name"] -eq "a.bmp") "next falls back to loop at the last folder"
  Check ($single.Values["folder_files"] -eq "2") "the fallback stays in the current folder"

  # Ask with no neighbour offers the in-folder wrap and has no target name.
  $askLoop = RunWrap "wrap-ask-loop" "ask" "End,Right,Return" (Join-Path $lonely "a.bmp")
  Check ($askLoop.Values["file_name"] -eq "a.bmp") "ask with no neighbour wraps to the first image"
  Check ($askLoop.Log -match "confirm: open=wrap next target=") "the fallback prompt has no target folder"
}
finally {
  Remove-Item -Recurse -Force $work -ErrorAction SilentlyContinue
}

Write-Host ""
if ($script:failures -gt 0) {
  Write-Host ("FAILED: {0} of {1} watch checks failed" -f $script:failures, $script:checks) -ForegroundColor Red
  exit 1
}
Write-Host ("OK: {0} watch checks passed" -f $script:checks) -ForegroundColor Green
exit 0
