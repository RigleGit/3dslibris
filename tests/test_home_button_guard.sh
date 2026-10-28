#!/usr/bin/env bash
set -euo pipefail
source "$(dirname "$0")/test_build.sh"
build_test test_home_button_guard "$TEST_ROOT/tests/test_home_button_guard.cpp" \
  -I"$TEST_ROOT/tests/stubs"
