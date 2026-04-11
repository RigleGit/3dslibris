#!/bin/bash
set -eu
source "$(dirname "$0")/test_build.sh"
build_test test_browser_display_name \
  "$TEST_ROOT/tests/test_browser_display_name.cpp" \
  "$TEST_ROOT/source/shared/string_utils.cpp"
