#!/usr/bin/env bash
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
source "$ROOT/tests/test_build.sh"
TASK_TMP="$(mktemp -d)"
trap 'rm -rf "$TASK_TMP"' EXIT
for signature in 'void ReaderController::StartSearch(' 'void ReaderController::RunSearchFrame(' 'void ReaderController::EditSearchText('; do
  python3 "$ROOT/tests/extract_test_function.py" "$ROOT/source/reader/reader_search.cpp" "$signature" >> "$TASK_TMP/reader_search_under_test.inc"
done
for signature in 'bool SetPage(' 'bool TurnPage('; do
  printf '\nnamespace book_nav {\n' >> "$TASK_TMP/reader_search_under_test.inc"
  python3 "$ROOT/tests/extract_test_function.py" "$ROOT/source/reader/book_page_nav.cpp" "$signature" >> "$TASK_TMP/reader_search_under_test.inc"
  printf '\n}\n' >> "$TASK_TMP/reader_search_under_test.inc"
done
build_test test_reader_search_state -- -I"$TASK_TMP" -I"$ROOT/tests/stubs" -I"$ROOT/include" "$ROOT/tests/test_reader_search_state.cpp" "$ROOT/source/book/text_search.cpp" "$ROOT/source/reader/fixed_layout_input_utils.cpp"
