#!/bin/bash
set -eu
source "$(dirname "$0")/test_build.sh"
"${CXX:-c++}" -std=c++11 -O2 -I"$TEST_ROOT/tests/stubs" -I"$TEST_ROOT/include" "$TEST_ROOT/tests/test_cache_io_probe.cpp" "$TEST_ROOT/source/shared/cache_io_probe.cpp" -o "$TEST_OUTDIR/test_cache_io_probe"
"$TEST_OUTDIR/test_cache_io_probe"
