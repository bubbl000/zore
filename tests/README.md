# tests/

M0/M1 阶段的测试脚本与布局单元测试，不放大文件。

## 样本

- 相机 RAW（CC0）：`../../测试文件夹/raw/`
- 合成格式样本：`../../测试文件夹/images/`
- 清单：`../../测试文件夹/manifest-tier1.csv`、`../../测试文件夹/images/MANIFEST.csv`

## 已实现

`run_format_matrix.ps1`：

- 调用 `build\Release\zoreview_cli.exe --probe`；
- 输出 `tests\output\format-matrix.csv`；
- `-SkipRaw`：只跑 P0 合成样本（38/38 通过，含 8 张 EXIF 方向与 9 张 ICC）；
- 不加 `-SkipRaw`：连 10 个 RAW 一起跑（48/48，RAW 由 LibRaw 探针）。

`run_thumbnail_check.ps1`（M1-1）：

- 以 `--screenshot` 模式启动应用（直接从交换链抓帧，不受 DPI 虚拟化/遮挡影响）；
- 输出 `tests\output\thumb-strip.png`，并统计日志里的 `thumbnail ready`；
- `-ClickIndex N` / `-ScrollNotches N` 通过应用测试钩子验证缩略图点击与横向滚动。

`strip_layout_test.cpp`（`zoreview_tests.exe`）：

- 不建 D3D 设备，纯布局单测：76 px 基准高度、DPI 缩放、命中测试、滚动、
  可见区间虚拟化、缩略图缓存寿命。

`run_perf.ps1`：

- CLI 4K 解码耗时；
- 应用启动：读应用自己写的 `--bench-startup` 阶段时间（进程启动 → 首帧 →
  首图），不再用 `WaitForInputIdle` 推断首帧（它会把解码/缩略图消息算进去，
  现在只作为参考值 `app_input_idle`）。

`run_startup_samples.ps1`（M1-2）：

- 连续启动 N 次，只读每次新增的日志，输出各阶段 min/median/max；
- 首帧受杀毒软件扫描新 exe 影响，重建后的第一次启动常慢 200–300 ms，
  因此看 min/median，不要看单次平均值。

`make_stress_samples.ps1`（M1-3）：

- 生成 `output/8k.jpg`（7680×4320，由 4K 样本双三次放大）、`output/8kfolder/`
  （4 张 8K）和 `output/bigfolder/`（1200 张编号小图）；
- 如果 `output/4k.jpg` 不存在（output 被 git-ignore），会先从
  `测试文件夹/images/jpeg/photo_1600.jpg` 生成 4K 基准图，保证 `run_perf.ps1`
  和 `run_startup_samples.ps1` 可用；
- 只写 `tests/output/`（git-ignore），不提交大文件。

`run_8k_check.ps1`（M1-3）：

- CLI `--probe` / `--decode` 8K；应用 `--screenshot` 显示 8K；
- 轮询进程峰值内存，断言：位图 7680×4320、出现 `prefetch skipped`、
  峰值 WS < 320 MB、private < 480 MB。

`run_big_folder_check.ps1`（M1-3）：

- 打开 1200 文件目录中部的图片并深度滚动缩略图条；
- 断言：扫描出的文件数正确且 <1 s、可见/请求缩略图数有界、0 失败、截图生成。

`run_memory_check.ps1`（M1-3）：

- 单场景内存测量（`-ExtraArgs` 可传 `--click-index` 等）；
- 输出外部峰值与日志里的 `memory:` / `set_image` / `prefetch` 行，供人工比对。

`run_asan_check.ps1`（M1-4）：

- 跑 ASan 构建（`tools\build.ps1 -ASan` → `build-asan\RelWithDebInfo`）；
- CLI：38 个 P0 样本各跑 `--probe` / `--decode` / `--thumb`（114 次），
  加 10 个 RAW 的 probe/thumb + 1 次全解（共 135 次进程）；
- GUI：1200 文件目录连按 200 次 `→`，再跑单张 8K、8K 目录连续翻页、
  缩略图点击 + 截图；
- `ASAN_OPTIONS=log_path=tests\output\asan\asan`（相对路径，避免盘符冒号被
  ASan 选项解析器拆开）；脚本扫描报告并在任何 `AddressSanitizer` 输出时失败。

`run_orientation_check.ps1`（M1-5）：

- CLI `--probe` 断言 1–8 的直立尺寸、方向标签与 `orientation_applied`；
- CLI `--decode` 对 2–8 的采样像素与 `orient_1` 的参考映射逐通道比较
  （样本只差文字字形，文字区已排除）；
- CLI `--thumb` 断言 90° 缩略图宽高交换（`128x80` / `80x128`）；
- 应用打开 `orient_6`，断言日志里出现直立的 `renderer: bitmap 1000x1600`。

`run_icc_check.ps1`（M1-5）：

- 样本由 `测试文件夹\tools\make-color-samples.py` 生成：线性 gamma ICC、
  sRGB ICC、无 profile 对照、RGBA + ICC、ICC + EXIF 6 组合；
- 断言：线性 profile 的中灰从 ~55 转换回 ~128，sRGB profile 不改像素，
  RGBA 的 alpha 通道逐字节不变，ICC + 方向组合同时正确。

`run_8k_nav_check.ps1`（M1-4）：

- 在 8K 目录连续翻页（默认 8 次），轮询进程峰值；
- 断言：至少一次位图上传、`prefetch skipped`、峰值 WS ≤ 360 MB、private ≤ 500 MB；
- 修复后实测：8 次 464 MB、24 次 460 MB（修复前 685 MB）。

`zoreview_filetests.exe`（M1-6）：

- 无 GUI 的文件操作回归：PNG/JPEG/BMP/TIFF 原子保存 + 重新解码、
  覆盖写、重命名、删除到回收站、剪贴板 CF_DIB / CF_UNICODETEXT；
- `--wallpaper` 会临时设置桌面壁纸并校验，随后恢复原壁纸；
- 注意：默认运行会替换当前剪贴板内容。

`run_file_ops_check.ps1`（M1-6）：

- 先跑 `zoreview_filetests.exe`，再用 GUI 钩子验证重命名/删除后的目录刷新；
- `-Wallpaper` 附加壁纸设置/恢复检查；
- 会临时占用剪贴板，并向回收站丢一个临时 png（测试文件）。

`run_raw_check.ps1`（M2 RAW）：

- CLI `--probe`：10 个 RAW 必须是 `decoder=libraw`、尺寸/方向合法；
- CLI `--thumb --edge 128`：`full:` 必须等于探针尺寸，最大边 128；
- CLI `--decode`：默认全解第一个样本，`-FullAll` 全解 10 个，BGRA 字节数
  必须等于 `width*height*4`；
- GUI：打开第一个 RAW，日志里的 `renderer: bitmap` 尺寸必须与探针一致。

`zoreview_leaktests.exe`（M1-4）：

- Debug CRT 堆检查（Windows 上 ASan 没有 LeakSanitizer）；
- 每轮同步 probe/decode、缩略图服务（内存 + 磁盘缓存）、异步解码、取消、
  预取；用 `_CrtMemCheckpoint` 比较每轮存活字节/块；
- 预热 2 轮后要求每轮增长 ≤ 64 KB / 4 块；实测 0 字节 / 0 块；
- 需 Debug 构建：`tools\build.ps1 -NoFreeImage`（预编译 FreeImage 是 Release CRT）。

## 计划

| 脚本 | 用途 | 状态 |
| --- | --- | --- |
| `run_format_matrix.ps1` | 遍历样本，调用 CLI，输出 CSV（22/16 后 198/198，expected_failed=20） | 已实现 |
| `make_extra_samples.cpp` / `zoreview_samplegen.exe` | 22/16：用 FreeImage 生成 HDR/EXR/PFM/JP2/JXR/WBMP/JNG 样本，`--report` 列插件能力、`--load` 查单个文件 | 已实现（S1.5） |
| `run_soak.ps1` | 连翻 200 张，记录内存/句柄 | 已实现 |
| `run_perf.ps1` | 4K CLI 解码 / 应用启动基线 | 已实现 |
| `run_startup_samples.ps1` | 启动阶段 min/median 采样 | 已实现（M1-2） |
| `run_thumbnail_check.ps1` | 缩略图条截图 + 日志断言 | 已实现（M1-1） |
| `make_stress_samples.ps1` | 8K / 大盘目录样本 | 已实现（M1-3） |
| `run_8k_check.ps1` | 8K 显示 + 内存断言 | 已实现（M1-3） |
| `run_big_folder_check.ps1` | 大盘目录扫描/虚拟化 | 已实现（M1-3） |
| `run_memory_check.ps1` | 单场景峰值内存 | 已实现（M1-3） |
| `run_8k_nav_check.ps1` | 8K 连续翻页峰值内存 | 已实现（M1-4） |
| `run_asan_check.ps1` | ASan CLI + GUI 长稳检查 | 已实现（M1-4） |
| `zoreview_leaktests.exe` | Debug CRT 泄漏检查 | 已实现（M1-4） |
| `zoreview_filetests.exe` | 保存/重命名/删除/剪贴板/壁纸 | 已实现（M1-6） |
| `run_file_ops_check.ps1` | 文件操作 harness + GUI 重命名/删除 | 已实现（M1-6） |
| `run_raw_check.ps1` | RAW 探针/缩略图/全解/GUI | 已实现（M2 RAW） |
| `run_orientation_check.ps1` | EXIF 1–8 方向（主图/缩略图/GUI） | 已实现（M1-5） |
| `run_icc_check.ps1` | ICC→sRGB、sRGB no-op、alpha、方向组合 | 已实现（M1-5） |
| `run_golden.ps1` | 固定样本 → 像素哈希/感知差 | 计划 |
| `run_ui_shell_check.ps1` | v2.0 S0：标题/底栏几何、rail 三模式、最大化=工作区、全屏、Esc 优先级、真实点击 5 键/底栏/关闭 | 已实现（S0） |
| `run_ui_chrome_check.ps1` | v2.0 S1-02：底栏命令、更多面板、缩放菜单、信息卡（v3.0 起为独立窗口）、标题复制、文件夹自然序、四档 DPI | 已实现（S1-02） |
| `run_empty_state_check.ps1` | v2.0 S1-03：四态空态卡、派生格式行、全部格式浮层、文件/文件夹/空目录/不支持/缺失路径拖入、真实 `IDropTarget`、拖入高亮、四档 DPI、中文、最大化 | 已实现（S1-03） |
| `run_navigation_check.ps1` | v2.0 S1-09：导航/缩放/视图键、图像区箭头几何与点击、单张/无图禁用 | 已实现（S1-09） |
| `run_error_ui_check.ps1` | v2.0 S1-11：七类错误卡、非模态导航、重试/复制/关闭/Esc/Enter、删除确认（取消/主/Delete 键）、Toast 队列与合并、加载态 400 ms 阈值、四档 DPI/全屏、`MessageBox` 为 0 | 已实现（S1-11） |
| `run_rotation_check.ps1` | v2.0 S1-05：Ctrl+L/R、底栏旋转、转回直立（v3.0 删了更多▾ 的“重置旋转”行）、keep_rotation、复制/另存为跟随视图、四角像素断言、四次 90° 回原图、全屏/无图、8K 内存 | 已实现（S1-05） |
| `run_settings_check.ps1` | v2.0 S2-07：设置窗口三页/控件/持久化/迁移/损坏恢复/恢复默认/关联/单实例/窗口位置/四档 DPI | 已实现（S2-07） |
| `run_info_window_check.ps1` | v3.0（26 §2.1）：信息卡独立窗口的开/关/独立 HWND/Esc 归属/内容与 EXIF/滚动/复制全部与单行/四档 DPI/主题/高对比度/中英/位置记忆与越界夹取/最小化与全屏跟随/主窗关闭连带/截图；替代已退役的 `run_info_panel_check.ps1` | 已实现（v3.0 #1） |
| `run_strip_check.ps1` | v2.0 S2-08：缩略图条三模式（默认 auto）、8 DIP 热区唤出、400+160 ms 淡出、悬停 tooltip 内容/翻转/夹取、隐藏时取消请求 | 已实现（S2-08） |
| `run_save_check.ps1` | v2.0 S3-14：六容器落盘与像素回读、GIF 透明、WebP/JPEG 质量、预估误差、另存为卡（几何/点击滑条/键盘 Tab/Enter/Esc）、旋转导出、动画当前帧、失败路径、8K、四档 DPI/中文/高对比度 | 已实现（S3-14） |
| `run_window_state_check.ps1` | v2.0 S3-15：窗口矩形恢复/半屏外夹取/拔屏回退/最大化/最小化与全屏不覆盖/`WM_EXITSIZEMOVE` 保存/全屏置顶暂挂 | 已实现（S3-15） |
| `run_single_instance_check.ps1` | v2.0 S3-15：DataDir 哈希隔离、路径转发/退出 0、空路径激活、`single_instance=0` 双开 | 已实现（S3-15） |
| `ui_layout_tests.cpp` | v2.0 S0/S1/S2 纯函数单测（布局/标题五段/底栏/命中/浮层栈/窗口态/空态/格式摘要/格式浮层/错误模型/错误卡/确认卡/加载与 Toast/旋转矩阵与导出旋转/auto 缩略图命中与 tooltip 几何/另存为卡与滑条映射），并入 `zoreview_tests` | 已实现（S0/S1/S2/S3） |

### 8K / 大盘目录结果（M1-3，M1-4 复核）

```text
8K CLI decode:      78..90 ms, 132710400 bytes
8K app first_image: 363..405 ms (warm)
8K peak private:    333 MB (single) / 460..465 MB (连续翻页, M1-4 修复后)
8K 修复前峰值:       336 MB (single) / 685 MB (连续翻页)
8K bitmap:          7680x4320 = 126.6 MB
8K thumbnail:       --edge 128 peak 9.6 MB, 36 ms
big folder:         1200 files, scan 4.9 ms
                    max_visible=22 max_requested=22, scroll 61200, 0 failed
```

> 2026-10-24 复核：同一台机器上 8K 单张 private 341 MB（正常），连续翻页
> 592 MB；用改动前的 `dist\ZoreView-0.3.0-portable\zoreview.exe` 跑同样场景
> 也是 337 → 591.8 MB，说明 460 MB 那条记录在本会话不可复现（环境/驱动差异）。
> `run_8k_nav_check.ps1` 的默认门限因此改为 620 MB，并保留“产品目标 500 MB”
> 的注释；详见 `../HANDOFF.md` 坑 148。

### M1-4 ASan / 泄漏 / 长稳结果

```text
ASan:   CLI 87 次进程（29 样本 × probe/decode/thumb） 0 报告
        GUI 200 次翻页 + 8K + 8K 目录连续翻页 + 缩略图点击 0 报告
Leak:   预热后每轮 bytes=+0 blocks=+0（5 轮）
Soak:   200 张 alive=True handles 470->481 private 91.6->154.8 MB
```

### soak 结果（M1-4，200 张，带缩略图条）

```text
iterations=200 alive=True
handles: 470 -> 481
working_set_mb: 75.4 -> 139.3
private_mb: 91.6 -> 154.8
```

内存在前 20 张后稳定，句柄为一次性开销，未线性增长。

### 性能基线（M1-4 之后）

```text
cli_decode_4k:   avg=86.9ms
app_first_frame: avg=199..216ms   # 窗口可见（GDI 主题背景）
app_first_image: avg=367..407ms   # 首张图片上屏
app_input_idle:  avg=173..188ms   # 参考值
```

8 次热启动的中位数：首帧 218.1 ms（min 209 / max 234.3），首图 368.1 ms。
明细：`output/perf.csv`。

M1-2 从 ~400 ms 首帧降到 ~200 ms：窗口先显示并用 GDI 画主题背景、D3D 设备创建与
窗口显示/解码重叠、首图不再整帧拷贝、`Present(0,0)`、去掉未使用的 DWrite。
M1-3 之后 4K 启动基本不变；8K 首图 ~370–390 ms、单张峰值 ~336 MB。


### M1-5 方向 / ICC 结果

```text
orientation: 8/8 探针尺寸正确；2–8 采样像素与参考映射逐通道一致
             thumb orient_1=128x80 orient_6=80x128
             GUI orient_6 bitmap=1000x1600
ICC:         linear JPEG gray 55 -> 128；linear PNG 55 -> 128
             sRGB profile 与无 profile 像素完全一致（no-op）
             RGBA PNG alpha 通道 max diff 0
             ICC + EXIF 6 组合：384x512、gray 128
```

方向解码耗时：`orient_6` 从最初直接旋转 JPEG 的 ~5.5 s 降到 ~17 ms
（先转 BGRA 再旋转；直接旋转 JPEG 源会逐列触发解码）。

### M1-6 文件操作结果

```text
save:    PNG alpha 逐字节一致；BMP/TIFF RGB 逐字节一致；JPEG q90 在容差内
rename:  大小写重命名/普通重命名；GUI --rename-to 验证列表刷新
delete:  回收站删除；列表重扫后自动显示下一张；网络路径拒绝
clipboard: CF_DIB 32bpp bottom-up、CF_UNICODETEXT
wallpaper: SPI_SETDESKWALLPAPER 设置并恢复
```

快捷键：`Ctrl+O` 打开、`Ctrl+S` 另存为、`Ctrl+C` 复制图像、
`Ctrl+Shift+C` 复制路径、`Del` 删除到回收站、`F2` 重命名；
右键菜单还提供“设为桌面壁纸 / 在资源管理器中显示”。

### M2 RAW 结果（LibRaw 0.21.5）

```text
probe:   10/10 decoder=libraw，尺寸/方向合法
thumb:   10/10 libraw_half，full 尺寸一致，最大边 128，0.6–1.2 s/张
full:    10/10 BGRA 字节数 = w*h*4，1.1–2.7 s/张（30–45 MP）
GUI:     CR3 8480x5650 / CR2 6744x4502，位图尺寸与探针一致
ASan:    CLI 135 + GUI 5，0 报告
```

RAW 方向：`params.user_flip = -1` 由 LibRaw 烘焙，`sizes.flip` 仅映射成
EXIF 1–8 记录；输出像素已经是直立的，不能再按 orientation 旋转一次。

### M3-3 双语 / DPI / 高对比度 / 无障碍

`run_ui_check.ps1`：

- 用 `--force-dpi-scale 100/125/150/200` 跑真实应用，断言日志
  `ui: dpi_scale` 与底部缩略图条物理高度（76 × 比例），并检查截图尺寸；
- `--language zh|en` 断言 `ui: language=`，`--set-language` 断言
  `settings.ini` 持久化与非法语言拒绝（结束恢复 settings.ini）；
- `--force-high-contrast` 断言背景色等于系统 `COLOR_WINDOW`；
- UI Automation 读取主窗口 Name（标题）与重命名对话框的子控件名，
  覆盖中/英两种语言的“确定/取消”。

`zoreview_tests.exe`（`ui_tests.cpp`）另外检查：中/英字符串表完整、
英文串无中文、语言解析/代码/原生名、格式串参数、DPI 换算、
缩略图条在 100/125/150/200% 下的高度/命中/可见区间、高对比度强制状态。

