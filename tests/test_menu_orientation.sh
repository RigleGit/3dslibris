#!/usr/bin/env bash
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
SOURCE_ROOT="${MENU_ORIENTATION_SOURCE_ROOT:-$ROOT}"
source "$ROOT/tests/test_build.sh"
TASK_TMP="$(mktemp -d)"
trap 'rm -rf "$TASK_TMP"' EXIT
python3 "$ROOT/tests/extract_test_function.py" \
  "$SOURCE_ROOT/source/menus/paged_list_menu.cpp" \
  'void PagedListMenu::HandleInput(' > "$TASK_TMP/menu_orientation_under_test.inc"
for signature in 'void FontMenu::handleTargetInput(' 'void FontMenu::handleFileInput('; do
  python3 "$ROOT/tests/extract_test_function.py" \
    "$SOURCE_ROOT/source/settings/font.cpp" "$signature" \
    >> "$TASK_TMP/menu_orientation_under_test.inc"
done
for signature in 'static u8 NormalizeVisibleCount(' 'static void ClampSelectedIndex(' 'void SettingsController::PrefsHandleEvent('; do
  python3 "$ROOT/tests/extract_test_function.py" \
    "$SOURCE_ROOT/source/settings/app_prefs.cpp" "$signature" \
    >> "$TASK_TMP/menu_orientation_under_test.inc"
done
build_test test_menu_orientation -- -I"$ROOT/tests/stubs" -I"$TASK_TMP" \
  "$ROOT/tests/test_menu_orientation.cpp" \
  "$ROOT/source/settings/prefs_input_utils.cpp"
