#!/bin/sh
set -eu

ROOT="$(CDPATH= cd -- "$(dirname "$0")/.." && pwd)"
OUTDIR="${TMPDIR:-/tmp}/3dslibris-tests"
mkdir -p "$OUTDIR"

"${CXX:-c++}" -std=c++11 \
  ${CXXFLAGS:-} \
  "$ROOT/tests/test_fixed_layout_blit_utils.cpp" \
  "$ROOT/source/formats/common/fixed_layout_blit_utils.cpp" \
  "$ROOT/source/shared/image_scale_utils.cpp" \
  -I"$ROOT/tests/stubs" \
  -I"$ROOT/include" \
  ${LDFLAGS:-} \
  -o "$OUTDIR/test_fixed_layout_blit_utils"

"$OUTDIR/test_fixed_layout_blit_utils"
