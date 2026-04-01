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
- Current app version: `2.0.1`
- Focus: stable daily reading on 3DS hardware and Citra/Azahar
- Repository status: public release available and under active maintenance
- Latest downloadable binaries and SD package: [GitHub Releases](https://github.com/RigleGit/3dslibris/releases)
- Releases also include `3dslibris-debug.3dsx`, which enables verbose diagnostic logging in `3dslibris.log`
- Supported install paths: `.3dsx` plus `3dslibris-sdmc.zip`, or `3dslibris.cia`

## v2.0.1 release notes
- Publishes the full post-`v2.0.0` branch state as the actual public release, so the GitHub tag now matches the code that was already finished locally.
- Keeps the complete fixed-layout and reflow stack from `v2.0.0`, including MuPDF-backed `PDF` / `CBZ` / `XPS`, deferred MOBI open, library cover generation, and the CIA runtime bundle.
- Reorganizes shared code and path handling so SD/cache/runtime paths live in a central place and common utilities are easier to maintain.
- Moves bundled `expat` sources under `third_party/` and documents the current 3DS/NDS hardware and architecture references in the repo.
- Extracts MOBI page-cache serialization into its own module and adds a shared native test-build helper for the text-layout/unit scripts.
- Full release notes: [.github/release-notes/v2.0.1.md](.github/release-notes/v2.0.1.md)

Commits already included in the `v2.0.1` line:
- `3fde057` `build: serialize MuPDF minimal generation`
- `322dc39` `docs: simplify release notes highlights for end users`
- `c4ab9fe` `docs: add NDS and 3DS hardware reference from GBATek`
- `89e27b6` `refactor: reorganize shared/ utilities and move expat to third_party/`
- `0e4f1a1` `refactor: centralize SD paths and add architecture documentation`
- `db6e400` `refactor: remove original files after reorganization`
- `4cb884b` `refactor: extract mobi page cache helpers`

## v2.0.0 release notes
- Adds MuPDF-backed fixed-layout reading for `PDF`, `CBZ`, and `XPS`.
- Ships a progressive fixed-layout pipeline with preview-first rendering, strip refinement, worker-thread acceleration on New 3DS, and stronger cache reuse.
- Expands the reflow stack with asynchronous MOBI open/reflow on New 3DS, better persistent caches, and a much faster deferred TOC path for large books.
- Improves the real-hardware open path by deferring expensive cache writes out of the critical path, buffering debug logging, and keeping deferred MOBI pagination responsive while you start reading.
- Adds a more aggressive library-cover pipeline with visible-page cache reuse, selected-book warmup, and generated thumbs for `EPUB`, `FB2`, `MOBI`, `PDF`, and `CBZ` on actual 3DS hardware.
- Tightens fixed-layout and browser rendering by tracking dirty rectangles precisely, reusing physical framebuffer caches, and avoiding redraw stalls that previously hid freshly generated cover thumbs.
- Improves EPUB and FB2 layout instrumentation and shared text-layout performance while keeping existing reader behavior stable.
- Keeps the CIA packaging flow self-contained by bundling default runtime assets through `romfs`.
- Full release notes: [.github/release-notes/v2.0.0.md](.github/release-notes/v2.0.0.md)

Commits already included in the `v2.0.0` line:
- `2e9b5d6` `fix: bundle CIA runtime assets via romfs`
- `983be1f` `feat: integrate MuPDF-backed PDF reader`
- `7b1e91f` `feat: finalize v1.2.0 PDF progressive rendering`
- `3ad063d` `feat: add native cbz reader`
- `b3d2f2c` `feat: add xps support via mupdf`
- `8be3c1f` `Optimize async MOBI parsing and deferred reflow`
- `cc5ffb6` `Optimize shared EPUB/FB2 layout instrumentation`
- `5dab1de` `feat: optimize deferred rendering and library covers`

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
- GitHub Releases: pushing a tag like `v2.0.0` triggers `.github/workflows/release.yml` and attaches `3dslibris.cia`, `3dslibris.3dsx`, `3dslibris-debug.3dsx`, `dist/3dslibris-sdmc.zip`, and `dist/3dslibris-source.tar.gz` to the release

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
