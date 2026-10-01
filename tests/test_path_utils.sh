#!/bin/bash
set -eu
source "$(dirname "$0")/test_build.sh"
TASK_TMP="$(mktemp -d)"
trap 'rm -rf "$TASK_TMP"' EXIT
python3 "$TEST_ROOT/tests/extract_test_function.py" \
  "$TEST_ROOT/source/ui/font_manager.cpp" \
  'static bool FilenameMatchesCjkPattern(' > "$TASK_TMP/font_filename_match.inc"
build_test test_path_utils -- \
  -I"$TASK_TMP" -I"$TEST_ROOT/include" \
  "$TEST_ROOT/tests/test_path_utils.cpp" \
  "$TEST_ROOT/source/library/cover_override_utils.cpp"
