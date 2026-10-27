# 拖出到其他程序（Ctrl+拖 / 拖缩略图）

> 状态：已完成（2026-10-25，v2.0 S3 / `22/19`）
> 依赖：`22/03` OLE 拖入、`22/08` 缩略图条、`22/09` 平移
> 测试：`tests/run_drag_out_check.ps1`（17 项）、`zoreview_dragtests.exe`（49 项）

---

## 1. 行为

| 区域 | 操作 | 结果 |
| --- | --- | --- |
| 图片区 | 左键拖动 | 平移（原有行为） |
| 图片区 | **Ctrl + 左键拖动**（越过 4 DIP） | 拖出当前文件（`CF_HDROP` + 文本），只允许复制 |
| 图片区 | Ctrl + 单击（未越阈值） | 无动作；第一次显示“按住 Ctrl 拖动可把文件拖到其他程序”提示 |
| 缩略图条 | 左键单击 | 跳到该张（现在的“按下再松开”） |
| 缩略图条 | 左键拖动（越过 4 DIP） | 拖出**按下时**的那一项，不需要 Ctrl |
| 箭头/底栏/面板/空态 | 拖动 | 不触发（命中优先） |

- 阈值 4 DIP，按窗口 DPI 换算成物理像素；判断用欧氏距离。
- 缩略图条的点击从“按下即跳转”改为“松开时跳转”，否则无法区分点击和拖出。
- 拖动中按 `Esc` 取消；松开左键完成；`DoDragDrop` 用 `DROPEFFECT_COPY`。
- 拖回自己的窗口不会重新打开同一个文件（记 `drag: self-drop ignored`）。

## 2. 模块

| 文件 | 作用 |
| --- | --- |
| `src/platform/file_data_object.*` | `IDataObject`：`CF_HDROP`（`DROPFILES` + `fWide=TRUE` + 双 `\0`）与 `CF_UNICODETEXT`；每次 `GetData` 新建 `HGLOBAL`；`SetData` 拒绝 |
| `src/platform/file_drop_source.*` | `IDropSource`：Esc 取消、松开左键完成、只给 COPY；`BeginFileDrag()` 包 `DoDragDrop` 并成对 `Release` |
| `src/ui/drag_threshold.h` | 纯策略 `ShouldStartDragOut` / `ExceedsDragThreshold`（4 DIP，图像区需 Ctrl，缩略图不需要） |
| `src/app/main.cpp` | 按下快照（路径/区域/Ctrl）、移动阈值、`StartFileDrag`、自拖忽略、`--simulate-*` 钩子 |
| `tests/drag_out_test.cpp` | 数据对象负载/格式枚举、拖放源状态机、阈值规则 |
| `tests/run_drag_out_check.ps1` | 单元 harness + 干跑集成（当前图 / 缩略图第 N 项 / 越界 / 空态 / 拖入回归） |

## 3. 实现要点

- **按下时快照**：`DragOutPress{path, strip_index, anchor, ctrl}`。拖出用的路径永远是
  按下那一项，不会被之后的 `current`/排序变化替换（坑 5）。
- **一次只有一种手势**：图片区 Ctrl+按下不再进入平移；缩略图按下不立刻跳转。
  越过阈值后 `MaybeStartFileDrag()` 返回 true，鼠标移动处理立即结束。
- **捕获**：按下时 `SetCapture`；`StartFileDrag` / 松开左键先 `ReleaseCapture`（并且
  只在 `GetCapture()==hwnd` 时释放），`WM_CAPTURECHANGED` 清掉残留候选，避免
  “窗口一直抓着鼠标”（坑 2/17）。
- **自拖**：`DoDragDrop` 期间 `g_self_drag_path` 非空，`OnDroppedPaths` 对同一路径
  直接忽略并记日志；从资源管理器拖同一个文件仍然正常打开。
- **OLE 初始化**：沿用 `22/03` 的 `OleInitialize` + `OleUninitialize`，无窗口命令路径
  不初始化 OLE。
- **拖出期间不做重活**：只走 `DoDragDrop` 的嵌套消息循环，不启动解码、不改当前图。

## 4. 测试钩子

| 参数 | 说明 |
| --- | --- |
| `--simulate-drag-out` | 对**当前文件**走一遍 `StartFileDrag`（干跑，不进入 `DoDragDrop`），日志 `drag: out path=... source=image effect=1` |
| `--simulate-strip-drag <index>` | 对缩略图第 N 项干跑，日志带 `source=strip index=N` |
| `--simulate-ole-drop <path>` | 原有拖入钩子；用于确认拖出标记不会误伤外部拖入 |

- 干跑只跳过 `DoDragDrop`，路径选择、日志、指针状态清理、UI 刷新都走产品代码。
- 布局 dump 在带这两个钩子时延后到 1800 ms（动作在 1200 ms），
  否则 dump 会先退出进程（坑 271 同类）。

## 5. 手工验收清单（无法脚本化，因为目标在别的进程）

本机已用合成输入跑过一次真实 Ctrl+拖，日志：

```text
drop: enter
drop: leave
drag: self-drop ignored
drag: out path=...\probe.png source=image effect=1
```

（进程正常退出，无挂起。）

发布前请再按下面过一遍：

1. Ctrl+拖图片到资源管理器 → 出现副本，源文件仍在。
2. Ctrl+拖到 Word / 微信 / QQ → 作为文件插入或发送。
3. 拖缩略图第 3 项 → 导出第 3 项，不是当前图。
4. 拖动中按 `Esc` → 取消，无副本、无移动。
5. 松开 Ctrl 后再拖（仍按住左键）→ 拖动继续，不中断。
6. 拖回自己的窗口 → 不重新打开、不闪。
7. 不按 Ctrl 拖图片 → 仍然是平移。
8. 缩略图单击（不移动）→ 仍然跳转。

## 6. 坑

| # | 现象 | 原因 | 对策 |
| --- | --- | --- | --- |
| 1 | `DoDragDrop`/`RegisterDragDrop` 返回 `CO_E_NOTINITIALIZED` | 只 `CoInitializeEx` | 启动用 `OleInitialize`（已由 22/03 完成） |
| 2 | 拖出后鼠标仍“抓住”窗口 | 捕获/状态没清 | `StartFileDrag` 里 `ReleaseCapture` + 清 `g_drag_press`；`WM_CAPTURECHANGED` 兜底 |
| 3 | 平移和拖出同时发生 | 判断顺序错 | Ctrl+按下只建候选，不置 `dragging`；越阈值后直接 return |
| 4 | 点缩略图轻微移动就拖出 | 阈值太小 | 4 DIP×DPI，欧氏距离 |
| 5 | 拖出的是当前图而不是按下的缩略图 | 用了 `current` | 按下时快照路径/索引 |
| 6 | 目标收到“移动” | 给了 `DROPEFFECT_MOVE` | 只传 `DROPEFFECT_COPY`；结果里 MOVE-only 也降为 NONE |
| 7 | 拖出后源文件被删 | 目标执行 move | 不注册 `CFSTR_PREFERREDDROPEFFECT`，不实现 Move |
| 8 | 路径乱码/截断 | ANSI `DROPFILES` | `fWide=TRUE` + UTF-16 + 双 NUL |
| 9 | 长路径/UNC 失败 | 规范化掉了 `\\?\` | 原样传 `std::filesystem::path::wstring()` |
| 10 | Esc 不能取消 | 没判 `fEscape` | `QueryContinueDrag` 显式处理 |
| 11 | 拖出时动画/解码卡住 | 嵌套循环里做重活 | 拖出期间不发起解码 |
| 12 | 拖回自己又打开一次 | 自己的 drop target 接受 | `g_self_drag_path` 期间忽略同一路径 |
| 13 | 内存泄漏 | data/src 没 Release | `BeginFileDrag` 成对 Release |
| 14 | 高 DPI 阈值不对 | 固定 4 px | `kDragOutThresholdDip × dpi_scale` |
| 15 | 触摸/笔误触发 | 只处理鼠标 | `WM_POINTER` 不参与（P2） |
| 16 | 单测里 `GetData` 崩溃 | `HGLOBAL` 生命周期/对齐 | `GlobalAlloc(GHND)` + `GlobalLock`，测试后 `GlobalFree` |
| 17 | 松开后缩略图仍高亮 | 状态没清 | 拖出结束清 hover/tooltip |
| 18 | 无窗口命令缺 OLE | 提前返回 | 无头路径不初始化 OLE，拖出只在有窗口路径 |
| 19 | 缩略图点击变成拖出 | 按下即跳转/无阈值 | 点击延后到松开 + 4 DIP 阈值 |
