#!/bin/sh
set -eu

ROOT="$(CDPATH= cd -- "$(dirname "$0")/.." && pwd)"
OUTDIR="${TMPDIR:-/tmp}/3dslibris-tests"
mkdir -p "$OUTDIR"

c++ -std=c++11 \
  "$ROOT/tests/test_layout_reflow.cpp" \
  "$ROOT/source/core/layout_reflow.cpp" \
  -I"$ROOT/include" \
  -I"$ROOT/source/core" \
  -o "$OUTDIR/test_layout_reflow"

"$OUTDIR/test_layout_reflow"
