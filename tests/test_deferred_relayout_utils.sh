#!/bin/sh
set -eu

ROOT="$(CDPATH= cd -- "$(dirname "$0")/.." && pwd)"
OUTDIR="${TMPDIR:-/tmp}/3dslibris-tests"
mkdir -p "$OUTDIR"

c++ -std=c++11 \
  "$ROOT/tests/test_deferred_relayout_utils.cpp" \
  "$ROOT/source/book/layout_reflow.cpp" \
  -I"$ROOT/include" \
  -I"$ROOT/source/book" \
  -o "$OUTDIR/test_deferred_relayout_utils"

"$OUTDIR/test_deferred_relayout_utils"
