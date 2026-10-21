#!/usr/bin/env bash
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
source "$ROOT/tests/test_build.sh"
TASK_TMP="$(mktemp -d)"
trap 'rm -rf "$TASK_TMP"' EXIT
for signature in 'namespace {' 'void ReaderController::OpenImageViewer(' 'void ReaderController::DrawImageViewer(' 'void ReaderController::RunImageViewerFrame(' 'void ReaderController::SuspendImageViewer('; do
  python3 "$ROOT/tests/extract_test_function.py" "$ROOT/source/reader/image_viewer.cpp" "$signature" >> "$TASK_TMP/image_viewer_under_test.inc"
done
build_test test_image_viewer_controls -- -I"$TASK_TMP" -I"$ROOT/tests/stubs/image_viewer" -I"$ROOT/tests/stubs" -I"$ROOT/include" -I"$ROOT/third_party/utf8proc" -I"$ROOT/third_party/libunibreak/src" \
  "$ROOT/tests/test_image_viewer_controls.cpp" "$ROOT/source/ui/button.cpp" \
  "$ROOT/source/formats/common/pdf_view_utils.cpp" \
  "$ROOT/source/shared/text_unicode_utils.cpp" "$ROOT/source/shared/text_layout_utils.cpp" \
  "$ROOT/source/shared/text_bidi_utils.cpp" "$ROOT/source/shared/text_arabic_shaping.cpp" \
  "$ROOT/tests/stubs/mupdf_bidi_stub.cpp"
