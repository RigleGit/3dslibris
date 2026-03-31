#!/bin/sh
set -eu

ROOT="$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)"
OUTDIR="${TMPDIR:-/tmp}/3dslibris-tests"
mkdir -p "$OUTDIR"

c++ -std=c++17 -Wall -Wextra -I"$ROOT/include" \
  "$ROOT/tests/test_reflow_cache_save_utils.cpp" \
  -o "$OUTDIR/test_reflow_cache_save_utils"

"$OUTDIR/test_reflow_cache_save_utils"
