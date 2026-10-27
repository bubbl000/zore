# 图片信息面板与 EXIF（v2.0 S2 / `規劃/22/06`）

右下角非模态浮层：`I`、`Alt+Enter`、底栏 `ⓘ` 开关，`Esc` 关闭。
四组共 21 行（缺失值显示 `—`），EXIF 在后台 worker 读取，导航时面板保持打开并刷新。

## 1. 面板结构

```
┌──────────────────────────────┐
│ 图片信息                  ✕  │  标题 30 DIP + 关闭按钮 22 DIP
├──────────────────────────────┤
│ 文件                          │  分组标题（10 DIP，muted，semibold）
│ 名称        exif_full.jpg     │  行高 24 DIP；标签列 96 DIP，值单行省略
│ 路径        C:\…\exif_full.jpg│
│ 文件大小    84 KB             │
│ 修改时间    2026-10-24 06:25  │
│ 图像                          │
│ 格式 / 尺寸 / 位深 / 帧数      │  位深行 = "8 位 / 无|有"
│ 拍摄                          │
│ 相机 / 镜头 / 拍摄时间         │
│ 曝光 / 光圈 / ISO / 焦距       │
│ 曝光补偿 / 闪光 / 测光 / 白平衡 │
│ GPS                           │
│ 色彩                          │
│ ICC                           │
├──────────────────────────────┤
│                    [复制全部] │  页脚 44 DIP
└──────────────────────────────┘
```

- 宽度 360 DIP（窗口更窄时取客户区宽），高度 =
  `(30 + 44 + 2×12) + 行数×24`（DIP），上限为图像区高度的 **70%**；超出后
  body 滚动（3 DIP 细滚动条 + 部分可见行淡出）。
- 位置：右侧 16 DIP，底边 = BottomStack 栈顶 − 12（自动包含 pinned 缩略图条；
  全屏时即客户区底 − 16）。几何全部来自 `ui/layout.cpp: ComputeLayout` 与
  `ui/info_panel.cpp: ComputeInfoPanelLayout`，绘制与命中读同一份。
- 行顺序固定；缺失值保留行并显示 `—`（让用户知道"相机没写"而不是"软件没读"）。
- 路径行中间省略（保留盘符 + 文件名），复制/`复制全部` 永远给完整路径。

## 2. 数据来源

| 组 | 来源 | 时机 |
| --- | --- | --- |
| 文件（名称/路径/大小/修改时间） | `std::filesystem` + `GetFileAttributesEx` | 立即 |
| 图像（格式/尺寸/位深/Alpha/帧数） | 已解码的 `ImageInfo` + 扩展名 | 解码完成 |
| 拍摄（12 行） | `core/exif.*`（后台 worker） | 打开/切图后数十毫秒 |
| 色彩（ICC 描述 + 是否转 sRGB） | `ImageInfo.icc_profile` + `icc_applied` | 立即 |

EXIF 读取按容器分派（`core/exif.cpp: ReadExif`）：

- **JPEG**：扫 APP1 `Exif\0\0`，自己解析 TIFF IFD，不依赖 WIC 元数据查询。
- **PNG/APNG**：解析 `eXIf` chunk（同样自己解析）。
- **TIFF**：整个文件就是一个 TIFF 块。
- **HEIC/AVIF**：回退到 WIC `IWICMetadataQueryReader`（字段多少由系统 codec 决定）。
- **RAW**：`decoders/libraw_decoder.cpp: ReadExif`，只 `open`（不 unpack），
  取 `idata.make/model/software`、`lens.Lens`、`other.iso_speed/shutter/
  aperture/focal_len/timestamp`、`other.parsed_gps`。
- PSD/SVG/GIF/BMP/TGA/…：不读，整组 `—`。

> 为什么不用 WIC 读 JPEG/PNG：WIC 对嵌套 GPS IFD 的暴露不一致，且 rational 会以
> `VT_UI8`/`VT_VECTOR|VT_UI4`/`VT_R8` 多种形态返回。自研解析器只处理 EXIF 的
> 12 种 TIFF 类型，边界检查完整（目录条目上限 512、块上限 8 MB、文件上限 32 MB）。

## 3. 后台服务

`core/exif_service.*`：1 个低优先级 worker、`CoInitializeEx(MTA)`。

- `Request(path)`：key = **路径 + mtime**；已在显示/在途/排队则跳过；
  队列只保留最新一条（快速翻页不堆积，坑 19）；命中 8 条 LRU 时仍走
  `WM_APP+3` 消息，UI 只有一条应用路径。
- 完成消息携带 `ExifCompletion{path, key, data}`；UI 线程比对
  `completion->path == g_state->path` 才应用，否则丢弃（坑 3，快速翻页不串台）。
- 切图时 `g_exif_data` 立即清空、`g_info_scroll` 归零；面板打开时立即请求。
- 读取失败/无 EXIF：`exif: done ok=0`，不弹错误卡、不写 Toast（"没有 EXIF"不是错误）。

## 4. 格式化规则（固定，不跟系统区域）

| 字段 | 规则 | 示例 |
| --- | --- | --- |
| 曝光时间 | `<1` → `1/N 秒`（约分）；否则最多 2 位小数 | `1/125 秒` / `2.5 秒` |
| 光圈 | `f/` + 1 位小数 | `f/2.8` |
| ISO | `ISO <整数>` | `ISO 400` |
| 焦距 | 整数或 1 位小数 + `mm` | `50 mm` |
| 曝光补偿 | 带符号 1 位 + `EV`，0 为 `0 EV` | `+0.3 EV` |
| 闪光 | bit0 → 已闪光/未闪光；0xFFFF → `—` | `未闪光` |
| 测光 | 0 未知→`—`；1–6 映射 | `评价测光` |
| 白平衡 | 0 自动 / 1 手动 / 其他 `—` | `自动` |
| 色彩空间 | 1=`sRGB`, 0xFFFF=`未校准` | `sRGB` |
| 时间 | `YYYY-MM-DD HH:MM:SS`（`: `→`-`，`T`→空格） | `2024-08-12 14:33:05` |
| GPS | 十进制度 6 位；S/W 取负；海拔带符号 1 位 | `-33.860000, -151.210000` |
| 文本 | 首个 `\0` 截断，trim，最长 128 字符；UTF-8 → ACP 回退 | `Canon` |

时间/大小/单位全部用固定格式，不用 `GetDateFormatEx`，截图与断言不随机器语言变化。

## 5. 交互

| 操作 | 行为 |
| --- | --- |
| `I` / `Alt+Enter` / 底栏 `ⓘ` | 开/关 |
| `Esc` | 关（`OverlayStack` 优先级：设置→模态→popup→info→错误卡→全屏） |
| 滚轮（指针在面板上） | 滚动 body，**不缩放图片**；面板不可滚时也消费掉滚轮 |
| 左键（面板内） | 消费，不开始拖动、不切换图片 |
| 右键某行 | 复制该行的值 + Toast「已复制」 |
| `复制全部` | `label: value`，每行一条，跳过分组标题与缺失值 + Toast「已复制全部信息」 |
| 面板外点击 | **不关闭**（方便边看边翻） |
| `←/→` 等 | 面板不抢焦点，照常切图并刷新 |

`Ctrl+C` 仍是"复制图像"（面板不设焦点，避免与既有快捷键冲突）；复制信息用右键行
或页脚按钮。

## 6. 测试钩子

- `--info-scroll <notches>`：面板打开后滚动 N 格（每格 3 行）；`--layout-dump`
  与 `--screenshot` 都能看到滚动后的状态（一次生效）。
- `--click-hit info.close` / `info.copy_all` / `info.row.N`：按布局矩形点击。
- `--click-hit info.copy_row.N`：向该行中心发真实 `WM_CONTEXTMENU`，走右键复制路径。
- `--layout-dump` 新增：
  `info_open`、`info_rows`（行数，含分组标题）、`info_exif_ok`、`info_scroll`、
  `info_scroll_max`、`info_scrollable`、`info_first_visible`、`info_last_visible`、
  `info_line_N`（`h:组名` 或 `标签|值`）、`info_header/close/body/footer/copy_all`
  矩形，以及 `info_name/path/size/format/dimension/depth/frames/camera/lens/taken/
  exposure/aperture/iso/focal/bias/flash/metering/whitebalance/gps/icc`。
- `zoreview_cli --exif <file>`：打印全部 21 个字段（`exif_<field>: value`），
  `exif: yes|no`；无 EXIF 时退出码 3。用于样本/格式化核对。

## 7. 日志

```
exif: request queued
exif: done ok=1 fields=20
exif: stale result dropped
ui: info=1 / ui: info=0 via close
ui: info_copy=row 16 / ui: info_copy=all lines=20
test: info_scroll=3
```

## 8. 坑（实现中实际踩到/防住的）

1. **Pillow 把 rational 写成 SHORT**：`(1,125)` 元组会被写成 type 3 count 2，
   解析器只按 RATIONAL 解析会全空。样本生成必须用 `IFDRational`；像素尺寸同理
   （规范是 LONG，Pillow 写 SHORT），解析器两种都接受。
2. **RAW 的大写路径 + PowerShell `Start-Process -ArgumentList`**：数组元素按空格
   拼接、不自动加引号，`EOS 5D Mark IV` 会被截断成 `EOS`，表现为"文件不存在"。
   脚本里所有含空格路径都要自己加引号。
3. **`--layout-dump` 会先退出进程**：带 `--send-keys` 时 dump 在最后一个键后
   800 ms 触发并 `PostQuitMessage`，比 `--screenshot` 的 2600 ms 更早，截图永远
   不会发生。截图用例不要同时传 `--layout-dump`。
4. **点击钩子与按键钩子的时序**：`--click-hit` 原先固定 1.2 s，和 `--send-keys`
   同时使用时会在按键之前落下（面板还不存在）。现在点击延迟会加上按键耗时。
5. **面板行可能在可视区外**：`info.copy_row.N` 必须先用 `--info-scroll` 把该行
   滚进 body，否则 `InfoLineAt` 正确地返回 −1（右键会落到应用菜单）。
6. **EXIF 与缓存无关**：磁盘/内存缓存命中时也要独立读 EXIF（缓存只存像素），
   所以请求挂在 `LoadPath` 入口，而不是解码完成回调。
7. **绝对路径**：`--layout-dump`/面板显示使用 `std::filesystem::absolute`，
   否则相对路径命令行会在面板里显示 `..\…`。
8. **快速翻页**：完成消息必须校验路径；队列"最新一条"策略 + path+mtime key，
   连续翻 4 张不会把上一张的相机名留在屏幕上（脚本 `nav1/nav2/nav3` 覆盖）。
9. **70% 上限 + 滚动**：25 行在 125% DPI 下只能显示约 9 行，其余必须滚动；
   行高不能被"部分可见行"拉低（统一 24 DIP，裁切在 body 上做）。
10. **高对比度**：面板颜色全部来自 `ColorTokens`（HC 下是系统色），
    复制按钮用系统高亮色，不能硬编码 accent。

## 9. 验收

`tests/run_info_panel_check.ps1`（129 项）：

- 25 行固定顺序、四组标题、面板/关闭/复制全部/body/footer 几何、70% 上限；
- `exif_full` 全字段文本（相机/镜头/时间/曝光/光圈/ISO/焦距/补偿/闪光/测光/白平衡/GPS）；
- `exif_partial` 缺失字段 `—`、行数不变；`exif_none` 无 EXIF；`exif_png` eXIf；
  `exif_south` 南纬/西经为负；
- 连续导航不串台（Canon → Nikon → FUJIFILM → 无 EXIF）；
- 面板打开时 `→` 仍导航、行左键不导航、滚轮偏移与钳制；
- 复制全部（剪贴板逐行、跳过标题/缺失值）与右键单行复制（剪贴板精确值）；
- 关闭按钮 / `Esc` / 第二次 `Esc`；100/125/150/200% 四档 DPI；中文标签；高对比度；
- 全屏；RAW（CR2）经 LibRaw 的相机/ISO/快门/光圈；
- 截图：`tests/output/info-panel/panel-{en,zh,hc,partial}.png`。

相关回归：`zoreview_tests`（EXIF 格式化 + TIFF 解析 + 面板布局，共 655 项）、
`run_ui_chrome_check` 90/90、`run_navigation_check` 68/68、`run_empty_state_check`
123/123、`run_error_ui_check` 131/131、`run_rotation_check` 80/80、
`run_ui_shell_check` 41/41。
