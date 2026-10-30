#!/usr/bin/env bash
set -euo pipefail
source "$(dirname "$0")/test_build.sh"
build_test test_library_search -- -I"$TEST_ROOT/tests/stubs" -I"$TEST_ROOT/include" \
  -I"$TEST_ROOT/third_party/utf8proc" "$TEST_ROOT/tests/test_library_search.cpp" \
  "$TEST_ROOT/source/library/library_search.cpp" "$TEST_ROOT/source/library/library_walk.cpp" \
  "$TEST_ROOT/source/shared/app_flow_utils.cpp" "$TEST_ROOT/source/shared/utf8_utils.cpp"
