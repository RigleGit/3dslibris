#!/usr/bin/env bash
set -euo pipefail
source "$(dirname "$0")/test_build.sh"
TASK_TMP="$(mktemp -d)"
trap 'rm -rf "$TASK_TMP"' EXIT
cd "$TASK_TMP"
for mode in release debug; do
  flags=()
  if [ "$mode" = debug ]; then
    flags+=(-DDSLIBRIS_DEBUG)
  fi
  build_test "test_boot_trace_$mode" -- \
      -I"$TEST_ROOT/tests/stubs/boot_trace" \
      ${flags[@]+"${flags[@]}"} \
      "$TEST_ROOT/tests/test_boot_trace.cpp" \
      "$TEST_ROOT/source/shared/boot_trace.cpp"
done
