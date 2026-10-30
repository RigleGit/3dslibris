#!/usr/bin/env bash
set -euo pipefail
source "$(dirname "$0")/test_build.sh"
build_test test_library_walk "$TEST_ROOT/tests/test_library_walk.cpp" \
 "$TEST_ROOT/source/library/library_walk.cpp" "$TEST_ROOT/source/shared/app_flow_utils.cpp" \
 "$TEST_ROOT/source/shared/utf8_utils.cpp"
