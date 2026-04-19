#!/bin/sh
set -eu
source "$(dirname "$0")/test_build.sh"
build_test test_mobi_toc_finalize_policy \
  "$TEST_ROOT/tests/test_mobi_toc_finalize_policy.cpp" \
  "$TEST_ROOT/source/formats/mobi/mobi_toc_finalize_policy.cpp" \
  "$TEST_ROOT/source/formats/mobi/mobi_position_map.cpp"
