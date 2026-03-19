#!/bin/sh
set -eu

ROOT="$(CDPATH= cd -- "$(dirname "$0")/.." && pwd)"
OUTDIR="${TMPDIR:-/tmp}/3dslibris-tests"
mkdir -p "$OUTDIR"

c++ -std=c++11 \
  "$ROOT/tests/test_epub_image_utils.cpp" \
  "$ROOT/source/formats/common/epub_image_utils.cpp" \
  "$ROOT/third_party/minizip/source/ioapi.c" \
  "$ROOT/third_party/minizip/source/unzip.c" \
  -I"$ROOT/include" \
  -I"$ROOT/third_party/minizip" \
  -I"$ROOT/third_party/minizip/include" \
  -I"$ROOT/third_party/minizip/source" \
  -lz \
  -o "$OUTDIR/test_epub_image_utils"

"$OUTDIR/test_epub_image_utils"
