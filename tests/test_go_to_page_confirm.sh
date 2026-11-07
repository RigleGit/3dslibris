#!/usr/bin/env bash
set -euo pipefail
source "$(dirname "$0")/test_build.sh"
TASK_TMP="$(mktemp -d)"
trap 'rm -rf "$TASK_TMP"' EXIT
python3 "$TEST_ROOT/tests/extract_test_function.py" "$TEST_ROOT/source/menus/go_to_page_dialog.cpp" 'bool GoToPageDialog::Confirm(' > "$TASK_TMP/go_to_confirm_owner.inc"
build_test test_go_to_page_confirm -- -I"$TASK_TMP" -I"$TEST_ROOT/include" "$TEST_ROOT/tests/test_go_to_page_confirm.cpp" "$TEST_ROOT/source/settings/go_to_page_slider_utils.cpp"
