#!/usr/bin/env bash
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
source "$ROOT/tests/test_build.sh"
TASK_TMP="$(mktemp -d)"
trap 'rm -rf "$TASK_TMP"' EXIT
build_test test_prefs_file_utils -- -I"$TASK_TMP" -I"$ROOT/include" "$ROOT/tests/test_prefs_file_utils.cpp" "$ROOT/source/settings/prefs_file_utils.cpp"
