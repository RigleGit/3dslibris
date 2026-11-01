#!/bin/bash
set -eu
source "$(dirname "$0")/test_build.sh"
"${CXX:-c++}" -std=c++11 -O2 -fno-exceptions -fno-rtti -I"$TEST_ROOT/include" "$TEST_ROOT/tests/test_page_cache_codec.cpp" "$TEST_ROOT/source/formats/common/page_cache_codec.cpp" -lz -o "$TEST_OUTDIR/test_page_cache_codec"
"$TEST_OUTDIR/test_page_cache_codec"
