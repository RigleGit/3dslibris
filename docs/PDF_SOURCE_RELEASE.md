# PDF Source Release and Installation Information

This document describes how to satisfy the corresponding-source obligations for
distributed `3dslibris` builds that include MuPDF-based document rendering.

## Scope

These instructions apply to builds that link project document viewers against
MuPDF from `third_party/mupdf/`.

## Corresponding source

For a distributed release from this repository, the corresponding source is the
exact tagged source tree used to build that release, including:

- project source under `source/`, `include/`, `assets/`, `resources/`, `sdmc/`
- vendored MuPDF under `third_party/mupdf/`
- build scripts such as `scripts/build_mupdf_minimal.sh`
- build orchestration in `Makefile`
- release packaging files in `docker/` and `.github/workflows/`
- the license and notice files in `LICENSE`, `LICENSES/`, and
  `THIRD_PARTY_NOTICES.md`

GitHub release assets should include:

- `3dslibris.cia`
- `3dslibris.3dsx`
- `3dslibris-debug.3dsx`
- `3dslibris-sdmc.zip`
- `3dslibris-source.tar.gz`

The `3dslibris-source.tar.gz` bundle is intended to be the exact source
snapshot for that release tag.

## Rebuilding

Recommended reproducible build path:

```bash
docker build -f docker/Dockerfile.cia -t 3dslibris-build .

docker run --rm \
  -v "$(pwd):/project" -w /project \
  -e DEVKITPRO=/opt/devkitpro \
  -e DEVKITARM=/opt/devkitpro/devkitARM \
  3dslibris-build \
  sh -lc 'make clean && make -j2 && make zip-sdmc && make debug-3dsx && make cia && make source-release'
```

## Installing a modified build

You do not need the project's original release binaries to run your own
modified build.

### Homebrew Launcher path

1. Build `3dslibris.3dsx`.
2. Copy it to `sdmc:/3ds/3dslibris/3dslibris.3dsx`.
3. Copy or update the runtime tree from `dist/sdmc/`.
4. Launch it from Homebrew Launcher on a homebrew-enabled 3DS.

### CIA path

1. Build `3dslibris.cia`.
2. Copy it to the SD card of a 3DS with custom firmware/homebrew tooling.
3. Install it with the normal homebrew installer path you already use for
   unsigned titles, such as FBI.
4. Put books in `sdmc:/3ds/3dslibris/book/`.

The app does not require vendor signing keys from this repository. It uses the
standard homebrew build/install flow supported by devkitPro toolchains and
homebrew-enabled devices.

## Notes

- PDF support is viewer-only in this branch.
- MuPDF is part of the distributed build path used by this repository.
- Preserve the GPL notices already present in the inherited source tree
  alongside the AGPL obligations that apply to the combined distributed work.
