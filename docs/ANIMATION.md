# 动画面板与帧步进（v2.0 S2 / `22/12`）

> 状态：已完成（2026-10-24）
> 依赖：`01`（浮层/控件）、`02`（底栏）、`05`（旋转不重置视图）
> 测试：`tests/run_animation_check.ps1`（96 项）、`zoreview_tests`、
> `zoreview_formattests` 的 WebP 段

---

## 1. 模块

| 文件 | 作用 |
| --- | --- |
| `src/core/animation_controller.*` | 纯状态机：`kStopped/kPlaying/kPaused/kEnded`、帧索引、循环计数、最小化记忆。不依赖 HWND，可单测 |
| `src/ui/anim_pill.*` | 帧控件几何/命中/绘制（`◀ n/N ▶ ⏸`、label-only 降级态） |
| `src/decoders/webp_anim.*` | WebP 动画：RIFF `VP8X/ANIM/ANMF` 解析 + WIC 逐帧 BGRA + 画布合成 |
| `src/app/main.cpp` | 定时器、`SetImage(..., false)`、标题、最小化/恢复、勾子 |
| `tests/run_animation_check.ps1` | 96 项 GUI/状态检查（临时便携目录，不碰用户配置） |

定时器与状态是分开的：控制器只回答“下一帧是几号、是否结束”，应用负责
`SetTimer` / `KillTimer` 和上传位图。

---

## 2. 状态机

```text
打开动画        → kPlaying（从第 0 帧，现有“自动播放”）
Space / ⏸      → kPlaying ↔ kPaused
, / .           → 先 kPaused，再 ±1（两端循环）
有限 loop 播完  → kEnded：停在最后一帧、按钮变 ▶、控件显示“已结束”
kEnded 按播放   → 从第 0 帧重播（不是从结尾继续）
切图/删除/关闭  → kStopped（KillTimer + Reset）
最小化          → 若在播则 kPaused，记 was_playing
恢复            → 只有 was_playing 才继续；手动暂停不会被恢复“唤醒”
```

- `loop_count == 0` 表示无限循环，`Advance()` 永不返回 `ended`。
- 有限循环的最后一帧**不 wrap**，否则“结束”会跳回第一帧（坑 5/6）。
- 帧率由每帧 `delay_ms` 重设定时器（`≥16 ms`），不累加固定间隔。

---

## 3. 帧控件几何

```text
位置：BottomStack 栈顶 + 12 DIP，水平居中
      （栈顶 = 底栏 / pinned 缩略图条 / auto 条淡入后的上沿；全屏 = 客户区底 + 12）
尺寸：248 × 36 DIP，圆角 18，1 px border.subtle，bg.panel 94%
┌──────────────────────────────────────────┐
│  ◀      12 / 48 · 已结束      ▶      ⏸    │
└──────────────────────────────────────────┘
  28            剩余宽度            28    28
```

- 三个按钮 28×28，内边距 10、间距 6；图标 16 DIP（`IconId::kChevronLeft/
  kChevronRight/kPlay/kPause`）。
- 命中顺序：标题/底栏 → 空态按钮 → 箭头 → **帧控件按钮** → 缩略图条 → 图像。
  控件胶囊的空白处不参与命中，点击会落到图像（不会出现“看得出点不动”）。
- 2 秒没有鼠标移动淡到 35%，`WM_MOUSEMOVE` 立即回到 100%；只改 opacity，
  不移动控件。高对比度下不淡出、用系统色不透明绘制。
- 降级态（仅首帧）：没有按钮，胶囊缩到文字宽度（绘制时收缩，布局矩形仍为
  248 DIP 供 dump/命中判断）。

---

## 4. WebP 动画

- 检测：RIFF `WEBP` + `VP8X` 的 animation 位（`ANMF` 至少 2 个）。
- 计时/循环：从容器解析 `ANIM`（loop count）与 `ANMF`（x/y/w/h/duration/
  dispose/blend）。WIC 只提供像素（`GetFrame(i)` + BGRA 转换）。
- 合成：帧尺寸等于画布时直接整帧替换（WebP 常见布局，也避免残影）；
  子矩形才按 `blend` 做 straight source-over、按 dispose 清背景。
- 上限沿用 512 帧 / 128 MB；超限截断并标记 `info.frames_truncated`，
  控件显示“已截断”。
- 失败回退：`FormatRouter` 用 WIC 首帧显示，`info.animated` 保持 true，
  控件变成“仅首帧”；日志 `webp: playback failed (...)`。
- 动画仍**不进**内存/磁盘缓存；相邻预取只解首帧（`DecodeOptions.animation=false`）。
- **HEIC/多页 TIFF 不是动画**（V3-06 维护）：WIC 只解 frame 0，`wic::Decode` 固定
  `animated=false`，它们按静态图进缓存、不出动画 pill；容器帧数仍显示在信息面板的
  Frames 行。GIF/APNG/WebP 的 WIC 回退仍把 `animated` 恢复成 `frame_count>1`，
  所以“仅首帧”的退化态只留给真正的动画格式（HANDOFF 坑 326）。

---

## 5. 与其它功能

| 功能 | 行为 |
| --- | --- |
| 旋转 | 帧更新 `SetImage(..., false)`，视角/缩放/平移/旋转全部保留 |
| 缩放 | 步进/播放不改变 zoom；`+` 后逐帧仍是同一个 zoom |
| 全屏 | 控件跟随底边 12 DIP；标题/底栏隐藏不影响控件 |
| 信息面板 | 不打断播放；`I` 打开后 `,`/`.` 仍可步进（先暂停） |
| 缩略图条 | pinned 时控件上移，不覆盖条；auto 淡入时同样上移 |
| 最小化 | 暂停；恢复只在之前播放时继续 |
| 静态图 | 无控件；`,`/`.` 记录 `cmd: blocked=animation.next` |

---

## 6. 日志

```text
webp: animation frames=4 loop=2 canvas=160x100
webp: animation frames=4 loop=2 canvas=160x100 truncated
animation: started frames=4 loop=2 source=webp
animation: paused / resumed frame=3
animation: frame=2/4
animation: ended frame=4/4
animation: paused minimized / resumed restored
animation: stopped
cmd: id=anim.prev|anim.next|anim.toggle
cmd: blocked=animation.next      （静态图）
```

---

## 7. 测试钩子（仅调试）

- `--send-keys "space"` / `"comma"` / `"period"`：真实 `WM_KEYDOWN`（逗号/句号
  不能用 `,`/`.` 本身，`--send-keys` 以逗号分隔）。
- `--click-hit anim.prev|anim.next|anim.toggle`：按缓存布局点击；降级态记
  `skipped=hidden`。
- `--force-anim first|paused|ended|truncated`：把控件钉在确定状态，供截图/
  dump 使用（不依赖循环时序）。
- `--minimize-at <ms>` + `--restore-at <ms>`（新增）：验证最小化暂停/恢复；
  常与 `--dump-at`/`--quit-at` 同用。
- `--layout-dump` 新增：
  `anim_visible/anim_buttons/anim_state/anim_frame/anim_frames/anim_ended/
  anim_truncated/anim_first_frame_only/anim_opacity/anim_label` 与
  `anim_pill/anim_prev/anim_label_rect/anim_next/anim_toggle` 矩形。

---

## 8. 坑（实现时踩到的）

1. **`--send-keys ","` 会被当成空序列**：它本身用逗号分隔键；帧步进用
   `comma`/`period` 名字（`ParseKeyToken` 已支持）。
2. **点击帧按钮必须先加进 `WM_LBUTTONDOWN` 的“可按下”白名单**：漏了
   `IsAnimPillButton` 时按钮画得出来、hover 正常，但按下不记录 pressed，
   `WM_LBUTTONUP` 永远不执行命令。
3. **`kEnded` 必须停在最后一帧**：`Advance()` 在“最后一帧 + 循环用尽”时
   直接返回 ended，不能先 wrap 到 0 再停。
4. **重播要从 0 开始**：`Toggle()` 在 `kEnded` 时 `PlayFrom(0)`，并且应用要
   立刻上传第 0 帧（定时器要等一整个 delay 才到下一帧）。
5. **最小化恢复不要“恢复暂停”**：只恢复 `was_playing`；用户手动暂停的动画
   在最小化/恢复后必须仍然暂停。
6. **`WM_SIZE(SIZE_MINIMIZED)` 的客户区是 0**：最小化分支要在既有的
   `width==0 return` 之前处理，否则永远进不去暂停逻辑。
7. **label-only 胶囊不要保留按钮矩形**：否则会出现“看得见的位置点不到”或
   反过来；`ComputeAnimPillLayout(buttons=false)` 只给 label。
8. **WebP 的 ANMF 帧可能只覆盖子矩形**：一律先试“整帧替换”，只有尺寸小于
   画布才合成；否则正常文件会叠出残影。
9. **WebP 的 x/y 是 24 位整数**，按规范存的是 (x/2)，解析时要乘 2。
10. **帧控件和缩略图条/底栏抢空间**：位置必须读 `LayoutFrame::pill`
    （BottomStack 算好的），不要在 `anim_pill.cpp` 里另算底边距。
11. **`anim_pill()` 布局依赖 `UiRoot::Update()`**：`--click-hit` 用的是上一帧
    缓存，必须先走一次 `RefreshUi`（解码完成分支已调用）。
12. **中文标签不要用 `Get-Content` 默认编码断言**：dump 是 UTF-8 无 BOM，
    PS 5.1 默认 ANSI；测试脚本用 `-Encoding UTF8`。
13. **播放时计数不会自己刷新**：布局是缓存的，`AdvanceAnimation()` 上传新帧后
    必须 `RefreshUi(hwnd, false)`，否则图片已经换了、胶囊还写着 `1 / 4`
    （截图复核时踩到）。反过来，`RefreshUi` 里的淡出计时器只在“控件出现/消失”
    时启用，不能每帧把已淡出的控件重新点亮。
14. **截图前要把真实光标移开**：停在缩略图槽上会画出 tooltip，污染控件截图；
    测试脚本在每次截图前 `Cursor.Position = (5,5)`。

---

## 9. 验收

```powershell
powershell -ExecutionPolicy Bypass -File tests\run_animation_check.ps1
.\build\Release\zoreview_tests.exe
.\build\Release\zoreview_formattests.exe     # webp/anim.webp frames max_diff=0
```

- 96 项：WebP/GIF/APNG 控件、几何（4 档 DPI）、播放/暂停/逐帧、
  loop=1 自然结束、kEnded 重播、最小化/恢复、全屏、旋转/缩放/信息面板下
  视图不重置、静态图禁用、降级态、10 张截图。
- `zoreview_tests` 新增 34 项（控制器 19 + 控件布局/命中 15）。
- `zoreview_formattests` 新增 5 项：4 帧逐像素 `max_diff=0`、时长 120 ms、
  loop=2/1、静态 WebP 不产生帧序列。
- 截图：`tests/output/anim/{webp-playing,gif-paused,webp-ended,
  pill-fullscreen,pill-dpi100,pill-dpi125,pill-dpi150,pill-dpi200,
  pill-first-frame,pill-zh}.png`。
