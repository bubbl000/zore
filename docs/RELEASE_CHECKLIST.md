# 发布检查清单

本清单是 ZoreView 公开发布前的门槛。任何一项未完成都不应标记为正式版。

## 1. 构建与测试

- [ ] `tools\build.ps1 -Clean` 干净构建通过，无警告（`/W4 /WX`）。
- [ ] `tests\run_format_matrix.ps1` 全过（当前 160/160，含 RAW/PSD/HEIC/AVIF/SVG/P2/损坏样本）。
- [ ] `zoreview_tests.exe`、`zoreview_formattests.exe`、`zoreview_filetests.exe` 全过。
- [ ] `tests\run_shell_thumb_check.ps1` 35/35，注册表清理干净。
- [ ] `tests\run_shell_verbs_check.ps1` 19/19，无动词残留。
- [ ] `tests\run_install_check.ps1` 71/71，无安装/关联残留。
- [ ] `tests\run_ui_check.ps1` 全过（DPI/语言/高对比度/UIA）。
- [ ] ASan：`tools\build.ps1 -ASan` + `tests\run_asan_check.ps1` 0 报告。
- [ ] Debug 泄漏：`tools\build.ps1 -NoFreeImage` + `zoreview_leaktests.exe --rounds 5` 预热后 0 增长。
- [ ] 性能：`run_startup_samples.ps1 -Iterations 8` 首帧中位 ≈200 ms；8K 检查通过。
- [ ] 8 小时混合目录长跑：0 崩溃，内存走平，句柄不再增长（当前仍是待办项）。
- [ ] `tests\run_release_check.ps1` 全过（含打包/校验/SBOM/签名测试）。

## 2. 版本与资源

- [ ] `src/core/version_numbers.h` 的版本已确认，且与发布说明一致。
- [ ] `zoreview.exe`/`ZoreViewThumb.dll` 文件属性显示版本、产品名、版权。
- [ ] `VERSION.txt` 与 exe 版本一致。

## 3. 许可与合规

- [ ] `LICENSE`（GPL-3.0）与 `NOTICE`、`THIRD_PARTY_LICENSES/` 一致。
- [ ] `THIRD_PARTY_LICENSES\` 包含 FIPL、IJG、LGPL-2.1、CDDL-1.0、zlib 全文。
- [ ] FreeImage FIPL：静态链接随包分发许可文本。
- [ ] **LibRaw LGPL 法务审查**：静态链接的再链接义务（提供目标文件/源码获取方式
      或改为动态链接）已由法务确认并记录；未确认前不能对外宣称 LGPL 已清。
- [ ] `SBOM.cdx.json` 与 `third_party/NOTICE.md` 的版本/哈希一致。
- [ ] 未引入未列出的新依赖；图标/字体/样本许可已核对。

## 4. 签名与完整性

- [ ] 用正式 OV/EV 证书签名：`package.ps1 -Sign -RequireSignature`。
- [ ] `SIGNATURES.txt` 为 `signed=true`，两个二进制状态 Valid，含时间戳。
- [ ] `verify_package.ps1 -VerifySignature -RequireSignature` 通过。
- [ ] 在干净机器上重新下载 zip，核对 `.sha256`，运行 `Verify.cmd`。
- [ ] SmartScreen/Defender/主流杀软复核（首次运行提示已记录）。

## 5. 发布产物

- [ ] `tools\package.ps1 -Mode portable` 与 `-Mode installer` 均生成 zip + `.sha256`。
- [ ] zip 内包含 `README.md`、`PRIVACY.md`、`CHANGELOG.md`、`RELEASE_NOTES.md`、
      `LICENSE`、`NOTICE.md`、`SBOM.cdx.json`、`Verify.ps1`/`Verify.cmd`。
- [ ] 便携包 `--print-paths` 显示 portable=1、data=包目录。
- [ ] 安装包安装/卸载/关联恢复在干净用户配置下复测。
- [ ] 发布包不含 PDB、测试数据、源码、`tests\output`、临时证书。
- [ ] 备份上一版本 zip + 哈希，记录回滚步骤。

## 6. 文档与支持

- [ ] `README.md`（HELP）格式/快捷键/已知限制与当前构建一致。
- [ ] `PRIVACY.md` 说明“不联网、不采集”。
- [ ] `RELEASE_NOTES.md` 列出新功能、修复、已知问题。
- [ ] 日志路径、错误码、反馈方式可查。
- [ ] 发布页附 zip 哈希、签名指纹、SBOM 哈希。

## 7. 发布记录

在发布记录中保存：

```text
version:
build date:
git/tree identifier (if any):
zip sha256 (portable):
zip sha256 (installer):
signer subject / thumbprint:
SBOM sha256:
test report location:
known issues:
```
