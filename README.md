# 3dslibris

Nintendo 3DS homebrew ebook reader based on the original Nintendo DS project `dslibris`.

`3dslibris` ports the original architecture to `libctru`, keeps the fast text-first reading model, and adds practical 3DS UX improvements (grid library, cover thumbs, indexed navigation, procedural UI skin, orientation-aware touch, etc.).

## Project status
- Current app version: `1.0.0`
- Focus: stable daily reading on 3DS hardware and Citra/Azahar
- Repository status: publication-hardening in progress (docs/legal/CI/governance)

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

## Known limitations (realistic)
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
- `banner.png`
- `assets/cia/banner-silence.wav` (banner audio, required by `bannertool makebanner`)
- `icon-32x32.png` (small icon)
- `icon-64x64.png` (large icon)
- `icon.png` (48x48, used for `.3dsx/.smdh`)
- `3dslibris.rsf`

## Install layout (SD)

```text
sdmc:/3ds/3dslibris/3dslibris.3dsx
sdmc:/3ds/3dslibris/book/*.epub|*.fb2|*.txt|*.rtf|*.odt|*.mobi
sdmc:/3ds/3dslibris/font/*.ttf
sdmc:/3ds/3dslibris/resources/...
```

## Controls (default)
- `A/B/L/R`: turn pages
- `D-Pad Left/Right`: jump between bookmarks
- `Y`: toggle bookmark
- `SELECT`: settings
- `START`: return to library
- Touch UI for library, settings, index, bookmarks, font menus

## Documentation
- Technical deep-dive: [DOCS.md](DOCS.md)
- Chronological change log (git-derived): [revision_cronologica_cambios.md](revision_cronologica_cambios.md)
- Contribution guide: [CONTRIBUTING.md](CONTRIBUTING.md)
- Third-party notices: [THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md)
- Technical backlog: [BACKLOG_TECNICO.md](BACKLOG_TECNICO.md)
- Release process: [RELEASE_CHECKLIST.md](RELEASE_CHECKLIST.md)

## License
This project is distributed under **GNU GPL v2 or later**.
See [LICENSE](LICENSE).

## Credits
- Original `dslibris`: Ray Haleblian
- 3DS port and maintenance: Rigle
