# 缩略图条增强（v2.0 S2 / `規劃/22/08`）

> 三模式（`auto` 渐隐 / `pinned` 常显 / `off` 关闭）、底部热区（条 + 底栏，v3.0）、
> **220 ms 渐显/渐隐 + 180 ms 宽限（V3.1-09：原 160/400 ms）**、
> 悬停 500 ms 文件名 tooltip、请求策略。
> 排序与“更多▾”开关在 `22/02` 已经完成，本阶段只调整模式语义和显示行为。
>
> **2026-10-27 V3.1-09**：“更多▾”里的开关改名为 **智能缩略图 / Smart thumbnails**，
> 语义变成纯开关——**开 = `auto`（悬停渐隐条）**，**关 = `off`（完全没有条）**；
> `pinned`（常显）只在设置页选。默认仍是 `auto`（开）。同时把消失的宽限
> 400→**180 ms**、渐隐 160→**220 ms**，并给日志加了
> `ui: strip=hidden fade_ms=<实际时长>`，这样“到底有没有渐隐”可以被量化验证
> （实测 234 ms）。

---

## 1. 三种模式

| 模式 | 绘制 | 内容矩形 | 命中 | 缩略图请求 |
| --- | --- | --- | --- | --- |
| `auto`（默认） | 覆盖在图像底部，opacity 0–1 | **不**扣除 | 淡入后才可点（条+底栏热区负责唤出） | 可见后请求；完全隐藏时取消 |
| `pinned` | 常显，opacity 1 | 扣除 76 DIP | 始终可点 | 可见 ±2 |
| `off` | 不绘制 | 不扣除 | 无矩形、无命中 | 不请求 |

- **无图时强制隐藏**：三种模式都一样。`BuildUiInputs()` 里
  `strip_visible = has_image && strip.visible() && mode != off`，空态卡独占内容区。
- **全屏（v3.0 反转，規劃/26 §2.7）**：`strip_available` 不再带 `!fullscreen`——条矩形保留
  （贴客户区底部，承接热区），但全屏下**所有非 `off` 模式都是 reveal-only**：进入全屏时
  连 `pinned` 也先隐藏，移到窗口底部才淡入；`strip_counts_in_content` 仍恒为 false。
- **`auto` 的矩形始终存在**：它承载热区；因此“没有矩形”只可能是
  `off`。
- **命中门禁**：`LayoutFrame::strip_interactive` 只在 `pinned` 或
  `auto && strip_shown` 时为真；`HitTestFrame` 只在该标志下返回 `kStrip`。
  隐藏时同一点落到 `kImage`（或热区保护，见 §4）。

### 1.1 模式入口（单一数据源）

```text
Settings.thumbstrip_mode  ∈ { auto, pinned, off }      // 唯一状态
更多▾ “智能缩略图” checked = (mode != off)             // V3.1-09
  开（勾选）→ auto      关（不勾）→ off
Ctrl+T：同“更多▾”（auto/off 之间切；pinned 视作开，按一次变 off）
设置页：自动渐隐 / 常显 / 关闭 三选一                    // pinned 只在这里
```

- 所有入口都走 `Settings` + `SaveSettings()`，不存在镜像变量。
- `--strip=pinned|auto|off` 仍是**会话覆盖**（`g_strip_override`），不写盘。
- 启动默认 `auto`；`Settings` 结构体、`Normalize()` 回退值和旧 `[ui]` 迁移默认值
  三处一致（`platform/settings.*`）。
- **V3.1-09 的取舍**：`pinned` 从“更多▾”里撤掉，是因为那个开关被用户当成
  “智能缩略图开/关”（渐隐条 = 开，没有条 = 关）；常显属于“高级显示选项”，
  留在设置页。旧的 `pinned` 状态读作“开”，第一次点击就变 `off`。

---

## 2. 空间模型（为什么 auto 不扣内容）

`規劃/22/08 §3.1` 选的是混合模型：

```text
content = client − title(32) − bottom(44) − [pinned ? 76 : 0]
strip   = 底栏上方 76 DIP（auto 时覆盖在图像上，底部贴底栏）
```

- `auto` 渐显/渐隐**只改 opacity**，不改内容矩形，所以浏览时图像不会每帧抖动。
- 唯一会移动的是底部堆叠（pill/toast/info）：`ComputeLayout` 在
  `auto && strip_shown` 时把 `stack_top` 上移 76 DIP。这只在“淡入开始 / 淡出结束”
  各发生一次（`g_strip_opacity` 跨过 0 时 `RefreshUi`）。
- 从 `pinned` 切到 `auto` 时内容矩形变高，图像会跳一次；这是用户主动操作，
  可接受（不重置 zoom/pan）。

---

## 3. 渐显/渐隐

```text
g_strip_opacity ∈ [0,1]，g_strip_target ∈ {0,1}
ShowStrip()      : target=1，启动 16 ms 定时器，记 ui: strip=show
ScheduleStripHide: 指针离开条/热区 → 180 ms 宽限（kStripHideTimer）
宽限到           : target=0，记 ui: strip=hide
TickStripFade()  : opacity += dt / 220ms，到 0/1 停定时器
opacity == 0     : CancelPending()，记 ui: strip=hidden fade_ms=<实际时长>
```

- **按时间推进**（`dt/160ms`），不是固定步长；定时器抖动不会改变时长。
- **减少动态效果**（`SPI_GETCLIENTAREAANIMATION=0`）：`ShowStrip()`/隐藏分支直接
  把 opacity 设为 1/0 并重排一次，不启动 16 ms 定时器；日志仍然是
  `strip=show` / `hide` / `hidden`，行为与淡入完成一致。
- 绘制用 **D2D Layer**（`ThumbnailStrip::Render(..., opacity)`）：一次
  `PushLayer` 把背景、格子、选中框、滚动条一起变透明，不重建画刷。
- 命中不看 opacity 数值，只看 `strip_shown`（>0.001）→ 与视觉一致。

---

## 4. 底部热区（`strip` + 底栏，v3.0）

- **v3.0 (`規劃/26 §2.10`) 把热区从“条矩形最底部 8 DIP”扩成 `strip` 与
  `bottom_bar` 的并集**：只要指针落在条位或底栏任意位置就唤出，不再需要精确压到
  底栏上沿那 8 DIP。`metrics::kStripHotZone` 只剩 `--hover-hotzone` 钩子和
  `WM_LBUTTONDOWN` 的“别把图拖走”保护还在用。
- `auto` 且隐藏时：指针进入热区或（淡入后）整条 → `ShowStrip()`；
  离开两者 → `ScheduleStripHide()`。
- **按住不放时不唤出**（用户 2026-10-26 定的规则）：`g_state->dragging`（图片平移）、
  `g_drag_press.active`（Ctrl+拖出候选）或 `GetCapture()==hwnd`（任何按下的控件）
  任一为真时，移动进热区**只保持现状**，不 `ShowStrip()`；按住拖过底栏不会突然
  弹出缩略图条，平移/拖出照常继续。
- 隐藏时在**图像区的 8 DIP 热区**按下鼠标不会开始平移图像：`WM_LBUTTONDOWN` 在
  `kImage` 分支之前识别热区并只唤出条（否则用户会在条刚要出现时把图拖走）。
  底栏按下走自己的命中，不受这条影响。
- 弹层（更多▾/缩放菜单）打开时热区仍响应（现有门禁只挡 modal，不挡 popup）。
- 窗口失焦后热区仍响应（规划建议保留；未做额外激活判断）。
- **测试脚本必须先把真实指针停到 (5,5)**：热区包含底栏后，指针若停在窗口底部，
  “默认隐藏”断言就会依赖上一次运行把鼠标留在哪。`run_strip_check` /
  `run_navigation_check` / `run_ui_chrome_check` 都已这么做。

---

## 5. 悬停 tooltip

- 触发：指针停在某个槽内 **500 ms**；换槽、滚动、点击、模式变化、淡出、
  离开窗口都会取消计时。
- 内容：文件名（不含路径），最长 320 DIP，超出省略。
- 位置（`ui/strip_tooltip.*` 纯函数）：
  - 优先槽上方 `6 DIP`；
  - 上方放不下（`top < client.top + 4 DIP`）→ 翻到槽下方；
  - 水平以槽中心对齐，再夹取到客户区内 `4 DIP` 边距。
- 样式：`bg_panel` + 1 px `border_subtle` + 4 DIP 圆角 + 2 DIP 偏移阴影，
  12 DIP 字号；tooltip 不参与命中。
- 绘制层：`kBottomPanels` 内、条之后、底栏之前（“条之上、底栏之下”）。
- 锚点是 `ThumbnailStrip::CellRect(index, strip_top)`（整个槽，不只是缩略图）。

---

## 6. 缩略图请求

```text
pinned           : 可见 ±2（沿用）
auto 淡入/可见   : 可见 ±2；淡入过程中每个 tick 幂等补请求
auto 完全隐藏    : CancelPending()，不再请求
off              : 直接 return（不 Cancel，语义上从未请求）
```

- `ThumbnailService::Request` 自身按 `path+mtime` 去重，所以淡入时重复调用不会
  重复排队。
- 滚动、点击、排序、目录重建仍走原来的 `RequestVisibleThumbnails()`。
- `--strip=pinned` 的测试脚本（缩略图/大目录/几何）因此保持原有行为。

---

## 7. 测试钩子

| 钩子 | 用途 |
| --- | --- |
| `--strip=pinned\|auto\|off` | 会话覆盖模式（不落盘） |
| `--hover-thumb <index>` | 首图后向槽中心发真实 `WM_MOUSEMOVE`（同时移动真实光标，避免 `TME_LEAVE` 立刻发 `WM_MOUSELEAVE`） |
| `--hover-hotzone` | 向底部 8 DIP 发一次移动，验证热区唤出 |
| `--hover-bottom` | 向**底栏中心**发一次移动（v3.0：底栏属于热区） |
| `--drag-to-bottom` | 图片中心按下 → 右移 40 DIP 起拖 → 移到**底栏中心**且保持按下；dump 应记录 `dragging=1`、`strip_shown=0` |
| `--hover-cycle` | 热区唤出 → 700 ms 后离开 → 180 ms 宽限 + 220 ms 淡出，一次跑完显示/隐藏 |
| `--layout-dump` | 新增 `strip_shown`、`strip_opacity`、`strip_interactive`、`strip_hover`、`strip_tooltip`、`strip_tooltip_above`、`strip_tooltip_panel`；另有 `dragging`（v3.0，供按住拖拽负例） |

- 带以上某个 hover 钩子时，`--layout-dump` 由钩子自己调度（`g_hover_owns_dump`）：
  tooltip 在合成移动后 900 ms、热区 600 ms、cycle 在 `done` 后 200 ms 写盘。
  这样 ASan/慢启动下“图片还没加载完就 dump”不会发生；不带 hover 钩子时才用
  原来的固定 900 ms 调度。
- 钩子只发真实消息，不直接改状态；`WM_MOUSEMOVE` 走的路径与用户操作完全一致。

---

## 8. 日志

```text
ui: strip_mode=auto|pinned|off          启动/设置变更
ui: toggle thumbstrip=pinned|auto       更多▾ / Ctrl+T
ui: strip=show                          热区或条内 → 开始淡入
ui: strip=hide                          180 ms 宽限结束 → 开始淡出
ui: strip=hidden fade_ms=234             淡出到 0，取消待办（V3.1-09 起带实际时长）
ui: strip_hover=3                       悬停槽变化（-1 = 无）
ui: strip_tooltip=photo.jpg             tooltip 显示
```

---

## 9. 数据（2026-10-24，125% DPI）

```text
zoreview_tests                : 全过（新增 auto 命中门禁 / tooltip 几何 12 项）
run_strip_check.ps1            : 48/48（v3.0：+底栏唤出、按住拖拽负例）
run_ui_chrome_check.ps1        : 98/98（模式语义 + 持久化 + 重启）
run_navigation_check.ps1       : 71/71（Ctrl+T auto↔pinned）
run_settings_check.ps1         : 105/105（恢复默认 = auto）
run_ui_shell_check.ps1         : 全过（显式 --strip=pinned 保持几何基线）
默认 auto：strip_shown=0、opacity=0.000、strip_interactive=0、content 到 745
热区唤出：opacity=1.000、shown=1、content 仍到 745（不扣空间）
tooltip ：panel 1108,610–1275,642（槽 650 上方 6 DIP、高 26 DIP、右侧夹取）
```

---

## 10. 坑（实现时确认过）

1. **动画期间不要改内容矩形**：只改 opacity；`RefreshUi` 只在 `strip_shown`
   跨过 0 时调用，否则每帧重排会让图像抖动。
2. **隐藏时命中要一起隐藏**：`strip_interactive` 为假时 `HitTestFrame` 返回
   `kImage`；否则“看不见的条”会吃掉点击。
3. ~~**热区不能盖住底栏**~~ **v3.0 反转（`規劃/26 §2.10`）**：热区现在**包含**底栏，
   靠“底栏命中先于热区”保证底栏按钮不被抢——热区只负责 `ShowStrip()`，不消费点击；
   `WM_LBUTTONDOWN` 的热区保护只在 `hit == kImage` 内（见 §4）。反转后新增的约束是
   **按住/拖拽时不唤出**（`dragging` / `g_drag_press.active` / 捕获）。
4. **隐藏时点热区不能开始平移**：`WM_LBUTTONDOWN` 在 `kImage` 之前拦下。
5. **`EnsureVisible(kNone)` 会滚到最末项**：`ThumbnailStrip::EnsureVisible`
   内部 `std::min(kNone, size-1)` 会得到 `size-1`；调它之前必须判
   `current != kNone`（现有 WM_SIZE/Paint 已判）。
6. **tooltip 用真实光标**：只 `PostMessage(WM_MOUSEMOVE)` 时真实指针仍在窗口外，
   `TrackMouseEvent(TME_LEAVE)` 会立刻发 `WM_MOUSELEAVE` 清掉 hover；测试钩子
   必须同时 `SetCursorPos`。该坑同样适用于以后任何“合成 hover”的钩子。
7. **tooltip 不参与命中**：`HitTestFrame` 不认识它；否则鼠标移到 tooltip 上会
   “离开槽 → 隐藏 → 又回到槽”地闪烁。
8. **滚动时重置悬停**：滚轮横向滚动后槽内容变了，必须先隐藏 tooltip 再重新
   计时，否则 tooltip 会停在旧文件名上。
9. **`Ctrl+T` 是 auto↔pinned**：`off` 只从设置页进入；从 auto 按 Ctrl+T 得到
   pinned，再按回 auto（会立即隐藏，等待下一次 hover）。
10. **更多▾ 的 checked 是“常显”**：`context.strip_visible` 现在表示
    `mode == pinned`，不是“条存在”。命名沿用旧字段，语义已收窄。
11. **`off` 不请求也不 Cancel**：`RequestVisibleThumbnails` 的 off 分支直接返回；
    auto 隐藏分支才 `CancelPending()`（off 下队列本来就不会有本目录请求）。
12. **默认值三处一致**：`Settings` 默认、`Normalize()` 回退、旧 `[ui]` 迁移
    默认都是 `auto`；只改一处会让“损坏文件恢复/迁移/新装”出现三种默认值。
13. **测试脚本的 `--strip=pinned`**：默认改成 auto 后，依赖“条常显”的旧脚本
    （几何、缩略图、大目录）必须显式传 `pinned`，否则断言的对象根本不存在。
14. **含中文的 `.ps1` 不能丢 BOM**：本次编辑 `run_ui_shell_check.ps1` 后 BOM
    丢失，PS 5.1 按 ANSI 读，`测试文件夹` 变乱码、图片打不开（“image was
    decoded before the dump”失败）。改完脚本要检查前三字节 `239,187,191`。
15. **D2D Layer 缓存要跟着设备释放**：`fade_layer_` 在
    `ReleaseDeviceResources()` 里 Reset；设备丢失重建后第一次 Render 重新 Create。
16. **`opacity <= 0.01` 直接不画**：避免 0 透明层仍然 `EvictOutside`/扫格子。
17. **tooltip 文本来自 `folder.files[index]`**：悬停索引在排序/重建后可能越界，
    取文件名前必须校验 `< files.size()`（`BuildUiInputs` 与 `ShowStripTooltip`
    各一处）。
18. **不要用 `GetBounds` 判断 tooltip/图标是否为空**：描边几何没有填充面积
    （与坑 163 同源）；以 `visible`/矩形宽高为准。
19. **用例之间要“停好光标”**：`auto` 的唤出看真实指针位置；上一个 hover 用例
    把光标留在窗口底部后，下一个“默认隐藏”用例会随机变成已唤出。
    `run_strip_check.ps1` 在每次启动前 `SetCursorPos(5,5)`。
20. **hover 钩子要接管 dump**：固定 900 ms 的 dump 在 ASan/慢机器上会早于
    “图片加载完 + hover 500 ms”，表现为热区用例随机失败；钩子成功后自己
    `SetTimer(kLayoutDumpTimer)`（`g_hover_owns_dump`）。
