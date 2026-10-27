# 设置模型与设置窗口（v2.0 S2/S3 · 規劃/22/07 + 22/15）

> 代码：`src/platform/settings.*`、`src/ui/settings_layout.*`、
> `src/ui/settings_window.*`、`src/platform/file_association.*`、
> `src/platform/window_placement.*`、`src/platform/single_instance.*`
> 测试：`tests/settings_test.cpp`、`tests/window_session_tests.cpp`、
> `tests/run_settings_check.ps1`、`tests/run_window_state_check.ps1`、
> `tests/run_single_instance_check.ps1`
> 入口：`Ctrl+,`、标题栏 `☰` → “设置…”，或右键菜单“设置…”

---

## 1. 设置文件

位置：`DataDir()\settings.ini`（安装版 `%LOCALAPPDATA%\ZoreView\`；便携版 exe 同目录）。

### 1.1 Schema（version 1）

```ini
[general]
version=1
language=            ; 空 = 跟随 Windows 显示语言；否则 zh / en
theme=dark           ; dark | light | system（跟随系统 AppsUseLightTheme）
checker=1
checker_opacity=60   ; 0-100，棋盘格向窗口背景混合
strip_mode=auto      ; auto | pinned | off
birdseye=0
always_on_top=0
sort_mode=name       ; name | date | size —— v3.0 起无消费者，仅保留旧值
remember_window=1
single_instance=0

[view]
wheel=zoom           ; zoom | page（page 时 Ctrl+滚轮仍缩放）
zoom_step=1.15       ; 1.05 | 1.10 | 1.15 | 1.25
pixel_mode=0         ; 1 = 最近邻放大（仅 ≥100%；缩小仍 CUBIC，22/18）
keep_rotation=1
fullscreen_bg=theme  ; theme | black | opacity（旧文件里的 dim 也接受）
folder_wrap=loop     ; loop | next | ask

[window]
x=640
y=320
w=1280
h=800
maximized=0
saved=0              ; 1 = 已保存过位置，0 = 首次启动居中
```

全部值为 ASCII。`dark_theme` **不落盘**：它由 `theme`（以及 `system` 时的系统设置）
解析出来，供渲染器/UI 使用。

### 1.2 读取

`LoadSettings()` → `LoadSettingsFromFile(path)`：

1. 文件存在但不是可读 INI（空文件、无任何 section、包含无 BOM 的 NUL 字节）
   → 改名为 `settings.ini.bak-YYYYMMDD-HHMMSS`，日志
   `settings: unreadable file backed up to ...`，用默认值重建。
2. `[general] version` 缺失或 < 1 → 迁移旧版 `[ui]` 键
   （`dark_theme/checkerboard/language/thumbstrip_mode/sort_mode/birdseye/
   topmost/keep_rotation/zoom_step`）。
3. 逐键读取，缺失用默认值；非法值回落：
   - `checker_opacity` 夹 0–100
   - `zoom_step` 只接受四个档位，其余 1.15
   - `theme/strip/wheel/fullscreen_bg/folder_wrap` 枚举白名单（`sort_mode` 仅保留值，不参与行为）
   - 窗口尺寸 < 640×480 或 > 16384 回落 1280×800；x/y 超过 ±32768 归零
   - `language` 不是 zh/en（含 `zh-CN` 等可解析写法）时视为跟随系统
   - 主题解析失败按 dark
4. 迁移成功后：写新 schema（原子），再删除旧 `[ui]` 键；日志
   `settings: migrated legacy [ui] keys`。第二次启动不会重复迁移。

### 1.3 保存（原子）

`SaveSettings()` → `SaveSettingsToFile()`：

1. `CopyFile(settings.ini, settings.ini.tmp)`（保留未知键、注释与顺序）；
   目标不存在时直接写新 tmp。
2. 逐键 `WritePrivateProfileStringW(..., tmp)`。
3. `WritePrivateProfileStringW(NULL, NULL, NULL, tmp)` 强制 flush。
   **不 flush 就 `MoveFileEx` 会移动未写完的文件**。
4. `MoveFileEx(tmp, ini, REPLACE_EXISTING | WRITE_THROUGH)`，
   失败重试 3 次（30 ms），仍失败则删除 tmp、保留旧文件、返回 false。
5. 失败时设置窗口页脚显示“设置未保存”，主程序写 `settings: save failed`。

并发：多实例最后写者胜（已知限制，写日志），不做文件锁。

### 1.4 何时写盘

- 离散控件（单选/复选/导航/按钮）立即写盘。
- 透明度滑条拖动时**只预览**（`checker_opacity_live`），松开鼠标才写一次
  （对应规划里的 500 ms 防抖，用“提交时保存”实现，避免每像素写盘）。
- 关闭主窗口（`WM_CLOSE`）保存窗口位置。
- 拖动/缩放结束（`WM_EXITSIZEMOVE`）也保存一次，拖动过程中不写盘。
- 最小化与全屏期间不写：最小化的 `rcNormalPosition` 不可靠，全屏的矩形是显示器
  尺寸而不是用户窗口（規劃/22/15 坑 1/2/3）。退出前没有额外队列。

---

## 2. 生效时机

| 字段 | 生效点 |
| --- | --- |
| `theme` / `checker` / `checker_opacity` | 立即：主窗口 + 设置窗口重绘 |
| `language` | 立即：重建文字/标题/菜单，设置窗口重新取文案 |
| `strip_mode` | 立即：重新布局、重排列表并按路径保持当前图 |
| `sort_mode` | **无消费者**（v3.0 固定资源管理器自然序）；只读写不改行为 |
| `birdseye` / `always_on_top` | 立即（置顶调用 `SetWindowPos`） |
| `wheel` / `zoom_step` / `pixel_mode` / `keep_rotation` | 立即影响下一次输入 |
| `fullscreen_bg` | 立即影响下一次 Clear（全屏中也会刷新） |
| `folder_wrap` | 立即影响下一次翻页 |
| `remember_window` / `single_instance` | 立即记入设置；下一次启动生效 |
| 文件关联 | 立即写 HKCU（“设为默认”二次确认） |

`theme=system` 在 `WM_SETTINGCHANGE("ImmersiveColorSet")` 时重新读取
`HKCU\...\Themes\Personalize\AppsUseLightTheme` 并重绘。

---

## 3. 设置窗口

- 顶层 owned 窗口 + `WS_EX_TOOLWINDOW`（不占任务栏）、`WS_POPUP` 自绘、
  Win11 圆角、无最大化；`Esc`/关闭按钮关闭。**非模态**：改主题能实时看主窗口。
- 尺寸 760×560 DIP（`metrics::kSettingsWidth/Height`），按窗口 DPI 创建并夹到
  显示器工作区；首次打开居中于主窗口所在显示器。
- 布局（`ui/settings_layout.*`，纯函数、单测覆盖）：
  - 标题栏 32 DIP（标题 + 关闭 ✕），可拖动（`WM_NCHITTEST` → `HTCAPTION`）。
  - 左栏 160 DIP：三个页面，当前页 accent 文字 + 3 DIP 竖条 + `bg_panel`。
  - 右栏：分组标题 28 DIP、控件行 36 DIP、标签列 120 DIP；超出滚动
    （3 DIP 滚动条 + 滚轮）。
  - 底部 56 DIP：`恢复默认`、`打开配置目录`、`关闭`（主按钮）。
- 控件：复选 18×18、单选 18 圆环 + 8 圆点、滑条 4 DIP 轨道 + 12 圆点、
  文字按钮高 32。全部颜色取自 `ui/theme_tokens.*`（高对比度自动换系统色）。
- 键盘：`Tab/Shift+Tab` 在页面、控件、底部按钮间循环；`Space/Enter` 激活；
  `←/→` 改单选/滑条（滑条 `Shift` ±10）；`Esc` 关闭。焦点环 2 DIP accent，
  只在窗口获得键盘焦点时绘制。
- 关闭时不需要 flush：所有改动已经应用/保存。

### 3.1 页面

**常规**：语言（跟随系统/中文/English）、主题（深/浅/跟随）、棋盘格、
透明度、缩略图条（自动渐隐/常显/关闭）、鸟瞰图、
记住窗口位置、单实例、始终置顶。（v3.0 删掉了“排序”行，见 `規劃/26 §2.5`。）

**查看**：滚轮（缩放/翻页）、缩放步长（1.05/1.10/1.15/1.25）、
最近邻放大（像素模式，仅 ≥100%）、保留视图旋转、全屏背景（主题/纯黑/暗化）、
到达边界（本文件循环/下个文件夹/询问）。`next` 会进入相邻含图文件夹（空目录跳过），
已经是最后一个时回本文件夹第一张；`ask` 的询问浮层见 `FOLDER-WATCH.md`。

**文件关联**：39 个图片文档扩展名（`ImageDocumentExtensions()`，不含 `.ico/.cur`）
的四列复选网格、全选、已注册计数、`注册所选`、`取消注册`、`设为默认…`，
底部两行状态（计数 / 当前是否默认 / 最近一次结果）。

- 注册/取消只改 `OpenWithProgids`，不抢默认；取消时若默认仍指向我们会恢复原值。
- “设为默认…”需要点两次（第二次按钮变为“确认设为默认”）；成功后复用
  `RegisterFileAssociations(make_default=true)` 的备份/`UserChoice` 逻辑。
- 失败在状态行显示原因（不弹 MessageBox），并写日志。
- 关联操作是同步注册表写入（39 项 <100 ms）。

### 3.2 恢复默认

“恢复默认”第一次点击只把按钮变成“确认恢复默认”并在页脚提示；
第二次点击执行 `DefaultsForRestore()`：
保留**语言、窗口位置/尺寸/最大化/已保存标记**、文件关联；
其余全部回到内置默认（主题 dark、缩放 1.15、滑条 60、缩略图 auto 等）。

### 3.3 单实例与窗口位置（規劃/22/15）

**单实例**

- `single_instance=1` 时启动先取得命名互斥体
  `Local\ZoreView.Single.<DataDir 的 FNV1a-64>`，然后创建 **message-only 接收窗口**
  `ZoreView.InstanceReceiver.<同一哈希>`。窗口在设备初始化之前就存在，所以
  第二个实例不会因为主窗口还没建好而丢失（坑 7）。
- 互斥体名字带 `DataDir` 哈希：便携版（exe 旁边）与安装版（`%LOCALAPPDATA%`）
  互不吞并（坑 10）。
- 第二个实例：`AllowSetForegroundWindow(第一个实例 pid)` → 用
  `WM_COPYDATA`（`'NV1'` 魔数 + 版本 + UTF-16 路径）转发，`SendMessageTimeout`
  最多 2 s → 成功则退出 0。空路径表示只激活窗口。
- 转发失败（接收窗口不存在、超时、版本不认识、路径过长）时**照常打开新窗口**
  并写 warning，不静默吞掉启动。该进程不再创建接收窗口，避免抢占后续启动。
- 接收端：有路径则 `OpenFile()`，无路径只激活；然后
  `ShowWindow(SW_RESTORE/SW_SHOW)` + `SetForegroundWindow` + `FlashWindowEx`。
- 接收窗口在主窗口创建前收到请求时保留最后一条，主窗口可见后转交
  （`SetHandler` 时立即派发）。无头命令（`--install/--register-*` 等）在单实例
  判定之前就返回，不受影响。

**窗口位置**

- `remember_window=1` 且 `[window] saved=1` 时启动恢复普通位置。
- 可见性：与任一显示器工作区的交集 ≥ **100×100** 物理像素才恢复；否则主显示器
  工作区居中（防止拔掉显示器后在屏幕外，坑 4/15）。
- 可恢复时先把矩形**夹到交集最大的工作区**（同时按当前 DPI 夹到最小
  640×480 DIP），所以半屏外、任务栏移动、DPI 变化都不会留下越界窗口
  （坑 16）。坐标允许为负（副屏在左/上）。
- 关闭/拖动结束时用 `GetWindowPlacement().rcNormalPosition` 记录，最大化时
  保存还原矩形并置 `maximized=1`。最小化、全屏、`remember_window=0` 不写。
- `always_on_top` 全屏期间临时 `HWND_NOTOPMOST`，退出全屏后按设置恢复；
  设置本身不变（坑 6）。

---

## 4. 测试钩子

| 钩子 | 作用 |
| --- | --- |
| `--open-settings` | 首图后打开设置窗口 |
| `--settings-page general\|view\|association` | 打开后切到指定页 |
| `--settings-click <name>`（可重复） | 按名称激活控件（同鼠标点击路径） |
| `--settings-dump <file>` | 写设置窗口状态 + 模型 `key=value`，然后退出 |
| `--settings-screenshot <png>` | `PrintWindow` 捕获客户区 PNG，然后退出 |
| `--move-window x,y,w,h` | 设置窗口矩形并发送一次 `WM_EXITSIZEMOVE`（验证“拖动结束保存”） |

控件名：`page.general|view|association`、`row.<field>[.<index>]`
（`language/theme/checker/checker_opacity/strip/birdseye/remember/single/
topmost/wheel/zoom_step/pixel/keep_rotation/fullscreen_bg/folder_wrap`）、
`footer.reset|open_config|close`、`assoc.select_all|register|unregister|default|cell.N`。
滑条也可直接给值：`row.checker_opacity.25`。

`--settings-dump` 字段：`open/page/scroll/assoc_scroll`、全部 schema 字段
（`language/theme/dark_theme/checker/checker_opacity/strip_mode/birdseye/
remember_window/single_instance/topmost/wheel/zoom_step/pixel_mode/
keep_rotation/fullscreen_bg/folder_wrap/window_*`；`sort_mode` 随排序行一起移除，
但 `--print-settings` 仍输出它，因为键还在文件里）、
`assoc_count/assoc_registered/assoc_defaults/assoc_selected`、
`nav_rows/rows/content_h/body_h/max_scroll/title_rect/close_rect`。

日志：`settings: opened/closed/page=/change=/applied=/assoc action=/
window=/window restored/window placement invalid/migrated/recovered`、
`single_instance: receiver ready/forwarded to existing window/open=/forwarded
activate/activate existing/forward failed/no receiver`。

---

## 5. 已知限制 / 偏差

- 没有自定义快捷键编辑器、主题编辑器、缓存目录迁移（规划明确不做）。
- 多实例同时保存最后写者胜。
- 设置窗口固定大小（不可拖拽缩放）；内容超出时滚动。
- 滑条用“松开提交”替代 500 ms 定时防抖，效果相同（拖动期间不写盘）。
- 关联页的“设为默认”一次作用于全部 39 个扩展名（不做单扩展默认值编辑）；
  Windows 10/11 的 `UserChoice` 保护项会被跳过并显示 warning。
- 设置窗口不实现自己的 UIA provider（沿用自绘 UI 口径：文字可见 + Tab 可达）；
  窗口标题、控件文字走 i18n。

## 6. 坑（本阶段新增）

1. **`settings.ini` 不能直接 `WritePrivateProfileString` 写原文件**：先复制到
   `.tmp`、逐键写、flush、再 `MoveFileEx`；否则崩溃/断电会留下空文件，未知键也会丢。
2. **profile API 有写缓存**：不调用 `WritePrivateProfileString(NULL,NULL,NULL,tmp)`
   就移动文件，最后几个键可能还没落盘。
3. **迁移后必须删旧 `[ui]` 键**：否则新旧两套值并存，行为取决于读取顺序；
   删键要在原子保存之后做（保存本身会复制旧键）。
4. **损坏判定不要依赖解析异常**：INI API 几乎不报错。判据用空文件、无 section、
   或无 BOM 的 NUL 字节；UTF-16 BOM 的文件是合法的。
5. **`dark_theme` 是派生值**：`theme` 才是持久化字段；`T` 键把 system 变成显式
   dark/light，`system` 只在设置页选择。
6. **`--language` 不得改写模型**：会话覆盖只改 `nv::i18n` 的当前语言；
   只有菜单/设置页的显式选择才写 `language`（否则一次 `--language en` 会把
   用户配置永久改成 en）。
7. **滑条不能每像素保存**：拖动时用 `checker_opacity_live` 只应用，松开才保存；
   否则整段拖动会触发上百次原子写。
8. **关联页刷新要保留勾选**：`RefreshAssociation()` 会重新读注册表，必须先保存
   `selected` 再按扩展名数量恢复，否则每次注册后勾选被重置成全选。
9. **`Escape` 属于设置窗口自己的消息循环**：主窗口的 `OverlayStack` 不会收到，
   所以设置窗口开着时 Esc 只关设置窗口（规划坑 11）。
10. **设置窗口对象不能在 `WM_DESTROY` 回调里删除自己**：主程序只清
    `g_settings_window_open` 标志，`unique_ptr` 留到进程退出；否则回调栈上的
    `this` 会被释放。
11. **`--settings-dump`/`--settings-screenshot` 会结束进程**：不要和
    `--layout-dump`、`--screenshot` 混用；需要两者时拆成两次运行。
12. **窗口位置恢复要判可见性并夹取**：与工作区交集 ≥100×100 才恢复，然后夹到该
    工作区；否则居中（防止拔掉显示器/DPI 变化后越界，規劃/22/15 坑 4/16）。
    负坐标（左侧副屏）是合法值，不要用 `x<0` 判坏。
13. **单实例接收窗口是 message-only**：`FindWindow` 找不到它，必须
    `FindWindowEx(HWND_MESSAGE, ...)`；名字要带 `DataDir` 哈希，否则便携版和
    安装版会互相吞启动（規劃/22/15 坑 7/10）。
14. **前向失败必须开新窗口**：发送超时/版本不符/无接收窗口时不能静默退出；
    本进程也不要再建接收窗口抢占后续启动（坑 7/8/9）。
15. **缓存临时文件带 pid+tick**：固定 `.tmp` 会被第二个实例截断；读侧坏文件要
    删除当未命中，trim 跳过 `*.tmp-*`（規劃/22/15 坑 13/14）。
13. **`PrintWindow` 是截图钩子的实现**：D2D HwndRenderTarget 在 `WM_PAINT`
    绘制，`PW_CLIENTONLY` 能拿到内容；不要改成屏幕区域截图（会被遮挡/DPI 影响）。
