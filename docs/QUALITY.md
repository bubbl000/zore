# 缩放画质（插值 / 像素模式 / 显示代理）

> 状态：已完成（2026-10-25，v2.0 S3 / `22/18`）
> 依赖：`22/05` 视图旋转、`22/09` 缩放命令
> 测试：`tests/run_quality_check.ps1`（57 项）、`zoreview_tests` 的
> `TestDrawQuality`（23 项）、`tests/run_8k_check.ps1`

---

## 1. 模块

| 文件 | 作用 |
| --- | --- |
| `src/render/draw_quality.h` | 纯函数策略：`DrawQuality`、`SelectDrawQuality`、`ShouldUseProxy`、`ProxyEdge`；可单测 |
| `src/render/renderer.*` | 质量状态机、150 ms 交互窗口、`D2D1_INTERPOLATION_MODE_*`、1/4 代理、截图静态质量 |
| `src/app/main.cpp` | `kQualityUpgradeTimerId` 重绘、`--force-quality`/`--proxy`、`quality*`/`proxy*` dump 字段、动画播放标志 |
| `tests/ui_layout_tests.cpp` | `TestDrawQuality`：像素/静止/交互/动画/代理门槛/内存上限 |
| `tests/run_quality_check.ps1` | 截图/参考图/像素块/日志/dump 验收（57 项） |
| `tests/tools/quality_compare.py` | 与 Pillow LANCZOS 参考比较（mean abs diff + ±2 px 对齐搜索） |
| `tests/tools/make_quality_sample.py` | 生成 8K 高频细节样本（3 px 条纹 + 11 px 横条） |

---

## 2. 插值策略

```text
pixel_mode 且 scale >= 1.0        → NEAREST_NEIGHBOR   (kPixel)
正在平移/缩放/拖窗口（150 ms 内）   → LINEAR             (kFast)
动画播放中（未暂停）                → LINEAR             (kFast)
其他（静止）                       → HIGH_QUALITY_CUBIC (kGood)
```

- `scale` 是**物理像素/源像素**（`Renderer::EffectiveScale()`）；旋转后仍按屏幕缩放
  判断，不按源宽高（坑 12）。
- 像素模式低于 100% 仍走 CUBIC：缩小时最近邻会严重锯齿（坑 4）。
- 像素模式在交互中也不降级：≥100% 本来就没有可插值的内容。
- 交互窗口：`MarkInteractive()` 记录 `QPC + 150 ms` 并在窗口上挂一次性
  `kQualityUpgradeTimerId`（30）；定时器到点只 `InvalidateRect`，质量由时间戳决定，
  所以连续滚轮不会把升级“一直往后推”（坑 1/2）。
- 动画：`Renderer::SetAnimating()` 由 `WM_PAINT` 从 `AnimationController::playing()`
  同步；暂停后下一次重绘即升级（坑 9）。

## 3. 显示代理（1/4）

- 条件（`ShouldUseProxy`）：非像素模式、`scale < 0.5`、源像素 > 4×内容像素、
  开启代理。8K fit（0.141）满足；1600×1200 fit（0.508）不满足。
- 尺寸固定 **1/4**（`ProxyEdge`），8K = 1920×1080×4 ≈ **7.9 MB**；**禁止 1/2**
  （33 MB 会把 8K 峰值推向 500 MB 上限）。
- 生成：GPU 单次 `DrawBitmap(...HIGH_QUALITY_CUBIC...)` 到 `ID2D1Bitmap1` 目标，
  不复制源像素、不占后台线程；实测 **7.5 ms**，只在第一张符合条件的静止帧烘一次。
- **只在 `kFast`（交互/动画）帧使用**；静态帧仍用原图 + CUBIC。
  理由（实测，见 §5）：代理会二次重采样，静态误差 `0.235` 比直接 CUBIC `0.208`
  更差；把它留给不判画质的交互帧，既省采样又不牺牲静止画质。
- 失效：切图/`ClearImage`（`ReleaseProxy`）、`--proxy off`；旋转、窗口尺寸、
  DPI、全屏都**不需要**重建（代理按源尺寸，绘制目标矩形不变，坑 6/8）。
- 生成失败不致命：记 `render: proxy bake failed`，继续画原图。

## 4. 勾子与 dump

| 参数 / 字段 | 说明 |
| --- | --- |
| `--force-quality fast\|good\|pixel\|auto` | 固定插值（截图/A-B）；`auto` 恢复策略 |
| `--proxy on\|off`、`--proxy=off` | 关闭代理做对照（日志不再出现 proxy 分配） |
| `quality=` | 当前策略结果（dump 时刻） |
| `quality_last=` | 最后一帧实际用的质量 |
| `quality_forced=` | `auto` 或强制值 |
| `interactive=` / `animating=` | 交互窗口是否仍有效 / 动画是否在播 |
| `proxy_enabled=` / `proxy_ready=` / `proxy_active=` | 开关 / 已烘 / 本帧是否用代理 |
| 日志 | `render: quality=<q> proxy=<0/1> scale=<s>`（只在变化时写）、`renderer: proxy WxH bgra=..MB bake=..ms` |

`--send-keys` 模拟按键期间只使用模拟修饰键：真实键盘的 Ctrl/Shift/Alt 不会改变
按键含义（否则用户在旁边按 Ctrl 会让 `+` 变成 Ctrl+plus 而静默失败）。

**截图**（`RenderAndCapture`）使用“交互/动画都已结束”的自动策略：普通图强制
`kGood`，像素模式 ≥100% 时仍拍 NEAREST。`--force-quality` 优先，便于拍 fast/pixel。
这样截图与用户静止看到的画面一致（坑 10），且像素模式截图不会退化成 CUBIC。

## 5. 验收数字（本机 125% DPI，1280×800 客户区，8K 7680×4320 → 1084×610）

| 场景 | linear（旧） | cubic（新） | 说明 |
| --- | --- | --- | --- |
| `8k.jpg`（真实照片） | mean_abs_diff **0.2235** | **0.1895** | 对 Pillow LANCZOS 参考，改善 **15.2%** |
| 高频 8K 样本（3 px 条纹） | **8.0934** | **0.2431** | 改善 **97.0%**（linear 混叠） |

- 高频样本用来证明“真的换成了高质量滤波”；真实照片本身细节较少，改善 15% 是
  诚实数字，不再宣称 30%。
- 截图：`auto == --force-quality good`（SHA-256 相同）；`fast` 不同。
- 像素模式：放大后中心 40×40 只有 **2** 种颜色；同位置 CUBIC 有 130+ 种。
- 8K 内存：`run_8k_check` private **363 MB** / ws **214 MB**（限值 480/360），
  比改造前约 +8 MB，正是 1/4 代理。
- 首图：代理烘一次 ~7.5 ms，在启动方差（560–790 ms）内测不出差异；
  交互路径只替换采样源，没有新增 CPU 工作。

## 6. 坑

| # | 现象 | 原因 | 对策 |
| --- | --- | --- | --- |
| 1 | 平移掉帧 | 每帧 CUBIC | 交互期 LINEAR + 150 ms 后升级 |
| 2 | 停止后“闪一下变清楚” | 两次绘制插值不同 | 可接受；只换插值不改布局/位置 |
| 3 | 像素模式放大仍模糊 | 没切 NEAREST | 按 `scale >= 1` 切，由 `SelectDrawQuality` 单测钉住 |
| 4 | 像素模式缩小锯齿 | 一直 NEAREST | `< 1` 用 CUBIC |
| 5 | 8K fit 仍糊 | 双线性采样不足 | CUBIC；代理只用于交互帧 |
| 6 | 代理/原图切换跳 | 两个矩形取整不同 | 代理与源共用同一 dest 矩形，只换源位图 |
| 7 | 代理内存超预算 | 1/2 是 33 MB | 固定 1/4；单测 + 日志 + `run_8k_check` 三道断言 |
| 8 | 切图后仍画旧代理 | 忘了失效 | `SetImage`/`ClearImage` 统一 `ReleaseProxy()` |
| 9 | 动画每帧 CUBIC 掉帧 | 播放也高质量 | `SetAnimating(true)` 走 fast，暂停升级 |
| 10 | 截图与屏幕不一致 | 截图时处于交互态 | 截图用静态策略（像素模式仍 NEAREST） |
| 11 | 高 DPI “100%”不是物理像素 | 用 DIP 算 scale | 仍用 `ContentRect()` 物理像素 |
| 12 | 旋转后判断错 | 用源宽高 | 用 `display_width/height`（`EffectiveScale`） |
| 13 | 旧驱动 CUBIC 慢 | GPU 差异 | `--force-quality fast` 可现场降级；设置页开关（可选）未做 |
| 14 | 透明图边缘发灰 | 预乘 alpha + 插值 | 源位图本来就是 `PREMULTIPLIED`，代理目标也用同格式 |
| 15 | 代理阻塞 UI | UI 线程做 CPU 缩放 | 走 GPU 烘一次（7.5 ms），不做 CPU 全图重采样 |
| 16 | 脚本按 `+` 没反应 | 用户在旁边按 Ctrl，`GetKeyState` 把 `+` 变成 Ctrl+plus（kNone） | `--send-keys` 期间只用模拟修饰键（`g_test_sending_key`），`WM_KEYUP` 清状态 |

## 7. 明确不做

- 自定义 Lanczos 着色器、CPU 全图 Lanczos、超分、HDR 显示管线。
- “快速渲染”设置项（计划里的 `fast_render`，可选）：本阶段用
  `--force-quality fast` 作为排障/低端机验证入口，不进设置页，避免多一个
  全局状态；如果后续要做，只加一个 `Settings.fast_render` 让
  `SelectDrawQuality` 在非交互帧也返回 `kFast`。
