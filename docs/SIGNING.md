# 代码签名指南

ZoreView 的公开版本必须用受信任的 Authenticode 证书签名，否则 SmartScreen
会拦截下载、部分杀软会误报，资源管理器加载缩略图 DLL 也可能受影响。

## 证书

- **正式发布**：从公共 CA 购买 OV 或 EV Code Signing 证书（DigiCert、Sectigo、
  GlobalSign 等）。EV 证书可更快建立 SmartScreen 信誉；OV 也可用，但需要时间。
- **不要**用自签名证书做正式分发；自签名只用于本仓库的自动化测试
  （`tests\run_release_check.ps1 -TestSigning`）。
- 证书私钥不要提交到仓库。CI 中用密钥库/环境变量注入 PFX 与密码。

## 签署二进制

```powershell
$env:NV_SIGN_PASSWORD = '<pfx-password>'

powershell -ExecutionPolicy Bypass -File tools\sign.ps1 `
  -Path build\Release\zoreview.exe, build\Release\ZoreViewThumb.dll `
  -CertificatePath C:\keys\zoreview.pfx `
  -TimestampUrl http://timestamp.digicert.com `
  -Verify
```

或使用证书存储中已安装的证书：

```powershell
powershell -ExecutionPolicy Bypass -File tools\sign.ps1 `
  -Path build\Release `
  -CertificateThumbprint <40-hex-thumbprint> `
  -Verify
```

`sign.ps1` 使用 `signtool sign /fd SHA256 /tr <url> /td SHA256`：SHA-256 摘要 +
RFC 3161 时间戳，签名在证书过期后仍有效。`signtool.exe` 从 PATH 或 Windows SDK
自动查找，也可用 `NV_SIGNTOOL` 指定。

## 打包时签名（推荐）

`package.ps1` 在复制完文件后、生成 SBOM/校验和之前签名，因此发布包里的哈希
对应已签名的二进制：

```powershell
$env:NV_SIGN_PASSWORD = '<pfx-password>'
powershell -ExecutionPolicy Bypass -File tools\package.ps1 -Mode installer `
  -Sign -CertificatePath C:\keys\zoreview.pfx `
  -TimestampUrl http://timestamp.digicert.com `
  -RequireSignature
```

- `-RequireSignature`：没有签名或签名校验失败时打包失败。
- `SIGNATURES.txt` 记录每个二进制的状态、签名者、指纹、时间戳。
- `release-manifest.json` 记录 `signed` 与签名者信息。

## 验证

```powershell
# 包完整性 + SBOM + 许可证 + 签名（要求受信任）
powershell -ExecutionPolicy Bypass -File tools\verify_package.ps1 `
  -PackageDirectory dist\ZoreView-0.3.0-installer `
  -VerifySignature -RequireSignature

# 直接看签名
Get-AuthenticodeSignature dist\ZoreView-0.3.0-installer\zoreview.exe |
  Format-List Status, StatusMessage, SignerCertificate
```

`signtool verify /pa /v <file>` 可用于人工复核；`/pa` 表示使用 Authenticode
策略而不是驱动策略。

## 测试签名（自动化）

`tests\run_release_check.ps1 -TestSigning` 会：

1. 用 `New-SelfSignedCertificate -Type CodeSigningCert` 生成临时证书；
2. 导出 PFX，对包内副本执行 `sign.ps1`（不带时间戳，避免联网）；
3. 验证签名者存在、`Get-AuthenticodeSignature` 能读到证书；
4. 删除临时证书和 PFX。

这条路径只证明签名工具链可用，**不证明证书受信任**。正式发布前必须在有
真实证书的机器上跑一次 `-VerifySignature -RequireSignature`。

## 发布后检查

- 在干净的 Windows 10/11 虚拟机上从下载链接重新下载，运行 `Verify.cmd`
  并确认签名状态为 Valid。
- 检查 SmartScreen：首次运行可能仍提示“更多信息 → 仍要运行”，EV 证书或
  积累下载信誉后会消失。
- 把最终的 zip 哈希、签名指纹、SBOM 哈希写进发布记录。
