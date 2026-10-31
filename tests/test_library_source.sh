#!/usr/bin/env bash
set -euo pipefail
source "$(dirname "$0")/test_build.sh"
TASK_TMP="$(mktemp -d)"
trap 'rm -rf "$TASK_TMP"' EXIT
for signature in 'static bool PathIsDirectory(' 'static std::string LibraryBookSource(' 'static bool PathIsRegularFile(' 'static std::string BasenameFromPath(' 'static Book *FindBookByFolderAndFile(' 'static Book *FindBookByFileNameOnly(' 'static bool PathContainsFileCaseInsensitive(' 'static bool PathContainsExactRegularFile(' 'int LibraryController::FindBooks(' 'Book *LibraryController::RestoreSavedBookSelection('; do
  python3 "$TEST_ROOT/tests/extract_test_function.py" "$TEST_ROOT/source/library/library_controller.cpp" "$signature" >> "$TASK_TMP/library_source.inc"
done
export LIBRARY_SOURCE_TEST_DIR="$TASK_TMP"
build_test test_library_source -- -I"$TASK_TMP" -I"$TEST_ROOT/include" \
 "$TEST_ROOT/tests/test_library_source.cpp" "$TEST_ROOT/source/shared/app_flow_utils.cpp" "$TEST_ROOT/source/shared/utf8_utils.cpp"
