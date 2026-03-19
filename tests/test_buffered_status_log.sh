#!/bin/sh
set -eu

ROOT="$(CDPATH= cd -- "$(dirname "$0")/.." && pwd)"
OUTDIR="${TMPDIR:-/tmp}/3dslibris-tests"
mkdir -p "$OUTDIR"

c++ -std=c++11 \
  "$ROOT/tests/test_buffered_status_log.cpp" \
  "$ROOT/source/core/buffered_status_log.cpp" \
  -I"$ROOT/include" \
  -o "$OUTDIR/test_buffered_status_log"

"$OUTDIR/test_buffered_status_log"
