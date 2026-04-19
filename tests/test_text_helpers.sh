#!/bin/sh
set -eu
source "$(dirname "$0")/test_build.sh"
build_test test_text_helpers \
  "$TEST_ROOT/tests/test_text_helpers.cpp" \
  "$TEST_ROOT/source/formats/common/text_helpers.cpp" \
  "$TEST_ROOT/source/shared/utf8_utils.cpp"
