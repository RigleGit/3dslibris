#!/usr/bin/env bash
set -euo pipefail
source "$(dirname "$0")/test_build.sh"
TASK_TMP="$(mktemp -d)"
trap 'rm -rf "$TASK_TMP"' EXIT
for signature in 'bool LibraryController::PreloadRoot(' 'bool LibraryController::PreloadCancelled(' 'Book *LibraryController::PreloadBook(' 'LibraryController::PreloadMetadata(' 'LibraryController::PreloadCover(' 'void LibraryController::ResumeLibraryPreload(' 'void LibraryController::RunLibraryPreloadFrame(' 'void LibraryController::CloseLibraryPreload('; do
  case "$signature" in LibraryController::PreloadMetadata*|LibraryController::PreloadCover*) printf "library_preload::Result\n" >> "$TASK_TMP/preload_ui.inc";; esac
  python3 "$TEST_ROOT/tests/extract_test_function.py" "$TEST_ROOT/source/library/library_preload_view.cpp" "$signature" >> "$TASK_TMP/preload_ui.inc"
done
build_test test_library_preload_ui -- -I"$TEST_ROOT/tests/stubs/image_viewer" -I"$TASK_TMP" -I"$TEST_ROOT/tests/stubs" -I"$TEST_ROOT/include" \
 "$TEST_ROOT/tests/test_library_preload_ui.cpp" "$TEST_ROOT/source/library/library_preload.cpp" "$TEST_ROOT/source/library/library_walk.cpp" \
 "$TEST_ROOT/source/library/browser_view_utils.cpp" "$TEST_ROOT/source/shared/app_flow_utils.cpp" "$TEST_ROOT/source/shared/utf8_utils.cpp"
