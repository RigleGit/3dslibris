#!/usr/bin/env bash
set -euo pipefail
source "$(dirname "$0")/test_build.sh"
TASK_TMP="$(mktemp -d)"
trap 'rm -rf "$TASK_TMP"' EXIT
for signature in 'void LibraryController::RestoreSearchOrigin(' 'void LibraryController::ActivateLibrarySearchResult(' 'void LibraryController::CloseLibrarySearch('; do
 python3 "$TEST_ROOT/tests/extract_test_function.py" "$TEST_ROOT/source/library/library_search_view.cpp" "$signature" >> "$TASK_TMP/search_navigation.inc"
done
build_test test_library_search_navigation -- -I"$TEST_ROOT/tests/stubs/image_viewer" \
 -I"$TASK_TMP" -I"$TEST_ROOT/tests/stubs" -I"$TEST_ROOT/include" \
 "$TEST_ROOT/tests/test_library_search_navigation.cpp" "$TEST_ROOT/source/library/library_search.cpp" \
 "$TEST_ROOT/source/shared/app_flow_utils.cpp" "$TEST_ROOT/source/shared/utf8_utils.cpp"
