# 另存为增强（v2.0 S3 / `22/14`；V3.1-06 §06c 起改成一段式系统对话框）

> 范围：PNG / JPEG / BMP / TIFF / WebP / GIF 六种容器、质量选择与预估大小、
> 原子写、按当前视图（含旋转）导出、动画只存当前帧。
> 代码：`platform/image_io.*`、`platform/encoder_probe.*`、`platform/gif_encoder.*`、
> `platform/save_service.*`、`platform/save_dialog.*`（一段式 `IFileSaveDialog` + `IFileDialogCustomize`）。
>
> **2026-10-27 更新（06c 完成 + 死代码清理）**：质量/预估/"Original format" 已经嵌进
> **系统保存对话框**；`ui/save_options.*`、`platform/size_estimate.*`、
> `OverlayKind::kSaveOptions`、`--click-hit save.*` 与 `save_*` dump 字段**全部删除**。
> 本文 §4/§5 描述的是**旧两段式卡片**，保留作历史对照；当前行为见 §5.1 与
> `规划/v3.1/06` 的 06c 进度行。

---

## 1. 流程

```text
Ctrl+S / 右键“另存为”
  → 系统保存框（过滤器只列本机真能编码的容器，默认 = 源格式）
     + 内嵌选项区（质量 1–100 下拉 / 预估大小 / “Original format” 复选）
  → 由扩展名定格式（未知扩展名 → 沿用过滤器格式并补扩展名）
  → 源未修改且勾了 Original format → 字节复制；否则 SaveService 工作线程：
    按视图旋转 → 编码 → 同目录 .nvtmp → MoveFileEx
  → 完成：Toast「已保存」/「已保存当前帧」；失败：错误卡，原文件不动
```

- 不需要质量的容器（PNG/BMP/TIFF/GIF）不弹选项卡，直接后台保存。
- 系统框负责覆盖确认，自绘卡只做质量，不会二次询问（坑 19）。
- 保存期间复用 400 ms 延迟的加载 chip（文案「正在保存…」）；大图不卡 UI。
- 保存到当前目录会进入目录监视的 2.5 s 抑制窗口，并立刻重扫列表（`22/10`）。

## 2. 编码能力探测

启动时 `ProbeEncoders()` 用 **2×2 图像走真实保存路径**试编码一次（含透明像素），
成功才算可用；结果写日志：

```text
save: encoders=png,jpeg,bmp,tiff,webp,gif
```

- PNG / JPEG / BMP / TIFF：WIC `CreateEncoder` + 帧 + 提交。
- WebP：`freeimage.lib` 内静态编译的 libwebp 简单 API（`WebPEncodeBGRA` /
  `WebPFree`），质量参数 1–100 真正生效；本机 WIC 只有 WebP 解码器，没有编码器。
- GIF：自研写容器（见 §3），量化用 FreeImage 的 Wu 算法（无 FreeImage 的构建
  用 6×6×6 色立方回退）。
- 不可用的容器不会出现在保存框里，也不会因为“选了才失败”而报错（坑 1）。

## 3. GIF 写入

`platform/gif_encoder.*` 直接写 GIF89a：

1. `FreeImage_ColorQuantizeEx(FIQ_WUQUANT, 255)`（保留 1 个索引给透明）。
2. alpha < 128 的像素映射到索引 255；GCE 只在真的存在透明像素时设置透明标志。
3. 调色板补到 256 项，LZW 最小码长固定 8。
4. 自写 LZW：4096 码表、12 位上限、写满发 clear；每个数据子块 ≤255 字节。

GIF 是 1-bit 透明，半透明会按阈值变全透明或不透明（坑 10）。

## 4. 质量与预估

- JPEG：WIC `ImageQuality` 1–100（默认 90）。
- WebP：libwebp `quality_factor` 1–100（默认 90）。
- 预估：**同一编码器、同一像素（视图旋转后）、同一质量**编码到内存流，因此与
  实际文件大小基本一致（测试断言误差 <10%）。
- 触发：选项卡打开、质量变化；`core` 侧的 worker 做 **250 ms debounce**，拖滑条
  只在停下后编码一次。
- 上限：超过 **32 MP**（如 8K）不预估，显示「—」；避免试编码 127 MB + 缓冲（坑 7）。
- 打开/关闭/改质量都会推进 epoch，旧结果直接丢弃。
- 大小文案：`< 1024` → `512 B`，否则 KB/MB/GB，<10 保留一位小数。

## 5. 选项浮层（**历史：06c 起已删除**）

> 下面这段是 v2.0 的两段式卡片，**代码已不存在**（`ui/save_options.*`、`OverlayKind::kSaveOptions`、
> `--click-hit save.quality|save.primary|save.cancel`、`save_*` dump 字段都在 2026-10-27 删除）。
> 当前机制见 §5.1。

`ui/save_options.*`，宽 **360 DIP**、内边距 20、标题 24、行高 36、按钮 88×32：

```text
┌──────────────────────────────────┐
│ 保存为 JPEG                      │
│ 质量  [==========----]  90       │
│ 预计  ~ 1.2 MB                   │
│                    [取消] [保存] │
└──────────────────────────────────┘
```

- 居中于客户区（`OverlayKind::kSaveOptions`，modal）；scrim 吞掉所有外部点击。
- 质量行只对 JPEG/WebP 出现；无质量时卡片少 44 DIP。
- 滑条：点击/拖动按 x 映射 1–100（`QualityFromTrackX`），松手才提交；
  键盘 `←/→` ±1、`Shift` ±10、`Home/End` 到 1/100。
- `Tab`/`Shift+Tab` 在 滑条 → 保存 → 取消 之间循环，焦点环 2 DIP accent；
  `Enter` 保存、`Esc` 取消。
- 文字、颜色、DPI 全部走 token/DIP；高对比度使用系统色（截图 `modal-hc.png`）。

## 5.1 当前机制（V3.1-06 §06c，一段式）

- `platform/save_dialog.*` 在 **Explorer 风格系统对话框**里用 `IFileDialogCustomize` 放三样东西：
  **质量 1–100 下拉**（Customize 没有滑条，这是与旧卡片的唯一可见差异）、
  **预估大小**（后台线程编码 + 200 ms 定时器轮询，>32 MP 显示「—」）、
  **"Original format" 复选**（源未修改时字节复制，SHA-256 相同；旋转后自动改为重编码）。
- 产品路径**不再有第二个窗口/浮层**，也没有 `save.*` 的合成点击目标；
  测试改用 `NV_SAVEAS`（`type=…;quality=…;original=1;accept=1|cancel=1`）驱动真实对话框。
- 编码仍走 `SaveService`（单 worker + epoch + 退出 join），预估走对话框自己的线程，
  两者都不在 UI 线程。
- `IFileSaveDialog` 创建/Show 失败时退回 `GetSaveFileNameW`（没有选项区，保存仍可用）。

## 6. 导出内容

- **按当前视图**：`MakeExportImage(shared_ptr, rotation)` 复制一份后按 `rotation_`
  CPU 旋转 90° 的整数倍；解码结果、缓存、`ImageInfo` 保持直立（`22/05`）。
- **动画只存当前显示帧**：导出的副本清空 `frames`；Toast 用「已保存当前帧」，
  日志 `animated=1`（坑 15）。
- 只有预览（两段式加载未完成）时拒绝导出，Toast「原图仍在加载，请稍候」。

## 7. 原子写与失败

- WIC 容器：`SHCreateStreamOnFileEx` 写 `target.nvtmp` → 释放 stream → `MoveFileEx(
  REPLACE_EXISTING|WRITE_THROUGH)`。
- GIF/WebP：编码到内存 → 写 `.nvtmp` → `FlushFileBuffers` → 替换。
- 任何失败：删 `.nvtmp`，错误卡 + `ClassifyOperationError(kSaveFailed)`；
  原文件不受影响（坑 3/14）。
- 磁盘满/只读/目标被目录占用都走同一条失败路径；测试用“同名目录”验证。
- **已知限制**：保存过程不能取消（WIC 的 commit 无法安全中断）。大图保存会显示
  加载 chip，关闭窗口时会等当前编码结束再退出；原子写保证中途失败不会留下半个
  文件。

## 8. 测试钩子

| 钩子 | 用途 |
| --- | --- |
| `--save-to <file> [--save-quality N]` | 同步保存当前视图（支持 webp/gif），供脚本用 |
| `--save-as <file>` | 旧钩子（旋转回归脚本用），等价 `--save-to` + 默认质量 |
| `--save-options <file>` | 直接打开选项卡（按扩展名定格式），不经过系统框 |
| `--click-hit save.quality[.N]` | 点击滑条中点 / 精确百分比位置（真实鼠标消息） |
| `--click-hit save.primary\|save.cancel` | 点保存/取消 |
| `--layout-dump` | 增 `save_options/save_format/save_quality/save_has_quality/ save_focus/save_estimate_state/save_estimate/save_estimate_text/save_busy/ save_target` 与 `save_card/save_title/save_quality_label/save_track/ save_quality_value/save_estimate_label/save_estimate_value/save_button/ save_cancel` |

日志：

```text
save: encoders=png,jpeg,bmp,tiff,webp,gif
save: dialog format=jpeg
save: estimate_request format=jpeg quality=90
save: estimate=116743 ok=1 ms=10
save: quality=60            (拖动中)
save: quality_committed=60  (松手)
save: dialog=save|cancel
save: start format=jpeg quality=90 path=...
save: done format=jpeg bytes=4477 ms=12 animated=0 path=...
saved image: ... rotation=0 format=png quality=90 bytes=...   (同步钩子)
```

## 9. 坑

1. **GIF LZW 码长切换**：编码器必须在“下一个可分配码 > 2^码长”时才加宽
   （`next_code > (1 << code_size)`）。写成 `== 2^code_size` 或 `-1` 都会让解码器
   从 ~512 个码之后开始输出垃圾（本次实测：24000 像素里 2 万多个错）。
2. WIC 的 `SetPixelFormat` 返回**最接近**的格式，必须再比较 GUID；JPEG/BMP 喂
   BGRA 会被拒绝。
3. 替换目标前必须释放 frame/encoder/stream（COM 持有文件句柄 → `MoveFileEx`
   共享冲突）。
4. `SHCreateStreamOnFileEx` 是 6 个参数（模板 stream 也要传，可传 `nullptr`）。
5. 估计与保存必须共用同一份像素与质量，否则预估没有意义。
6. 超过 32 MP 不预估；8K 试编码 1–2 秒且多一份 127 MB 缓冲。
7. 拖动滑条时不能每个像素都 `SaveSettings`/编码：debounce + epoch。
8. 退出时先 `Shutdown()` 两个 worker（join），避免向已销毁的窗口 `PostMessage`。
9. 只有预览时禁止导出（`22/13`）；导出必须经过 `MakeExportImage`，否则旋转丢失。
10. GIF 半透明按 128 阈值；不透明区域的颜色经过 255 色量化，容差 ~12。
11. 系统保存框的 `lpstrDefExt` 不能指向临时 `std::wstring` 之外的内存。
12. 自绘卡不负责覆盖确认（系统框已经确认过）。

## 10. 验收

- `zoreview_filetests`：六种容器保存/读回、PNG alpha 逐字节、BMP/TIFF RGB 精确、
  JPEG 容差、WebP/GIF 可读、WebP 质量单调、预估误差 <10%。
- `tests/run_save_check.ps1`：**105 项**——编码器探测、六格式落盘与像素、
  GIF 透明、质量、预估、选项卡几何/点击/键盘/Tab/Enter/Esc、旋转导出、
  动画当前帧、失败路径（无 `.nvtmp`）、8K、四档 DPI/中文/高对比度截图。
- 单测 `zoreview_tests`：+38（选项卡布局/命中/滑条映射/大小文案）。
