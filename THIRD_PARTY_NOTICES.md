# Third-Party Notices

This repository includes third-party software. Preserve these notices when
redistributing source or binary builds.

## Project licensing summary

- Inherited `3dslibris` code remains under `GNU GPL v2 or later`.
- This repository's distributed builds link against MuPDF.
- MuPDF open-source editions are distributed under `GNU AGPL v3 or later`.
- When you distribute the combined application, you must satisfy the AGPL
  obligations for that combined work, including corresponding source
  availability and preservation of notices.

See:

- [LICENSE](LICENSE)
- [LICENSES/GPL-2.0-or-later.txt](LICENSES/GPL-2.0-or-later.txt)
- [LICENSES/AGPL-3.0-or-later.txt](LICENSES/AGPL-3.0-or-later.txt)
- [docs/PDF_SOURCE_RELEASE.md](docs/PDF_SOURCE_RELEASE.md)

## Included third-party components

### MuPDF

- Upstream: `third_party/mupdf/`
- Upstream project: Artifex MuPDF
- License: `GNU Affero General Public License v3 or later`
- Repository text: `third_party/mupdf/COPYING`

This branch uses MuPDF only for PDF document opening, outline loading, and page
rendering. If you distribute a binary built from this repository, you must make
the corresponding source for that binary available.

### utf8proc

- Upstream: `third_party/utf8proc/`
- Used for Unicode normalization and text handling.
- Preserve the upstream notices shipped in that directory.

### libunibreak

- Upstream: `third_party/libunibreak/`
- Used for line-breaking behavior.
- Preserve the upstream notices shipped in that directory.

### stb

- Upstream: `third_party/stb/`
- Used for lightweight image decoding helpers.
- Preserve the upstream notices shipped in that directory.

### Expat

- Upstream code lives under `third_party/expat/`.
- Preserve the upstream Expat license notice embedded in that code.

### Fonts and MuPDF bundled resources

MuPDF vendors additional third-party assets and notices, including:

- `third_party/mupdf/resources/fonts/droid/NOTICE`
- `third_party/mupdf/resources/fonts/noto/COPYING`
- `third_party/mupdf/thirdparty/jbig2dec/COPYING`
- `third_party/mupdf/thirdparty/lcms2/plugins/fast_float/COPYING.GPL3`
- `third_party/mupdf/thirdparty/lcms2/plugins/threaded/COPYING.GPL3`

If you redistribute a source bundle, keep those files intact.

### Bundled runtime fonts

The default font set distributed with `3dslibris` (inside `font/` on SD and in
RomFS for `.cia` builds) includes:

**SIL Open Font License 1.1** — `font/OFL-1.1.txt`

- **Liberation** (Red Hat, Inc. / Google Corporation): Latin serif, sans-serif,
  and monospace families (`Liberation*.ttf`).
- **Noto Naskh Arabic** (Google LLC): Arabic variable font
  (`NotoNaskhArabic-VariableFont_wght.ttf`).
- **Noto Sans Hebrew** (Google LLC): Hebrew variable font
  (`NotoSansHebrew-VariableFont_wdth,wght.ttf`).

**Apache License 2.0** — `font/Apache-2.0.txt`

- **Droid Sans Fallback** (Android Open Source Project): CJK fallback font
  covering Japanese, Korean, and Chinese scripts
  (`DroidSansFallbackFull.ttf`).
