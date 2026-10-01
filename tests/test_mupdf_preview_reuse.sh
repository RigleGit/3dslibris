#!/usr/bin/env bash
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
source "$ROOT/tests/test_build.sh"
TASK_TMP="$(mktemp -d)"
trap 'rm -rf "$TASK_TMP"' EXIT
for signature in 'bool EnsureMuPdfDisplayListForPage(' 'bool EnsureCurrentMuPdfPreviewCache('; do
  python3 "$ROOT/tests/extract_test_function.py" "$ROOT/source/formats/mupdf/mupdf_worker.cpp" "$signature" >> "$TASK_TMP/mupdf_preview_under_test.inc"
done
python3 "$ROOT/tests/extract_test_function.py" "$ROOT/source/formats/mupdf/mupdf_viewport.cpp" "static void ResetMuPdfDeferredCachesForSynchronousRender(" >> "$TASK_TMP/mupdf_preview_under_test.inc"
build_test test_mupdf_preview_reuse -- -I"$TASK_TMP" -I"$ROOT/include" "$ROOT/tests/test_mupdf_preview_reuse.cpp"
