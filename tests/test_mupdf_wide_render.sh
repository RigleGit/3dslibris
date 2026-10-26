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
# Exercise the actual interactive-cache owner with real MuPDF. Only inactive
# worker platform operations are substituted in the test.
for signature in 'void ResetMuPdfRenderFailureState(' 'bool HasMuPdfInteractiveRenderFailed(' 'void CancelMuPdfIncrementalRenderState(' 'bool PromoteMuPdfAdjacentSlotIfMatching(' 'bool EnsureMuPdfDisplayListForPage(' 'bool EnsureCurrentMuPdfInteractiveTile('; do
  python3 "$ROOT/tests/extract_test_function.py" "$ROOT/source/formats/mupdf/mupdf_worker.cpp" "$signature" >> "$TASK_TMP/interactive_owner.inc"
done
for signature in 'MuPdfNavigationBounds GetMuPdfNavigationBounds(' 'static void GetMuPdfPreviewContentBounds(' 'pdf_view_utils::NormalizedRect ComputeCurrentMuPdfViewport('; do
  python3 "$ROOT/tests/extract_test_function.py" "$ROOT/source/formats/mupdf/mupdf_viewport.cpp" "$signature" >> "$TASK_TMP/viewport_owner.inc"
done
for signature in 'float ComputeFitScale(' 'float ComputeEffectiveMuPdfZoom('; do
  python3 "$ROOT/tests/extract_test_function.py" "$ROOT/source/formats/mupdf/mupdf_common.cpp" "$signature" >> "$TASK_TMP/viewport_owner.inc"
done
"${CXX:-c++}" -std=c++11 -ffunction-sections -fdata-sections ${CXXFLAGS:-} \
  -I"$TASK_TMP" -I"$ROOT/tests/stubs/wide_surface" -I"$ROOT/include" \
  -I"$ROOT/third_party/mupdf/include" \
  "$ROOT/tests/test_mupdf_wide_render.cpp" \
  "$ROOT/source/formats/mupdf/mupdf_render.cpp" \
  "$ROOT/source/formats/common/pdf_view_utils.cpp" \
  "$ROOT/source/shared/app_flow_utils.cpp" \
  "$MUPDF_HOST_LIB_DIR/libmupdf.a" "$MUPDF_HOST_LIB_DIR/libmupdf-third.a" \
  ${LDFLAGS:-} "$DEAD_STRIP" -lm -o "$TASK_TMP/test_mupdf_wide_render"
"$TASK_TMP/test_mupdf_wide_render" "$TASK_TMP/stripes.pdf"
