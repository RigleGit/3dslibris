#!/bin/sh
set -eu
cd "$(dirname "$0")/.."
. tests/test_build.sh
build_test test_go_to_page_slider_utils \
  "$TEST_ROOT/tests/test_go_to_page_slider_utils.cpp" \
  "$TEST_ROOT/source/settings/go_to_page_slider_utils.cpp"
