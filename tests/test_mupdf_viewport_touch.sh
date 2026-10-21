#!/usr/bin/env bash
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
source "$ROOT/tests/test_build.sh"
TASK_TMP="$(mktemp -d)"
trap 'rm -rf "$TASK_TMP"' EXIT
python3 "$ROOT/tests/extract_test_function.py" "$ROOT/include/formats/mupdf/mupdf_common.h" "struct MuPdfNavigationBounds " > "$TASK_TMP/mupdf_viewport_touch_under_test.inc"
printf ';\n' >> "$TASK_TMP/mupdf_viewport_touch_under_test.inc"
python3 "$ROOT/tests/extract_test_function.py" "$ROOT/include/formats/mupdf/mupdf_common.h" "inline bool BitmapCacheValid(" >> "$TASK_TMP/mupdf_viewport_touch_under_test.inc"
printf 'namespace app_flow_utils {\n' >> "$TASK_TMP/mupdf_viewport_touch_under_test.inc"
python3 "$ROOT/tests/extract_test_function.py" "$ROOT/source/shared/app_flow_utils.cpp" "float GetMuPdfReadingBaseZoom(" >> "$TASK_TMP/mupdf_viewport_touch_under_test.inc"
printf '}\n' >> "$TASK_TMP/mupdf_viewport_touch_under_test.inc"
python3 "$ROOT/tests/extract_test_function.py" "$ROOT/source/formats/mupdf/mupdf_common.cpp" "float ComputeEffectiveMuPdfZoom(" >> "$TASK_TMP/mupdf_viewport_touch_under_test.inc"
for signature in 'MuPdfNavigationBounds GetMuPdfNavigationBounds(' 'static pdf_view_utils::NormalizedPoint RecenterMuPdfViewportFromPreview(' 'static void GetMuPdfPreviewContentBounds(' 'pdf_view_utils::NormalizedRect ComputeCurrentMuPdfViewport(' 'bool Book::MoveMuPdfViewportToPreview('; do
  python3 "$ROOT/tests/extract_test_function.py" "$ROOT/source/formats/mupdf/mupdf_viewport.cpp" "$signature" >> "$TASK_TMP/mupdf_viewport_touch_under_test.inc"
done
build_test test_mupdf_viewport_touch -- -I"$TASK_TMP" -I"$ROOT/include" "$ROOT/tests/test_mupdf_viewport_touch.cpp" "$ROOT/source/formats/common/pdf_view_utils.cpp"
