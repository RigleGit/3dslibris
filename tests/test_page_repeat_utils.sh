#!/usr/bin/env bash
set -euo pipefail
source "$(dirname "$0")/test_build.sh"

build_test test_page_repeat_utils \
  "$TEST_ROOT/tests/test_page_repeat_utils.cpp" \
  "$TEST_ROOT/source/reader/page_repeat_utils.cpp"
"$TEST_OUTDIR/test_page_repeat_utils"
