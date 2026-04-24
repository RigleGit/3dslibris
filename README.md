<h1>
  <img src="assets/release/icon-64x64.png" alt="3dslibris icon" width="64" />
  3dslibris
</h1>

[![Release](https://img.shields.io/github/v/release/RigleGit/3dslibris?label=release)](https://github.com/RigleGit/3dslibris/releases)
[![CI](https://img.shields.io/github/actions/workflow/status/RigleGit/3dslibris/ci.yml?branch=main&label=ci)](https://github.com/RigleGit/3dslibris/actions/workflows/ci.yml)
[![Ko-fi](https://img.shields.io/badge/Ko--fi-rigle-FF5E5B?logo=kofi&logoColor=white)](https://ko-fi.com/rigle)
[![Universal-Updater](https://img.shields.io/badge/Universal--Updater-available-blue)](https://db.universal-team.net/3ds/3dslibris)
[![License: AGPL v3+](https://img.shields.io/badge/license-AGPL--3.0--or--later-blue.svg)](#license)

Nintendo 3DS homebrew ebook and manga reader based on the original Nintendo DS project [`dslibris`](https://github.com/rhaleblian/dslibris).

## Features

- Reads ebooks and manga on Nintendo 3DS hardware and Azahar.
- Library browser with grid and list views, cover thumbnails, metadata titles, and touch navigation.
- Six reading themes with matching splash screens and reader gradients.
- Supported formats: `EPUB`, `FB2`, `TXT`, `RTF`, `ODT`, `MOBI`, `PDF`, `CBZ` and `XPS`.
- `EPUB` reflow with TOC support, bookmarks, `go to page`, cached pagination, and broad inline/block formatting support.
- Fixed-layout viewer for manga and document formats (`CBZ`, `PDF` and `XPS`) with zoom, pan, outline navigation, and full-page preview.
- Bundled fallback fonts for broader language coverage (Latin, Cyrillic, Greek, CJK, Arabic, Hebrew, Thai, and more).


## Screenshots

<table>
  <tr>
    <td width="50%"><img width="640" alt="3dslibris dark sepia theme" src="https://github.com/user-attachments/assets/6de341bf-7811-4e70-be73-6e0aa4ca62ac" /></td>
    <td width="50%"><img width="640" alt="3dslibris light theme" src="https://github.com/user-attachments/assets/9874a40f-47d7-4c07-9f35-a90dd63edc9c" /></td>
  </tr>
  <tr>
    <td colspan="2" align="center">
      <img width="50%" src="assets/readme/screenshot2.jpeg" alt="Reading view screenshot" />
    </td>
  </tr>
</table>

## Installation

Recommended install (`.cia` via Universal-Updater):

1. Install `3dslibris` from [Universal-Updater](https://db.universal-team.net/3ds/3dslibris).
2. Launch it once so `sdmc:/3ds/3dslibris/` is created if needed.
3. Copy your books to `sdmc:/3ds/3dslibris/book/`.
4. Launch the installed title.

Manual install (`.3dsx`):

1. Download `3dslibris-sdmc.zip` from [GitHub Releases](https://github.com/RigleGit/3dslibris/releases).
2. Extract it to the root of the SD card so it expands into `sdmc:/`.
3. Copy your books to `sdmc:/3ds/3dslibris/book/`.
4. Launch `sdmc:/3ds/3dslibris/3dslibris.3dsx` from Homebrew Launcher.

Manual install (`.cia`):

1. Install `3dslibris.cia`.
2. Launch it once so `sdmc:/3ds/3dslibris/` is created if needed.
3. Copy your books to `sdmc:/3ds/3dslibris/book/`.
4. Launch the installed title.

Notes:

- The `.cia` includes default `font/` and `resources/` assets in `RomFS`.
- Books are read from `sdmc:/3ds/3dslibris/book/` and optionally `romfs:/3ds/3dslibris/book/`.
- If the same filename exists in both places, the SD version wins.
- Releases also provide `3dslibris-debug.3dsx` and `3dslibris-debug.cia`, which enable verbose logging to `sdmc:/3ds/3dslibris/3dslibris.log`.

## Controls

Library:

- `D-Pad`: move selection
- `A`: open selected book
- `L` / `R`: previous or next library page
- `Touch`: select and open books
- `Y` / `Select`: open `GENERAL` settings

Standard reading:

- `A` / `B` / `L` / `R`: turn pages
- `D-Pad Left` / `D-Pad Right`: jump between bookmarks
- `Y`: toggle bookmark
- `X`: change background color
- `SELECT`: open `BOOK` settings
- `START`: return to library

Fixed-layout documents (`PDF` / `CBZ` / `XPS`):

- `A`: zoom in
- `B`: zoom out
- `Left` / `Right`: previous or next page
- `Up` / `Down`: next or previous chapter when available, otherwise page navigation
- `Touch`: move the viewport on the page preview
- `SELECT`: open `BOOK` settings
- `START`: return to library

## Supported Formats

Strong support:

- `EPUB`: EPUB2 and EPUB3 reflow, NAV and NCX TOC support, cached pagination, bookmarks, configurable fonts, and broad inline/block formatting support.

Good support:

- `FB2`
- `TXT`
- `RTF`
- `ODT`

Experimental or best-effort:

- `MOBI`: can be slow on first open, TOC quality depends on file structure, and some files may fall back to safer but more limited parsing.
- `PDF`: viewer mode with zoomed reading area and full-page preview.
- `CBZ`: viewer mode for manga and image-based books.
- `XPS`: viewer mode with the same fixed-layout reader controls.

## Limitations

- No DRM support.
- EPUB is a reflow renderer, not a full browser engine. Complex CSS, JavaScript, multi-column layouts, and wide tables are simplified.
- EPUB tables are converted into text-oriented blocks for readability on 3DS screens.
- SVG support in EPUB is limited to common wrappers that reference supported raster images.
- Some malformed EPUB anchors and MOBI tables of contents can produce approximate navigation.
- Large or malformed MOBI files may open slowly or lose some rich formatting in safer fallback paths.
- After changing layout-related settings such as font size, spacing, orientation, or some format-specific options, reopening the current book may be necessary.

## Star History

[![Star History Chart](https://api.star-history.com/svg?repos=RigleGit/3dslibris&type=Date)](https://star-history.com/#RigleGit/3dslibris&Date)

## Build

Docker build flow:

```bash
docker build -f docker/Dockerfile.cia -t 3dslibris-build .

docker run --rm \
  -v "$(pwd):/project" -w /project \
  -e DEVKITPRO=/opt/devkitpro \
  -e DEVKITARM=/opt/devkitpro/devkitARM \
  3dslibris-build \
  sh -lc 'make clean && make -j2 && make zip-sdmc && make debug-3dsx && make cia && make source-release'
```

Expected outputs:

- `3dslibris.cia`
- `3dslibris-debug.cia`
- `3dslibris.3dsx`
- `3dslibris-debug.3dsx`
- `dist/3dslibris-source.tar.gz`

## Documentation

- [CONTRIBUTING.md](CONTRIBUTING.md)
- [THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md)
- [docs/PDF_SOURCE_RELEASE.md](docs/PDF_SOURCE_RELEASE.md) - source release and rebuild notes for distributed binaries

## License

This repository contains code under multiple licenses, but the distributed application is effectively governed by **GNU AGPL v3 or later** because it links against MuPDF.

- The inherited and base `3dslibris` code remains under **GNU GPL v2 or later**.
- MuPDF is included under **GNU AGPL v3 or later**.
- When distributed together as the `3dslibris` application, the combined work must be treated as **AGPL v3 or later**.

See:

- [LICENSE](LICENSE)
- [LICENSES/GPL-2.0-or-later.txt](LICENSES/GPL-2.0-or-later.txt)
- [LICENSES/AGPL-3.0-or-later.txt](LICENSES/AGPL-3.0-or-later.txt)
- [THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md)
- [docs/PDF_SOURCE_RELEASE.md](docs/PDF_SOURCE_RELEASE.md)

## Credits

- Original `dslibris`: Ray Haleblian
- 3DS port and maintenance: [Rigle](https://rigle.dev)
