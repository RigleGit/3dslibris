#!/bin/sh
set -eu

ROOT="$(CDPATH= cd -- "$(dirname "$0")/.." && pwd)"
OUTDIR="${TMPDIR:-/tmp}/3dslibris-tests"
mkdir -p "$OUTDIR"

c++ -std=c++11 \
  "$ROOT/tests/test_browser_nav.cpp" \
  "$ROOT/source/ui/browser_nav.cpp" \
  -I"$ROOT/include" \
  -o "$OUTDIR/test_browser_nav"

"$OUTDIR/test_browser_nav"
