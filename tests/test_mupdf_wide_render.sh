#!/usr/bin/env bash
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
source "$ROOT/tests/test_build.sh"
if [ -z "${MUPDF_HOST_LIB_DIR:-}" ]; then
  echo 'SKIP: native MuPDF libraries required (MUPDF_HOST_LIB_DIR)'
  exit 77
fi
TASK_TMP="$(mktemp -d)"
trap 'rm -rf "$TASK_TMP"' EXIT
if [ "$(uname -s)" = Darwin ]; then
  DEAD_STRIP=-Wl,-dead_strip
else
  DEAD_STRIP=-Wl,--gc-sections
fi
"${CXX:-c++}" -std=c++11 -ffunction-sections -fdata-sections ${CXXFLAGS:-} \
  -I"$ROOT/tests/stubs/wide_surface" -I"$ROOT/include" \
  -I"$ROOT/third_party/mupdf/include" \
  "$ROOT/tests/test_mupdf_wide_render.cpp" \
  "$ROOT/source/formats/mupdf/mupdf_render.cpp" \
  "$MUPDF_HOST_LIB_DIR/libmupdf.a" "$MUPDF_HOST_LIB_DIR/libmupdf-third.a" \
  ${LDFLAGS:-} "$DEAD_STRIP" -lm -o "$TASK_TMP/test_mupdf_wide_render"
"$TASK_TMP/test_mupdf_wide_render" "$TASK_TMP/stripes.pdf"
