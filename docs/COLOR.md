# 色彩管线与色彩回归（V3-06）

> 实现：`decoders/wic_decoder.cpp`（WIC + ICC）、`decoders/psd_decoder.cpp`（CMYK/索引/灰度）、
> `core/icc_profile.*`（头解析）。
> 测试：`tests/run_color_check.ps1`（15 项）、`tests/run_icc_check.ps1`、`tests/run_orientation_check.ps1`、
> `zoreview_formattests`（PSD 参考图）。
> 相关计划：`规划/25-v3.0-加载链路与目录监视-方案.md` §9。

---

## 1. 输出与转换顺序

所有解码器统一输出 **BGRA8**（`ImageInfo.stride = width × 4`，当前样本均为紧凑 stride）。

1. **EXIF 方向在解码阶段烘焙**（`image_transform.*`），`ImageInfo.width/height` 是直立尺寸；
2. **ICC**：只有 WIC 路径与 PSD 路径做“非 sRGB → sRGB”转换；FreeImage 路径（HDR/EXR/PFM/WBMP/MNG、
   WIC 失败位图）**不转 ICC**（文档口径，见 HANDOFF §1 的更正）；
3. **PSD 色彩模式**：RGB/RGBA/灰度/位图/索引/CMYK（含专色通道忽略）由自研解码器转换，
   与 Pillow 生成的 `images/psd/reference/*.png` **逐像素一致**（max diff = 0）；
4. **CMYK JPEG**：由 WIC 的 `IWICFormatConverter` 转成 BGRA；我们的代码看不到 CMYK 平面
   （见 §3）。

## 2. 回归断言（`tests/run_color_check.ps1`）

脚本只调用 `zoreview_cli --decode`（与产品同一条 `FormatRouter::Decode` 路径），把 BGRA 落到
临时文件，再用脚本内联的 C# helper 统计全图像素（PowerShell 逐像素循环对 1600×1000 太慢）。

| 样本 | 断言 | 实测 |
| --- | --- | --- |
| PSD CMYK（ZIP）| 走 PSD 解码器；vs 参考 max ≤ 1 | max **0** |
| PSD CMYK + 3 专色 | 同上 | max **0** |
| PSD 索引色 | 同上 | max **0** |
| PSD 灰度 | 同上 + R=G=B | max **0**，通道最大差 **0** |
| APNG 索引+alpha（首帧）| vs `palette4_f0.png` max ≤ 1 | max **0** |
| PNG 索引调色板 | vs 量化前 RGB：平均通道差 ≤ 10、非灰比例差 ≤ 0.05 | **3.61** / **0.0035** |
| CMYK JPEG | 尺寸一致；非灰 ≥ 0.5；luma 平均差 ≤ 18；通道相关 r≥0.9/g≥0.85/b≥0.7 | 0.833 / **11.67** / **0.99/0.91/0.78** |

> 阈值是“守住结构、不锁死实现”的口径，不是色度学精度承诺。索引 PNG 的 256 色量化本身有损，
> 所以用平均差；CMYK JPEG 的阈值与 WIC 的默认转换绑定（见 §3，换解码方式时这些数字应变好）。
> PSD CMYK 的 max=0 只证明与参考**同一公式**（HANDOFF 坑 64/322）：参考和我们的解码都用
> `(255-C)(255-K)/255`，所以 0 是“没回归”，不是“与 Photoshop ICC 一致”。

## 3. CMYK JPEG 的事实（V3-06 测量）

样本 `images/jpeg/photo_cmyk.jpg` 与 `photo_1600.jpg` 来自同一张合成源图
（`tools/make-synthetic.py`：RGB → Pillow `convert("CMYK")` → JPEG，Adobe APP14 `transform=0`，
无 ICC）。Windows 的 WIC 在格式转换阶段就把它变成 BGR（我们的代码拿到的是 BGR32），
其 CMYK→RGB 结果与 Pillow 的朴素逆变换不同：

| 探针（solid patch） | WIC（本机 Win11 26300） | Pillow（libjpeg 朴素逆变换） |
| --- | --- | --- |
| K=100% | 24,23,23 | 0,0,0 |
| C=100% | 0,158,213 | 0,255,255 |
| M=100% | 231,37,130 | 255,0,255 |

对整张 `photo_cmyk.jpg`：与 RGB 同源图的平均通道差 **20.21**、luma 平均差 **11.67**、
逐通道相关 **r=0.99 / g=0.91 / b=0.78**。**结论**：这是 WIC 的“通用印刷 CMYK→sRGB”解释，
不是通道/取反类解码错误（结构、亮度、通道归属都对），但**不是** Pillow 那种朴素逆变换的像素值；
本轮不改产品解码路径。若以后需要色度学准确或不依赖 WIC 的 CMYK 行为，得自建 CMYK→RGB
（嵌入 ICC 优先、否则明确选择朴素逆变换或指定印刷 profile），属于功能面决策。

## 4. 手动复核

```powershell
# 15 项色彩断言（不需要 GUI；CLI 路径）
powershell -ExecutionPolicy Bypass -File tests\run_color_check.ps1

# ICC（线性 gamma / sRGB no-op / alpha / EXIF 6）
powershell -ExecutionPolicy Bypass -File tests\run_icc_check.ps1

# PSD 参考图（115 checks，含 cmyk/indexed/gray/16/32 位）
.\build\Release\zoreview_formattests.exe
```

ASan 下用 `-Cli build-asan\RelWithDebInfo\zoreview_cli.exe` 指向 ASan CLI。

## 5. 坑

| # | 现象 | 原因 | 对策 |
| --- | --- | --- | --- |
| 1 | PowerShell 里 `Compare` 返回 `$null`/差异对象 | `compare` 是 `Compare-Object` 的内置别名，别名优先级高于函数 | 自定义函数不要叫 `Compare`（脚本用 `Compare-Images`） |
| 2 | 1600×1000 的像素统计跑几分钟 | PowerShell 逐字节循环 | 脚本内联 C#（`Add-Type`）统计 max/mean/luma/correlation |
| 3 | 含中文路径的 `.ps1` 在 PS 5.1 下读成乱码 | 无 UTF-8 BOM，按 ANSI 读（坑 60/136/306） | 保存时必须带 BOM（`run_color_check.ps1` 已带） |
| 4 | 把 CMYK JPEG 当“颜色错了”去查解码器 | WIC 在格式转换阶段就完成 CMYK→BGR，且用通用印刷 profile | 断言结构/亮度/通道，不锁像素；要精确需自建 CMYK 路径 |
