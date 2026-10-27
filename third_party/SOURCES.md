# 第三方组件来源

仓库里的 `third_party/lib/*.lib` 是上游开源项目用 MSVC Release（`/MD`）编译出的静态库。
对应的**完整源代码**可从下列上游地址按版本获取；如上游地址失效，可提 issue 索取，
本项目承诺在收到请求后提供对应版本的源码副本。

| 组件 | 版本 | 上游源码 | 校验参考 |
| --- | --- | --- | --- |
| FreeImage | 3.18.x | https://freeimage.sourceforge.io/download.html | `licenses/FreeImage-FIPL-1.0.txt` |
| LibRaw | 0.21.5 | https://www.libraw.org/download | `licenses/LibRaw-LGPL-2.1.txt` |
| libjpeg (IJG) | 9c | https://www.ijg.org/files/ | `licenses/LibJPEG-IJG.txt` |
| zlib | 1.2.11 | https://zlib.net/fossils/ | `licenses/zlib-1.2.11.txt`；`zlib/` 已含源码 |

`third_party/zlib/` 直接包含 zlib 1.2.11 源码（只编译 inflate 相关文件）。
`third_party/FreeImage/` 只含头文件，`third_party/LibRaw-0.21.5/` 只含头文件；
若需要在本仓库内完整重建这两个库，请按上表下载源码后用同一工具链编译。
