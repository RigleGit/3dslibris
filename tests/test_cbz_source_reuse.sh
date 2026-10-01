#!/usr/bin/env bash
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
source "$ROOT/tests/test_build.sh"
TASK_TMP="$(mktemp -d)"
trap 'rm -rf "$TASK_TMP"' EXIT
for signature in 'void ResetCbzPageBitmap(' 'bool PromoteCbzAdjacentSlotIfMatching(' 'inline bool CbzSourceValid(' 'bool DecodeCbzPageImageWithFallback(' 'bool EnsureCbzSourceLoaded(' 'bool EnsureCbzPreviewCache(' 'bool EnsureCbzInteractiveCache('; do
  python3 "$ROOT/tests/extract_test_function.py" "$ROOT/source/formats/cbz/cbz_view.cpp" "$signature" >> "$TASK_TMP/cbz_source_under_test.inc"
done
build_test test_cbz_source_reuse -- -I"$TASK_TMP" -I"$ROOT/tests/stubs" -I"$ROOT/include" "$ROOT/tests/test_cbz_source_reuse.cpp" "$ROOT/source/formats/common/pdf_view_utils.cpp"
