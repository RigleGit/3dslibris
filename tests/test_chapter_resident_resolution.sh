#!/bin/bash
set -eu
source "$(dirname "$0")/test_build.sh"
TASK_TMP=$(mktemp -d)
trap 'rm -rf "$TASK_TMP"' EXIT
python3 "$TEST_ROOT/tests/extract_test_function.py" "$TEST_ROOT/source/menus/chapter_menu.cpp" 'bool ChapterMenu::ResolveTargetPage(' > "$TASK_TMP/chapter_resolution_owner.inc"
build_test test_chapter_resident_resolution -I"$TASK_TMP" -- "$TEST_ROOT/tests/test_chapter_resident_resolution.cpp"
