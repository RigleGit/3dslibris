#!/usr/bin/env bash
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
SOURCE_ROOT="${MENU_ORIENTATION_SOURCE_ROOT:-$ROOT}"
source "$ROOT/tests/test_build.sh"
TASK_TMP="$(mktemp -d)"
trap 'rm -rf "$TASK_TMP"' EXIT
for signature in 'static bool UsesFixedLayoutMinimalHud(' 'static void FormatClockString(' 'static void FormatEtaMinutes(' 'static bool FormatEtaPair(' 'StatusController::StatusController(' 'void StatusController::RequestStatusRedraw(' 'void StatusController::UpdateStatus('; do
  python3 "$ROOT/tests/extract_test_function.py" \
    "$SOURCE_ROOT/source/app/status_controller.cpp" "$signature" \
    >> "$TASK_TMP/status_orientation_under_test.inc"
done
python3 "$ROOT/tests/extract_test_function.py" \
  "$ROOT/source/shared/app_flow_utils.cpp" 'StatusSnapshot ComputeStatusSnapshot(' \
  > "$TASK_TMP/status_snapshot_under_test.inc"
build_test test_status_orientation -- -I"$ROOT/tests/stubs" -I"$TASK_TMP" \
  "$ROOT/tests/test_status_orientation.cpp"
