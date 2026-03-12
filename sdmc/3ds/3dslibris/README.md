# 3dslibris SD Layout

This directory mirrors the expected install path on the SD card:

- `3dslibris.3dsx` goes here after build/package
- `book/` is still supported for per-app ebook storage
- `font/` ships with bundled Liberation `.ttf` files plus the font license text
- `resources/` contains versioned UI assets from this repository

To assemble a ready-to-copy tree from the repo root:

```bash
make package-sdmc
```

That command creates `dist/sdmc/3ds/3dslibris/` and copies the built
`3dslibris.3dsx` into it.

Put user ebooks in `sdmc:/3ds/3dslibris/book/`.

The bundled fonts are only defaults. Users can swap them for other compatible
`.ttf`, `.otf`, or `.ttc` files if they want to customize typography.
