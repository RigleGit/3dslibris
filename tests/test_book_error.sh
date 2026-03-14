#!/bin/sh
set -eu

ROOT="$(CDPATH= cd -- "$(dirname "$0")/.." && pwd)"
OUTDIR="${TMPDIR:-/tmp}/3dslibris-tests"
mkdir -p "$OUTDIR"

c++ -std=c++11 \
  "$ROOT/tests/test_book_error.cpp" \
  -I"$ROOT/include" \
  -o "$OUTDIR/test_book_error"

"$OUTDIR/test_book_error"
