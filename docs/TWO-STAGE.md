# 两段式加载（v2.0 S3 / `22/13`）

> 状态：已完成（2026-10-24）
> 依赖：缩略图磁盘缓存（M1/22/08）、RAW/LibRaw（M2）、渲染器与浮层（S0）
> 测试：`tests/run_two_stage_check.ps1`（54 项）、`zoreview_tests`（+22）、
> `run_format_matrix`、`run_raw_check`

---

## 1. 模块

| 文件 | 作用 |
| --- | --- |
| `src/core/preview_plan.*` | 纯策略：格式 + 文件大小 → `PreviewPlan{cache, embedded}`，可单测 |
| `src/core/thumbnail_service.*` | `TryLoadPreview(path)`：内存/磁盘缩略图缓存命中，UI 线程 ~1 ms |
| `src/decoders/libraw_decoder.*` | `DecodeEmbeddedPreview(path)`：`libraw_unpack_thumb` + WIC 内存解码 |
| `src/decoders/wic_decoder.*` | `DecodeMemory(data,size,options)`：与文件解码共用方向/ICC/alpha 管线 |
| `src/core/decode_service.*` | 独立预览线程 + 独立队列 + `WM_APP+4`；不排在两个全解 worker 后面 |
| `src/render/renderer.*` | `SetImage(..., preserve_view)`：替换时保持 zoom/中心/旋转 |
| `src/ui/preview_badge.*` | 右上角「预览」胶囊的布局/绘制（不参与命中） |
| `src/app/main.cpp` | 两段流程、badge 淡出、失败保留、导出禁用、测试钩子 |

---

## 2. 策略（`PlanPreview`）

| 条件 | 缓存预览 | 内嵌预览 | 说明 |
| --- | --- | --- | --- |
| RAW（CR2/CR3/NEF/ARW/…） | ✅ | ✅ | 先命中缩略图缓存；无缓存或只有 128 px 缓存时再请求内嵌 JPEG |
| PSD/PSB | ✅ | — | 只读缩略图缓存（不解析资源 1036） |
| 其他 ≥ 32 MB | ✅ | — | 大 TIFF/PNG 等；`kPreviewMinFileBytes` |
| 其他 < 32 MB | — | — | 避免普通 JPEG “糊一下”的闪烁（坑 1） |
| GIF/APNG/WebP | — | — | 动画首帧本来就快；预览会推迟播放（坑 14） |

- 内存/磁盘**全尺寸**缓存先于预览判断：命中时秒开，不需要预览。
- 预览只用于显示：不进内存 LRU、不写磁盘缓存、不触发相邻预取（坑 6/12）。
- 标题与信息面板始终显示 `full_*`（目标尺寸），不显示预览尺寸（坑 5）。

---

## 3. 流程

```text
LoadPath(path)
  CancelAll + 重置预览状态/badge
  内存 LRU → 磁盘全尺寸缓存 →（命中则结束）
  PlanPreview:
    cache → ThumbnailService.TryLoadPreview → SetImage(preview, reset_view)
    RAW 且无内嵌预览 → DecodeService.RequestPreview（独立线程）
      完成 WM_APP+4 → OnPreviewDone（校验 epoch + pending path）
  设置 pending、Request(full)
  预览可见：不显示 “Loading…” 标题、不启动 400 ms 加载 chip
  全解完成 WM_APP+1
    成功 → SetImage(full, preserve_view) + badge 160 ms 淡出 + 正常缓存/预取
    失败 → 保留预览 + 错误卡 + badge「预览（加载失败）」
```

- 预览/全解都做 epoch/路径校验：晚到的完成消息丢弃（坑 3）。
- 自己删除/改名/进入空目录/监视删除都会 `ResetPreviewState`，badge 不会残留。
- 启动首图在 D3D 设备之前打开（M1-2 的启动重叠）：预览先放进
  `g_pending_preview`，`ApplyPendingPreview()` 在 renderer 就绪后立刻上屏。

---

## 4. RAW 内嵌预览

- `libraw_open_buffer` → `libraw_unpack_thumb` → 只接受 `LIBRAW_THUMBNAIL_JPEG`
  （`tformat=1`）；长边用 `DecodeOptions.max_edge = 1920` 缩放（WIC scaler 是
  拉取式，内存可控）。
- 方向：内嵌 JPEG 通常没有 Orientation；用主图的 `sizes.flip` 经 `ApplyOrientation`
  烘焙，保证与全解同向（坑 2）。若 JPEG 自带方向，则保留 WIC 已应用的结果。
- 尺寸：`full_*` 记录主图直立全尺寸，供标题/信息面板使用。
- `twidth/theight` 不可信（有的相机报 0×0），尺寸过滤放在解码之后：< 320 px 跳过。
- **CR3 的限制**：Canon CR3 的相机预览常是 H.265（`LIBRAW_THUMBNAIL_H265`，
  `tformat=6`），WIC 无法从裸流解码，因此 CR3 首次打开走全解；第二次打开
  （缩略图缓存已存在）秒出 128 px 预览。日志：
  `libraw: thumb format=6 ...`、`preview: unavailable (LibRaw: no embedded JPEG
  preview (format=6))`。

---

## 5. 替换时保持视图

`Renderer::SetImage(info, pixels, reset_view, preserve_view)`：

- `reset_view` 优先：新文件从 fit 开始。
- `preserve_view`（仅手动视图）：替换前记录内容区中心下的**图像相对坐标**
  `(fx,fy)`，替换后按新尺寸重算 `offset`，使同一点仍在内容中心；
  `fit_` 为真时只需重算 fit，中心自然不变。
- `rotation_` 完全不参与替换：旋转是显示变换，与预览/全解无关（坑 13）。
- 缩略图缓存预览（128 px）被内嵌 JPEG 升级时也走 preserve：用户已在预览上
  缩放到 200% 的话不会跳回 fit。

---

## 6. 「预览」badge

```text
位置：图像区右上角，距内容边 12 DIP
尺寸：高 22 DIP，圆角 11，宽 = 文本 + 2×10 DIP（48–240 DIP 夹取）
颜色：bg.panel 85% + border.subtle；失败时文字用 danger
```

- 文案：`预览` / `预览（加载失败）`（i18n `kPreviewBadge*`）。
- 不参与命中；全解到达后 160 ms 线性淡出（`kPreviewBadgeTimer`，只改 opacity）。
- 高对比度下不透明并使用系统色。
- 窗口太小（宽/高不足）不画，不挤压图像。

---

## 7. 失败与导出

- 全解失败：预览保留 + `preview_failed=1` + badge 红字 + 错误卡（`22/11` 的
  `ShowError`）。重试仍走同一个 `LoadPath`，状态会重置（坑 17）。
- 只有预览时：另存为 / 复制图像 / 设为壁纸一律拒绝，日志
  `preview: export blocked while loading`，并弹 Toast「原图仍在加载，请稍候」。
  删除/重命名/在资源管理器显示不受影响（操作的是文件，不是像素）。
- 内嵌预览本身失败（无预览、太小、损坏）：静默回退，只写日志，不打扰用户。

---

## 8. 测试钩子

| 钩子 | 作用 |
| --- | --- |
| `--force-preview-fail` | 预览显示后让本次全解失败一次（失败路径确定性） |
| `--preview-zoom <percent>` | 预览上屏后立即缩放到该百分比（验证替换保持视图） |
| `--send-keys-at <ms>` | 提前发送 `--send-keys`；导出禁用需要在预览阶段按键 |
| `--dump-at <ms>` + `--quit-at <ms>` | 预览中/全解后的确定性快照（`--dump-at` 优先于 keys 的 dump） |

`--layout-dump` 新增：

```text
preview_active / preview_failed / preview_source / preview_target /
preview_ms / preview_badge_visible / preview_opacity / preview_text /
preview_badge=<l,t,r,b> / view_center=<x,y> / content_center=<x,y> /
image_width / image_height        （image_* 是目标尺寸）
```

日志：

```text
preview: requested source=embedded
preview: shown source=cache|embedded size=WxH target=WxH ms=N
libraw: thumb format=1 WxH bytes=N
libraw: preview WxH full=WxH orient=N bytes=N in Nms
preview: replaced with full image
preview: failed; keeping preview
preview: dropped stale completion / dropped stale queued preview
preview: unavailable (<reason>)
preview: export blocked while loading
prefetch skipped: raw neighbour
```

---

## 9. 坑

| # | 现象 | 原因 | 对策 |
| --- | --- | --- | --- |
| 1 | 普通 JPEG 也糊一下 | 没按条件过滤 | 只对 RAW/PSD/≥32 MB；普通图不预览 |
| 2 | 预览与全解方向不一致 | 内嵌 JPEG 没方向 | 主图 flip 经 `ApplyOrientation` 烘焙 |
| 3 | 全解到了预览又盖回来 | 完成消息晚到 | epoch + pending path + `preview_active` 校验 |
| 4 | 替换后视图跑偏 | 用了 reset_view | `preserve_view`；按相对坐标重算 offset |
| 5 | 预览尺寸进了标题 | 标题读当前帧 | 标题/信息面板用 `full_*` |
| 6 | 128 px 被当成原图缓存 | 写进 LRU/磁盘缓存 | 预览只显示 |
| 7 | 160×120 预览放大更糊 | 相机缩略图太小 | 解码后 <320 px 跳过 |
| 8 | 内嵌 JPEG 损坏导致崩溃 | 数据无效 | WIC 失败静默回退 |
| 9 | 全解失败画布变空 | 失败清图 | 失败保留预览 + 错误卡 |
| 10 | 磁盘缓存损坏花屏 | 没校验头 | `DiskCache::Load` 校验 NVIMG/长度 |
| 11 | 8K 更慢 | 多一次上传 | 8K 只在缓存命中时预览 |
| 12 | 预览触发相邻预取 | 复用了全解路径 | 预览分支不调 `PrefetchAdjacent` |
| 13 | 旋转在替换后丢失 | 替换重建 renderer 状态 | `rotation_` 不动 |
| 14 | 动画出现预览 badge | 没排除动画 | GIF/APNG/WebP 不预览 |
| 15 | 全屏切图闪黑 | 提前清图 | 预览到达前保留旧图 |
| 16 | 内存峰值上涨 | 预览 + 全解同时在 | 128 px(~64 KB) / 1920 px(~10 MB)，8K 不预览 |
| 17 | 重试后 badge 不消失 | 状态没重置 | `LoadPath` 开头 `ResetPreviewState` |
| 18 | 预览被 RAW 预取拖慢 | LibRaw 全局锁串行 | 不预取 RAW 邻居（`prefetch skipped: raw neighbour`） |
| 19 | 启动首图没有预览 | LoadPath 早于 D3D 设备 | `g_pending_preview` + `ApplyPendingPreview` |
| 20 | 预览被加载 chip 盖住 | 400 ms 计时器在预览前启动 | `ApplyPreviewFrame` 里 `CancelLoading` |
| 21 | 缓存预览只有 128 px | 缩略图缓存本来就是 128 | RAW 命中缓存后仍请求内嵌 JPEG 升级（保持视图） |
| 22 | 截图拍到 128 px 放大 | 测试的临时缓存被前面的 warm run 污染 | 截图前清 `cache` 目录 |

---

## 10. 验收

- CR2（6744×4502）：预览 ~320 ms 上屏（含 v2.0 启动 ~300 ms 地板；内嵌解码
  本身 ~125 ms），~1.6 s 变清晰；badge 出现/淡出；标题始终 6744×4502。
- CR3（8480×5650，H.265 预览）：首次无预览；第二次打开 0–1 ms 出 128 px 缓存预览。
- 普通 JPEG：日志无 `preview:`，无闪烁。
- 预览上缩放到 200% → 全解后仍 200%，视图中心与内容中心偏差 ≤2 px。
- 失败：预览保留 + 错误卡 + badge 红字；导出被拒绝并提示。
- 200% DPI：badge 44 px 高、右边距 24 px，仍在图像区内。
- 截图：`tests/output/two-stage/preview-badge.png`、`sharp.png`、
  `preview-failed.png`。
