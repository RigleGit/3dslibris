#!/bin/bash
set -eu
source "$(dirname "$0")/test_build.sh"
CXXFLAGS="${CXXFLAGS:-} -I$TEST_ROOT/tests/stubs"
FIXTURES="$(mktemp -d)"
trap 'rm -rf "$FIXTURES"' EXIT
python3 "$TEST_ROOT/tests/fixtures/generate_mobi_reading.py" "$FIXTURES"
export TEST_MOBI_FIXTURE_DIR="$FIXTURES"
build_test test_mobi_record_reader -- \
  "$TEST_ROOT/tests/test_mobi_record_reader.cpp" \
  "$TEST_ROOT/source/formats/mobi/mobi_record_reader.cpp" \
  "$TEST_ROOT/source/formats/mobi/mobi_record_scan.cpp" \
  "$TEST_ROOT/source/formats/mobi/mobi_record_decode.cpp" \
  "$TEST_ROOT/source/formats/mobi/mobi_parser_core.cpp" \
  "$TEST_ROOT/source/formats/mobi/mobi_text_decode.cpp" \
  "$TEST_ROOT/source/formats/common/file_read_utils.cpp" \
  "$TEST_ROOT/source/formats/common/text_helpers.cpp" \
  "$TEST_ROOT/source/shared/utf8_utils.cpp"
