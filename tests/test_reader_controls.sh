#!/usr/bin/env bash
set -euo pipefail
source "$(dirname "$0")/test_build.sh"

build_test test_reader_controls \
  "$TEST_ROOT/tests/test_reader_controls.cpp" \
  "$TEST_ROOT/source/reader/fixed_layout_input_utils.cpp"
"$TEST_OUTDIR/test_reader_controls"
