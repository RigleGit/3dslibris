#!/usr/bin/env bash
set -euo pipefail
source "$(dirname "$0")/test_build.sh"
TASK_TMP="$(mktemp -d)"
trap 'rm -rf "$TASK_TMP"' EXIT
for signature in 'size_t ResultPage(' 'size_t PageEnd(' 'size_t JumpPage(' 'bool LibraryController::SearchRoot(' 'void LibraryController::StartLibrarySearch(' 'void LibraryController::EditLibrarySearch(' 'void LibraryController::RunLibrarySearchFrame('; do
  python3 "$TEST_ROOT/tests/extract_test_function.py" "$TEST_ROOT/source/library/library_search_view.cpp" "$signature" >> "$TASK_TMP/library_search_ui.inc"
done
build_test test_library_search_ui -- -I"$TEST_ROOT/tests/stubs/image_viewer" \
  -I"$TASK_TMP" -I"$TEST_ROOT/tests/stubs" -I"$TEST_ROOT/include" \
  "$TEST_ROOT/tests/test_library_search_ui.cpp" "$TEST_ROOT/source/library/library_search.cpp" "$TEST_ROOT/source/library/library_walk.cpp" \
  "$TEST_ROOT/source/shared/app_flow_utils.cpp" "$TEST_ROOT/source/shared/utf8_utils.cpp" "$TEST_ROOT/source/shared/text_wrap_utils.cpp"
