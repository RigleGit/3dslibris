#!/bin/sh
set -eu
source "$(dirname "$0")/test_build.sh"
build_test test_debug_runtime_mode \
  "$TEST_ROOT/tests/test_debug_runtime_mode.cpp"
