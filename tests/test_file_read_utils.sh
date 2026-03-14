#!/bin/sh
set -eu

ROOT="$(CDPATH= cd -- "$(dirname "$0")/.." && pwd)"
OUTDIR="${TMPDIR:-/tmp}/3dslibris-tests"
mkdir -p "$OUTDIR"

c++ -std=c++11 \
  "$ROOT/tests/test_file_read_utils.cpp" \
  "$ROOT/source/core/file_read_utils.cpp" \
  -I"$ROOT/include" \
  -o "$OUTDIR/test_file_read_utils"

"$OUTDIR/test_file_read_utils"
