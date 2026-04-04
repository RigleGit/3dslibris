# Dependencies

Versions of all third-party components used in 3dslibris. Update this file when bumping any dependency.

## Vendored source (compiled by Makefile)

| Dependency | Version | Location | License | Purpose |
|------------|---------|----------|---------|---------|
| Expat | 2.7.5 | `third_party/expat/` | MIT | XML parsing (EPUB, FB2, ODT, prefs) |
| utf8proc | 2.11.3 | `third_party/utf8proc/` | MIT | Unicode normalization, case folding |
| libunibreak | 6.1 | `third_party/libunibreak/` | zlib | Line breaking algorithm |
| stb_image | 2.27 (2021-07-11) | `third_party/stb/stb_image.h` | MIT-0 | Image decoding (covers, inline images) |
| MuPDF | 1.27.2 | `third_party/mupdf/` | AGPL-3.0+ | PDF/CBZ/XPS rendering |
| minizip | 1.1 (2010-02-14) | `third_party/minizip/` | zlib | ZIP archive access (EPUB, ODT) |

## System (devkitPro portlibs)

Versions determined by devkitPro portlibs release channel; no pinned version in repo. Check `$DEVKITPRO/portlibs/3ds/` for installed versions.

| Dependency | Location | License | Purpose |
|------------|----------|---------|---------|
| FreeType 2 | devkitPro portlib | FTL / GPLv2+ | Font rasterization |
| libpng | devkitPro portlib | zlib | PNG decoding |
| zlib | devkitPro portlib | zlib | Compression (minizip dependency) |
| libctru | devkitPro SDK | zlib | 3DS system library |
| bzip2 | devkitPro portlib | BSD-4 | BZ2 decompression |

## Build tools

| Tool | Purpose |
|------|---------|
| devkitARM | ARM11/ARM9 cross-compilation toolchain |
| makerom | CIA packaging (built from source in Docker) |
| bannertool | CIA banner/icon (built from source in Docker) |
| tex3ds | 3DS texture conversion |
| bin2s | Binary-to-assembly conversion |

## Notes

- MuPDF is built with a custom minimal configuration via `scripts/build_mupdf_minimal.sh`
- All vendored dependencies are compiled as part of the project Makefile; no precompiled binaries are shipped in `lib/`
- The Docker image (`docker/Dockerfile.cia`) is based on `devkitpro/devkitarm`
