#!/bin/bash
set -eu
source "$(dirname "$0")/test_build.sh"
CXXFLAGS="${CXXFLAGS:-} -I$TEST_ROOT/tests/stubs"
build_test test_mobi_decode_session -- "$TEST_ROOT/tests/test_mobi_decode_session.cpp" "$TEST_ROOT/source/formats/mobi/mobi_text_decode.cpp" "$TEST_ROOT/source/formats/common/text_helpers.cpp" "$TEST_ROOT/source/shared/utf8_utils.cpp"
