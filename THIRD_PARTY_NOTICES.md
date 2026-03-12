# Third-Party Notices

This repository contains third-party code and assets used by `3dslibris`.

## Source code dependencies (vendored)

### Expat XML parser
- Location: `source/expat/*`, `include/expat*.h`
- Upstream: [Expat](https://libexpat.github.io/)
- License: MIT
- Notes: vendored parser used for XML parsing (EPUB/FB2/preferences).

### minizip / unzip
- Location: `source/core/ioapi.c`, `source/core/unzip.c`, `include/minizip/*`
- Upstream: minizip (zlib contrib)
- License: zlib
- Notes: ZIP container access for EPUB and ODT.

### stb_image
- Location: `include/stb_image.h`, `source/core/stb_image_impl.cpp`
- Upstream: [nothings/stb](https://github.com/nothings/stb)
- License: public domain
- Notes: image decoding for covers and inline images.

## Runtime/toolchain libraries (provided by devkitPro)

These are linked from toolchain/portlibs and are not vendored in this repository:
- `libctru`
- `freetype`
- `libpng`
- `zlib`
- `libbz2`

Refer to devkitPro package metadata for their exact license texts.

## UI icons

### Tabler Icons
- Source vectors: `assets/ui/icons/svg/*`
- Raster derivatives: `data/ui/icons/png/*`, `resources/ui/icons/*`, `gfx/ui_icons/*`
- Upstream: [Tabler Icons](https://tabler-icons.io/)
- License: MIT

## Project origin and attribution

`3dslibris` is derived from `dslibris` (Nintendo DS ebook reader) by Ray Haleblian.
The current 3DS port and subsequent modifications are maintained by Rigle.
See [AUTHORS.md](AUTHORS.md) for details.
