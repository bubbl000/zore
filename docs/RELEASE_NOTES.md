# ZoreView 0.3.0 发布说明

发布状态：**发布候选（Release Candidate）**  
日期：2026-10-23  
平台：Windows 10 1809+ / Windows 11，x64

## 本版重点

0.3.0 在 0.2.0-m3 的功能基础上完成发布工程：

- **版本资源**：`zoreview.exe`、`zoreview_cli.exe`、`ZoreViewThumb.dll`
  带 Win32 VERSIONINFO，文件属性/签名界面/安装信息显示同一版本号。
- **许可证与 NOTICE**：GPL-3.0 `LICENSE`、项目 `NOTICE`、以及
  FreeImage FIPL、libjpeg IJG、LibRaw LGPL/CDDL、zlib 的完整文本。
- **SBOM**：每个发布包带 `SBOM.cdx.json`（CycloneDX 1.5），包含供应商、
  版本、purl、许可证、哈希、依赖关系、作者与时间戳。
- **完整性**：`SHA256.txt` 覆盖包内所有文件；zip 另有 `.sha256`；
  `release-manifest.json` 记录文件哈希/字节数/签名信息；
  `Verify.cmd`/`Verify.ps1` 一键校验，`Install.cmd` 安装前强制执行。
- **代码签名**：`tools\sign.ps1` 与 `package.ps1 -Sign` 支持 PFX/证书存储、
  SHA-256、RFC 3161 时间戳；`-RequireSignature` 阻止未签名包发布。
- **文档**：`README.md`（帮助）、`PRIVACY.md`、`CHANGELOG.md`、
  `SIGNING.md`、`RELEASE_CHECKLIST.md`。

## 功能摘要

- 格式：JPEG/PNG/BMP/GIF/WebP/TIFF/ICO/CUR、RAW（CR2/CR3/NEF/ARW/RAF/ORF/
  RW2/PEF/SRW/DNG）、PSD/PSB、HEIC/HEIF、AVIF、SVG、TGA/PCX/PNM/XBM/XPM、APNG。
- 显示：D3D11 + Direct2D、深色/浅色、透明棋盘格、缩放/平移、缩略图条。
- 性能：热启动首帧约 200 ms；8K 单张峰值约 333 MB，连续翻页约 460 MB；
  1200 文件目录扫描约 5 ms。
- 图像处理：EXIF 方向烘焙、ICC→sRGB、GIF/APNG 播放、多页 TIFF 首帧。
- 文件操作：另存为、复制图像/路径、回收站删除、重命名、桌面壁纸。
- Shell：资源管理器缩略图（41 个扩展名）、右键 3 动词、可选默认关联、
  用户级/全机安装、便携版。
- 可访问性：中/英双语、高对比度、100–200% DPI、UIA 标题通知。

## 安装

1. 下载 `Zore-0.3.0-installer.zip` 和对应的 `.sha256`，核对 zip 哈希。
2. 解压后先运行 `Verify.cmd`，确认全部文件通过校验。
3. 运行 `Install.cmd`（当前用户，无需管理员），按提示选择是否设为默认查看器。
4. 便携用户解压 `Zore-0.3.0-portable.zip` 后直接运行 `zoreview.exe`。

发布包应带有效的 Authenticode 签名（`Verify.cmd` 会显示提示；生产发布要求
`SIGNATURES.txt` 中 `signed=true` 且签名状态为 Valid）。

## 已知限制

- Direct2D SVG 不渲染 `<text>`、部分滤镜/字体；隔行 APNG 只显示首帧。
- HEIC/HEIF、AVIF 依赖 Microsoft Store 的系统解码扩展。
- 多页 TIFF 只显示第一页；PSD/PSB 只显示文件内合并图。
- LibRaw 静态链接的 LGPL 再链接义务仍需法务确认（见项目发布检查清单）。
- 混合格式长时间浏览的句柄上限（D3D/D2D/Media Foundation 线程池）仍需 8 小时
  长跑确认；缩短版 3000 次内存稳定。
- 自定义重命名对话框的子控件在 UIA 中 ControlType 为 Pane（名称可用）。

## 验证与回滚

```powershell
# 完整性 / SBOM / 许可证 / 签名
powershell -ExecutionPolicy Bypass -File Verify.ps1 -PackageDirectory . -VerifySignature -RequireSignature

# 卸载（保留用户图片与设置）
"%LOCALAPPDATA%\Programs\ZoreView\zoreview.exe" --uninstall
```

回滚：保留上一版本 zip 与 `SHA256.txt`；卸载当前版本后重新解压/安装上一版本。
文件关联在卸载时仅当默认仍指向 ZoreView 才恢复原值；如需手动恢复，使用
`HKCU\Software\ZoreView\Associations\Backup\<ext>` 中的备份。
