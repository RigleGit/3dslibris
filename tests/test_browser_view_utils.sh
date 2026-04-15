#!/bin/sh
set -eu
cd "$(dirname "$0")/.."
. tests/test_build.sh
build_test test_browser_view_utils \
  "$TEST_ROOT/tests/test_browser_view_utils.cpp" \
  "$TEST_ROOT/source/library/browser_view_utils.cpp"
