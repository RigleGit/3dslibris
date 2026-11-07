#!/bin/sh
set -eu
ROOT="$(CDPATH= cd -- "$(dirname "$0")/.." && pwd)"
OUTDIR="${TMPDIR:-/tmp}/3dslibris-tests"
mkdir -p "$OUTDIR"
"${CXX:-c++}" -std=c++11 ${CXXFLAGS:-} -UNDEBUG \
  "$ROOT/tests/test_main_loop_preview_pan.cpp" \
  "$ROOT/source/app/main_loop_controller.cpp" \
  "$ROOT/source/formats/common/fixed_layout_preview_panel.cpp" \
  "$ROOT/source/formats/common/fixed_layout_preview_border.cpp" \
  "$ROOT/source/formats/common/fixed_layout_surface.cpp" \
  -I"$ROOT/tests/main_loop_preview_stubs" \
  -I"$ROOT/tests/preview_panel_stubs" -I"$ROOT/include" \
  ${LDFLAGS:-} -o "$OUTDIR/test_main_loop_preview_pan"
"$OUTDIR/test_main_loop_preview_pan"
