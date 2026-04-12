<h1>
  <img src="assets/release/icon-64x64.png" alt="3dslibris icon" width="64" />
  3dslibris
</h1>

[![Release](https://img.shields.io/github/v/release/RigleGit/3dslibris?label=release)](https://github.com/RigleGit/3dslibris/releases)
[![CI](https://img.shields.io/github/actions/workflow/status/RigleGit/3dslibris/ci.yml?branch=main&label=ci)](https://github.com/RigleGit/3dslibris/actions/workflows/ci.yml)

Nintendo 3DS homebrew ebook reader based on the original Nintendo DS project `dslibris`.

`3dslibris` ports the original architecture to `libctru`, keeps the fast text-first reading model, and adds practical 3DS UX improvements: grid library, cover thumbs, indexed navigation, EPUB reflow improvements, fallback fonts, procedural UI skin, orientation-aware touch, and fixed-layout document viewing.

The current `.cia` packaging flow is based on the same `makerom`/`bannertool` process used by [Universal-Updater](https://github.com/Universal-Team/Universal-Updater), adapted to this project's assets and release layout.

<table>
  <tr>
    <td width="50%"><img src="assets/readme/screenshot1.jpeg" alt="Library view screenshot" /></td>
    <td width="50%"><img src="assets/readme/screenshot2.jpeg" alt="Reading view screenshot" /></td>
  </tr>
</table>

## Project status
- Current app version: `2.2.0`
- Focus: stable daily reading on 3DS hardware and Azahar
- Repository status: public release available and under active maintenance
- Latest downloadable binaries and SD package: [GitHub Releases](https://github.com/RigleGit/3dslibris/releases)
- Releases also include `3dslibris-debug.3dsx`, which enables verbose diagnostic logging in `3dslibris.log`
- Supported install paths: `.3dsx` plus `3dslibris-sdmc.zip`, or `3dslibris.cia` with books stored on SD and optional bundled books in RomFS.
- Main reading focus in `2.2.0`: safer HOME/app lifecycle handling across `.3dsx` and `.cia`, more conservative old3DS cover scheduling under memory pressure, and improved EPUB spacing/TOC handling.

## Install

Recommended install:
1. Download `3dslibris-sdmc.zip` from [GitHub Releases](https://github.com/RigleGit/3dslibris/releases).
2. Extract that zip into the root of your SD card, so it expands into `sdmc:/`.
3. Put your books in `sdmc:/3ds/3dslibris/book/`.
4. Launch `sdmc:/3ds/3dslibris/3dslibris.3dsx` from Homebrew Launcher.

Alternative install:
1. Install `3dslibris.cia`.
2. Launch the installed title once so it creates `sdmc:/3ds/3dslibris/` if needed.
3. Put your books in `sdmc:/3ds/3dslibris/book/`.
4. Launch the installed title.

Important:
- The `.cia` bundles the default `font/` and `resources/` runtime assets inside `RomFS`, so it can boot without manually extracting `3dslibris-sdmc.zip`.
- Books are discovered from `sdmc:/3ds/3dslibris/book/` and `romfs:/3ds/3dslibris/book/`.
- If the same filename exists in both places, SD takes priority.
- `3dslibris-sdmc.zip` is still the recommended install for `.3dsx`, and it remains useful if you want the same runtime files laid out explicitly on SD.
- `3dslibris-debug.3dsx` uses the same SD layout and writes verbose diagnostics to `sdmc:/3ds/3dslibris/3dslibris.log`.
- The `.cia` build uses the Universal-Updater-style packaging flow and now also validates the bundled `RomFS` path in GitHub Actions.

Adding books:
- Copy supported ebook files into `sdmc:/3ds/3dslibris/book/`.
- You can do this either before first launch or after the app has already created its folders.
- On `.cia`, the app can also read bundled books from `romfs:/3ds/3dslibris/book/`, but the normal user drop folder is still the SD path above.
- The included `sdmc:/3ds/3dslibris/book/QuickStart.txt` also explains the expected folder layout and controls.

Generated install package targets:
- `make package-sdmc` stages `dist/sdmc/...` with `3dslibris.3dsx` included
- `make zip-sdmc` creates `dist/3dslibris-sdmc.zip`
- `make cia` creates `3dslibris.cia`
- `make source-release` creates `dist/3dslibris-source.tar.gz`
- GitHub Releases: pushing a tag like `v2.2.0` triggers `.github/workflows/release.yml` and attaches `3dslibris.cia`, `3dslibris.3dsx`, `3dslibris-debug.3dsx`, `dist/3dslibris-sdmc.zip`, and `dist/3dslibris-source.tar.gz` to the release

## Supported formats

### Strong support
- `EPUB`
  - EPUB2 + EPUB3 content parsing with NAV and NCX table-of-contents support, plus fallback chapter labels when source metadata is incomplete
  - persistent page cache keyed by layout inputs, so repeated opens can reuse compatible pagination instead of rebuilding every page
  - configurable serif, sans, and monospace font families; `pre` / `code` blocks now reflow and measure with the active monospace face
  - monospace regular, bold, italic, and bold-italic variants are preserved when matching fonts are available
  - inline formatting support includes bold, italic, underline, strikethrough, overline, superscript, subscript, and CSS-driven dotted/dashed/wavy underline markers
  - block formatting covers headings, paragraphs, lists, nested ordered lists, blockquotes, asides, figures, captions, definition lists, and horizontal rules
  - tables are linearized into readable label/value blocks for the 3DS screen instead of trying to preserve wide desktop table layout
  - common hidden accessibility/helper text is ignored when marked with `hidden`, `aria-hidden`, `display:none`, `visibility:hidden`, or `visually-hidden`-style classes
  - punctuation handling keeps Spanish opening/closing punctuation attached across inline style boundaries in common cases
  - inline SVG image wrappers are detected and resolved when they point to supported raster image assets

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
  - Uses the shared fixed-layout reader controls documented below
  - PDF-enabled builds in this branch are distributed with AGPL-driven notice and source-release requirements; see the license section below
- `CBZ`
  - Viewer-only path with MuPDF-backed image-page rendering
  - Uses the shared fixed-layout reader controls documented below
- `XPS`
  - Viewer-only path with MuPDF-backed rendering
  - Uses the shared fixed-layout reader controls documented below

## Known limitations
- Some EPUB files have malformed anchors; index jumps can be approximate when source metadata is broken.
- EPUB is a reflow renderer, not a browser engine: complex CSS layout, JavaScript, floats, multi-column pages, and wide tables are intentionally simplified for the 3DS screens.
- EPUB tables are converted to text blocks. This improves readability on 3DS, but it does not preserve the original grid geometry.
- EPUB SVG support is limited to common wrapper patterns that reference raster images; arbitrary SVG drawing is not rendered as vector graphics.
- After changing font size, paragraph spacing, orientation, reading fonts, or other EPUB layout settings, reopen the current book if a cached layout is still visible.
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
- `sdmc/3ds/3dslibris/font/Liberation*.ttf` (Latin; SIL OFL 1.1)
- `sdmc/3ds/3dslibris/font/NotoNaskhArabic-VariableFont_wght.ttf` (Arabic; SIL OFL 1.1)
- `sdmc/3ds/3dslibris/font/NotoSansHebrew-VariableFont_wdth,wght.ttf` (Hebrew; SIL OFL 1.1)
- `sdmc/3ds/3dslibris/font/DroidSansFallbackFull.ttf` (CJK fallback; Apache 2.0)
- `sdmc/3ds/3dslibris/font/OFL-1.1.txt`
- `sdmc/3ds/3dslibris/font/Apache-2.0.txt`

Notes:
- Homebrew Launcher path: keep the app at `sdmc:/3ds/3dslibris/3dslibris.3dsx`
- Debug build path: keep `3dslibris-debug.3dsx` in the same `sdmc:/3ds/3dslibris/` folder if you want verbose logs
- Default fonts are bundled in the SD package for `.3dsx`, and inside `RomFS` for `.cia`
- You can replace them with other `.ttf`, `.otf`, or `.ttc` fonts if you want to customize the reading/UI typefaces
- Runtime files such as `3dslibris.xml`, `3dslibris.log`, and `cache/*` are created by the app on first run

```text
sdmc:/3ds/3dslibris/3dslibris.3dsx
sdmc:/3ds/3dslibris/book/*.epub|*.fb2|*.txt|*.rtf|*.odt|*.mobi|*.pdf|*.xps|*.oxps|*.cbz
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

## Controls (PDF / CBZ / XPS)
- `A`: zoom in
- `B`: zoom out
- `Left/Right`: previous or next page
- `Up/Down`: next or previous chapter when the document exposes an outline; otherwise previous or next page
- `Touch`: move the viewport by tapping or dragging on the page preview
- `SELECT`: open settings
- `START`: return to library

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
