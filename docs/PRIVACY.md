# ZoreView 隐私说明

更新日期：2026-10-23

ZoreView 是一个本地图片查看器。**它不联网、不收集遥测、不含广告、不做用户画像。**

## 不收集的数据

- 不发送图片、缩略图、文件名、路径、使用记录或设备信息。
- 没有账号系统、云同步、在线更新检查或崩溃上报服务。
- 程序内没有 HTTP/HTTPS 客户端代码；HEIC/AVIF/SVG 解码也不访问网络。

## 本地保存的数据

以下数据只保存在本机，用于让下次启动更快/保持设置：

| 数据 | 位置（安装版） | 用途 |
| --- | --- | --- |
| 设置 | `%LOCALAPPDATA%\ZoreView\settings.ini` | 主题、语言、窗口状态 |
| 日志 | `%LOCALAPPDATA%\ZoreView\logs\zoreview.log` | 排障；含文件路径与解码耗时 |
| 崩溃转储 | `%LOCALAPPDATA%\ZoreView\crashes\` | 崩溃时生成 minidump |
| 图像/缩略图缓存 | `%LOCALAPPDATA%\ZoreView\cache\` | 加速再次打开 |
| 壁纸文件 | `%LOCALAPPDATA%\ZoreView\wallpaper\wallpaper.bmp` | Windows 桌面壁纸引用 |

便携版把上述内容放在 `zoreview.exe` 同目录。删除对应目录即可清除；
缓存和日志可以随时删除，程序会重新生成。

## 注册表

只有在你显式执行安装或 Shell 集成命令时写入，全部在当前用户范围内
（全机安装才写 HKLM）：

- `HKCU\Software\Classes\CLSID\{7B2E5C1A-...}`：缩略图处理器
- `HKCU\Software\Classes\SystemFileAssociations\<ext>\shell\ZoreView.*`：右键动词
- `HKCU\Software\Classes\ZoreView.Image`、`OpenWithProgids`：可选文件关联
- `HKCU\Software\ZoreView\Install`、卸载项：安装信息
- 关联备份：`HKCU\Software\ZoreView\Associations\Backup\<ext>`

卸载会删除这些项；文件关联只在当前默认仍指向 ZoreView 时恢复原值。

## 第三方组件

解码使用 FreeImage、LibRaw、libjpeg、zlib 和 Windows 自带的 WIC/Direct2D。
这些库在本地运行，不包含网络上报逻辑。许可证与版本见 `NOTICE.md`、
`THIRD_PARTY_LICENSES\` 和 `SBOM.cdx.json`。

## 联系

本项目没有远程支持通道。反馈问题时请自行决定是否附上日志；日志可能包含
本地文件路径，发送前请先检查/脱敏。
