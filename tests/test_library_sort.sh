#!/usr/bin/env bash
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
source "$ROOT/tests/test_build.sh"
TASK_TMP="$(mktemp -d)"
trap 'rm -rf "$TASK_TMP"' EXIT
export TEST_LIBRARY_SORT_DIR="$TASK_TMP"
for signature in 'static bool BookTitleLessThan(' 'static bool BookFilenameLessThan(' 'static bool BookAuthorLessThan(' 'static const char *BookFileExtension(' 'static bool BookFiletypeLessThan(' 'static time_t BookFileMtime(' 'static bool BookRecentLessThan(' 'void LibraryController::SortBooks('; do
  python3 "$ROOT/tests/extract_test_function.py" "$ROOT/source/library/library_controller.cpp" "$signature" >> "$TASK_TMP/library_sort.inc"
done
build_test test_library_sort -- -I"$TASK_TMP" -I"$ROOT/tests/stubs" -I"$ROOT/include" "$ROOT/tests/test_library_sort.cpp"
