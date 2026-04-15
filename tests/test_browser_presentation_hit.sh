#!/bin/sh
set -eu
cd "$(dirname "$0")/.."
. tests/test_build.sh
build_test test_browser_presentation_hit \
  "$TEST_ROOT/tests/test_browser_presentation_hit.cpp" \
  "$TEST_ROOT/source/library/browser_presentation_hit_utils.cpp"
