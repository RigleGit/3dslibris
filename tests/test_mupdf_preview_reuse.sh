#!/usr/bin/env bash
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
source "$ROOT/tests/test_build.sh"
TASK_TMP="$(mktemp -d)"
trap 'rm -rf "$TASK_TMP"' EXIT
python3 "$ROOT/tests/extract_test_function.py" "$ROOT/source/formats/mupdf/mupdf_viewport.cpp" 'bool MuPdfInteractiveTileCoversViewport(' > "$TASK_TMP/mupdf_preview_under_test.inc"
python3 "$ROOT/tests/extract_test_function.py" "$ROOT/source/formats/mupdf/mupdf_viewport.cpp" 'bool CanReuseMuPdfInteractiveRaster(' >> "$TASK_TMP/mupdf_preview_under_test.inc"
for signature in 'void ResetMuPdfRenderFailureState(' 'static void RetireMuPdfDisplayList(' 'bool EnsureMuPdfDisplayListForPage(' 'bool IsMuPdfPageRenderBlocked(' 'bool EnsureCurrentMuPdfPreviewCache(' 'bool EnsureCurrentMuPdfInteractiveTile('; do
  python3 "$ROOT/tests/extract_test_function.py" "$ROOT/source/formats/mupdf/mupdf_worker.cpp" "$signature" >> "$TASK_TMP/mupdf_preview_under_test.inc"
done
python3 "$ROOT/tests/extract_test_function.py" "$ROOT/source/formats/mupdf/mupdf_viewport.cpp" "static void ResetMuPdfDeferredCachesForSynchronousRender(" >> "$TASK_TMP/mupdf_preview_under_test.inc"
for signature in 'static bool BlitBitmapCacheViewport(' 'static bool BlitBitmapCacheViewportRegion('; do
  python3 "$ROOT/tests/extract_test_function.py" "$ROOT/source/formats/mupdf/mupdf_draw.cpp" "$signature" >> "$TASK_TMP/mupdf_preview_under_test.inc"
done
build_test test_mupdf_preview_reuse -- -I"$TASK_TMP" -I"$ROOT/tests/stubs" -I"$ROOT/include" "$ROOT/tests/test_mupdf_preview_reuse.cpp" "$ROOT/source/formats/common/fixed_layout_surface.cpp" "$ROOT/source/formats/common/pdf_view_utils.cpp" "$ROOT/source/formats/common/fixed_layout_blit_utils.cpp"
