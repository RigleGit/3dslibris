<h1>
  <img src="assets/release/icon-64x64.png" alt="3dslibris icon" width="64" />
  3dslibris
</h1>

[![Release](https://img.shields.io/github/v/release/RigleGit/3dslibris?label=release)](https://github.com/RigleGit/3dslibris/releases)
[![CI](https://img.shields.io/github/actions/workflow/status/RigleGit/3dslibris/ci.yml?branch=main&label=ci)](https://github.com/RigleGit/3dslibris/actions/workflows/ci.yml)

Nintendo 3DS homebrew ebook reader based on the original Nintendo DS project `dslibris`.

`3dslibris` ports the original architecture to `libctru`, keeps the fast text-first reading model, and adds practical 3DS UX improvements (grid library, cover thumbs, indexed navigation, procedural UI skin, orientation-aware touch, etc.).

The current `.cia` packaging flow is based on the same `makerom`/`bannertool` process used by [Universal-Updater](https://github.com/Universal-Team/Universal-Updater), adapted to this project's assets and release layout.

<table>
  <tr>
    <td width="50%"><img src="assets/readme/screenshot1.jpeg" alt="Library view screenshot" /></td>
    <td width="50%"><img src="assets/readme/screenshot2.jpeg" alt="Reading view screenshot" /></td>
  </tr>
</table>

## Project status
- Current app version: `1.2.0`
- Focus: stable daily reading on 3DS hardware and Citra/Azahar
- Repository status: public release available and under active maintenance
- Latest downloadable binaries and SD package: [GitHub Releases](https://github.com/RigleGit/3dslibris/releases)
- Releases also include `3dslibris-debug.3dsx`, which enables verbose diagnostic logging in `3dslibris.log`
- Supported install paths: `.3dsx` plus `3dslibris-sdmc.zip`, or `3dslibris.cia`

## v1.2.0 release notes
- Adds MuPDF-backed PDF reading with zoomed top-screen viewing, full-page preview on the bottom screen, outline navigation when available, and touch-controlled viewport movement.
- Introduces a progressive PDF rendering pipeline: preview first, interactive cache next, then full-page refinement in the background instead of a single blocking render.
- Adds progressive strip rendering for zoomed PDF pages, with strips composited on screen as they complete.
- Uses a dedicated PDF worker thread on the New Nintendo 3DS extra core when available, while keeping an automatic synchronous fallback path for Old 3DS hardware.
- Improves PDF cache behavior by stabilizing preview viewport updates, accelerating cache reuse, and deferring expensive prefetch work until page turns or idle periods.
- Tightens PDF release documentation and licensing notes for MuPDF-enabled builds, including corresponding-source guidance for release packaging.

Commits already included in the `v1.2.0` line:
- `983be1f` `feat: integrate MuPDF-backed PDF reader`
- `ee6fcc9` `docs: add AGPL compliance for PDF-enabled releases`
- `57737ac` `fix: stabilize PDF preview viewport rendering`
- `a756191` `perf: accelerate PDF render caching`
- `b326c54` `perf: defer PDF prefetch until page turns`
- `c422eca` `perf: add progressive PDF page rendering`

## Supported formats

### Strong support
- `EPUB` (EPUB2 + EPUB3 NAV/NCX parsing with robust fallbacks)

### Good support (text-oriented)
- `FB2`
- `TXT`
- `RTF`
- `ODT`

### Experimental / best-effort
- `MOBI`
  - First open can be slow on large books (decompress + parse + pagination)
  - Subsequent opens are accelerated by persistent page cache
  - TOC quality is heuristic for many files (can be approximate)
  - Inline MOBI images now reuse the same smart `inline / band / page` layout pipeline used by EPUB/FB2, with better caption flow on mixed photo spreads
  - Includes an optional per-book `line wrap fix` for badly converted files that hard-wrap prose line by line, while preserving embedded image markers during cleanup
  - Empty or corrupt books are reported with a readable error instead of a raw numeric code
- `PDF`
  - Viewer-only path with MuPDF-backed rendering
  - Top screen shows a zoomed page region; bottom screen shows the full-page preview and viewport box
  - `A/B` control zoom, `Left/Right` move page, `Up/Down` move through outline entries when available, and touch moves the viewport
  - PDF-enabled builds in this branch are distributed with AGPL-driven notice and source-release requirements; see the license section below

## Known limitations
- Some EPUB files have malformed anchors; index jumps can be approximate when source metadata is broken.
- MOBI TOC extraction depends on file structure and may omit or merge entries in some books.
- MOBI inline images depend on recoverable image references in the source markup, including the zero-padded `recindex` values commonly found in Kindle-generated books; malformed files can still miss some images.
- Some malformed MOBI sources still contain encoding or OCR artifacts that cannot be repaired reliably on the reader side.
- After changing font size, paragraph spacing, orientation, reading fonts, or the per-book MOBI `line wrap fix`, reopen the current book to apply the new layout.
- Reading position and existing bookmarks are remapped approximately after that reopen and can shift a few pages from their original location.
- No DRM support.

## Build (Docker, recommended)

```bash
docker build -f docker/Dockerfile.cia -t 3dslibris-build .

docker run --rm \
  -v "$(pwd):/project" -w /project \
  -e DEVKITPRO=/opt/devkitpro \
  -e DEVKITARM=/opt/devkitpro/devkitARM \
  3dslibris-build \
  sh -lc 'make clean && make -j2 && make zip-sdmc && make debug-3dsx && make cia && make source-release'
```

The Dockerfile in [`docker/Dockerfile.cia`](docker/Dockerfile.cia) matches the
release packaging flow used by the project, including the `.cia` toolchain.

Expected outputs:
- `3dslibris.cia`
- `3dslibris.3dsx`
- `3dslibris-debug.3dsx`
- `3dslibris.smdh`
- `3dslibris.elf`
- `dist/3dslibris-source.tar.gz`

## Install

Recommended install:
1. Download `3dslibris-sdmc.zip` from [GitHub Releases](https://github.com/RigleGit/3dslibris/releases).
2. Extract that zip into the root of your SD card, so it expands into `sdmc:/`.
3. Put your books in `sdmc:/3ds/3dslibris/book/`.
4. Launch `sdmc:/3ds/3dslibris/3dslibris.3dsx` from Homebrew Launcher.

Alternative install:
1. Install `3dslibris.cia`.
2. Put your books in `sdmc:/3ds/3dslibris/book/`.
3. Launch the installed title.

Important:
- The `.cia` now bundles the default `font/` and `resources/` runtime assets inside the application, so a plain CIA install can boot without manually extracting `3dslibris-sdmc.zip`.
- `3dslibris-sdmc.zip` is still the recommended install for `.3dsx`, and it remains useful if you want the same runtime files laid out explicitly on SD.
- `3dslibris-debug.3dsx` uses the same SD layout and writes verbose diagnostics to `sdmc:/3ds/3dslibris/3dslibris.log`.
- The `.cia` build uses the Universal-Updater-style packaging flow and now also bundles the default runtime assets through `romfs`.

Generated install package targets:
- `make package-sdmc` stages `dist/sdmc/...` with `3dslibris.3dsx` included
- `make zip-sdmc` creates `dist/3dslibris-sdmc.zip`
- `make cia` creates `3dslibris.cia`
- `make source-release` creates `dist/3dslibris-source.tar.gz`
- GitHub Releases: pushing a tag like `v1.2.0` triggers `.github/workflows/release.yml` and attaches `3dslibris.cia`, `3dslibris.3dsx`, `3dslibris-debug.3dsx`, `dist/3dslibris-sdmc.zip`, and `dist/3dslibris-source.tar.gz` to the release

## Library controls
- `D-Pad`: move the current selection around the library grid
- `A`: open the selected book
- `L` / `R`: jump to the previous or next library page
- `Touch`: tap a book to select it, tap it again to open it
- `Y` / `Select`: open settings

Bundled runtime files:
- `sdmc/3ds/3dslibris/resources/splash.jpg`
- `sdmc/3ds/3dslibris/resources/ui/icons/png/*.png`
- `sdmc/3ds/3dslibris/book/README.md`
- `sdmc/3ds/3dslibris/font/README.md`
- `sdmc/3ds/3dslibris/font/Liberation*.ttf`
- `sdmc/3ds/3dslibris/font/OFL-1.1.txt`

Notes:
- Homebrew Launcher path: keep the app at `sdmc:/3ds/3dslibris/3dslibris.3dsx`
- Debug build path: keep `3dslibris-debug.3dsx` in the same `sdmc:/3ds/3dslibris/` folder if you want verbose logs
- Default Liberation fonts are bundled in `sdmc:/3ds/3dslibris/font/`
- You can replace them with other `.ttf`, `.otf`, or `.ttc` fonts if you want to customize the reading/UI typefaces
- Runtime files such as `3dslibris.xml`, `3dslibris.log`, and `cache/*` are created by the app on first run

```text
sdmc:/3ds/3dslibris/3dslibris.3dsx
sdmc:/3ds/3dslibris/book/*.epub|*.fb2|*.txt|*.rtf|*.odt|*.mobi
sdmc:/3ds/3dslibris/font/*.ttf
sdmc:/3ds/3dslibris/resources/splash.jpg
sdmc:/3ds/3dslibris/resources/ui/icons/png/{back,gear,home,next,prev}.png
```

## Controls (default)
- `A/B/L/R`: turn pages
- `D-Pad Left/Right`: jump between bookmarks
- `Y`: toggle bookmark
- `X`: change background color
- `SELECT`: settings
- `START`: return to library
- Touch UI for library, settings, index, bookmarks, font menus...

## Documentation
- Contribution guide: [CONTRIBUTING.md](CONTRIBUTING.md)
- Third-party notices: [THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md)
- PDF source-release guide: [docs/PDF_SOURCE_RELEASE.md](docs/PDF_SOURCE_RELEASE.md)

Internal planning, release notes, and working docs are kept out of the public repo.

## License
This repository now carries a split licensing model:

- inherited and base 3dslibris code remains under **GNU GPL v2 or later**
- PDF-enabled builds in this branch combine that code with MuPDF and must be
  distributed with the AGPL-related obligations documented in:
  - [LICENSE](LICENSE)
  - [LICENSES/GPL-2.0-or-later.txt](LICENSES/GPL-2.0-or-later.txt)
  - [LICENSES/AGPL-3.0-or-later.txt](LICENSES/AGPL-3.0-or-later.txt)
  - [THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md)
  - [docs/PDF_SOURCE_RELEASE.md](docs/PDF_SOURCE_RELEASE.md)

## Credits
- Original `dslibris`: Ray Haleblian
- 3DS port and maintenance: [Rigle](https://rigle.dev)
