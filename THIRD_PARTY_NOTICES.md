# Third-Party Notices

This repository includes third-party software. Preserve these notices when
redistributing source or binary builds.

## Project licensing summary

- Legacy and non-PDF 3dslibris code is distributed under `GNU GPL v2 or later`.
- The PDF-enabled variant in this branch links against MuPDF.
- MuPDF open-source editions are distributed under `GNU AGPL v3 or later`.
- When you distribute a build that includes MuPDF PDF support, you must satisfy
  the AGPL obligations for the combined work, including corresponding source
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
rendering. If you distribute a PDF-enabled binary, you must make the
corresponding source for that binary available.

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

- Upstream code lives under `source/expat/`.
- Preserve the upstream Expat license notice embedded in that code.

### Fonts and MuPDF bundled resources

MuPDF vendors additional third-party assets and notices, including:

- `third_party/mupdf/resources/fonts/droid/NOTICE`
- `third_party/mupdf/resources/fonts/noto/COPYING`
- `third_party/mupdf/thirdparty/jbig2dec/COPYING`
- `third_party/mupdf/thirdparty/lcms2/plugins/fast_float/COPYING.GPL3`
- `third_party/mupdf/thirdparty/lcms2/plugins/threaded/COPYING.GPL3`

If you redistribute a source bundle, keep those files intact.
