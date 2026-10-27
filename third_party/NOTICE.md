# Third-party notice

NewViewer links the components below. They are vendored under `third_party/` so
the build is self-contained. Release packages copy the license texts from
`third_party/licenses/` into `THIRD_PARTY_LICENSES/`.

| Component | Version | License | Files |
| --- | --- | --- | --- |
| FreeImage | 3.18.x | GPLv3 (dual: FIPL / GPLv2+) (`licenses/FreeImage-FIPL-1.0.txt`) | `FreeImage/FreeImage.h`, `lib/freeimage.lib` |
| libjpeg (IJG) | 9c | IJG license (`licenses/LibJPEG-IJG.txt`) | `lib/libjpeg.lib` |
| LibRaw | 0.21.5 | LGPL-2.1 (used) or CDDL-1.0 (`licenses/LibRaw-LGPL-2.1.txt`, `licenses/LibRaw-CDDL-1.0.txt`) | `LibRaw-0.21.5/libraw/*.h`, `lib/libraw.lib` |
| zlib | 1.2.11 | zlib license (`licenses/zlib-1.2.11.txt`) | `zlib/*` |

## Provenance

`lib/freeimage.lib`, `lib/libraw.lib` and `lib/libjpeg.lib` are static archives
built from the upstream open-source releases listed above with the MSVC Release
toolchain (`/MD`). The digests at vendoring time were:

```text
d64d87243b49f7adfe96b3826eb2360ef6032c6baa5e5e6a05b8fb4ef3522c95  lib/libjpeg.lib
04b1035c12fdab1d8ba10376d7424b949d577db4cac19d01ea281515683f87ec  lib/freeimage.lib
63276dd6ef44becafa9796a49ee770e75d3c8b191a6b1d5413f9350b8ac98832  lib/libraw.lib
ac4484eb0bf18b29f12f0bbe599ad694c72673cd582c0e89dc6ffd62f4c22227  FreeImage/FreeImage.h
```

`FreeImage.h` matches `freeimage.lib`; the LibRaw headers match the 0.21.5
archive. `zlib/` is the 1.2.11 source tree; NewViewer compiles only the
inflate-side files for the PSD ZIP codec (`CMakeLists.txt` -> `newviewer_zlib`).

`tools/make_sbom.ps1` reads these versions and hashes when it generates the
CycloneDX SBOM, so update this file and the license bundle together whenever a
dependency changes.

> **Rebuild note**: the archives are prebuilt binaries. To make the tree fully
> reproducible from source, build FreeImage / LibRaw / libjpeg from their
> upstream releases with the same flags and replace the three `.lib` files
> (digests above change with the toolchain).

## Distribution notes

* FreeImage is statically linked under its **GPLv3** option, which matches this
  program's GPL-3.0. Ship `FreeImage-FIPL-1.0.txt` with every binary.
* LibRaw is LGPL-2.1; its section 3 permits relicensing under GPLv2-or-later, so
  it is compatible with GPL-3.0. Ship `LibRaw-LGPL-2.1.txt`.
* zlib and libjpeg only require attribution and their license texts.

The prebuilt archives were produced with the Release CRT (`/MD`). Debug builds
therefore disable FreeImage and LibRaw (`tools\build.ps1 -NoFreeImage`); ASan
uses `RelWithDebInfo`.
