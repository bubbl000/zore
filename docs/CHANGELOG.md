# 更新日志

版本号格式：`MAJOR.MINOR.PATCH`，里程碑记录在内部交接文档（不随仓库发布）。发布包版本取自
`src/core/version_numbers.h`。

## 未发布 — v2.0 S3（2026-10-25，拖出到其他程序）

- 拖出（`22/19`）：**Ctrl+拖图片**导出当前文件；**拖缩略图第 N 项**导出那一项
  （不需要 Ctrl）。数据对象提供 `CF_HDROP`（宽字符 + 双 NUL）与 `CF_UNICODETEXT`，
  拖放源只允许 `DROPEFFECT_COPY`，`Esc` 取消，4 DIP 阈值区分点击与拖出。
- 缩略图点击改为松开时跳转（否则无法区分拖动）；拖回自己的窗口不会重复打开；
  Ctrl+单击首次显示一次提示。
- 新增 `zoreview_dragtests`（49 项）与 `tests/run_drag_out_check.ps1`（17 项）、
  `docs/DRAG-OUT.md`；`--simulate-drag-out` / `--simulate-strip-drag` 干跑钩子。

## 未发布 — v2.0 S3（2026-10-25，缩放画质）

- 插值状态机（`22/18`）：静止用 `HIGH_QUALITY_CUBIC`，平移/缩放/拖窗口/动画播放
  用 `LINEAR`，交互结束 150 ms 后自动升级；`Renderer::MarkInteractive()` +
  窗口一次性定时器。
- 像素模式收窄为 **≥100% 才最近邻**；缩小仍 CUBIC，避免锯齿。
- 显示代理：8K 等超大图缩到 <50% 时烘一张 **1/4** GPU 代理（7.9 MB / 7.5 ms），
  只在交互/动画帧替换采样源；静态帧仍直接 CUBIC（实测代理二次重采样更差）。
- 截图固定“静态质量”（像素模式仍 NEAREST），保证截图与屏幕一致；
  新增 `--force-quality fast|good|pixel|auto`、`--proxy on|off` 与
  `quality*/proxy*` dump 字段。
- 测试：新增 `tests/run_quality_check.ps1`（57 项）、
  `tests/tools/quality_compare.py`、`tests/tools/make_quality_sample.py`；
  单测 1034 → **1057**。8K 内存 private 363 MB / ws 214 MB（含代理）。
- 测试钩子修正：`--send-keys` 期间只用模拟修饰键，用户在旁边按 Ctrl/Shift
  不会再让 `+` 变成 Ctrl+plus 而静默失效。

## 未发布 — v2.0 S1.5（2026-10-25，格式路由补齐）

- 新增 **HDR / EXR / PFM / WBMP / JP2 / J2K / JPC / JXR / WDP / MNG / JNG**：
  `ImageFormat`、magic（EXR→HDR→PFM→JP2→J2K→JXR→MNG）、扩展名表、`FormatName`
  与 CLI 全部接通；支持扩展名 41 → **52**，文档扩展名 39 → **50**。
- HDR/EXR/PFM：FreeImage 解码 + 固定 Reinhard（`L/(1+L)`）+ gamma 2.2；
  修正 RGBF 扫描线的 BGR 顺序；解码前解析头并限制 200 MP / 1.5 GB，
  截断 EXR/HDR 在进入插件前返回 `invalid ... header`。
- JP2/J2K/JPC、JXR/WDP：WIC 优先、FreeImage 兜底；`.wdp` 可开，WIC 路径保留
  JXR alpha；WIC/FreeImage 都失败时返回实际原因（不再误报 HEIC codec）。
- WBMP 按扩展名路由；MNG/JNG 只显示首帧并强制 `animated=false`。
- 空态格式行出现 HDR/EXR、总数 52；“全部格式”新增 JP2/JXR/WBMP/MNG。
- 样本与参考图：`测试文件夹/images/extra/`（含 `SOURCES.md`）、
  `zoreview_samplegen`、`测试文件夹/tools/make-extra-samples.py`；
  8 个截断损坏样本加入 `images/corrupt/`。
- 测试：格式矩阵 175 → **198/198**（expected_failed=20）、
  `zoreview_formattests` 0 failures、单测 1004 → **1034**。
- 文档：`HELP.md` 格式表、`EMPTY-STATE.md` 能力表/计数。

## 未发布 — v2.0 S3（2026-10-25，窗口状态与单实例）

- 窗口状态（`22/15`）：拖动/缩放结束（`WM_EXITSIZEMOVE`）与关闭时保存
  `GetWindowPlacement().rcNormalPosition`；最小化/全屏不覆盖；恢复前要求与任一
  显示器工作区交集 ≥100×100，再按当前 DPI 夹到该工作区（最小 640×480 DIP），
  否则主屏居中；负坐标副屏、拔屏、DPI 变化均有回退。
- 置顶：全屏期间临时取消，退出全屏按设置恢复；`always_on_top` 值不变。
- 单实例：互斥体与 message-only 接收窗口名带 `DataDir` FNV1a 哈希，便携版/安装版
  隔离；接收窗口早于主窗口创建，启动竞态不丢请求；`WM_COPYDATA` 带 `'NV1'`
  魔数/版本/路径，`SendMessageTimeout` 2 s，失败照常新开窗口；空路径只激活。
- 磁盘图片/缩略图缓存改为 `tmp-<pid>-<tick>-<seq>` + `FlushFileBuffers` +
  `MoveFileEx(REPLACE|WRITE_THROUGH)`；坏文件读取时删除当未命中；trim 跳过
  `*.tmp-*` 并只清理 1 小时以上的残留。
- 新增钩子 `--move-window x,y,w,h`；`--layout-dump` 增
  `window_rect`/`work_areas`；`--print-settings` 已有窗口字段沿用。
- 测试：新增 `tests/run_window_state_check.ps1`（**21 项**）、
  `tests/run_single_instance_check.ps1`（**19 项**），单测 953 → **1004 项**。
- 文档：`docs/SETTINGS.md` §3.3 重写。

## 未发布 — v2.0 S3（2026-10-24，另存为增强）

- 另存为（`22/14`）支持 **PNG / JPEG / BMP / TIFF / WebP / GIF** 六种容器；
  启动时用 2×2 图像真实试编码一次（`save: encoders=...`），不可用的编码器
  不出现在系统保存框里。
- 质量：JPEG 走 WIC `ImageQuality`，WebP 走 FreeImage 内静态链接的 libwebp
  简单 API（质量 1–100 真正生效）；默认 90。
- GIF：自研 GIF89a 写入（Wu 量化到 255 色 + 1-bit 透明 + 自写 LZW），
  半透明按 alpha 128 阈值。
- 选项浮层：360 DIP 模态卡，质量滑条 + 实时预估大小；`Enter` 保存、`Esc` 取消、
  `Tab` 循环、`←/→` ±1、`Shift` ±10；鼠标点击/拖动滑条按位置吸附。
- 预估：同一编码器/像素/质量编码到内存，250 ms debounce，>32 MP 不预估，
  实测与文件大小误差 <1%（验收要求 <10%）。
- 保存后台化（`SaveService`）：8K 不卡 UI；原子写保持 `.nvtmp` + `MoveFileEx`，
  失败删临时文件并弹错误卡，原文件不受影响。
- 导出遵循当前视图旋转，动画只保存当前显示帧（Toast「已保存当前帧」）。
- 新增钩子 `--save-to <file> [--save-quality N]`、`--save-options <file>`、
  `--click-hit save.quality[.N]|save.primary|save.cancel`；
  `--layout-dump` 增 `save_*` 状态与 9 个矩形。
- 测试：`tests/run_save_check.ps1`（**105 项**）、
  `zoreview_filetests` 六格式/预估用例、单测 915 → **953 项**；
  截图 `tests/output/save/`（四档 DPI、中文、高对比度）。
- 文档：`docs/SAVE-AS.md`；`HELP.md` 补质量/预估/格式说明。

## 未发布 — v2.0 S3（2026-10-24，两段式加载）

- 两段式加载（`22/13`）：RAW / PSD/PSB / ≥32 MB 的文件先显示预览，再替换为全解
  清晰图；普通 JPEG/PNG 不预览（避免“糊一下”）。
- 预览来源：缩略图磁盘缓存（`ThumbnailService::TryLoadPreview`，UI 线程 ~1 ms）
  + RAW 相机内嵌 JPEG（`libraw_unpack_thumb` → WIC 内存解码，本机 CR2 约 125 ms）；
  只有 128 px 缓存时，RAW 会用内嵌 JPEG 自动升级，并保持当前视图。
- 独立预览线程/队列（`DecodeService::RequestPreview` + `WM_APP+4`），不排在两个
  全解 worker 或相邻预取之后；RAW 相邻预取被跳过（LibRaw 全局锁会拖慢预览）。
- 右上角「预览」胶囊（22 DIP、12 DIP 边距、不参与命中），全解后 160 ms 淡出；
  失败时保留预览 + 红字「预览（加载失败）」+ 错误卡；只有预览时另存为/复制/
  壁纸被拒绝并 Toast 提示。
- 替换保持视图：`Renderer::SetImage(..., preserve_view)` 按内容中心下的图像相对
  坐标重算 offset，fit/zoom/pan/旋转都不跳。
- 启动首图在 D3D 设备前打开时，预览先缓存、renderer 就绪后立即上屏。
- 新增钩子 `--force-preview-fail`、`--preview-zoom`、`--send-keys-at`；
  `--layout-dump` 增 `preview_*/preview_badge/view_center/content_center/
  image_width/image_height`。
- 测试：`tests/run_two_stage_check.ps1`（54 项），单测 893 → **915 项**；
  截图 `tests/output/two-stage/`（预览 badge / 清晰图 / 失败态）。
- 文档：`docs/TWO-STAGE.md`；`HELP.md` 补预览说明。

## 未发布 — v2.0 S3（2026-10-24，动画面板与帧步进）

- 动画帧控件（`22/12`）：GIF / APNG / WebP 动画在底栏上方显示
  `◀ n/N · 状态 ▶ ⏸` 胶囊（248×36 DIP，水平居中，pinned 缩略图条出现时上移）。
  左右按钮逐帧、中间按钮播放/暂停（`Space` 同效），`←/→` 仍然切图。
- 播放状态机（`core/animation_controller.*`）：自动播放、暂停、有限循环播完
  **停在最后一帧**并显示“已结束”，再按播放从第一帧重播；2 秒无鼠标移动控件
  淡到 35%，移动立即恢复；高对比度下不淡出。
- WebP 动画：解析 RIFF `VP8X/ANIM/ANMF`（画布/循环/时长/dispose/blend），
  WIC 逐帧解码后合成；上限 512 帧 / 128 MB，超出显示“已截断”；解码器不可用时
  回退首帧并显示“仅首帧”（不提供无效按钮）。
- 最小化自动暂停；恢复时只继续之前正在播放的动画，手动暂停保持暂停。
- 帧更新不重置视图：旋转、缩放、平移、fit 在全屏/信息面板下保持不变。
- 新增 `--restore-at`、`--force-anim first|paused|ended|truncated`、
  `--click-hit anim.prev|next|toggle`；`--send-keys` 支持 `comma`/`period`；
  `--layout-dump` 增加 `anim_*` 状态与 5 个矩形。
- 测试：`tests/run_animation_check.ps1` **96 项**；单测 859 → **893 项**；
  `zoreview_formattests` 增加 WebP 4 帧逐像素 `max_diff=0`（90 项）。
- 文档：`docs/ANIMATION.md`；`HELP.md` 补动画控件、逐帧与 WebP 说明。

## 未发布 — v2.0 S2（2026-10-24，目录监视与文件夹穿透）

- 目录监视（`22/10`）：前台每 2 秒读一次目录快照（目录 mtime + 当前文件
  size/mtime）；外部新增/删除/重命名约 2 秒内刷新列表，当前图按路径保留；
  当前文件被删自动切下一张，删空回到空态；内容被修改只提示“文件已修改，
  按 F5 重新加载”，不自动重载。
- 网络盘只信目录 mtime、不做计数；本地盘每 5 次采样一次计数兜底。
  连续重扫最小间隔 2 秒（防抖窗口内的事件下一轮重试，不丢弃）。
- 自己的删除/重命名/另存为使用 2.5 秒抑制窗口并立即重建基线，不会闪两次；
  另存到当前目录会立刻刷新列表。
- 最小化 / 窗口不可见 / 设置窗口 / 确认浮层打开时暂停监视，全屏继续；
  日志只写 `watch: paused` / `watch: resumed`。
- 文件夹穿透三策略：`loop`（默认）/ `next`（进入相邻含图文件夹，空目录跳过，
  最后一个时回 loop）/ `ask`（询问浮层）。兄弟目录按 `StrCmpLogicalW`
  自然排序、只查同级、上限 200、缓存 2 秒；单张目录在 next/ask 下也可跨目录。
- `ask` 浮层复用确认卡：非危险 accent 主按钮、目标文件夹名、第三行
  “记住我的选择”复选框；`Enter` 进入、`Esc` 留下、`Space` 切换；
  主按钮记住→`next`，次按钮/Esc 记住→`loop`，立即原子写设置。
- 测试钩子：`--watch-interval`、`--dump-at`（写 dump 不退出）、`--quit-at`、
  `--minimize-at`；`--layout-dump` 增 `folder_path/file_name/folder_wrap/
  watch_interval/confirm_kind/confirm_target/confirm_remember/
  confirm_remember_row/confirm_remember_box`。
- 测试：`tests/folder_watch_tests.cpp`（新增 52 项单测）；`tests/run_watch_check.ps1`
  **71 项**；设置页“到达边界”的 `next` 标签改为“相邻文件夹”。
- 文档：`docs/FOLDER-WATCH.md`；`HELP.md` 补自动监视与穿透说明；
  `SETTINGS.md` 的边界行为小节同步。

## 未发布 — v2.0 S2（2026-10-24，缩略图条增强）

- 缩略图条三模式（`22/08`）：默认 **自动隐藏**（`auto`）——鼠标进入底栏上方
  8 DIP 热区或已显示的缩略图条时 160 ms 淡入，移开 0.4 秒后淡出；
  `pinned` 常显（内容区扣除 76 DIP）；`off` 关闭（设置页）。
- 模式语义统一：更多▾ 的“缩略图”开关 = `pinned ↔ auto`（选中 = 常显），
  `Ctrl+T` 同样在两者间切换，从 `off` 按 `Ctrl+T` 回到 `pinned`；
  `--strip=pinned|auto|off` 仍只覆盖本次会话。默认值、校验回退、旧配置迁移
  三处统一为 `auto`。
- 悬停 500 ms 显示文件名 tooltip：槽上方 6 DIP，靠近窗口顶边自动翻到下方，
  左右夹取在窗口内，最长 320 DIP，不参与命中；滚动/点击/换槽/淡出即隐藏。
- 请求策略：`auto` 完全隐藏时取消待办、不再请求；`off` 从不请求。
- 绘制：D2D Layer 统一透明度（背景/格子/选中框/滚动条），隐藏时
  `strip_interactive=0`，点击落到图像，热区点击不会开始平移。
- 测试钩子 `--hover-thumb <index>`、`--hover-hotzone`、`--hover-cycle`；
  `--layout-dump` 增 `strip_shown/strip_opacity/strip_interactive/strip_hover/
  strip_tooltip/strip_tooltip_above/strip_tooltip_panel`。
- 测试：单测新增 auto 命中门禁与 tooltip 几何；`tests/run_strip_check.ps1`
  **44 项**；`run_ui_chrome_check` 98 项、`run_navigation_check` 71 项、
  `run_settings_check` 105 项；依赖“条常显”的旧脚本显式传 `--strip=pinned`。
- 文档：`docs/THUMBNAIL-STRIP.md`；`UI-CHROME.md` 开关/默认值/钩子/日志更新；
  `HELP.md` 补自动隐藏、热区与悬停提示说明。

## 未发布 — v2.0 S2（2026-10-24，设置窗口与设置模型）

- 设置窗口（`22/07`）：自绘、非模态、760×560 DIP 的三页窗口（常规 / 查看 /
  文件关联），`Ctrl+,`、标题栏 `☰` 菜单和右键菜单打开；改动立即生效并保存，
  `Esc`/关闭按钮关闭，DPI/中英/高对比度全适配。
- 常规页：语言（跟随系统/中文/English）、主题（深/浅/跟随系统）、棋盘格与
  0–100 透明度滑条、缩略图条（自动渐隐/常显/关闭）、鸟瞰图、排序、记住窗口位置、
  单实例、始终置顶。
- 查看页：滚轮（缩放/翻页，翻页模式下 `Ctrl+滚轮` 仍缩放）、缩放步长
  （1.05/1.10/1.15/1.25）、最近邻放大、保留视图旋转、全屏背景
  （主题/纯黑/暗化）、到达边界（循环/停止/询问）。
- 文件关联页：39 个文档扩展名的四列勾选网格、全选、注册所选、取消注册、
  设为默认（二次确认，备份并可恢复）；失败原因显示在状态行，不弹对话框。
- 设置模型重写：schema v1（`[general]/[view]/[window]`）、原子写
  （tmp + flush + MoveFileEx）、旧 `[ui]` 自动迁移并删除、损坏文件备份为
  `.bak-<时间>` 并用默认值重建、全字段白名单校验。
- 单实例（`WM_COPYDATA` 转发路径）与窗口位置记忆（含显示器可见性校验）。
- 主题 `system` 跟随 `AppsUseLightTheme`，响应 `WM_SETTINGCHANGE`；
  `T` 键改为显式深/浅切换。
- 新增测试钩子 `--open-settings`、`--settings-page`、`--settings-click`、
  `--settings-dump`、`--settings-screenshot`；`--print-settings` 增
  `theme/checker_opacity/remember_window/single_instance/wheel/pixel_mode/
  fullscreen_bg/folder_wrap/window*`。
- 测试：`tests/settings_test.cpp`（新增 135 项模型/布局单测）、
  `tests/run_settings_check.ps1`（105 项），覆盖迁移/损坏恢复/原子写/页面/
  持久化/关联/单实例/窗口位置/四档 DPI/截图。
- 文档：`docs/SETTINGS.md`；`HELP.md` 增设置入口、滚轮翻页、边界行为说明。

## 未发布 — v2.0 S2（2026-10-24，图片信息与 EXIF）

- 信息面板（`22/06`）：右下 360 DIP 非模态浮层，四组 21 行（文件/图像/拍摄/色彩），
  缺失值显示“—”；高度按行数派生、上限图像区 70%，超出用 3 DIP 细滚动条。
- EXIF 后台读取（`core/exif_service.*`：1 worker、path+mtime 缓存 8 条、最新请求优先、
  完成消息校验路径）：JPEG（APP1）/PNG（eXIf）/TIFF 用自研 TIFF IFD 解析（含嵌套
  GPS IFD、S/W 取负、海拔符号），HEIC/AVIF 走 WIC，RAW 走 LibRaw 只 open 不 unpack；
  无 EXIF 的格式整组显示“—”，不弹错误。
- 交互：`I` / `Alt+Enter` / 底栏 ⓘ 开关、`Esc` 关闭、滚轮只滚面板、右键行复制该值、
  页脚“复制全部”、导航时保持打开并刷新且不串台；`Ctrl+C` 仍是复制图像。
- 新增 `zoreview_cli --exif`（打印 21 个字段）、`--info-scroll`、
  `--click-hit info.close|info.copy_all|info.row.N|info.copy_row.N`；
  `--layout-dump` 增 `info_rows/info_exif_ok/info_scroll*/info_line_N`、19 个具名值
  与 `info_header/close/body/footer/copy_all` 矩形。
- 样本：`测试文件夹/tools/make-exif-samples.py` 生成全字段/部分/无 EXIF/PNG eXIf/
  南半球 GPS 五个样本；`tests/run_info_panel_check.ps1` **129 项**；单测 572 → 655 项。
- 文档：`docs/INFO-PANEL.md`；`HELP.md` 补信息面板说明；`UI-CHROME.md` 面板章节更新。

## 未发布 — v2.0 S1（2026-10-24，界面命令与空态）

- 导航与缩放命令（`22/09`）：`Home`/`End`、`PgUp`/`PgDn`、`+`/`-`（含小键盘）、
  `0`/`Ctrl+0` 适应、`1`/`Ctrl+1` 1:1、`I`/`Alt+Enter` 信息面板、`Ctrl+T`
  缩略图条、`Enter` 全屏、`,`/`.` 动画帧步进；`Space` 在静态图上为下一张。
  新增 `src/ui/command.*` 统一键位表与 `QueryCommand` 可用性；无图/单张时
  命令置灰并记 `cmd: blocked=…`。
- 图像区上一张/下一张圆钮（56 DIP、内容区垂直居中、悬停 35%→100%、
  命中优先于平移）；缩放范围改为 10%–3200%，且允许 fit 低于 10%（8K）。
  新增 `render/zoom_math.h`；`zoom_step` 进入设置（默认 1.15）。
- 新增测试钩子 `--send-keys`（真实 `WM_KEYDOWN`）与 `--click-hit arrows.left/right`；
  `--layout-dump` 增 `arrows_*`、`zoom_min/max/percent`。
- 新增 `tests/run_navigation_check.ps1`（68 项）、`docs/NAVIGATION.md`；
  单测从 411 项扩到 485 项。

- 标题栏/底栏接真命令：底栏 `ⓘ`（信息浮层）、百分比（缩放菜单：
  适应/50/100/200/400%）、`更多▾` 浮层面板（鸟瞰图/缩略图/窗口置顶开关 +
  名称/修改时间/大小排序）；标题段点击复制（文件名/尺寸/文件大小）+ Toast；
  旋钮 `↺`/`↻` 仍是占位。
- 设置新增 `thumbstrip_mode` / `birdseye` / `sort_mode` / `topmost`，面板写、
  重启读；`--strip=` 只覆盖本次会话。排序按路径保持当前图。
- 启动空态卡：图标/标题/派生格式行/「选择图片」/「打开文件夹」/「查看全部格式」；
  格式行由 `FormatRouter` + 构建/系统 codec 能力派生（不写死扩展名），
  `HDR` 未接通自动隐藏，HEIC/AVIF 缺系统扩展时也隐藏。
- 「全部格式」模态卡片：每族显示名 + 扩展名 + 不可用原因；Esc/Close 关闭。
- OLE 拖放（`IDropTarget`）替换 `WM_DROPFILES`：拖入高亮 + 2 px 描边 +
  「松开以打开」；拖入文件/文件夹/空文件夹/不支持格式四态；空目录不排队解码；
  有图时拖不支持格式只提示、不清图；文件缺失按缺失处理。
- 空态键盘：`Tab`/`Shift+Tab` 循环三个按钮、`Enter` 激活、2 px 焦点环。
- 新增 `tests/run_empty_state_check.ps1`（123 项）、`docs/EMPTY-STATE.md`；
  单测扩到 411 项。测试钩子：`--simulate-drop`、`--simulate-ole-drop`、
  `--simulate-drag-over`、`--click-hit empty.all_formats|formats.close`。

## 未发布 — v2.0 S0（2026-10-24，自绘 UI 骨架）

- 新增 `src/ui/`：颜色 token、DIP 尺寸、`LayoutFrame` 布局、命中栈、
  `OverlayStack`（模态/popup/info/toast + Esc 优先级）、窗口三态机。
- DirectWrite 回归：标题栏/底栏文字、按 DPI 缓存的 TextFormat、矢量图标
  （20×20 网格、1.5 DIP 圆头描边）。
- 自绘无边框窗口（默认）：32 DIP 标题栏（文件名 · 尺寸 · 大小 · 第几张 · 百分比
  + 菜单/全屏/最小化/最大化/关闭），44 DIP 底栏骨架（ⓘ / 百分比 / 第几张 /
  适应 / 1:1 / ↺ / ↻ / 更多▾，未实现的命令画灰）。
- 窗口行为：保留系统 Snap/缩放/Alt+Space/Alt+F4；最大化客户区夹到工作区；
  Win11 圆角（最大化/全屏去圆角）；F11/标题键全屏并恢复原样式+矩形+最大化状态；
  Esc 一次只关一层（设置→模态→popup→info→全屏）。
- 内容矩形改为“客户区 − 标题栏 − 底栏 − pinned 缩略图条”；缩略图条支持
  pinned/auto/off 三种模式（auto 的渐隐留给 S2）。
- 新增 `tests/run_ui_shell_check.ps1`（41 项）与 60 项纯函数单测；
  `docs/UI-FRAMEWORK.md` 记录布局公式、层序、消息表与测试钩子。
- 测试钩子：`--classic-frame`、`--strip=pinned|auto|off`、`--layout-dump`、
  `--maximized`、`--fullscreen`。

## 0.3.0 — 2026-10-23（M3 完成 / 发布工程）

- 新增 exe/DLL 的 Win32 VERSIONINFO 资源（文件属性、签名界面可见版本）。
- 发布工程：`tools\package.ps1` 现在生成 `LICENSE`、`NOTICE`、`THIRD_PARTY_LICENSES`、
  `PRIVACY.md`、`CHANGELOG.md`、`RELEASE_NOTES.md`、CycloneDX `SBOM.cdx.json`、
  `release-manifest.json`、`SIGNATURES.txt`、`Verify.ps1`/`Verify.cmd`。
- Authenticode 签名：`tools\sign.ps1` + `package.ps1 -Sign`（PFX 或证书存储，
  SHA-256 + RFC 3161 时间戳），`-RequireSignature` 可把签名设为发布门槛。
- 校验和：`SHA256.txt` 覆盖包内所有文件，zip 另有 `.sha256`；`Verify.ps1`
  可校验校验和、manifest、SBOM、许可证与签名，`Install.cmd` 安装前强制执行。
- 安装器复制许可/文档/SBOM 到安装目录；卸载仍清理整个目录。
- 文档：帮助、隐私、更新日志、发布说明、签名指南、发布检查清单。
- 新增 `tests\run_release_check.ps1`（打包、校验、SBOM、版本资源、自签名测试）。

## 0.2.0-m3 — 2026-10-23（M3-1..M3-3）

- Shell 缩略图 `ZoreViewThumb.dll`：41 个扩展名，HKCU 注册/卸载，
  用户级 `DisableProcessIsolation`，2 秒看门狗，四种 Shell 尺寸验证。
- 右键 3 个动词（用 ZoreView 打开 / 设为桌面壁纸 / 复制图像）及无窗口命令
  `--set-wallpaper`、`--copy-image`、`--register-shell`、`--unregister-shell`。
- 可选文件关联（ProgID + OpenWithProgids，显式 `--set-default-assoc` 备份并恢复
  原默认值与 UserChoice）；当前用户/全机安装器；便携版。
- 中/英双语、高对比度、统一 DPI 换算、UIA 标题通知、重命名对话框 Tab 循环。

## 0.2.0-m2 — 2026-10-23（M2）

- RAW：LibRaw 直连（10 种相机格式），方向烘焙、缩略图半尺寸。
- PSD/PSB 自研合并图解码：Raw/RLE/ZIP/ZIP+prediction、1/8/16/32 位、
  RGB/RGBA/灰度/索引/位图/CMYK、alpha、内嵌 ICC。
- HEIC/HEIF、AVIF（WIC 系统扩展）、多页 TIFF 首帧。
- SVG（Direct2D 光栅化，viewBox/透明度/安全校验）。
- GIF/APNG 动画播放（合成帧、delay/dispose/blend/loop、空格暂停）。
- TGA/PCX/PNM/XBM/XPM（FreeImage）。
- 第三方库迁入 `third_party/` 并补 NOTICE。

## 0.1.0-m1 — 2026-10-22（M1）

- 缩略图条、异步生成、虚拟化滚动、点击跳转。
- 8K 显示、1200 文件目录、启动优化（首帧约 200 ms）。
- ASan/泄漏检查、8K 连续翻页内存优化（685 → 460 MB）。
- EXIF 1–8 与 ICC→sRGB、文件操作（另存为/复制/删除/重命名/壁纸）。

## 0.1.0-m0 — 2026-10-21（M0）

- CMake + MSBuild + MSVC（`/W4 /WX`）构建，Win32 + D3D11 + Direct2D。
- P0 格式（JPEG/PNG/BMP/GIF/WebP/TIFF/ICO/CUR）、解码线程池、内存/磁盘缓存、
  日志、minidump、设置持久化、格式矩阵与性能基线。
