# 目录监视与文件夹穿透（22/10；V3-05 起本地盘实时通知）

> 实现：`core/folder_watcher.*`（状态机）、`platform/dir_notify.*`（V3-05 本地盘通知）、
> `core/folder_navigator.*`、`main.cpp` 的 watch/prompt 接线；UI 复用 `ui/error_card.*`
> 的确认卡并新增「记住」行。
> 测试：`tests/folder_watch_tests.cpp`（单测，含真实 RDCW）+ `tests/run_watch_check.ps1`
> （97 项端到端）。

---

## 1. 目录监视

### 1.1 双轨：本地盘 ReadDirectoryChangesW，其余 2 s 轮询（V3-05）

**本地固定盘**（`!IsRemotePath()` 且 `Start()` 成功）用
`ReadDirectoryChangesW`：

```text
platform::DirNotifier         一个线程 + 一个目录句柄，只监视当前文件夹（不递归）
  句柄        CreateFileW(FILE_LIST_DIRECTORY, 共享读写删, FILE_FLAG_BACKUP_SEMANTICS|OVERLAPPED)
  过滤        FILE_NOTIFY_CHANGE_FILE_NAME|DIR_NAME|LAST_WRITE|SIZE
  缓冲        64 KB（溢出时返回 0 字节 → 仍当作“有变化”，交给快照判断）
  合并        收到一批后继续挂读，静默 250 ms 才 PostMessage(WM_APP+7) 一次
  收尾        SetEvent → CancelIoEx → join → CloseHandle；ERROR_OPERATION_ABORTED 属正常退出
```

收到消息后执行**与轮询完全相同**的动作：`TakeFolderSnapshot` → `FolderWatcher::Update`
→ rescan。抑制窗口、模态/最小化暂停、导航静默重建基线、自改抑制全部复用，没有第二套状态机。

**回退到现有 2 s 轮询**的情形（代码与 v2.0 相同）：

- `IsRemotePath()` 为真（UNC 或 `DRIVE_REMOTE`）——按用户决定不做共享盘/NAS 场景，
  也不为其做优化；只保留现状；
- `DirNotifier::Start()` 失败（打不开目录 / 第一次 `ReadDirectoryChangesW` 失败）；
- 监听线程在会话中途死亡（句柄被撤销、共享冲突等）→ 收到最后一条通知时改回轮询；
- `--watch-poll`（测试钩子，强制走回退分支）。

```text
FolderSnapshot {               两种来源共用
  folder_key       当前目录（小写、lexically_normal）
  current_key      当前文件（同上，空 = 无图）
  dir_write_time   GetFileAttributesExW(dir).ftLastWriteTime
  file_count       可选；轮询时本地盘每 5 次采样一次（通知时每次都采），网络盘不采样
  current { exists, size, write_time }
}
```

- NTFS 上目录 mtime 对「新建 / 删除 / 重命名」可靠；
- **改文件内容不会改目录 mtime**，所以当前文件的 size/mtime 每次都要单独查；
- 网络盘（UNC / `DRIVE_REMOTE`）只信目录 mtime，不枚举计数（1200 项目的远端
  目录每 2 s 扫一遍会明显拖慢）。
- **暂停**：最小化/不可见/设置窗口/确认浮层/保存卡期间不重扫。通知在暂停期间到达时
  记一个 pending，并用 500 ms 的 `kWatchTimer` 重试，恢复后补一次；否则最小化期间
  的变化会因为没有第二个事件而永久丢失。**同一重试也用于 2 s 最小重扫窗口**：通知落在
  窗口内时 `FolderWatcher::Update` 按设计不 adopt 基线，`rescan_deferred()` 为真，
  重试持续到门开（轮询本来靠“下一轮”完成这件事）。

### 1.2 状态机（`FolderWatcher`）

| 事件 | 判定 | 应用层动作 |
| --- | --- | --- |
| `kFolderChanged` | 目录 mtime / 计数变化 | 重扫列表、按**路径**重新定位当前图；图不重载、zoom/旋转不动 |
| `kCurrentRemoved` | 上一次存在、这一次不存在 | Toast「当前文件已不存在」→ 前进到同槽位（或最后一张）；空目录 → 空态 |
| `kCurrentModified` | 存在且 size/mtime 变化 | Toast「文件已修改，按 F5 重新加载」，**不自动重载** |

- **导航不产生事件**：`folder_key`/`current_key` 变化时静默重建基线，避免切图被
  误报成「文件已修改」。
- **防抖**：两次重扫至少间隔 2 s；防抖窗口内的事件**不丢弃**，下一次轮询会重试。
- **抑制窗口**：自己的删除/重命名/另存为调用
  `FolderWatcher::Suppress(now, 2500 ms)`，随后 `SyncWatchBaseline()` 把新状态
  直接设为基线，因此不会闪两次、也不会把自己写的文件当成外部变化。
- **暂停**：窗口最小化 / 不可见 / 设置窗口打开 / 确认浮层打开时轮询直接返回；
  日志只在状态切换时写 `watch: paused` / `watch: resumed`。通知路径的 pending 重试见 §1.1。
- **全屏继续监视**：用户可能在全屏看一个正在被写入的目录。

### 1.3 重扫细节

- 重扫走 `BuildFolderListFromFolder`（与首次扫描同一顺序：资源管理器自然序，v3.0 起唯一顺序）。
- `RescanFolder(list, keep)` 按小写规范化路径找回当前图；找不到返回 false，
  调用方进入 removed 分支（不做「大小/时间相近就猜是同一个」）。
- 重扫会 `ThumbnailService::CancelPending()` + `strip.SetItems()`，旧目录的缩略图
  不会串进来；`EnsureVisible` 保持当前槽可见。
- 重扫**不取消**当前解码（路径没变），也不重置 zoom/rotation。

---

## 2. 文件夹穿透

`Settings.folder_wrap`：`loop`（默认）/ `next` / `ask`，设置页「查看 → 到达边界」
可改，立即生效并原子落盘。

| 策略 | 末尾按 `→` | 首张按 `←` |
| --- | --- | --- |
| `loop` | 本文件夹第一张 | 本文件夹最后一张 |
| `next` | 进入下一个**含支持图片**的兄弟文件夹第一张；没有则回 loop | 进入上一个的最后一张；没有则回 loop |
| `ask` | 浮层询问；有目标显示目标文件夹名，没有则询问是否回第一张 | 对称 |

### 2.1 相邻文件夹查找

- 只枚举 `parent_path()` 的直接子目录，`StrCmpLogicalW` 自然排序（与资源管理器一致）。
- 从当前文件夹向 `±1` 逐个检查，**至少 1 个受支持扩展名**才算命中；空目录、无权限、
  只有不支持格式的目录都跳过。
- 上限 200 个目录；超过就放弃并回 loop，日志带 `probed=`。
- 兄弟目录列表缓存 2 s（`CachedSiblingFolders`），离开/进入文件夹时失效。
- `parent_path()` 为空（盘根 / UNC 根）→ 直接回 loop。
- 单张图片的文件夹在 `next`/`ask` 下也允许跨目录；`QueryCommand` 通过
  `CommandContext.wrap_crosses_folders` 放行 prev/next，箭头也随之显示。
  纯 `loop` 下仍是「单张禁用」。

### 2.2 `ask` 浮层

- 复用确认卡（`OverlayKind::kConfirm`，440 DIP 宽，沿用 22/11 几何）；新增：
  - 非危险样式：accent 主按钮 + info 图标（删除确认仍是红色警告）；
  - 第三行「记住我的选择」复选框（16 DIP，点和文字都可命中）；
  - 主按钮文案：有目标 = 「进入」，无目标 = 「继续」；次按钮 = 「留在本文件夹」。
- 键盘：`Enter` = 主按钮，`Esc` = 次按钮，`Space` = 切换记住；浮层是模态，
  导航命令被禁用（不会在询问期间又穿一层）。
- 记住规则：
  - **主按钮**（进入/继续）→ `folder_wrap = next`；
  - **次按钮/Esc**（留在本文件夹）→ `folder_wrap = loop`；
  - 立即写 `settings.ini`；写失败弹错误 Toast，不静默。
- Esc 走 `CloseConfirm(false)`（而不是直接清状态），因此「记住 + Esc」也会落盘。

---

## 3. 测试钩子

| 钩子 | 作用 |
| --- | --- |
| `--watch-interval <ms>` | 轮询间隔（100–10000，默认 2000；仅测试，回退分支用） |
| `--watch-poll` | V3-05：本地盘也强制走回退轮询（验证回退分支；仅测试） |
| `--dump-at <ms>` | 到点写 `--layout-dump` 文件但**不退出**，供脚本在运行中改文件 |
| `--quit-at <ms>` | 到点 `WM_CLOSE`，给 watch 场景收尾 |
| `--minimize-at <ms>` / `--restore-at <ms>` | 最小化 / 恢复，验证「暂停不重扫、恢复后补扫」 |
| `--send-keys "End,Right,Space,Esc"` | 真实 `WM_KEYDOWN` 驱动边界/浮层 |
| `--layout-dump` 新字段 | `folder_path`、`file_name`、`folder_wrap`、`watch_interval`、`watch_mode`（`notify`/`poll`）、`watch_merge_ms`（通知 250 / 轮询 0）、`confirm_kind`、`confirm_target`、`confirm_remember`、`confirm_remember_row/box` |

日志（`zoreview.log`）：

```text
watch: mode=notify folder=<path> merge=250
watch: mode=poll interval=2000 reason=remote|forced|no-folder|start-failed error=...
watch: notify
watch: notify stopped, mode=poll interval=2000
watch: rescan files=N index=I
watch: current=removed path=...
watch: current=modified path=...
watch: advance index=I files=N
watch: paused / watch: resumed
nav: peek dir=±1 probed=N found=0/1
nav: folder=<path> first=0/1
confirm: open=wrap next|prev target=<name>
confirm: remember=0/1
confirm: action=primary|cancel remember=0/1
nav: wrap_remember=next|loop
nav: wrap=first|last|next-folder|prev-folder|loop
```

---

## 4. 测试

```powershell
# 单测（70 项：策略决策 / 自然排序 / 跳过空目录 / 探测上限 / 重扫 / 状态机 /
# 真实 RDCW 通知与合并 / Start 失败 / 20 次 Start-Stop 句柄 soak / 重试标记）
.\build\Release\zoreview_tests.exe

# 端到端 98 项：外部增删改重命名、空目录里出现新图、自己删除/重命名、
# 最小化暂停与恢复补扫、通知时延 ≤500 ms、100 连写合并、2 s 门内重试、
# --watch-poll 回退、三种穿透策略、询问浮层 Enter/Esc/Space/记住、无邻居回退、无目标询问
powershell -ExecutionPolicy Bypass -File tests\run_watch_check.ps1

# ASan
powershell -ExecutionPolicy Bypass -File tests\run_watch_check.ps1 `
  -App build-asan\RelWithDebInfo\zoreview.exe
```

手工观察：

```powershell
# 打开目录后用资源管理器复制/删除一张图，V3-05 起本地盘约 0.3 s 反映（旧轮询最坏 2 s）
.\build\Release\zoreview.exe --portable --layout-dump $env:TEMP\w.txt `
  "..\测试文件夹\images\jpeg\photo_1600.jpg"

# 回退分支：同上，但加 --watch-poll（仍是 2 s）
.\build\Release\zoreview.exe --portable --watch-poll --layout-dump $env:TEMP\w2.txt `
  "..\测试文件夹\images\jpeg\photo_1600.jpg"

# 边界询问浮层（需要 [view] folder_wrap=ask）
.\build\Release\zoreview.exe --portable --send-keys "End,Right" `
  --screenshot $env:TEMP\prompt.png --shot-delay 2600 "..\测试文件夹\images\jpeg\photo_1600.jpg"
```

---

## 5. 坑

| # | 现象 | 原因 | 对策 |
| --- | --- | --- | --- |
| 1 | 自己删除/重命名后列表闪两次 | 操作 + 轮询各触发一次重扫 | 写前 `Suppress(2.5 s)`，写后 `SyncWatchBaseline()` |
| 2 | 每次轮询都重扫、CPU 高 | 只比了 count 或没比 mtime | 先比目录 mtime；相同且当前文件存在就返回 |
| 3 | 改文件内容但列表没反应 | 目录 mtime 不变 | 单独比较当前文件 size/mtime，只发「按 F5」Toast |
| 4 | 当前文件删除后还显示旧图 | 重扫后没处理 current | removed 分支：前进同槽位或空态，绝不猜 |
| 5 | 网络盘每 2 s 很慢 | 枚举了子项计数 | 远端只信目录 mtime |
| 6 | 杀软/同步盘抖动导致反复重扫 | 没有防抖 | 最小 2 s 间隔；窗口内的事件下一轮重试而不是丢弃 |
| 7 | 重扫后缩略图串目录 | 缩略图队列没清 | `CancelPending()` + `SetItems()` |
| 8 | 重扫把 zoom 重置 | 走了 `LoadPath` 默认 | 只更新列表/标题/缩略图，不重载当前图 |
| 9 | 进入空文件夹 | 只检查目录存在 | `FolderHasSupportedImages` 至少 1 张受支持图片 |
| 10 | 盘根 / UNC 根穿透死循环 | parent 为空 | `parent_path().empty()` → loop |
| 11 | 边界询问时按 `→` 又穿一层 | 键盘没拦 | 询问是 `kConfirm` 模态，`BlocksNavigation()` 已生效 |
| 12 | 记住后重启没生效 | 只改内存 | `folder_wrap` 写 `Settings` + `SaveSettingsNow()` |
| 13 | 最小化后仍在扫盘 | 只看 `IsWindowVisible` | 再加 `IsIconic`，日志 `watch: paused` |
| 14 | 单张图目录按 `→` 没反应 | `file_count > 1` 的旧门槛 | `wrap_crosses_folders` 放行 next/ask |
| 15 | 询问浮层的「记住」只点文字无效 | 只把 checkbox 当命中区 | `remember` 行 + `remember_box` 都返回 `kConfirmRemember` |
| 16 | Esc 记住选择没落盘 | Esc 直接清状态 | Esc 统一走 `CloseConfirm(false)` |
| 17 | 100 个文件连写触发 100 次重扫 | 每个事件都 `PostMessage` | 通知线程保持挂读，**每来一批就重置 250 ms 静默窗口**，安静后才发一条 |
| 18 | 暂停期间的变化恢复后丢失 | 暂停时直接 `return`，事件只来一次 | 通知到达先记 pending；暂停时用 500 ms `kWatchTimer` 重试，恢复后补一次（不能只靠下一个事件） |
| 19 | 监听线程中途死掉后不再刷新 | 只有 `Start()` 失败才回退 | 线程错误也 `PostMessage`；处理时发现 `running()==false` → 停通知、改回 2 s 轮询（日志 `watch: notify stopped`） |
| 20 | 内容修改的 Toast 断言假失败 | 通知把 Toast 提前约 1.3 s，dump 太晚就抓不到（2500 ms 生命周期） | 测试窗口按 Toast 生命期重算（`run_watch_check` 的 modify 用例 dump-at 2600） |
| 21 | `--watch-poll` 下 dump 仍显示 `notify` | 选择源时只看了文件夹是否变化 | 源选择比较 `(folder, 强制回退)`，回退时先 `Stop()` 再置 `poll`；dump 断言 `watch_mode=poll` |
| 22 | 通知只来一次，第二次变化丢了 | 它落在 2 s 最小重扫窗口内，`Update` 不 adopt 基线等“下一轮”，但通知没有下一轮 | `FolderWatcher::rescan_deferred()` + 500 ms 重试；`notify-twice` 用例覆盖 |
