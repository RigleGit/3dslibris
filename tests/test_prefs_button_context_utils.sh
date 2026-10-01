#!/usr/bin/env bash
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
source "$ROOT/tests/test_build.sh"
TASK_TMP="$(mktemp -d)"
trap 'rm -rf "$TASK_TMP"' EXIT
for signature in 'static void ToggleClockFormatSetting(' 'static void ToggleReopenLastBookSetting(' 'static void CycleColorMode(' 'static void TogglePublisherTextIndentSetting(' 'static void TogglePublisherBlockMarginsSetting(' 'static void TogglePublisherHorizontalMarginsSetting('; do
  python3 "$ROOT/tests/extract_test_function.py" "$ROOT/source/settings/app_prefs.cpp" "$signature" >> "$TASK_TMP/prefs_actions_under_test.inc"
done
build_test test_prefs_button_context_utils -- -I"$TASK_TMP" -I"$ROOT/include" "$ROOT/tests/test_prefs_button_context_utils.cpp" "$ROOT/source/settings/prefs_button_context_utils.cpp" "$ROOT/source/settings/prefs_action_utils.cpp" "$ROOT/source/settings/prefs_input_utils.cpp"
