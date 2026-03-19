#!/bin/sh
set -eu

ROOT="$(CDPATH= cd -- "$(dirname "$0")/.." && pwd)"
OUTDIR="${TMPDIR:-/tmp}/3dslibris-tests"
mkdir -p "$OUTDIR"

c++ -std=c++11 \
  "$ROOT/tests/test_epub_image_utils.cpp" \
  "$ROOT/source/core/epub_image_utils.cpp" \
  "$ROOT/source/core/ioapi.c" \
  "$ROOT/source/core/unzip.c" \
  -I"$ROOT/include" \
  -I"$ROOT/include/minizip" \
  -I"$ROOT/source/core" \
  -lz \
  -o "$OUTDIR/test_epub_image_utils"

"$OUTDIR/test_epub_image_utils"
