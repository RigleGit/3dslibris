#!/bin/bash
set -eu
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
source "$ROOT/tests/test_build.sh"
TASK_TMP="$(mktemp -d)"
trap 'rm -rf "$TASK_TMP"' EXIT
for signature in 'void DetachCurrentBookForSwitch(' 'void CloseFailedOpenBook('; do
  python3 "$ROOT/tests/extract_test_function.py" "$ROOT/source/reader/app_book_internal.cpp" "$signature" >> "$TASK_TMP/book_switch_under_test.inc"
done
build_test test_book_switch_utils -- -I"$TASK_TMP" -I"$ROOT/include" "$ROOT/tests/test_book_switch_utils.cpp"
