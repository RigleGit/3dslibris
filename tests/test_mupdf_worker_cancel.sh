#!/usr/bin/env bash
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
source "$ROOT/tests/test_build.sh"
TASK_TMP="$(mktemp -d)"
trap 'rm -rf "$TASK_TMP"' EXIT
python3 "$ROOT/tests/extract_test_function.py" "$ROOT/source/formats/mupdf/mupdf_worker.cpp" "void CancelMuPdfIncrementalRenderState(" >> "$TASK_TMP/mupdf_cancel_under_test.inc"
python3 "$ROOT/tests/extract_test_function.py" "$ROOT/source/formats/mupdf/mupdf_worker.cpp" "void ShutdownMuPdfWorker(" >> "$TASK_TMP/mupdf_cancel_under_test.inc"
build_test test_mupdf_worker_cancel -- -I"$TASK_TMP" -I"$ROOT/include" "$ROOT/tests/test_mupdf_worker_cancel.cpp"
