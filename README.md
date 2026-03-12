<h1>
  <img src="assets/release/icon-64x64.png" alt="3dslibris icon" width="64" />
  3dslibris
</h1>

[![Release](https://img.shields.io/github/v/release/RigleGit/3dslibris?label=release)](https://github.com/RigleGit/3dslibris/releases)
[![CI](https://img.shields.io/github/actions/workflow/status/RigleGit/3dslibris/ci.yml?branch=main&label=ci)](https://github.com/RigleGit/3dslibris/actions/workflows/ci.yml)

Nintendo 3DS homebrew ebook reader based on the original Nintendo DS project `dslibris`.

`3dslibris` ports the original architecture to `libctru`, keeps the fast text-first reading model, and adds practical 3DS UX improvements (grid library, cover thumbs, indexed navigation, procedural UI skin, orientation-aware touch, etc.).

<table>
  <tr>
    <td width="50%"><img src="assets/readme/screenshot1.jpeg" alt="Library view screenshot" /></td>
    <td width="50%"><img src="assets/readme/screenshot2.jpeg" alt="Reading view screenshot" /></td>
  </tr>
</table>

## Project status
- Current app version: `1.0.1`
- Focus: stable daily reading on 3DS hardware and Citra/Azahar
- Repository status: public release available and under active maintenance
- Latest downloadable binaries and SD package: [GitHub Releases](https://github.com/RigleGit/3dslibris/releases)
- `1.0.1` improves cold EPUB open times by indexing ZIP entries before spine parsing
- Releases also include `3dslibris-debug.3dsx`, which enables verbose diagnostic logging in `3dslibris.log`

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

## Known limitations
- Some EPUB files have malformed anchors; index jumps can be approximate when source metadata is broken.
- MOBI TOC extraction depends on file structure and may omit or merge entries in some books.
- No DRM support.

## Build (Docker, recommended)

```bash
docker run --rm \
  -v "$(pwd):/project" -w /project \
  -e DEVKITPRO=/opt/devkitpro \
  -e DEVKITARM=/opt/devkitpro/devkitARM \
  devkitpro/devkitarm make clean && \

docker run --rm \
  -v "$(pwd):/project" -w /project \
  -e DEVKITPRO=/opt/devkitpro \
  -e DEVKITARM=/opt/devkitpro/devkitARM \
  devkitpro/devkitarm make
```

Expected outputs:
- `3dslibris.3dsx`
- `3dslibris.smdh`
- `3dslibris.elf`

## Build CIA (optional, for console install)

Local (if `bannertool` and `makerom` are already in your PATH):

```bash
make cia
```

Docker (tooling included in repo):

```bash
docker build -f docker/Dockerfile.cia -t 3dslibris/devkitarm-cia .

docker run --rm \
  -v "$(pwd):/project" -w /project \
  -e DEVKITPRO=/opt/devkitpro \
  -e DEVKITARM=/opt/devkitpro/devkitARM \
  3dslibris/devkitarm-cia make cia
```

Expected output:
- `3dslibris.cia`

Assets used by default:
- `assets/release/banner.png`
- `assets/cia/banner-silence.wav` (banner audio, required by `bannertool makebanner`)
- `assets/release/icon-32x32.png` (small icon)
- `assets/release/icon-64x64.png` (large icon)
- `assets/release/icon.png` (48x48, used for `.3dsx/.smdh`)
- `3dslibris.rsf`

## Install layout (SD)

Versioned SD template in this repo:
- `sdmc/3ds/3dslibris/resources/splash.jpg`
- `sdmc/3ds/3dslibris/resources/ui/icons/png/*.png`
- `sdmc/3ds/3dslibris/book/README.md`
- `sdmc/3ds/3dslibris/font/README.md`
- `sdmc/3ds/3dslibris/font/Liberation*.ttf`
- `sdmc/3ds/3dslibris/font/OFL-1.1.txt`

Generated install package targets:
- `make package-sdmc` stages `dist/sdmc/...` with `3dslibris.3dsx` included
- `make zip-sdmc` creates `dist/3dslibris-sdmc.zip`
- GitHub Releases: pushing a tag like `v1.0.1` triggers `.github/workflows/release.yml` and attaches `3dslibris.3dsx`, `3dslibris-debug.3dsx`, `3dslibris.cia`, and `dist/3dslibris-sdmc.zip` to the release

Notes:
- Homebrew Launcher path: keep the app at `sdmc:/3ds/3dslibris/3dslibris.3dsx`
- Debug build path: keep `3dslibris-debug.3dsx` in the same `sdmc:/3ds/3dslibris/` folder if you want verbose logs
- Put your ebooks in `sdmc:/3ds/3dslibris/book/`
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

Internal planning, release notes, and working docs are kept out of the public repo.

## License
This project is distributed under **GNU GPL v2 or later**.
See [LICENSE](LICENSE).

## Credits
- Original `dslibris`: Ray Haleblian
- 3DS port and maintenance: [Rigle](https://rigle.dev)
