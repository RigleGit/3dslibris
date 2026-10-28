#!/usr/bin/env bash
set -euo pipefail
source "$(dirname "$0")/test_build.sh"
build_test test_fs_handle_close -- -I"$TEST_ROOT/tests/fs_close_stubs" \
  "$TEST_ROOT/tests/test_fs_handle_close.cpp" \
  "$TEST_ROOT/source/shared/fs_handle_close.cpp"
