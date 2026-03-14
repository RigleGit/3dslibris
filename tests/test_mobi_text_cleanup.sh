#!/bin/sh
set -eu

ROOT="$(CDPATH= cd -- "$(dirname "$0")/.." && pwd)"
OUTDIR="${TMPDIR:-/tmp}/3dslibris-tests"
mkdir -p "$OUTDIR"

c++ -std=c++11 \
  "$ROOT/tests/test_mobi_text_cleanup.cpp" \
  "$ROOT/source/core/mobi_text_cleanup.cpp" \
  -I"$ROOT/include" \
  -I"$ROOT/source/core" \
  -o "$OUTDIR/test_mobi_text_cleanup"

"$OUTDIR/test_mobi_text_cleanup"
