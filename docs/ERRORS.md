# 错误与状态反馈（v2.0 S1 / 規劃 `22/11`）

> 交付：`src/core/ui_error.*`、`src/ui/error_card.*`、`src/ui/toast.*`、
> `src/ui/loading_state.*`；`main.cpp` 不再有 `MessageBoxW`。
> 测试：`zoreview_tests` 530 项、`tests/run_error_ui_check.ps1` 131 项。

---

## 1. 错误模型

`core/ui_error.h` 把解码/文件操作的 `Error`（`core/error.h`）转成一份
`UiError{kind, code, path, detail, actions, message_id, hint_id}`：

| 字段 | 用途 |
| --- | --- |
| `kind` | 分类（缺文件/权限/不支持/损坏/缺 codec/内存/保存/打印/预览/Shell/取消/未知） |
| `code` | 内部错误码或 HRESULT，只进日志和详情行 |
| `path` | 失败的文件；动作按钮（复制/打开方式/显示）有它才出现 |
| `detail` | 技术文本（解码器消息），卡片第三行（11 DIP，muted） |
| `actions` | 位掩码，`DefaultErrorActions(kind)` 给默认值 |
| `message_id` / `hint_id` | i18n 键；调用方可覆盖（例如删除失败仍用 `kErrorDelete` 作标题） |

分类优先级（`ClassifyDecodeError`）：

1. `hr == 0x88982F50`（`WINCODEC_ERR_COMPONENTNOTFOUND`）→ `kCodecMissing`；
2. `E_OUTOFMEMORY` → `kOutOfMemory`；
3. 文件不存在（磁盘再查一次）→ `kFileMissing`（**先于**解码错误，避免把
   “文件被删”报成“文件损坏”）；
4. `E_ACCESSDENIED` → `kAccessDenied`；
5. 路由格式 `kUnknown` → `kUnsupported`；
6. 其余 → `kCorrupt`。

`kCancelled` 完全不呈现（只写 `error: kind=cancelled (silent)`），这是快速切图
不误报的关键。

### 1.1 默认动作

| kind | 动作 |
| --- | --- |
| `kFileMissing` / `kAccessDenied` | 复制路径 / 在资源管理器中显示 |
| `kUnsupported` / `kCodecMissing` | 用其他程序打开 / 复制路径 |
| `kCorrupt` | 重试 / 用其他程序打开 / 复制路径 |
| `kOutOfMemory` | 重试 |
| `kSaveFailed` | 重试（重新打开另存为）/ 复制路径 |
| 其他 | 重试 / 复制路径（没有 `path` 时自动过滤掉） |

卡片最多三个动作 + 关闭；动作顺序固定为
`retry → open_with → copy_path → reveal`。`Enter` 运行默认动作（有重试就是
重试），`Esc` 关闭。

---

## 2. 三级呈现

### 2.1 Toast（轻）

- 位置：`LayoutFrame::toast`（BottomStack 顶 + 12 DIP；pinned 缩略图条之上）；
  `ComputeToastRect()` 按文本测量结果自适应宽度（180–480 DIP）。
- 最多 3 条，最新的在最下面；**相同文本合并**（移到最新位置，不重复）。
- 2600 ms 自动消失；鼠标停在任意 Toast 上时暂停（300 ms 轮询检查），离开后
  继续计时。
- 左侧 3 DIP 红色竖条表示失败类 Toast（`PushToast(text, danger=true)`）。

用于：已复制、已保存、已设壁纸、已删除、不支持保存/重命名扩展名、复制失败、
删除/改名/壁纸/资源管理器失败等“说一句就够”的结果。

### 2.2 错误卡（中）

`ComputeErrorCardLayout(content, dpi, button_widths, has_detail)`：

```text
宽 = min(480 DIP, content - 48)   水平/垂直居中于图像内容区
高 = padding*2 + 24(标题) + 4 + 20(正文) + (2+18)(详情) + 16 + 32(按钮)
标题行：20 DIP 警告图标 + 15 semibold 文本
正文：12 DIP secondary（hint）
详情：11 DIP muted（技术消息，单行省略）
按钮：右对齐，关闭永远在最右；默认动作画成 accent 实心
```

- **非模态**：只有按钮命中；卡片主体、图片区照常接收平移/导航
  （`HitTestErrorCard` 只返回按钮，`UiRoot::HitTest` 否则继续往下走）。
- 单例：新错误替换旧错误；打开新图（`OpenFile`/`EnterFolder`）、`Esc`、关闭
  按钮都会清掉。
- 层级：图像之上、bottom/top 面板之下；`OverlayStack` 的 Esc 顺序是
  `settings → modal → popup → info → error → fullscreen`。
- 失败时不再画启动空态卡：失败本身就是状态（空态让位）。
- 动作语义：
  - 重试：`kSaveFailed` → 重新打开另存为；其余带 `path` → `LoadPath(path, true)`；
  - 复制路径：`CF_UNICODETEXT` + Toast，卡片保持打开；
  - 用其他程序打开：`ShellExecuteW(L"openas", path)`；
  - 在资源管理器中显示：`RevealInExplorer`。

### 2.3 确认浮层（危险）

`ComputeConfirmCardLayout(client, dpi)`：440 DIP 宽、20 padding、scrim、圆角卡；
主按钮（`删除`）红色实心在右，次按钮（`取消`）在左。

- `OverlayStack::OpenModal(kConfirm)`：模态，期间所有看图命令、右键菜单、
  滚轮都被拦掉，外部点击无效。
- `Esc` / 次按钮 / `Enter` 都取消（**安全默认**）；只有主按钮才执行删除。
- 替代了原 `DeleteCurrent()` 里的 `MessageBoxW`；`DeleteCurrentFile()` 仍是唯一
  删除实现。

### 2.4 加载态

- 解码请求发出 400 ms 后仍未完成才显示（`kLoadingDelayMs`）；
  成功/失败/取消/切图立即隐藏。
- 中央 200×68 DIP 的半透明 chip（保证亮色照片上可读），内部 120×3 DIP 轨道 +
  40% accent 滑块（40 ms 定时器往返），下方 12 DIP “正在加载…”。
- 只在 UI 线程用一个定时器；不创建位图资源。

---

## 3. 接线（`main.cpp`）

- `ShowError(hwnd, UiError)`：唯一入口；写
  `error: kind=… code=0x… actions=0x… path=… detail=…`，打开卡并刷新布局。
- `ShowErrorToast(hwnd, text, detail)`：轻量失败；详情只进日志。
- `DeleteCurrent()`：打开确认卡；`CloseConfirm(run_primary)` 执行/取消。
- `OnDecodeDone`：
  - 成功 → `load: done ms=…`；
  - 失败 → `ClassifyDecodeError` + `load: failed ms=…` + 错误卡；
  - `SetImage` 失败（D2D 位图）→ `kErrorRenderFailed` 卡（重试/打开方式/复制）。
- `LoadPath`：切换前 `load: cancelled path=…`；请求前 `load: start` 和
  `BeginLoading()`（缓存命中不触发）。
- 启动 D3D/D2D 初始化失败不再弹 `MessageBox`：设置 `g_fatal_error_text`，
  用 GDI 在窗口里画出错误并跑正常消息循环（`RunFatalErrorWindow`）。

后台失败不打扰：预取失败只日志（`prefetched:` 成功 / `prefetch failed:`），
缩略图失败只日志 + 占位，取消完全静默。

---

## 4. 文案（i18n）

新增 `StringId`（中/英）：`kErrorActionRetry/CopyPath/OpenWith/Reveal/Close`、
`kErrorFileMissing(+Hint)`、`kErrorAccessDenied(+Hint)`、`kErrorUnsupported(+Hint)`、
`kErrorCorrupt(+Hint)`、`kErrorCodecMissing(+Hint)`、`kErrorOutOfMemory(+Hint)`、
`kErrorSaveFailedHint`、`kErrorOperationFailed(+Hint)`、`kErrorRenderFailed(+Hint)`、
`kLoadingText`、`kButtonDelete`、`kToastImageSaved/WallpaperSet/FileDeleted`。

规则不变：英文表不得含 CJK（`ui_tests` 检查）；用户主文案里不出现 HRESULT，
技术文本只在详情/日志里。

---

## 5. 日志与测试钩子

```text
error: kind=corrupt code=0x88982F60 actions=0x07 path=… detail=…
error: closed
error: cleared=new_image
load: start path=… | load: done ms=… | load: failed ms=… | load: cancelled path=…
load: shown path=… after_ms=… | load: hidden
confirm: open=delete path=… | confirm: action=primary|cancel
ui: toast=… [danger=1]
cmd: error=retry|copy_path|open_with|reveal
cmd: id=error.0..2|error.close|confirm.primary|confirm.secondary
```

测试钩子（发布前保留、不宣传）：

| 钩子 | 作用 |
| --- | --- |
| `--force-error missing\|access\|unsupported\|corrupt\|codec\|oom\|save` | 启动后立即显示对应错误卡（path = 打开的文件） |
| `--force-toast <text>`（可重复） | 排队 Toast，验证 3 条上限/合并 |
| `--force-confirm` | 700 ms 后打开删除确认卡 |
| `--force-loading [ms]` | 模拟慢加载：400 ms 后显示；带 `ms` 时到时隐藏；`ms < 400` 表示快加载（不显示） |
| `--click-hit error.N\|error.close\|confirm.primary\|confirm.secondary` | 按绘制矩形点击 |
| `--send-keys "Delete"` / `"Esc"` / `"Enter"` | 真实 `WM_KEYDOWN` 路径 |

`--layout-dump` 新增：

```text
error_open / error_kind / error_buttons / error_default
error_card / error_title / error_message / error_detail / error_close
error_button_N
confirm_open / confirm_card / confirm_primary / confirm_secondary
loading_shown / loading_state / loading_chip / loading_bar / loading_label
toast_count / toast_N
```

---

## 6. 坑

1. **别只按 `has_image` 判断空态**：解码失败后 `g_pending_path` 已清空，空态卡
   会冒出来；错误卡可见时显式不画空态卡。
2. **取消不是错误**：`ClassifyDecodeError` 之前先判断请求是否仍是 `CurrentId`；
   被取消的 completion 只删对象。
3. **重试要取消旧请求**：`LoadPath` 开头 `CancelAll()` + `CancelLoading()`，
   连点重试不会堆解码。
4. **Toast 位置不能用底栏高度硬编码**：读 `LayoutFrame::toast`；全屏时
   BottomStack 顶就是客户区底部（`ComputeLayout` 已处理）。
5. **Toast 队列有上限且合并**：`PushToast` 先删同文本再压入，超过 3 条丢最旧。
6. **错误卡不能整块拦截鼠标**：只把按钮放进命中栈；卡片主体返回 `kNone`，
   否则无法平移/翻页（非模态要求）。
7. **Enter 优先级**：确认卡（取消）→ 错误卡（默认动作）→ 空态按钮 → 命令表
   （全屏）。写在 `WM_KEYDOWN` 的同一个函数里，顺序不能反。
8. **主文案不能拼 HRESULT**：`message_id/hint_id` 是固定人话，技术文本只进
   `detail` 和日志。
9. **长路径/长错误要省略**：`detail` 截 160 字符 + 单行省略；完整路径用
   复制动作。
10. **OOM 时卡片资源**：卡片只用文字 + 矢量图标，不分配大位图。
11. **高对比度**：卡片背景/文字/按钮全部走 token（HC 是系统色），红色 danger
    在 HC 下会退化成系统色，属于预期。
12. **英文文案长度**：按钮宽度由 `UiRoot` 实测文本后传入布局，溢出时按比例
    收缩；不要在 `error_card.cpp` 里写死像素宽度。
13. **后台失败不要走 `ShowError`**：预取/缩略图/EXIF 失败只日志，否则浏览大
    目录时会连续弹卡。
14. **确认卡是模态**：期间右键菜单/滚轮/方向键全部拦截，否则会出现“确认框
    还开着就把图删了/换了”。
15. **全屏 Toast 位置**：BottomStack 顶 = 客户区底部；不要再减底栏高度。
16. **退出要杀新定时器**：`kLoadingDelayTimer/kLoadingTickTimer/kConfirmTimer/
    kLoadingEndTimer` 都在 `WM_DESTROY` 里 `KillTimer`。
17. **读屏器**：自绘错误卡没有 UIA provider（与空态同口径），状态仍进窗口标题
    与日志；作为已知限制记录。
18. **不要为卡片新建窗口**：全部画在主窗口的 overlay 层里，不抢焦点、不改
    激活状态。
19. **`--force-loading` 的可选参数只认数字**：否则 `--force-loading <图片>` 会把
    图片路径当成毫秒值吃掉，窗口里就没有图。
20. **加载态起点别提前清零**：`g_load_start_ms` 在 `CancelLoading` 真正清零时
    才重置，否则被强制延迟的加载会记录 `after_ms=0`。
21. **删除确认的默认是取消**：`Enter` 不能删文件；只有主按钮执行。
22. **`OpenFile`/`EnterFolder` 要主动关错误卡**：否则旧错误会跟着新图显示。
    运行中的新图成功上屏也走 `DismissErrorOnNewImage()`（日志
    `error: cleared=new_image`）；`--force-error` 是 sticky 的，测试期间不会被
    首个解码完成清掉。
23. **确认卡正文要单行**：原来复用 `kDeleteConfirmation`（内含 `\n\n`），40 DIP
    的行高只能显示第一行，文件名看不到；现在用 `kConfirmDeleteBody`
    （“将删除到回收站：%s” / “Move to the Recycle Bin: %s”）。
24. **确认卡 Esc/Enter 都取消**：`WM_KEYDOWN` 里先判 `IsModal(kConfirm)` 再判
    错误卡；确认卡打开期间不能命中命令表。
